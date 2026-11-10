/* ps5-memcard core. See memcard.h for the card layout. */
#include "memcard.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MC_IO_CHUNK (512 * 1024)
#define MC_TMP_SUFFIX ".mctmp"
#define MC_PREV_SUFFIX ".prev"
#define MC_INDEX_NAME "index.tsv"
#define MC_PROFILE_NAME "profile.txt"
#define MC_LABEL_NAME "label.txt"
#define MC_ACTIVITY_MAX 60
#define MC_RETRY_SECONDS 60
#define MC_FREE_MARGIN (8ull * 1024 * 1024)

/* ── Types ──────────────────────────────────────────────────────── */

typedef struct {
    int enabled;
    int poll_seconds;
    int settle_seconds;
    int min_interval_seconds; /* per save, while its game is running */
    int keep_previous;
    int notify;
    int include_system_backups; /* sdimg_sce_bu_* copies the system keeps itself */
    int usb;                    /* -1 = every memory card, N = only the drive on usbN */
    int all_drives;             /* treat every connected drive as a memory card */
} mc_config_t;

typedef struct {
    char uid[16];
    char title[32];
    char name[128];
    char path[MC_PATH];
    uint64_t size;
    int64_t mtime;
    int is_ps4;
} mc_save_t;

typedef struct {
    char uid[16];
    char title[32];
    char name[128];
    uint64_t size; /* of the console save this copy was made from */
    int64_t mtime;
    uint64_t hash;
    int64_t backed_at;
    int is_ps4;
    char title_name[96];
    char main_title[96];
    char sub_title[96];
} mc_index_t;

typedef struct {
    char key[192];
    uint64_t size;
    int64_t mtime;
    int64_t since;    /* monotonic second this size/mtime was first seen */
    int64_t retry_at; /* monotonic; 0 = no backoff */
} mc_pending_t;

typedef struct {
    char uid[16];
    char title[32];
    char name[128];
    int usb; /* -1 = every card */
} mc_request_t;

/* One USB port. A connected drive is a memory card when it is "present" here. */
typedef struct {
    int present;
    char path[MC_PATH]; /* <usb>/memcard */
    char label[48];
    mc_index_t *index;
    int index_n, index_cap;
    int synced, waiting; /* console saves, as of the last pass */
} mc_card_t;

/* A card as seen by code that works outside the state lock. */
typedef struct {
    int usb;
    char path[MC_PATH];
    char label[48];
} mc_slot_t;

typedef struct {
    char title[32];
    int64_t due;
} mc_deep_t;

typedef struct {
    int64_t at;
    char kind[12];
    char text[232];
} mc_event_t;

typedef struct {
    mc_save_t *v;
    int n, cap;
} mc_save_list_t;

typedef struct {
    char *p;
    size_t n, cap;
} sb_t;

/* ── State ──────────────────────────────────────────────────────── */

static mc_env_t g_env;
/* g_io serialises everything that writes a card or the staging dir.
 * g_st guards the in-memory state below. g_refresh keeps two drive scans from
 * interleaving. Order: g_refresh, g_io, g_st.
 * Environment callbacks are never called with g_io or g_st held. */
static pthread_mutex_t g_refresh = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_io = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_st = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static int g_wake_pending;

static mc_config_t g_cfg = {1, 5, 15, 300, 1, 1, 0, -1, 0};

static mc_card_t g_cards[MC_MAX_USB]; /* by usb port */
static int g_drive[MC_MAX_USB];       /* a drive is connected there, card or not */
static uint64_t g_drive_free[MC_MAX_USB], g_drive_total[MC_MAX_USB];

static mc_pending_t *g_pending;
static int g_pending_n, g_pending_cap;

static mc_request_t g_queue[64];
static int g_queue_n;

static mc_deep_t g_deep[16];
static int g_deep_n;

static mc_event_t g_events[MC_ACTIVITY_MAX];
static int g_event_head, g_event_n;

static char g_running[32];
static int64_t g_last_sweep;
static int g_console_n, g_synced_n, g_waiting_n;

static struct {
    int active;
    int manual;
    int usb;
    char uid[16], title[32], name[128];
    uint64_t done, total;
} g_job;

/* ── Small helpers ──────────────────────────────────────────────── */

static int64_t now_wall(void) {
    return g_env.now_wall ? g_env.now_wall() : (int64_t)time(NULL);
}

static int64_t now_mono(void) {
    if (g_env.now_mono) return g_env.now_mono();
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return (int64_t)time(NULL);
    return (int64_t)ts.tv_sec;
}

static int stopping(void) {
    return g_env.stopping ? g_env.stopping() : 0;
}

static void mc_log(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g_env.log) g_env.log(line);
}

/* Caller must not hold g_st. */
static void event_add(const char *kind, const char *fmt, ...) {
    mc_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.at = now_wall();
    snprintf(ev.kind, sizeof(ev.kind), "%s", kind);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev.text, sizeof(ev.text), fmt, ap);
    va_end(ap);
    mc_log("[memcard] %s: %s\n", kind, ev.text);
    pthread_mutex_lock(&g_st);
    g_events[g_event_head] = ev;
    g_event_head = (g_event_head + 1) % MC_ACTIVITY_MAX;
    if (g_event_n < MC_ACTIVITY_MAX) g_event_n++;
    pthread_mutex_unlock(&g_st);
}

static int has_suffix(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

/* One path component that is safe to append to a card or console path. */
static int component_ok(const char *s, size_t max) {
    size_t n = strlen(s);
    if (n == 0 || n >= max || s[0] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == '/' || c == '\\' || c == 0x7f) return 0;
    }
    return 1;
}

static int id_ok(const char *s, size_t max) {
    size_t n = strlen(s);
    if (n == 0 || n >= max) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '_' && s[i] != '-') return 0;
    return 1;
}

static int is_card_private(const char *name) {
    return name[0] == '.' || has_suffix(name, MC_TMP_SUFFIX) || has_suffix(name, MC_PREV_SUFFIX) ||
           has_suffix(name, ".bin");
}

static void mkdirs(const char *path) {
    char tmp[MC_PATH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void remove_tree(const char *path, int depth) {
    struct stat st;
    if (lstat(path, &st) < 0) return;
    if (S_ISDIR(st.st_mode) && depth < 16) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char child[MC_PATH];
                if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= (int)sizeof(child))
                    continue;
                remove_tree(child, depth + 1);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

static uint64_t tree_size(const char *path, int depth) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (uint64_t)st.st_size;
    if (depth >= 16) return 0;
    uint64_t total = 0;
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[MC_PATH];
        if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= (int)sizeof(child)) continue;
        total += tree_size(child, depth + 1);
    }
    closedir(d);
    return total;
}

static int first_byte(const char *path) {
    unsigned char b = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = pread(fd, &b, 1, 0);
    close(fd);
    return r == 1 ? b : -1;
}

#define FNV_BASIS 1469598103934665603ULL
#define FNV_PRIME 1099511628211ULL

static uint64_t fnv_update(uint64_t h, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= FNV_PRIME;
    }
    return h;
}

/* Copy src to dst. Reports progress into g_job when track is set. */
static int copy_stream(const char *src, const char *dst, uint64_t *hash_out, int track,
                       char *err, size_t err_size) {
    int in = open(src, O_RDONLY);
    if (in < 0) {
        snprintf(err, err_size, "cannot read %s (errno %d)", src, errno);
        return -1;
    }
    int out = open(dst, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (out < 0) {
        snprintf(err, err_size, "cannot create %s (errno %d)", dst, errno);
        close(in);
        return -1;
    }
    unsigned char *buf = malloc(MC_IO_CHUNK);
    if (!buf) {
        snprintf(err, err_size, "out of memory");
        close(in);
        close(out);
        unlink(dst);
        return -1;
    }
    uint64_t hash = FNV_BASIS;
    int rc = 0;
    for (;;) {
        if (stopping()) {
            snprintf(err, err_size, "interrupted by shutdown");
            rc = -1;
            break;
        }
        ssize_t r = read(in, buf, MC_IO_CHUNK);
        if (r < 0) {
            if (errno == EINTR) continue;
            snprintf(err, err_size, "read failed (errno %d)", errno);
            rc = -1;
            break;
        }
        if (r == 0) break;
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                snprintf(err, err_size, errno == ENOSPC ? "the drive is full" : "write failed (errno %d)",
                         errno);
                rc = -1;
                break;
            }
            off += w;
        }
        if (rc < 0) break;
        hash = fnv_update(hash, buf, (size_t)r);
        if (track) {
            pthread_mutex_lock(&g_st);
            g_job.done += (uint64_t)r;
            pthread_mutex_unlock(&g_st);
        }
    }
    free(buf);
    close(in);
    if (rc == 0 && fsync(out) < 0 && errno != EINVAL && errno != ENOTSUP) {
        snprintf(err, err_size, "flush failed (errno %d)", errno);
        rc = -1;
    }
    if (close(out) < 0 && rc == 0) {
        snprintf(err, err_size, "close failed (errno %d)", errno);
        rc = -1;
    }
    if (rc < 0) unlink(dst);
    if (hash_out) *hash_out = hash;
    return rc;
}

static int copy_tree(const char *src, const char *dst, int depth, char *err, size_t err_size) {
    struct stat st;
    if (stat(src, &st) < 0) {
        snprintf(err, err_size, "cannot read %s", src);
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) return copy_stream(src, dst, NULL, 0, err, err_size);
    if (depth >= 16) {
        snprintf(err, err_size, "folder nesting is too deep");
        return -1;
    }
    mkdirs(dst);
    DIR *d = opendir(src);
    if (!d) {
        snprintf(err, err_size, "cannot open %s", src);
        return -1;
    }
    struct dirent *e;
    int rc = 0;
    while (rc == 0 && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[MC_PATH], b[MC_PATH];
        if (snprintf(a, sizeof(a), "%s/%s", src, e->d_name) >= (int)sizeof(a) ||
            snprintf(b, sizeof(b), "%s/%s", dst, e->d_name) >= (int)sizeof(b)) {
            snprintf(err, err_size, "path is too long");
            rc = -1;
            break;
        }
        rc = copy_tree(a, b, depth + 1, err, err_size);
    }
    closedir(d);
    return rc;
}

static int file_hash(const char *path, uint64_t *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    unsigned char *buf = malloc(MC_IO_CHUNK);
    if (!buf) {
        close(fd);
        return -1;
    }
    uint64_t hash = FNV_BASIS;
    int rc = 0;
    for (;;) {
        if (stopping()) {
            rc = -1;
            break;
        }
        ssize_t r = read(fd, buf, MC_IO_CHUNK);
        if (r < 0) {
            if (errno == EINTR) continue;
            rc = -1;
            break;
        }
        if (r == 0) break;
        hash = fnv_update(hash, buf, (size_t)r);
    }
    free(buf);
    close(fd);
    *out = hash;
    return rc;
}

/* ── String builder / JSON ──────────────────────────────────────── */

static void sb_reserve(sb_t *sb, size_t extra) {
    if (sb->n + extra + 1 <= sb->cap) return;
    size_t cap = sb->cap ? sb->cap * 2 : 1024;
    while (cap < sb->n + extra + 1) cap *= 2;
    char *p = realloc(sb->p, cap);
    if (!p) return;
    sb->p = p;
    sb->cap = cap;
}

static void sb_put(sb_t *sb, const char *s) {
    size_t n = strlen(s);
    sb_reserve(sb, n);
    if (sb->n + n + 1 > sb->cap) return;
    memcpy(sb->p + sb->n, s, n + 1);
    sb->n += n;
}

static void sb_printf(sb_t *sb, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_put(sb, tmp);
}

/* Quoted JSON string. Bytes >= 0x80 pass through (names are UTF-8). */
static void sb_str(sb_t *sb, const char *s) {
    sb_put(sb, "\"");
    char tmp[8];
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)c;
            tmp[2] = 0;
        } else if (c < 0x20 || c == 0x7f) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", c);
        } else {
            tmp[0] = (char)c;
            tmp[1] = 0;
        }
        sb_put(sb, tmp);
    }
    sb_put(sb, "\"");
}

static char *sb_done(sb_t *sb) {
    if (!sb->p) return strdup("{\"error\":\"Out of memory\"}");
    return sb->p;
}

static char *json_error(const char *fmt, ...) {
    char msg[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    sb_t sb = {0};
    sb_put(&sb, "{\"error\":");
    sb_str(&sb, msg);
    sb_put(&sb, "}");
    return sb_done(&sb);
}

int mc_url_param(const char *url, const char *key, char *out, size_t size) {
    out[0] = 0;
    const char *q = strchr(url, '?');
    size_t klen = strlen(key);
    while (q && *q) {
        q++;
        if (!strncmp(q, key, klen) && q[klen] == '=') {
            const char *v = q + klen + 1;
            size_t o = 0;
            while (*v && *v != '&' && o + 1 < size) {
                if (*v == '%' && isxdigit((unsigned char)v[1]) && isxdigit((unsigned char)v[2])) {
                    char hex[3] = {v[1], v[2], 0};
                    out[o++] = (char)strtol(hex, NULL, 16);
                    v += 3;
                } else if (*v == '+') {
                    out[o++] = ' ';
                    v++;
                } else {
                    out[o++] = *v++;
                }
            }
            out[o] = 0;
            return 1;
        }
        q = strchr(q, '&');
    }
    return 0;
}

static int json_int(const char *json, const char *key, int *out) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!strncmp(p, "true", 4)) {
        *out = 1;
        return 1;
    }
    if (!strncmp(p, "false", 5)) {
        *out = 0;
        return 1;
    }
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out = (int)v;
    return 1;
}

/* ── Config ─────────────────────────────────────────────────────── */

static void config_clamp(mc_config_t *c) {
    c->enabled = !!c->enabled;
    c->keep_previous = !!c->keep_previous;
    c->notify = !!c->notify;
    c->include_system_backups = !!c->include_system_backups;
    if (c->poll_seconds < 2) c->poll_seconds = 2;
    if (c->poll_seconds > 600) c->poll_seconds = 600;
    if (c->settle_seconds < 0) c->settle_seconds = 0;
    if (c->settle_seconds > 600) c->settle_seconds = 600;
    if (c->min_interval_seconds < 0) c->min_interval_seconds = 0;
    if (c->min_interval_seconds > 86400) c->min_interval_seconds = 86400;
    c->all_drives = !!c->all_drives;
    if (c->usb < -1 || c->usb >= MC_MAX_USB) c->usb = -1;
}

static void config_parse(const char *json, mc_config_t *c) {
    json_int(json, "enabled", &c->enabled);
    json_int(json, "poll_seconds", &c->poll_seconds);
    json_int(json, "settle_seconds", &c->settle_seconds);
    json_int(json, "min_interval_seconds", &c->min_interval_seconds);
    json_int(json, "keep_previous", &c->keep_previous);
    json_int(json, "notify", &c->notify);
    json_int(json, "include_system_backups", &c->include_system_backups);
    json_int(json, "usb", &c->usb);
    json_int(json, "all_drives", &c->all_drives);
    config_clamp(c);
}

static void config_json(sb_t *sb, const mc_config_t *c) {
    sb_printf(sb,
              "{\"enabled\":%s,\"poll_seconds\":%d,\"settle_seconds\":%d,"
              "\"min_interval_seconds\":%d,\"keep_previous\":%s,\"notify\":%s,"
              "\"include_system_backups\":%s,\"usb\":%d,\"all_drives\":%s}",
              c->enabled ? "true" : "false", c->poll_seconds, c->settle_seconds,
              c->min_interval_seconds, c->keep_previous ? "true" : "false",
              c->notify ? "true" : "false", c->include_system_backups ? "true" : "false", c->usb,
              c->all_drives ? "true" : "false");
}

static void config_load(void) {
    if (!g_env.config_path) return;
    FILE *f = fopen(g_env.config_path, "rb");
    if (!f) return;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    mc_config_t c = g_cfg;
    config_parse(buf, &c);
    g_cfg = c;
}

static int config_save(const mc_config_t *c) {
    if (!g_env.config_path) return -1;
    char dir[MC_PATH];
    snprintf(dir, sizeof(dir), "%s", g_env.config_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        mkdirs(dir);
    }
    sb_t sb = {0};
    config_json(&sb, c);
    if (!sb.p) return -1;
    FILE *f = fopen(g_env.config_path, "wb");
    if (!f) {
        free(sb.p);
        return -1;
    }
    fwrite(sb.p, 1, sb.n, f);
    fputc('\n', f);
    fclose(f);
    free(sb.p);
    return 0;
}

/* ── Index, one per card (g_st held by the caller unless noted) ──── */

static mc_index_t *index_find(mc_card_t *c, const char *uid, const char *title, const char *name) {
    for (int i = 0; i < c->index_n; i++)
        if (!strcmp(c->index[i].name, name) && !strcmp(c->index[i].title, title) &&
            !strcmp(c->index[i].uid, uid))
            return &c->index[i];
    return NULL;
}

static mc_index_t *index_put(mc_card_t *c, const mc_index_t *e) {
    mc_index_t *old = index_find(c, e->uid, e->title, e->name);
    if (old) {
        *old = *e;
        return old;
    }
    if (c->index_n >= c->index_cap) {
        int cap = c->index_cap ? c->index_cap * 2 : 64;
        mc_index_t *p = realloc(c->index, (size_t)cap * sizeof(*p));
        if (!p) return NULL;
        c->index = p;
        c->index_cap = cap;
    }
    c->index[c->index_n] = *e;
    return &c->index[c->index_n++];
}

static void index_remove(mc_card_t *c, const char *uid, const char *title, const char *name) {
    mc_index_t *e = index_find(c, uid, title, name);
    if (!e) return;
    *e = c->index[c->index_n - 1];
    c->index_n--;
}

static void tsv_clean(char *s) {
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

static char *tsv_next(char **cursor) {
    char *s = *cursor;
    if (!s) return "";
    char *tab = strchr(s, '\t');
    if (tab) {
        *tab = 0;
        *cursor = tab + 1;
    } else {
        *cursor = NULL;
    }
    return s;
}

static void index_load(mc_card_t *c) {
    c->index_n = 0;
    char path[MC_PATH + 32];
    snprintf(path, sizeof(path), "%s/%s", c->path, MC_INDEX_NAME);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n || line[0] == '#') continue;
        char *cur = line;
        mc_index_t e;
        memset(&e, 0, sizeof(e));
        snprintf(e.uid, sizeof(e.uid), "%s", tsv_next(&cur));
        snprintf(e.title, sizeof(e.title), "%s", tsv_next(&cur));
        snprintf(e.name, sizeof(e.name), "%s", tsv_next(&cur));
        e.size = strtoull(tsv_next(&cur), NULL, 10);
        e.mtime = strtoll(tsv_next(&cur), NULL, 10);
        e.hash = strtoull(tsv_next(&cur), NULL, 16);
        e.backed_at = strtoll(tsv_next(&cur), NULL, 10);
        e.is_ps4 = atoi(tsv_next(&cur));
        snprintf(e.title_name, sizeof(e.title_name), "%s", tsv_next(&cur));
        snprintf(e.main_title, sizeof(e.main_title), "%s", tsv_next(&cur));
        snprintf(e.sub_title, sizeof(e.sub_title), "%s", tsv_next(&cur));
        if (!id_ok(e.uid, sizeof(e.uid)) || !id_ok(e.title, sizeof(e.title)) ||
            !component_ok(e.name, sizeof(e.name)))
            continue;
        index_put(c, &e);
    }
    fclose(f);
}

static int index_save(mc_card_t *c) {
    if (!c->present) return -1;
    char path[MC_PATH + 32], tmp[MC_PATH + 48];
    snprintf(path, sizeof(path), "%s/%s", c->path, MC_INDEX_NAME);
    snprintf(tmp, sizeof(tmp), "%s%s", path, MC_TMP_SUFFIX);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    fprintf(f, "#ps5-memcard-index v1\tuid\ttitle\tsave\tsize\tmtime\thash\tbacked_at\tps4\ttitle_name\tmain_title\tsub_title\n");
    for (int i = 0; i < c->index_n; i++) {
        mc_index_t *e = &c->index[i];
        fprintf(f, "%s\t%s\t%s\t%llu\t%lld\t%016llx\t%lld\t%d\t%s\t%s\t%s\n", e->uid, e->title,
                e->name, (unsigned long long)e->size, (long long)e->mtime,
                (unsigned long long)e->hash, (long long)e->backed_at, e->is_ps4, e->title_name,
                e->main_title, e->sub_title);
    }
    int bad = ferror(f);
    fflush(f);
    fsync(fileno(f));
    if (fclose(f) != 0 || bad) {
        unlink(tmp);
        return -1;
    }
    /* Some USB filesystems refuse to rename over an existing file. */
    unlink(path);
    if (rename(tmp, path) < 0) return -1;
    return 0;
}

/* ── Pending table (g_st held) ──────────────────────────────────── */

static mc_pending_t *pending_get(const char *key, int create) {
    for (int i = 0; i < g_pending_n; i++)
        if (!strcmp(g_pending[i].key, key)) return &g_pending[i];
    if (!create) return NULL;
    if (g_pending_n >= g_pending_cap) {
        int cap = g_pending_cap ? g_pending_cap * 2 : 32;
        mc_pending_t *p = realloc(g_pending, (size_t)cap * sizeof(*p));
        if (!p) return NULL;
        g_pending = p;
        g_pending_cap = cap;
    }
    mc_pending_t *e = &g_pending[g_pending_n++];
    memset(e, 0, sizeof(*e));
    snprintf(e->key, sizeof(e->key), "%s", key);
    return e;
}

static void pending_drop(const char *key) {
    for (int i = 0; i < g_pending_n; i++) {
        if (!strcmp(g_pending[i].key, key)) {
            g_pending[i] = g_pending[g_pending_n - 1];
            g_pending_n--;
            return;
        }
    }
}

/* ── Console scan ───────────────────────────────────────────────── */

static mc_save_t *list_add(mc_save_list_t *l) {
    if (l->n >= l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        mc_save_t *p = realloc(l->v, (size_t)cap * sizeof(*p));
        if (!p) return NULL;
        l->v = p;
        l->cap = cap;
    }
    mc_save_t *s = &l->v[l->n++];
    memset(s, 0, sizeof(*s));
    return s;
}

static int list_has(const mc_save_list_t *l, const char *uid, const char *title, const char *name) {
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->v[i].name, name) && !strcmp(l->v[i].title, title) && !strcmp(l->v[i].uid, uid))
            return 1;
    return 0;
}

static void scan_kind(mc_save_list_t *l, const char *uid, const char *kind, int ps4_dir,
                      int include_sys) {
    char base[MC_PATH];
    snprintf(base, sizeof(base), "%s/%s/%s", g_env.home_root, uid, kind);
    DIR *d = opendir(base);
    if (!d) return;
    struct dirent *te;
    while ((te = readdir(d))) {
        if (!id_ok(te->d_name, 32)) continue;
        char tdir[MC_PATH];
        if (snprintf(tdir, sizeof(tdir), "%s/%s", base, te->d_name) >= (int)sizeof(tdir)) continue;
        DIR *td = opendir(tdir);
        if (!td) continue;
        struct dirent *se;
        while ((se = readdir(td))) {
            const char *name = se->d_name;
            if (!component_ok(name, 128) || has_suffix(name, ".bin")) continue;
            if (ps4_dir && strncmp(name, "sdimg_", 6) != 0) continue;
            if (!include_sys && !strncmp(name, "sdimg_sce_bu_", 13)) continue;
            /* Names the card uses for its own bookkeeping cannot be mirrored. */
            if (has_suffix(name, MC_TMP_SUFFIX) || has_suffix(name, MC_PREV_SUFFIX)) continue;
            char path[MC_PATH];
            if (snprintf(path, sizeof(path), "%s/%s", tdir, name) >= (int)sizeof(path)) continue;
            struct stat st;
            if (stat(path, &st) < 0 || !S_ISREG(st.st_mode)) continue;
            if (list_has(l, uid, te->d_name, name)) continue;
            mc_save_t *s = list_add(l);
            if (!s) break;
            snprintf(s->uid, sizeof(s->uid), "%s", uid);
            snprintf(s->title, sizeof(s->title), "%s", te->d_name);
            snprintf(s->name, sizeof(s->name), "%s", name);
            snprintf(s->path, sizeof(s->path), "%s", path);
            s->size = (uint64_t)st.st_size;
            s->mtime = (int64_t)st.st_mtime;
            s->is_ps4 = first_byte(path) == 0x01;
        }
        closedir(td);
    }
    closedir(d);
}

static void scan_console(mc_save_list_t *l, int include_sys) {
    DIR *home = opendir(g_env.home_root);
    if (!home) return;
    struct dirent *ue;
    while ((ue = readdir(home))) {
        if (!id_ok(ue->d_name, 16)) continue;
        /* PS4 saves first: a stray PS4 image under savedata_prospero is a known
         * leftover of old importers and must not shadow the complete copy. */
        scan_kind(l, ue->d_name, "savedata", 1, include_sys);
        scan_kind(l, ue->d_name, "savedata_prospero", 0, include_sys);
    }
    closedir(home);
}

/* ── Card discovery ─────────────────────────────────────────────── */

static int usb_root(int n, char *out, size_t size) {
    snprintf(out, size, g_env.usb_fmt, n);
    struct stat st;
    if (stat(out, &st) < 0 || !S_ISDIR(st.st_mode)) return 0;
    return g_env.is_mounted ? g_env.is_mounted(out) : 1;
}

static void card_key(int usb, const mc_save_t *s, char *key, size_t size) {
    snprintf(key, size, "%d:%s/%s/%s", usb, s->uid, s->title, s->name);
}

/* g_st held. */
static void pending_drop_card(int usb) {
    char prefix[8];
    int n = snprintf(prefix, sizeof(prefix), "%d:", usb);
    for (int i = 0; i < g_pending_n;) {
        if (!strncmp(g_pending[i].key, prefix, (size_t)n)) g_pending[i] = g_pending[--g_pending_n];
        else i++;
    }
}

static void label_clean(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        unsigned char c = (unsigned char)*r;
        if (c < 0x20 || c == 0x7f) continue;
        if (w == s && c == ' ') continue;
        *w++ = (char)c;
    }
    *w = 0;
    while (w > s && w[-1] == ' ') *--w = 0;
}

static void read_label(const char *card, char *out, size_t size) {
    out[0] = 0;
    char path[MC_PATH + 32];
    snprintf(path, sizeof(path), "%s/%s", card, MC_LABEL_NAME);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t n = fread(out, 1, size - 1, f);
    out[n] = 0;
    fclose(f);
    label_clean(out);
}

/* Which mounted drives are memory cards right now:
 *  - the settings pin one port: only that drive;
 *  - "every drive" is on: all of them;
 *  - otherwise every drive that already has a memcard folder, and when no
 *    drive has one the first drive becomes the card.
 * Returns how many cards there are. */
static int cards_refresh(void) {
    pthread_mutex_lock(&g_refresh);
    pthread_mutex_lock(&g_st);
    int pin = g_cfg.usb, all = g_cfg.all_drives;
    int was[MC_MAX_USB];
    for (int n = 0; n < MC_MAX_USB; n++) was[n] = g_cards[n].present;
    pthread_mutex_unlock(&g_st);

    int mounted[MC_MAX_USB] = {0}, has[MC_MAX_USB] = {0}, want[MC_MAX_USB] = {0};
    static char path[MC_MAX_USB][MC_PATH]; /* guarded by g_refresh */
    char label[MC_MAX_USB][48];
    uint64_t free_b[MC_MAX_USB] = {0}, total_b[MC_MAX_USB] = {0};
    int first = -1, any = 0;
    for (int n = 0; n < MC_MAX_USB; n++) {
        char root[MC_PATH - 16];
        label[n][0] = 0;
        mounted[n] = usb_root(n, root, sizeof(root));
        if (!mounted[n]) continue;
        snprintf(path[n], MC_PATH, "%s/%s", root, MC_CARD_DIR);
        struct stat st;
        has[n] = stat(path[n], &st) == 0 && S_ISDIR(st.st_mode);
        any |= has[n];
        if (first < 0) first = n;
        if (g_env.space) g_env.space(root, &free_b[n], &total_b[n]);
    }
    if (pin >= 0) {
        want[pin] = mounted[pin];
    } else {
        for (int n = 0; n < MC_MAX_USB; n++) want[n] = mounted[n] && (all || has[n]);
        if (!all && !any && first >= 0) want[first] = 1;
    }
    int changed = 0, count = 0;
    for (int n = 0; n < MC_MAX_USB; n++) {
        /* A card always has its folder, so it stays a card when a second one shows up. */
        if (want[n] && !has[n]) mkdir(path[n], 0777);
        if (want[n]) read_label(path[n], label[n], sizeof(label[n]));
        if (want[n] != was[n]) changed = 1;
        count += want[n];
    }

    int appeared[MC_MAX_USB] = {0}, vanished[MC_MAX_USB] = {0}, indexed[MC_MAX_USB] = {0};
    if (changed) pthread_mutex_lock(&g_io); /* nobody mid-copy while an index is swapped */
    pthread_mutex_lock(&g_st);
    for (int n = 0; n < MC_MAX_USB; n++) {
        mc_card_t *c = &g_cards[n];
        g_drive[n] = mounted[n];
        g_drive_free[n] = free_b[n];
        g_drive_total[n] = total_b[n];
        if (want[n] && !c->present) {
            c->present = 1;
            snprintf(c->path, sizeof(c->path), "%s", path[n]);
            index_load(c);
            c->synced = c->waiting = 0;
            appeared[n] = 1;
            indexed[n] = c->index_n;
        } else if (!want[n] && c->present) {
            c->present = 0;
            c->index_n = 0;
            c->synced = c->waiting = 0;
            pending_drop_card(n);
            vanished[n] = 1;
        }
        if (want[n]) snprintf(c->label, sizeof(c->label), "%s", label[n]);
    }
    pthread_mutex_unlock(&g_st);
    if (changed) pthread_mutex_unlock(&g_io);
    pthread_mutex_unlock(&g_refresh);

    for (int n = 0; n < MC_MAX_USB; n++) {
        if (appeared[n]) event_add("card", "memory card ready on usb%d (%d saves indexed)", n, indexed[n]);
        if (vanished[n]) event_add("card", "memory card removed from usb%d", n);
    }
    return count;
}

/* The cards as of now, for work done outside the state lock. */
static int cards_snapshot(mc_slot_t *out) {
    int count = 0;
    pthread_mutex_lock(&g_st);
    for (int n = 0; n < MC_MAX_USB; n++) {
        if (!g_cards[n].present) continue;
        out[count].usb = n;
        snprintf(out[count].path, sizeof(out[count].path), "%s", g_cards[n].path);
        snprintf(out[count].label, sizeof(out[count].label), "%s", g_cards[n].label);
        count++;
    }
    pthread_mutex_unlock(&g_st);
    return count;
}

static void card_root_of(const char *card, char *root, size_t size) {
    snprintf(root, size, "%s", card);
    char *slash = strrchr(root, '/');
    if (slash) *slash = 0;
}

static void slot_name(const mc_slot_t *slot, char *out, size_t size) {
    if (slot->label[0]) snprintf(out, size, "%s (usb%d)", slot->label, slot->usb);
    else snprintf(out, size, "usb%d", slot->usb);
}

/* ── Backup of one save to one card ─────────────────────────────── */

static void ps4_bin_name(const char *save_name, char *out, size_t size) {
    snprintf(out, size, "%s.bin", !strncmp(save_name, "sdimg_", 6) ? save_name + 6 : save_name);
}

static void write_profile(const char *card, const char *uid, const char *user) {
    if (!user[0]) return;
    char path[MC_PATH + 64], old[64] = {0};
    snprintf(path, sizeof(path), "%s/%s/%s", card, uid, MC_PROFILE_NAME);
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(old, 1, sizeof(old) - 1, f);
        old[n] = 0;
        fclose(f);
        if (!strcmp(old, user)) return;
    }
    f = fopen(path, "wb");
    if (!f) return;
    fputs(user, f);
    fclose(f);
}

static void read_profile(const char *card, const char *uid, char *out, size_t size) {
    out[0] = 0;
    char path[MC_PATH + 64];
    snprintf(path, sizeof(path), "%s/%s/%s", card, uid, MC_PROFILE_NAME);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t n = fread(out, 1, size - 1, f);
    out[n] = 0;
    fclose(f);
    tsv_clean(out);
}

/* Replace dst with tmp, keeping the old dst as .prev when asked. */
static int publish(const char *tmp, const char *dst, int keep_previous) {
    char prev[MC_PATH + 16];
    snprintf(prev, sizeof(prev), "%s%s", dst, MC_PREV_SUFFIX);
    int had = exists(dst);
    if (had) {
        if (keep_previous) {
            unlink(prev);
            if (rename(dst, prev) < 0) unlink(dst);
        } else {
            unlink(dst);
        }
    }
    /* A stale generation from before the option was turned off would be misleading. */
    if (!keep_previous) unlink(prev);
    if (rename(tmp, dst) < 0) {
        int saved = errno;
        if (had && keep_previous) rename(prev, dst);
        errno = saved;
        return -1;
    }
    return 0;
}

/* 0 = copied, 1 = the save changed while copying (try again later), -1 = error. */
static int backup_one(const mc_save_t *s, const mc_slot_t *slot, const mc_config_t *cfg, int manual,
                      char *err, size_t err_size) {
    const char *card = slot->path;

    /* Descriptions come from callbacks, so fetch them before taking any lock. */
    mc_index_t e;
    memset(&e, 0, sizeof(e));
    snprintf(e.uid, sizeof(e.uid), "%s", s->uid);
    snprintf(e.title, sizeof(e.title), "%s", s->title);
    snprintf(e.name, sizeof(e.name), "%s", s->name);
    e.is_ps4 = s->is_ps4;
    if (g_env.title_name) g_env.title_name(s->title, e.title_name, sizeof(e.title_name));
    if (g_env.save_info)
        g_env.save_info(s->uid, s->title, s->name, s->is_ps4, e.main_title, sizeof(e.main_title),
                        e.sub_title, sizeof(e.sub_title));
    char user[64] = {0};
    if (g_env.user_name) g_env.user_name(s->uid, user, sizeof(user));
    tsv_clean(e.title_name);
    tsv_clean(e.main_title);
    tsv_clean(e.sub_title);
    tsv_clean(user);

    char root[MC_PATH];
    card_root_of(card, root, sizeof(root));
    uint64_t free_b = 0, total_b = 0;
    int have_space = g_env.space && g_env.space(root, &free_b, &total_b) == 0;

    pthread_mutex_lock(&g_io);
    char dir[MC_PATH], dst[MC_PATH], tmp[MC_PATH + 16];
    if (snprintf(dir, sizeof(dir), "%s/%s/%s", card, s->uid, s->title) >= (int)sizeof(dir) ||
        snprintf(dst, sizeof(dst), "%s/%s", dir, s->name) >= (int)sizeof(dst) - 16) {
        pthread_mutex_unlock(&g_io);
        snprintf(err, err_size, "path is too long");
        return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s%s", dst, MC_TMP_SUFFIX);

    struct stat before;
    if (stat(s->path, &before) < 0 || !S_ISREG(before.st_mode)) {
        pthread_mutex_unlock(&g_io);
        snprintf(err, err_size, "the save is gone");
        return 1;
    }
    /* Without keep_previous the old copy is replaced, so its space comes back. */
    uint64_t reusable = 0;
    struct stat old;
    if (!cfg->keep_previous && stat(dst, &old) == 0) reusable = (uint64_t)old.st_size;
    if (have_space && free_b + reusable < (uint64_t)before.st_size + MC_FREE_MARGIN) {
        pthread_mutex_unlock(&g_io);
        snprintf(err, err_size, "not enough free space on the drive (%llu MB needed, %llu MB free)",
                 (unsigned long long)(before.st_size >> 20) + 1, (unsigned long long)(free_b >> 20));
        return -1;
    }

    mkdirs(dir);
    if (!exists(dir)) {
        pthread_mutex_unlock(&g_io);
        snprintf(err, err_size, "cannot create the folder on usb%d (is the drive writable?)", slot->usb);
        return -1;
    }

    pthread_mutex_lock(&g_st);
    memset(&g_job, 0, sizeof(g_job));
    g_job.active = 1;
    g_job.manual = manual;
    g_job.usb = slot->usb;
    snprintf(g_job.uid, sizeof(g_job.uid), "%s", s->uid);
    snprintf(g_job.title, sizeof(g_job.title), "%s", s->title);
    snprintf(g_job.name, sizeof(g_job.name), "%s", s->name);
    g_job.total = (uint64_t)before.st_size;
    pthread_mutex_unlock(&g_st);

    int rc = copy_stream(s->path, tmp, &e.hash, 1, err, err_size);

    char bin_src[MC_PATH + 200] = {0}, bin_dst[MC_PATH + 200] = {0}, bin_tmp[MC_PATH + 216] = {0};
    int have_bin = 0;
    if (rc == 0 && s->is_ps4) {
        char bin[160], src_dir[MC_PATH];
        ps4_bin_name(s->name, bin, sizeof(bin));
        snprintf(src_dir, sizeof(src_dir), "%s", s->path);
        char *slash = strrchr(src_dir, '/');
        if (slash) *slash = 0;
        snprintf(bin_src, sizeof(bin_src), "%s/%s", src_dir, bin);
        snprintf(bin_dst, sizeof(bin_dst), "%s/%s", dir, bin);
        snprintf(bin_tmp, sizeof(bin_tmp), "%s%s", bin_dst, MC_TMP_SUFFIX);
        if (exists(bin_src)) {
            rc = copy_stream(bin_src, bin_tmp, NULL, 0, err, err_size);
            have_bin = rc == 0;
        }
    }

    if (rc == 0) {
        struct stat after;
        if (stat(s->path, &after) < 0 || after.st_size != before.st_size ||
            after.st_mtime != before.st_mtime) {
            snprintf(err, err_size, "the save changed while it was being copied");
            rc = 1;
        }
    }

    if (rc == 0 && publish(tmp, dst, cfg->keep_previous) < 0) {
        snprintf(err, err_size, "cannot finalize the copy (errno %d)", errno);
        rc = -1;
    }
    if (rc == 0 && have_bin && publish(bin_tmp, bin_dst, cfg->keep_previous) < 0) {
        snprintf(err, err_size, "cannot finalize the PS4 key copy (errno %d)", errno);
        rc = -1;
    }
    if (rc != 0) {
        unlink(tmp);
        if (bin_tmp[0]) unlink(bin_tmp);
    }

    if (rc == 0) {
        e.size = (uint64_t)before.st_size;
        e.mtime = (int64_t)before.st_mtime;
        e.backed_at = now_wall();
        write_profile(card, s->uid, user);
        pthread_mutex_lock(&g_st);
        int saved = -1;
        mc_card_t *c = &g_cards[slot->usb];
        if (c->present && !strcmp(c->path, card)) {
            index_put(c, &e);
            saved = index_save(c);
        }
        pthread_mutex_unlock(&g_st);
        if (saved < 0) mc_log("[memcard] warning: could not write %s/%s\n", card, MC_INDEX_NAME);
    }

    pthread_mutex_lock(&g_st);
    g_job.active = 0;
    pthread_mutex_unlock(&g_st);
    pthread_mutex_unlock(&g_io);
    return rc;
}

/* ── Watcher pass ───────────────────────────────────────────────── */

enum { MC_SYNCED, MC_CARD_NEWER, MC_PENDING, MC_MISSING, MC_NO_CARD }; /* ordered: worst last */

static const char *state_name(int st) {
    switch (st) {
    case MC_SYNCED: return "synced";
    case MC_MISSING: return "missing";
    case MC_PENDING: return "pending";
    case MC_CARD_NEWER: return "card_newer";
    default: return "no_card";
    }
}

/* g_st held. card_file_exists is checked by the caller (it stats the drive). */
static int classify(const mc_save_t *s, const mc_index_t *ix, int card_file_exists) {
    if (!ix || !card_file_exists) return MC_MISSING;
    if (ix->size == s->size && ix->mtime == s->mtime) return MC_SYNCED;
    if (s->mtime < ix->mtime) return MC_CARD_NEWER;
    return MC_PENDING;
}

static int card_file_exists(const char *card, const mc_save_t *s) {
    char path[MC_PATH];
    if (snprintf(path, sizeof(path), "%s/%s/%s/%s", card, s->uid, s->title, s->name) >=
        (int)sizeof(path))
        return 0;
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void save_label(const mc_save_t *s, char *out, size_t size) {
    char name[96] = {0}, user[64] = {0};
    if (g_env.title_name) g_env.title_name(s->title, name, sizeof(name));
    if (g_env.user_name) g_env.user_name(s->uid, user, sizeof(user));
    snprintf(out, size, "%s: %s / %s", user[0] ? user : s->uid, name[0] ? name : s->title, s->name);
}

/* One console popup per pass, however many saves were copied in it. */
typedef struct {
    int count;
    const mc_save_t *last_save; /* a save copied to two cards still counts once */
    char last[320];
} mc_tally_t;

/* many: more than one card is in use, so the log says which one. */
static void run_backup(const mc_save_t *s, const mc_slot_t *slot, const mc_config_t *cfg, int manual,
                       const char *running, mc_tally_t *tally, int many) {
    char key[200], label[400], err[256] = {0};
    card_key(slot->usb, s, key, sizeof(key));
    save_label(s, label, sizeof(label) - 80);
    if (many) {
        char where[72];
        slot_name(slot, where, sizeof(where));
        snprintf(label + strlen(label), 80, " -> %s", where);
    }
    int rc = backup_one(s, slot, cfg, manual, err, sizeof(err));
    int64_t mono = now_mono();
    pthread_mutex_lock(&g_st);
    if (rc == 0) {
        pending_drop(key);
    } else {
        mc_pending_t *p = pending_get(key, 1);
        if (p) {
            /* A save that moved under us just needs to settle again. */
            p->size = s->size;
            p->mtime = s->mtime;
            p->since = mono;
            p->retry_at = rc < 0 ? mono + MC_RETRY_SECONDS : 0;
        }
    }
    pthread_mutex_unlock(&g_st);
    if (rc == 0) {
        event_add("backup", "%s%s (%.1f MB)", manual ? "manual: " : "", label,
                  (double)s->size / 1048576.0);
        /* No popups over a game that is being played; manual runs always report. */
        int in_game = running[0] && !strcmp(running, s->title);
        if ((manual || !in_game) && tally->last_save != s) {
            tally->count++;
            tally->last_save = s;
            save_label(s, tally->last, sizeof(tally->last));
        }
    } else if (rc < 0) {
        event_add("error", "%s: %s", label, err);
        if (manual && cfg->notify && g_env.notify) {
            char msg[256];
            snprintf(msg, sizeof(msg), "PS5 Memcard: backup failed, %.200s", err);
            g_env.notify(msg);
        }
    } else {
        event_add("info", "%s: %s, will retry", label, err);
    }
}

static int request_matches(const mc_request_t *r, const mc_save_t *s) {
    return (!r->uid[0] || !strcmp(r->uid, s->uid)) && (!r->title[0] || !strcmp(r->title, s->title)) &&
           (!r->name[0] || !strcmp(r->name, s->name));
}

void mc_tick(void) {
    pthread_mutex_lock(&g_st);
    mc_config_t cfg = g_cfg;
    pthread_mutex_unlock(&g_st);

    cards_refresh();
    mc_slot_t slots[MC_MAX_USB];
    int slot_n = cards_snapshot(slots);
    int many = slot_n > 1;

    /* Track the running game: when it closes, its saves get a content check. */
    char running[32] = {0};
    if (g_env.running_title && !g_env.running_title(running, sizeof(running))) running[0] = 0;
    int64_t mono = now_mono(), wall = now_wall();
    pthread_mutex_lock(&g_st);
    if (g_running[0] && strcmp(g_running, running) != 0) {
        int slot = -1;
        for (int i = 0; i < g_deep_n; i++)
            if (!strcmp(g_deep[i].title, g_running)) slot = i;
        if (slot < 0 && g_deep_n < (int)(sizeof(g_deep) / sizeof(g_deep[0]))) slot = g_deep_n++;
        if (slot >= 0) {
            snprintf(g_deep[slot].title, sizeof(g_deep[slot].title), "%s", g_running);
            g_deep[slot].due = mono + cfg.settle_seconds;
        }
    }
    snprintf(g_running, sizeof(g_running), "%s", running);
    mc_request_t queue[64];
    int queue_n = g_queue_n;
    memcpy(queue, g_queue, sizeof(mc_request_t) * (size_t)queue_n);
    g_queue_n = 0;
    pthread_mutex_unlock(&g_st);

    mc_save_list_t list = {0};
    scan_console(&list, cfg.include_system_backups);

    if (!slot_n) {
        pthread_mutex_lock(&g_st);
        g_console_n = list.n;
        g_synced_n = 0;
        g_waiting_n = 0;
        g_last_sweep = wall;
        pthread_mutex_unlock(&g_st);
        if (queue_n) event_add("error", "manual backup skipped: no USB drive");
        free(list.v);
        return;
    }

    mc_tally_t tally = {0};
    /* Manual requests first; they ignore settle time, throttling and "card is newer". */
    char *done = calloc((size_t)list.n * MC_MAX_USB + 1, 1); /* [save][usb] */
    for (int q = 0; q < queue_n && !stopping(); q++) {
        int matched = 0, current = 0, target = 0;
        for (int k = 0; k < slot_n; k++)
            if (queue[q].usb < 0 || queue[q].usb == slots[k].usb) target++;
        if (!target) {
            event_add("error", "manual backup: usb%d is not a memory card", queue[q].usb);
            continue;
        }
        for (int i = 0; i < list.n && !stopping(); i++) {
            mc_save_t *s = &list.v[i];
            if (!request_matches(&queue[q], s)) continue;
            matched = 1;
            for (int k = 0; k < slot_n && !stopping(); k++) {
                mc_slot_t *slot = &slots[k];
                if (queue[q].usb >= 0 && queue[q].usb != slot->usb) continue;
                char *mark = done ? &done[i * MC_MAX_USB + slot->usb] : NULL;
                if (mark && *mark) continue;
                if (mark) *mark = 1;
                /* Copying an identical save again would only push the real
                 * previous generation out of its .prev slot. */
                int on_card = card_file_exists(slot->path, s);
                pthread_mutex_lock(&g_st);
                mc_index_t *ix = index_find(&g_cards[slot->usb], s->uid, s->title, s->name);
                int same = classify(s, ix, on_card) == MC_SYNCED;
                uint64_t want = ix ? ix->hash : 0, have = 0;
                pthread_mutex_unlock(&g_st);
                if (same && file_hash(s->path, &have) == 0 && have == want) {
                    current++;
                    continue;
                }
                run_backup(s, slot, &cfg, 1, running, &tally, many);
            }
        }
        if (current)
            event_add("info", "manual backup: %d cop%s already up to date", current,
                      current == 1 ? "y" : "ies");
        if (!matched)
            event_add("error", "manual backup: no console save matches %s/%s/%s",
                      queue[q].uid[0] ? queue[q].uid : "*", queue[q].title[0] ? queue[q].title : "*",
                      queue[q].name[0] ? queue[q].name : "*");
    }

    /* Which closed titles are due for their content check? */
    char deep_titles[16][32];
    int deep_n = 0;
    mono = now_mono();
    pthread_mutex_lock(&g_st);
    for (int i = 0; i < g_deep_n;) {
        if (g_deep[i].due <= mono && strcmp(g_deep[i].title, running) != 0) {
            snprintf(deep_titles[deep_n++], 32, "%s", g_deep[i].title);
            g_deep[i] = g_deep[--g_deep_n];
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&g_st);

    int card_synced[MC_MAX_USB] = {0}, card_waiting[MC_MAX_USB] = {0};
    int synced = 0, waiting = 0;
    for (int i = 0; i < list.n && !stopping(); i++) {
        mc_save_t *s = &list.v[i];
        int everywhere = 1, behind = 0;
        int deep = 0;
        for (int d = 0; d < deep_n; d++)
            if (!strcmp(deep_titles[d], s->title)) deep = 1;
        uint64_t source_hash = 0;
        int have_hash = 0;

        for (int k = 0; k < slot_n && !stopping(); k++) {
            mc_slot_t *slot = &slots[k];
            mc_card_t *c = &g_cards[slot->usb];
            int handled = done && done[i * MC_MAX_USB + slot->usb]; /* a manual run dealt with it */
            char key[200];
            card_key(slot->usb, s, key, sizeof(key));
            int on_card = card_file_exists(slot->path, s);
            mono = now_mono();
            wall = now_wall();

            pthread_mutex_lock(&g_st);
            mc_index_t *ix = index_find(c, s->uid, s->title, s->name);
            int st = classify(s, ix, on_card);
            uint64_t ix_hash = ix ? ix->hash : 0;
            int64_t ix_backed = ix ? ix->backed_at : 0;
            int go = 0;
            if (st == MC_SYNCED) {
                pending_drop(key);
            } else if (st != MC_CARD_NEWER && cfg.enabled && !handled) {
                mc_pending_t *p = pending_get(key, 1);
                if (p) {
                    if (p->size != s->size || p->mtime != s->mtime) {
                        p->size = s->size;
                        p->mtime = s->mtime;
                        p->since = mono;
                        p->retry_at = 0;
                    }
                    /* Stable when untouched for the settle time, by file time or by what we watched. */
                    int stable = (wall >= s->mtime && wall - s->mtime >= cfg.settle_seconds) ||
                                 mono - p->since >= cfg.settle_seconds;
                    int in_game = running[0] && !strcmp(running, s->title);
                    int throttled = in_game && ix_backed && wall >= ix_backed &&
                                    wall - ix_backed < cfg.min_interval_seconds;
                    go = stable && !throttled && (!p->retry_at || mono >= p->retry_at);
                }
            }
            pthread_mutex_unlock(&g_st);

            if (st == MC_SYNCED) {
                /* Same size and time as the copy: after its game closed, compare the bytes too. */
                if (deep && cfg.enabled && !handled) {
                    if (!have_hash) have_hash = file_hash(s->path, &source_hash) == 0 ? 1 : -1;
                    if (have_hash == 1 && source_hash != ix_hash) {
                        mc_log("[memcard] %s changed without a new timestamp\n", key);
                        run_backup(s, slot, &cfg, 0, running, &tally, many);
                    }
                }
                card_synced[slot->usb]++;
                continue;
            }
            int ok = 0;
            if (go) {
                run_backup(s, slot, &cfg, 0, running, &tally, many);
                pthread_mutex_lock(&g_st);
                mc_index_t *now_ix = index_find(c, s->uid, s->title, s->name);
                ok = now_ix && now_ix->size == s->size && now_ix->mtime == s->mtime;
                pthread_mutex_unlock(&g_st);
            }
            if (ok) {
                card_synced[slot->usb]++;
            } else {
                everywhere = 0;
                if (st != MC_CARD_NEWER) {
                    card_waiting[slot->usb]++;
                    behind = 1;
                }
            }
        }
        if (everywhere) synced++;
        else if (behind) waiting++;
    }
    free(done);
    if (tally.count && cfg.notify && g_env.notify) {
        char msg[400];
        if (tally.count == 1) snprintf(msg, sizeof(msg), "PS5 Memcard: backed up\n%.300s", tally.last);
        else snprintf(msg, sizeof(msg), "PS5 Memcard: %d saves backed up to USB", tally.count);
        g_env.notify(msg);
    }

    pthread_mutex_lock(&g_st);
    g_console_n = list.n;
    g_synced_n = synced;
    g_waiting_n = waiting;
    for (int k = 0; k < slot_n; k++) {
        g_cards[slots[k].usb].synced = card_synced[slots[k].usb];
        g_cards[slots[k].usb].waiting = card_waiting[slots[k].usb];
    }
    g_last_sweep = now_wall();
    pthread_mutex_unlock(&g_st);
    free(list.v);
}

int mc_wait(void) {
    pthread_mutex_lock(&g_st);
    int seconds = g_cfg.poll_seconds;
    pthread_mutex_unlock(&g_st);
    for (int slice = 0; slice < seconds * 5; slice++) {
        if (stopping()) return -1;
        pthread_mutex_lock(&g_st);
        if (g_wake_pending) {
            g_wake_pending = 0;
            pthread_mutex_unlock(&g_st);
            return 0;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&g_wake, &g_st, &ts);
        pthread_mutex_unlock(&g_st);
    }
    pthread_mutex_lock(&g_st);
    g_wake_pending = 0;
    pthread_mutex_unlock(&g_st);
    return stopping() ? -1 : 0;
}

void mc_wake(void) {
    pthread_mutex_lock(&g_st);
    g_wake_pending = 1;
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_st);
}

void mc_note_resume(void) {
    /* The drives may have been re-enumerated while the console slept. */
    pthread_mutex_lock(&g_st);
    g_pending_n = 0;
    pthread_mutex_unlock(&g_st);
    event_add("info", "console resumed, rescanning");
    mc_wake();
}

int mc_detect_cards(void) {
    return cards_refresh();
}

void mc_init(const mc_env_t *env) {
    g_env = *env;
    config_load();
    if (g_env.stage_dir) remove_tree(g_env.stage_dir, 0);
    event_add("info", "ps5-memcard %s started, auto backup %s", g_env.version ? g_env.version : "",
              g_cfg.enabled ? "on" : "off");
}

/* ── HTTP: status ───────────────────────────────────────────────── */

static void users_json(sb_t *sb) {
    sb_put(sb, "[");
    DIR *home = opendir(g_env.home_root);
    int first = 1;
    if (home) {
        struct dirent *ue;
        while ((ue = readdir(home))) {
            if (!id_ok(ue->d_name, 16)) continue;
            char path[MC_PATH];
            snprintf(path, sizeof(path), "%s/%.15s", g_env.home_root, ue->d_name);
            struct stat st;
            if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode)) continue;
            char name[64] = {0};
            if (g_env.user_name) g_env.user_name(ue->d_name, name, sizeof(name));
            if (!first) sb_put(sb, ",");
            first = 0;
            sb_put(sb, "{\"id\":");
            sb_str(sb, ue->d_name);
            sb_put(sb, ",\"name\":");
            sb_str(sb, name[0] ? name : ue->d_name);
            sb_put(sb, "}");
        }
        closedir(home);
    }
    sb_put(sb, "]");
}

static char *http_status(void) {
    sb_t sb = {0};
    char running_name[96] = {0}, running[32];
    pthread_mutex_lock(&g_st);
    snprintf(running, sizeof(running), "%s", g_running);
    pthread_mutex_unlock(&g_st);
    if (running[0] && g_env.title_name) g_env.title_name(running, running_name, sizeof(running_name));

    sb_put(&sb, "{\"app\":\"ps5-memcard\",\"version\":");
    sb_str(&sb, g_env.version ? g_env.version : "");
    sb_put(&sb, ",\"users\":");
    users_json(&sb);

    pthread_mutex_lock(&g_st);
    sb_printf(&sb, ",\"now\":%lld", (long long)now_wall());
    /* Every connected drive; "card" says whether it is used as a memory card. */
    sb_put(&sb, ",\"drives\":[");
    int first = 1, cards = 0, card_saves = 0;
    for (int n = 0; n < MC_MAX_USB; n++) {
        mc_card_t *c = &g_cards[n];
        if (!g_drive[n] && !c->present) continue;
        sb_printf(&sb, "%s{\"usb\":%d,\"card\":%s,\"label\":", first ? "" : ",", n,
                  c->present ? "true" : "false");
        first = 0;
        sb_str(&sb, c->present ? c->label : "");
        sb_put(&sb, ",\"path\":");
        sb_str(&sb, c->present ? c->path : "");
        sb_printf(&sb, ",\"free\":%llu,\"total\":%llu,\"saves\":%d,\"synced\":%d,\"waiting\":%d}",
                  (unsigned long long)g_drive_free[n], (unsigned long long)g_drive_total[n],
                  c->present ? c->index_n : 0, c->present ? c->synced : 0,
                  c->present ? c->waiting : 0);
        if (c->present) {
            cards++;
            card_saves += c->index_n;
        }
    }
    sb_printf(&sb, "],\"card\":{\"present\":%s,\"count\":%d,\"saves\":%d}", cards ? "true" : "false",
              cards, card_saves);
    sb_printf(&sb, ",\"watcher\":{\"enabled\":%s,\"last_sweep\":%lld,\"running_title\":",
              g_cfg.enabled ? "true" : "false", (long long)g_last_sweep);
    sb_str(&sb, running);
    sb_put(&sb, ",\"running_name\":");
    sb_str(&sb, running_name);
    sb_printf(&sb, ",\"console_saves\":%d,\"synced\":%d,\"waiting\":%d,\"queued\":%d}", g_console_n,
              g_synced_n, g_waiting_n, g_queue_n);
    sb_printf(&sb, ",\"job\":{\"active\":%s,\"manual\":%s,\"usb\":%d,\"uid\":",
              g_job.active ? "true" : "false", g_job.manual ? "true" : "false", g_job.usb);
    sb_str(&sb, g_job.uid);
    sb_put(&sb, ",\"title\":");
    sb_str(&sb, g_job.title);
    sb_put(&sb, ",\"save\":");
    sb_str(&sb, g_job.name);
    sb_printf(&sb, ",\"done\":%llu,\"total\":%llu}", (unsigned long long)g_job.done,
              (unsigned long long)g_job.total);
    sb_put(&sb, ",\"settings\":");
    config_json(&sb, &g_cfg);
    sb_put(&sb, ",\"activity\":[");
    for (int i = 0; i < g_event_n; i++) {
        /* newest first */
        int idx = (g_event_head - 1 - i + MC_ACTIVITY_MAX * 2) % MC_ACTIVITY_MAX;
        sb_printf(&sb, "%s{\"at\":%lld,\"kind\":", i ? "," : "", (long long)g_events[idx].at);
        sb_str(&sb, g_events[idx].kind);
        sb_put(&sb, ",\"text\":");
        sb_str(&sb, g_events[idx].text);
        sb_put(&sb, "}");
    }
    sb_put(&sb, "]}");
    pthread_mutex_unlock(&g_st);
    return sb_done(&sb);
}

/* ── HTTP: what is on the cards ─────────────────────────────────── */

typedef struct {
    int usb;
    char uid[16], title[32], name[128];
    int folder, is_ps4, has_prev;
    uint64_t size;
    int64_t mtime;
    char title_name[96];
    char profile[64];
} mc_card_item_t;

typedef struct {
    mc_card_item_t *v;
    int n, cap;
} mc_card_items_t;

static void scan_card(const mc_slot_t *slot, mc_card_items_t *out) {
    const char *card = slot->path;
    DIR *cd = opendir(card);
    struct dirent *ue;
    while (cd && (ue = readdir(cd))) {
        if (!id_ok(ue->d_name, 16)) continue;
        char udir[MC_PATH];
        if (snprintf(udir, sizeof(udir), "%s/%.15s", card, ue->d_name) >= (int)sizeof(udir)) continue;
        struct stat ust;
        if (stat(udir, &ust) < 0 || !S_ISDIR(ust.st_mode)) continue;
        char profile[64];
        read_profile(card, ue->d_name, profile, sizeof(profile));
        DIR *ud = opendir(udir);
        struct dirent *te;
        while (ud && (te = readdir(ud))) {
            if (!id_ok(te->d_name, 32)) continue;
            char tdir[MC_PATH];
            if (snprintf(tdir, sizeof(tdir), "%s/%.31s", udir, te->d_name) >= (int)sizeof(tdir)) continue;
            DIR *td = opendir(tdir);
            if (!td) continue;
            struct dirent *se;
            while ((se = readdir(td))) {
                if (!component_ok(se->d_name, 128) || is_card_private(se->d_name)) continue;
                char path[MC_PATH], extra[MC_PATH + 16];
                if (snprintf(path, sizeof(path), "%s/%.127s", tdir, se->d_name) >= (int)sizeof(path) - 16)
                    continue;
                struct stat st;
                if (stat(path, &st) < 0) continue;
                int folder = S_ISDIR(st.st_mode);
                if (folder) {
                    /* A decrypted save folder somebody dropped on the card. */
                    snprintf(extra, sizeof(extra), "%s/sce_sys", path);
                    if (!exists(extra)) continue;
                } else if (!S_ISREG(st.st_mode)) {
                    continue;
                }
                if (out->n >= out->cap) {
                    int cap = out->cap ? out->cap * 2 : 64;
                    mc_card_item_t *p = realloc(out->v, (size_t)cap * sizeof(*p));
                    if (!p) break;
                    out->v = p;
                    out->cap = cap;
                }
                mc_card_item_t *it = &out->v[out->n++];
                memset(it, 0, sizeof(*it));
                it->usb = slot->usb;
                snprintf(it->uid, sizeof(it->uid), "%.15s", ue->d_name);
                snprintf(it->title, sizeof(it->title), "%.31s", te->d_name);
                snprintf(it->name, sizeof(it->name), "%.127s", se->d_name);
                snprintf(it->profile, sizeof(it->profile), "%s", profile);
                it->folder = folder;
                it->size = folder ? tree_size(path, 0) : (uint64_t)st.st_size;
                it->mtime = (int64_t)st.st_mtime;
                it->is_ps4 = folder ? !strncmp(te->d_name, "CUSA", 4) : first_byte(path) == 0x01;
                snprintf(extra, sizeof(extra), "%s%s", path, MC_PREV_SUFFIX);
                it->has_prev = !folder && exists(extra);
                if (g_env.title_name) g_env.title_name(it->title, it->title_name, sizeof(it->title_name));
            }
            closedir(td);
        }
        if (ud) closedir(ud);
    }
    if (cd) closedir(cd);
}

/* usb=N limits the list to one card. */
static char *http_card(const char *url) {
    char arg[8];
    int only = mc_url_param(url, "usb", arg, sizeof(arg)) && arg[0] ? atoi(arg) : -1;
    mc_slot_t slots[MC_MAX_USB];
    int slot_n = cards_snapshot(slots);
    sb_t sb = {0};

    mc_card_items_t items = {0};
    for (int k = 0; k < slot_n; k++)
        if (only < 0 || only == slots[k].usb) scan_card(&slots[k], &items);

    sb_printf(&sb, "{\"present\":%s,\"cards\":[", slot_n ? "true" : "false");
    for (int k = 0; k < slot_n; k++) {
        sb_printf(&sb, "%s{\"usb\":%d,\"label\":", k ? "," : "", slots[k].usb);
        sb_str(&sb, slots[k].label);
        sb_put(&sb, ",\"path\":");
        sb_str(&sb, slots[k].path);
        sb_put(&sb, "}");
    }
    sb_put(&sb, "],\"saves\":[");
    pthread_mutex_lock(&g_st);
    for (int i = 0; i < items.n; i++) {
        mc_card_item_t *it = &items.v[i];
        mc_card_t *c = &g_cards[it->usb];
        mc_index_t *ix = c->present ? index_find(c, it->uid, it->title, it->name) : NULL;
        char id[192];
        snprintf(id, sizeof(id), "%s/%s/%s", it->uid, it->title, it->name);
        sb_printf(&sb, "%s{\"usb\":%d,\"id\":", i ? "," : "", it->usb);
        sb_str(&sb, id);
        sb_put(&sb, ",\"uid\":");
        sb_str(&sb, it->uid);
        sb_put(&sb, ",\"user_name\":");
        sb_str(&sb, it->profile);
        sb_put(&sb, ",\"title_id\":");
        sb_str(&sb, it->title);
        sb_put(&sb, ",\"title_name\":");
        sb_str(&sb, it->title_name[0] ? it->title_name : ix ? ix->title_name : "");
        sb_put(&sb, ",\"save\":");
        sb_str(&sb, it->name);
        sb_put(&sb, ",\"main_title\":");
        sb_str(&sb, ix ? ix->main_title : "");
        sb_put(&sb, ",\"sub_title\":");
        sb_str(&sb, ix ? ix->sub_title : "");
        sb_printf(&sb,
                  ",\"type\":\"%s\",\"format\":\"%s\",\"size\":%llu,\"mtime\":%lld,"
                  "\"backed_at\":%lld,\"has_previous\":%s}",
                  it->is_ps4 ? "ps4" : "ps5", it->folder ? "folder" : "image",
                  (unsigned long long)it->size, (long long)(ix ? ix->mtime : it->mtime),
                  (long long)(ix ? ix->backed_at : it->mtime), it->has_prev ? "true" : "false");
    }
    pthread_mutex_unlock(&g_st);
    sb_put(&sb, "]}");
    free(items.v);
    return sb_done(&sb);
}

/* ── HTTP: what is on the console ───────────────────────────────── */

static char *http_console(void) {
    pthread_mutex_lock(&g_st);
    mc_config_t cfg = g_cfg;
    pthread_mutex_unlock(&g_st);
    mc_slot_t slots[MC_MAX_USB];
    int slot_n = cards_snapshot(slots);

    mc_save_list_t list = {0};
    scan_console(&list, cfg.include_system_backups);

    /* Callbacks and drive stats first, state under the lock afterwards. */
    char (*names)[96] = calloc((size_t)list.n + 1, 96);
    char *on_card = calloc((size_t)list.n * MC_MAX_USB + 1, 1); /* [save][usb] */
    char users[32][2][64];
    int user_n = 0;
    for (int i = 0; i < list.n; i++) {
        mc_save_t *s = &list.v[i];
        if (names && g_env.title_name) g_env.title_name(s->title, names[i], 96);
        for (int k = 0; on_card && k < slot_n; k++)
            on_card[i * MC_MAX_USB + slots[k].usb] = (char)card_file_exists(slots[k].path, s);
        int known = 0;
        for (int u = 0; u < user_n; u++)
            if (!strcmp(users[u][0], s->uid)) known = 1;
        if (!known && user_n < 32) {
            snprintf(users[user_n][0], 64, "%s", s->uid);
            users[user_n][1][0] = 0;
            if (g_env.user_name) g_env.user_name(s->uid, users[user_n][1], 64);
            user_n++;
        }
    }

    sb_t sb = {0};
    sb_printf(&sb, "{\"card\":%s,\"saves\":[", slot_n ? "true" : "false");
    pthread_mutex_lock(&g_st);
    for (int i = 0; i < list.n; i++) {
        mc_save_t *s = &list.v[i];
        /* The overall state is that of the card that is furthest behind. */
        int worst = slot_n ? MC_SYNCED : MC_NO_CARD;
        int64_t backed_at = 0;
        mc_index_t *any_ix = NULL;
        sb_t per = {0};
        for (int k = 0; k < slot_n; k++) {
            mc_card_t *c = &g_cards[slots[k].usb];
            mc_index_t *ix = c->present ? index_find(c, s->uid, s->title, s->name) : NULL;
            int st = classify(s, ix, on_card ? on_card[i * MC_MAX_USB + slots[k].usb] : 0);
            if (st > worst) worst = st;
            if (ix && !any_ix) any_ix = ix;
            if (ix && (!backed_at || ix->backed_at < backed_at)) backed_at = ix->backed_at;
            sb_printf(&per, "%s{\"usb\":%d,\"state\":\"%s\",\"backed_at\":%lld}", k ? "," : "",
                      slots[k].usb, state_name(st), (long long)(ix ? ix->backed_at : 0));
        }
        const char *user = s->uid;
        for (int u = 0; u < user_n; u++)
            if (!strcmp(users[u][0], s->uid) && users[u][1][0]) user = users[u][1];
        sb_put(&sb, i ? ",{\"uid\":" : "{\"uid\":");
        sb_str(&sb, s->uid);
        sb_put(&sb, ",\"user_name\":");
        sb_str(&sb, user);
        sb_put(&sb, ",\"title_id\":");
        sb_str(&sb, s->title);
        sb_put(&sb, ",\"title_name\":");
        sb_str(&sb, names && names[i][0] ? names[i] : any_ix ? any_ix->title_name : "");
        sb_put(&sb, ",\"save\":");
        sb_str(&sb, s->name);
        sb_put(&sb, ",\"main_title\":");
        sb_str(&sb, any_ix ? any_ix->main_title : "");
        sb_put(&sb, ",\"sub_title\":");
        sb_str(&sb, any_ix ? any_ix->sub_title : "");
        sb_printf(&sb,
                  ",\"type\":\"%s\",\"size\":%llu,\"mtime\":%lld,\"state\":\"%s\",\"backed_at\":%lld,"
                  "\"cards\":[",
                  s->is_ps4 ? "ps4" : "ps5", (unsigned long long)s->size, (long long)s->mtime,
                  state_name(worst), (long long)backed_at);
        if (per.p) sb_put(&sb, per.p);
        free(per.p);
        sb_put(&sb, "]}");
    }
    pthread_mutex_unlock(&g_st);
    sb_put(&sb, "]}");
    free(names);
    free(on_card);
    free(list.v);
    return sb_done(&sb);
}

/* ── HTTP: actions ──────────────────────────────────────────────── */

/* usb=N from the query, -1 when absent, -2 when it is not a port number. */
static int usb_param(const char *url) {
    char arg[8];
    if (!mc_url_param(url, "usb", arg, sizeof(arg)) || !arg[0]) return -1;
    if (arg[1] || arg[0] < '0' || arg[0] >= '0' + MC_MAX_USB) return -2;
    return arg[0] - '0';
}

static char *http_backup(const char *url) {
    mc_request_t r;
    memset(&r, 0, sizeof(r));
    char all[8];
    mc_url_param(url, "uid", r.uid, sizeof(r.uid));
    mc_url_param(url, "title", r.title, sizeof(r.title));
    mc_url_param(url, "save", r.name, sizeof(r.name));
    mc_url_param(url, "all", all, sizeof(all));
    r.usb = usb_param(url);
    if ((r.uid[0] && !id_ok(r.uid, sizeof(r.uid))) || (r.title[0] && !id_ok(r.title, sizeof(r.title))) ||
        (r.name[0] && !component_ok(r.name, sizeof(r.name))) || r.usb == -2)
        return json_error("Invalid save reference");
    if (!r.uid[0] && !r.title[0] && !r.name[0] && strcmp(all, "1") != 0)
        return json_error("Say which save to back up, or pass all=1");
    /* A drive may have been plugged in since the last pass. */
    if (!cards_refresh()) return json_error("No USB drive is connected");
    pthread_mutex_lock(&g_st);
    int known = r.usb < 0 || g_cards[r.usb].present;
    int full = g_queue_n >= (int)(sizeof(g_queue) / sizeof(g_queue[0]));
    if (known && !full) g_queue[g_queue_n++] = r;
    pthread_mutex_unlock(&g_st);
    if (!known) return json_error("usb%d is not a memory card", r.usb);
    if (full) return json_error("Too many backups queued, try again in a moment");
    mc_wake();
    return strdup("{\"ok\":true,\"queued\":true}");
}

static int split_id(const char *id, char *uid, char *title, char *name) {
    const char *a = strchr(id, '/');
    if (!a) return -1;
    const char *b = strchr(a + 1, '/');
    if (!b) return -1;
    if ((size_t)(a - id) >= 16 || (size_t)(b - a - 1) >= 32 || strlen(b + 1) >= 128) return -1;
    memcpy(uid, id, (size_t)(a - id));
    uid[a - id] = 0;
    memcpy(title, a + 1, (size_t)(b - a - 1));
    title[b - a - 1] = 0;
    strcpy(name, b + 1);
    if (!id_ok(uid, 16) || !id_ok(title, 32) || !component_ok(name, 128) || is_card_private(name))
        return -1;
    return 0;
}

/* The card a save reference points at: the one asked for, or the first card
 * that has it. suffix is appended to the save name when looking (".prev"). */
static int pick_card(int usb, const char *uid, const char *title, const char *name,
                     const char *suffix, mc_slot_t *out) {
    mc_slot_t slots[MC_MAX_USB];
    int slot_n = cards_snapshot(slots);
    for (int k = 0; k < slot_n; k++) {
        if (usb >= 0 && slots[k].usb != usb) continue;
        char path[MC_PATH + 256];
        snprintf(path, sizeof(path), "%s/%s/%s/%s%s", slots[k].path, uid, title, name, suffix);
        if (usb >= 0 || exists(path)) {
            *out = slots[k];
            return 0;
        }
    }
    return -1;
}

static char *http_delete(const char *url) {
    char id[256], uid[16], title[32], name[128];
    int usb = usb_param(url);
    if (!mc_url_param(url, "id", id, sizeof(id)) || split_id(id, uid, title, name) < 0 || usb == -2)
        return json_error("Invalid save reference");
    mc_slot_t slot;
    if (pick_card(usb, uid, title, name, "", &slot) < 0) {
        if (usb >= 0) return json_error("usb%d is not a memory card", usb);
        return json_error("That save is not on a card");
    }
    char dir[MC_PATH], path[MC_PATH], extra[MC_PATH + 200], bin[160];
    if (snprintf(dir, sizeof(dir), "%s/%s/%s", slot.path, uid, title) >= (int)sizeof(dir) ||
        snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path) - 16)
        return json_error("Path is too long");
    if (!exists(path)) return json_error("That save is not on the card");

    pthread_mutex_lock(&g_io);
    int was_ps4 = first_byte(path) == 0x01;
    remove_tree(path, 0);
    snprintf(extra, sizeof(extra), "%s%s", path, MC_PREV_SUFFIX);
    unlink(extra);
    snprintf(extra, sizeof(extra), "%s%s", path, MC_TMP_SUFFIX);
    unlink(extra);
    if (was_ps4) {
        ps4_bin_name(name, bin, sizeof(bin));
        snprintf(extra, sizeof(extra), "%s/%s", dir, bin);
        unlink(extra);
        snprintf(extra, sizeof(extra), "%s/%s%s", dir, bin, MC_PREV_SUFFIX);
        unlink(extra);
    }
    int gone = !exists(path);
    rmdir(dir); /* fails harmlessly when something is still inside */
    pthread_mutex_lock(&g_st);
    mc_card_t *c = &g_cards[slot.usb];
    if (gone && c->present && !strcmp(c->path, slot.path)) {
        index_remove(c, uid, title, name);
        index_save(c);
    }
    pthread_mutex_unlock(&g_st);
    pthread_mutex_unlock(&g_io);
    if (!gone) return json_error("Could not delete the save from the drive");
    event_add("delete", "removed %s from usb%d", id, slot.usb);
    return strdup("{\"ok\":true}");
}

/* POST /api/mc/card_init?usb=N: start using a connected drive as a memory card. */
static char *http_card_init(const char *url) {
    int usb = usb_param(url);
    if (usb < 0) return json_error("Say which drive: usb=0..%d", MC_MAX_USB - 1);
    char root[MC_PATH - 16], card[MC_PATH];
    if (!usb_root(usb, root, sizeof(root))) return json_error("No drive is connected on usb%d", usb);
    pthread_mutex_lock(&g_st);
    int pin = g_cfg.usb;
    pthread_mutex_unlock(&g_st);
    if (pin >= 0 && pin != usb)
        return json_error("The settings limit memory cards to usb%d. Change that first.", pin);
    snprintf(card, sizeof(card), "%s/%s", root, MC_CARD_DIR);
    pthread_mutex_lock(&g_io);
    mkdir(card, 0777);
    struct stat st;
    int ok = stat(card, &st) == 0 && S_ISDIR(st.st_mode);
    pthread_mutex_unlock(&g_io);
    if (!ok) return json_error("Could not create /%s on usb%d (is the drive writable?)", MC_CARD_DIR, usb);
    cards_refresh();
    mc_wake();
    return strdup("{\"ok\":true}");
}

/* POST /api/mc/label?usb=N&name=...: a name for the card, kept on the card itself. */
static char *http_label(const char *url) {
    int usb = usb_param(url);
    char name[96], path[MC_PATH + 32];
    mc_url_param(url, "name", name, sizeof(name));
    label_clean(name);
    if (strlen(name) > 40) {
        /* Cut at a character boundary, not in the middle of a UTF-8 sequence. */
        size_t len = 40;
        while (len && ((unsigned char)name[len] & 0xC0) == 0x80) len--;
        name[len] = 0;
    }
    mc_slot_t slot;
    if (usb < 0 || pick_card(usb, "", "", "", "", &slot) < 0)
        return json_error("That drive is not a memory card");
    snprintf(path, sizeof(path), "%s/%s", slot.path, MC_LABEL_NAME);
    pthread_mutex_lock(&g_io);
    int ok = 1;
    mkdirs(slot.path);
    if (name[0]) {
        FILE *f = fopen(path, "wb");
        ok = f != NULL;
        if (f) {
            fputs(name, f);
            ok = fclose(f) == 0;
        }
    } else {
        unlink(path);
    }
    pthread_mutex_lock(&g_st);
    mc_card_t *c = &g_cards[usb];
    if (ok && c->present) snprintf(c->label, sizeof(c->label), "%s", name);
    pthread_mutex_unlock(&g_st);
    pthread_mutex_unlock(&g_io);
    if (!ok) return json_error("Could not write the name to the drive");
    return strdup("{\"ok\":true}");
}

static char *http_settings(const char *method, const char *body) {
    if (!strcmp(method, "POST")) {
        if (!body || !body[0]) return json_error("Missing settings body");
        pthread_mutex_lock(&g_st);
        mc_config_t c = g_cfg;
        pthread_mutex_unlock(&g_st);
        config_parse(body, &c);
        int saved = config_save(&c);
        pthread_mutex_lock(&g_st);
        g_cfg = c;
        pthread_mutex_unlock(&g_st);
        event_add("info", "settings saved, auto backup %s", c.enabled ? "on" : "off");
        cards_refresh();
        mc_wake();
        if (saved < 0) return json_error("Settings applied but could not be saved to disk");
    }
    sb_t sb = {0};
    pthread_mutex_lock(&g_st);
    sb_put(&sb, "{\"ok\":true,\"settings\":");
    config_json(&sb, &g_cfg);
    sb_put(&sb, "}");
    pthread_mutex_unlock(&g_st);
    return sb_done(&sb);
}

static int route_is(const char *url, const char *route) {
    size_t n = strlen(route);
    return !strncmp(url, route, n) && (url[n] == 0 || url[n] == '?');
}

char *mc_http(const char *method, const char *url, const char *body) {
    int get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
    if (route_is(url, "/api/mc/status") && get) return http_status();
    if (route_is(url, "/api/mc/card") && get) return http_card(url);
    if (route_is(url, "/api/mc/console") && get) return http_console();
    if (route_is(url, "/api/mc/settings") && (get || post)) return http_settings(method, body);
    if (route_is(url, "/api/mc/backup") && post) return http_backup(url);
    if (route_is(url, "/api/mc/delete") && post) return http_delete(url);
    if (route_is(url, "/api/mc/card_init") && post) return http_card_init(url);
    if (route_is(url, "/api/mc/label") && post) return http_label(url);
    if (route_is(url, "/api/mc/rescan") && post) {
        mc_wake();
        return strdup("{\"ok\":true}");
    }
    return NULL;
}

/* ── Import staging ─────────────────────────────────────────────── */

void mc_import_cleanup(void) {
    if (!g_env.stage_dir) return;
    pthread_mutex_lock(&g_io);
    remove_tree(g_env.stage_dir, 0);
    pthread_mutex_unlock(&g_io);
}

int mc_import_prepare(const char *id, int usb, int previous, char *staged, size_t staged_size,
                      char *title_id, size_t title_size, char *error, size_t error_size) {
    char uid[16], title[32], name[128];
    if (split_id(id, uid, title, name) < 0 || usb < -1 || usb >= MC_MAX_USB) {
        snprintf(error, error_size, "Invalid save reference");
        return -1;
    }
    mc_slot_t probe[MC_MAX_USB], slot;
    if (!cards_snapshot(probe)) {
        snprintf(error, error_size, "No USB drive is connected");
        return -1;
    }
    const char *suffix = previous ? MC_PREV_SUFFIX : "";
    if (pick_card(usb, uid, title, name, suffix, &slot) < 0) {
        if (usb >= 0) snprintf(error, error_size, "usb%d is not a memory card", usb);
        else snprintf(error, error_size, previous ? "There is no previous copy of that save"
                                                  : "That save is not on a card");
        return -1;
    }
    char dir[MC_PATH], src[MC_PATH], out_dir[MC_PATH];
    if (snprintf(dir, sizeof(dir), "%s/%s/%s", slot.path, uid, title) >= (int)sizeof(dir) ||
        snprintf(src, sizeof(src), "%s/%s%s", dir, name, suffix) >= (int)sizeof(src)) {
        snprintf(error, error_size, "Path is too long");
        return -1;
    }
    struct stat st;
    if (stat(src, &st) < 0) {
        snprintf(error, error_size, previous ? "There is no previous copy of that save"
                                             : "That save is not on the card");
        return -1;
    }
    if (previous && !S_ISREG(st.st_mode)) {
        snprintf(error, error_size, "Only save images keep a previous copy");
        return -1;
    }

    pthread_mutex_lock(&g_io);
    remove_tree(g_env.stage_dir, 0);
    snprintf(out_dir, sizeof(out_dir), "%s/%s", g_env.stage_dir, title);
    mkdirs(out_dir);
    snprintf(staged, staged_size, "%s/%s", out_dir, name);
    char err[256] = {0};
    int rc = copy_tree(src, staged, 0, err, sizeof(err));
    if (rc == 0 && S_ISREG(st.st_mode) && first_byte(src) == 0x01) {
        /* PS4 image: the importer needs the sealed key next to it. */
        char bin[160], bin_src[MC_PATH + 200], bin_dst[MC_PATH + 200];
        ps4_bin_name(name, bin, sizeof(bin));
        snprintf(bin_src, sizeof(bin_src), "%s/%s%s", dir, bin, suffix);
        if (previous && !exists(bin_src)) snprintf(bin_src, sizeof(bin_src), "%s/%s", dir, bin);
        snprintf(bin_dst, sizeof(bin_dst), "%s/%s", out_dir, bin);
        if (!exists(bin_src)) {
            snprintf(err, sizeof(err), "the PS4 key file %s is missing from the card", bin);
            rc = -1;
        } else {
            rc = copy_stream(bin_src, bin_dst, NULL, 0, err, sizeof(err));
        }
    }
    if (rc != 0) remove_tree(g_env.stage_dir, 0);
    pthread_mutex_unlock(&g_io);
    if (rc != 0) {
        snprintf(error, error_size, "Could not read the save from the card: %s", err);
        return -1;
    }
    snprintf(title_id, title_size, "%s", title);
    return 0;
}

void mc_import_result(const char *id, const char *uid, int ok, const char *detail) {
    char user[64] = {0};
    if (g_env.user_name) g_env.user_name(uid, user, sizeof(user));
    if (ok) event_add("import", "%s imported to %s", id, user[0] ? user : uid);
    else event_add("error", "import of %s to %s failed: %s", id, user[0] ? user : uid, detail ? detail : "");
    mc_wake();
}

int mc_icon_path(const char *title_id, char *out, size_t size) {
    if (!g_env.appmeta_root || !id_ok(title_id, 32)) return -1;
    snprintf(out, size, "%s/%s/icon0.png", g_env.appmeta_root, title_id);
    return exists(out) ? 0 : -1;
}
