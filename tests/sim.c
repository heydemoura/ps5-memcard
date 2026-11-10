/* PC simulator: the real page and the real memcard core against a fake console
 * tree, so the whole thing can be driven from a browser without a PS5.
 *
 *   make sim            -> http://127.0.0.1:8083/
 *   build/sim --port N --ui build/ui.html [--root DIR] [--no-usb] [--empty]
 *
 * Only the import is faked (the real one needs the console's save crypto): it
 * stages the card save exactly like the payload does, then drops the staged
 * file into the fake profile.
 *
 * Test controls:  POST /sim/usb?n=0&on=1   POST /sim/running?title=PPSA01234
 */
#include "memcard.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static char g_root[512], g_ui[512] = "build/ui.html";
static int g_mounted[MC_MAX_USB];
static char g_running[32];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const struct {
    const char *id, *name;
} TITLES[] = {{"PPSA01325", "Astro's Playroom"},   {"PPSA04609", "Ghost of Tsushima"},
              {"PPSA02530", "Returnal"},           {"CUSA07408", "God of War"},
              {"PPSA08338", "Stellar Blade"},      {"PPSA01284", "Demon's Souls"}};

static int is_mounted(const char *path) {
    int n = path[strlen(path) - 1] - '0';
    return n >= 0 && n < MC_MAX_USB && g_mounted[n];
}
static int space(const char *path, uint64_t *free_b, uint64_t *total_b) {
    struct statvfs v;
    if (statvfs(path, &v) < 0) return -1;
    *free_b = (uint64_t)v.f_bavail * v.f_frsize;
    *total_b = (uint64_t)v.f_blocks * v.f_frsize;
    return 0;
}
static int running_title(char *out, size_t size) {
    pthread_mutex_lock(&g_lock);
    snprintf(out, size, "%s", g_running);
    pthread_mutex_unlock(&g_lock);
    return out[0] != 0;
}
static void title_name(const char *id, char *out, size_t size) {
    out[0] = 0;
    for (size_t i = 0; i < sizeof(TITLES) / sizeof(TITLES[0]); i++)
        if (!strcmp(TITLES[i].id, id)) snprintf(out, size, "%s", TITLES[i].name);
}
static void user_name(const char *uid, char *out, size_t size) {
    char path[1024];
    out[0] = 0;
    snprintf(path, sizeof(path), "%s/home/%s/username.dat", g_root, uid);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t n = fread(out, 1, size - 1, f);
    out[n] = 0;
    fclose(f);
}
static void save_info(const char *uid, const char *title, const char *name, int ps4, char *m,
                      size_t ms, char *s, size_t ss) {
    (void)uid;
    (void)ps4;
    title_name(title, m, ms);
    const char *digits = name + strlen(name);
    while (digits > name && digits[-1] >= '0' && digits[-1] <= '9') digits--;
    if (strstr(name, "AUTO")) snprintf(s, ss, "Autosave");
    else snprintf(s, ss, "Slot %d", atoi(digits) + 1);
}
static void logger(const char *msg) { fputs(msg, stderr); }
static void notify(const char *msg) { fprintf(stderr, "[notify] %s\n", msg); }

static void mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    mkdir(tmp, 0777);
}

static void put(const char *rel, int first, size_t size, int age_minutes) {
    char path[1024], dir[1024];
    snprintf(path, sizeof(path), "%s/%s", g_root, rel);
    snprintf(dir, sizeof(dir), "%s", path);
    *strrchr(dir, '/') = 0;
    mkdirs(dir);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    unsigned seed = (unsigned)size * 2654435761u;
    fputc(first, f);
    for (size_t i = 1; i < size; i++) {
        seed = seed * 1103515245u + 12345u;
        fputc((int)(seed >> 16) & 0xff, f);
    }
    fclose(f);
    time_t t = time(NULL) - age_minutes * 60;
    struct timeval tv[2] = {{t, 0}, {t, 0}};
    utimes(path, tv);
}

static void put_text(const char *rel, const char *text) {
    char path[1024], dir[1024];
    snprintf(path, sizeof(path), "%s/%s", g_root, rel);
    snprintf(dir, sizeof(dir), "%s", path);
    *strrchr(dir, '/') = 0;
    mkdirs(dir);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static void seed(void) {
    char p[1024];
    put_text("home/10000000/username.dat", "heyde");
    put_text("home/1b2c3d4e/username.dat", "player-two");
    put("home/10000000/savedata_prospero/PPSA01325/sdimg_ASTRO0000", 2, 3 << 20, 4000);
    put("home/10000000/savedata_prospero/PPSA04609/sdimg_GOTAUTO0000", 2, 6 << 20, 180);
    put("home/10000000/savedata_prospero/PPSA04609/sdimg_GOTMANUAL0001", 2, 6 << 20, 1500);
    put("home/10000000/savedata_prospero/PPSA04609/sdimg_sce_bu_GOTAUTO0000", 2, 6 << 20, 190);
    put("home/10000000/savedata_prospero/PPSA02530/sdimg_RETURNAL0000", 2, 12 << 20, 30);
    put("home/10000000/savedata/CUSA07408/sdimg_GOWSAVE0000", 1, 4 << 20, 9000);
    put("home/10000000/savedata/CUSA07408/GOWSAVE0000.bin", 's', 96, 9000);
    put("home/1b2c3d4e/savedata_prospero/PPSA08338/sdimg_STELLAR0000", 2, 5 << 20, 700);
    put("home/1b2c3d4e/savedata_prospero/PPSA01325/sdimg_ASTRO0000", 2, 3 << 20, 60);
    snprintf(p, sizeof(p), "%s/usb0", g_root);
    mkdirs(p);
    snprintf(p, sizeof(p), "%s/usb1", g_root);
    mkdirs(p);
    snprintf(p, sizeof(p), "%s/appmeta", g_root);
    mkdirs(p);
}

/* ── tiny HTTP server ── */

static void reply(int fd, const char *status, const char *type, const char *body, size_t len) {
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     status, type, len);
    if (write(fd, head, (size_t)n) < 0) return;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, body + off, len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
}
static void reply_json(int fd, const char *json) {
    reply(fd, "200 OK", "application/json", json, strlen(json));
}

static int copy_file(const char *src, const char *dst) {
    FILE *a = fopen(src, "rb"), *b = a ? fopen(dst, "wb") : NULL;
    if (!a || !b) {
        if (a) fclose(a);
        return -1;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, n, b);
    fclose(a);
    fclose(b);
    return 0;
}

static void fake_import(int fd, const char *url) {
    char id[256], uid[16], prev[8], staged[1024], title[32], err[256], json[1024];
    mc_url_param(url, "id", id, sizeof(id));
    mc_url_param(url, "uid", uid, sizeof(uid));
    mc_url_param(url, "previous", prev, sizeof(prev));
    char home[1024];
    struct stat st;
    snprintf(home, sizeof(home), "%s/home/%s", g_root, uid);
    if (!uid[0] || strchr(uid, '/') || strchr(uid, '.') || stat(home, &st) < 0) {
        reply_json(fd, "{\"error\":\"Pick a profile on this console to import to\"}");
        return;
    }
    if (mc_import_prepare(id, !strcmp(prev, "1"), staged, sizeof(staged), title, sizeof(title), err,
                          sizeof(err)) < 0) {
        snprintf(json, sizeof(json), "{\"error\":\"%s\"}", err);
        reply_json(fd, json);
        return;
    }
    char running[32];
    if (running_title(running, sizeof(running)) && !strcmp(running, title)) {
        mc_import_cleanup();
        reply_json(fd, "{\"error\":\"That game is running. Close it before importing its save.\"}");
        return;
    }
    const char *name = strrchr(staged, '/') + 1;
    int ok = 0;
    if (stat(staged, &st) == 0 && S_ISREG(st.st_mode)) {
        FILE *f = fopen(staged, "rb");
        int ps4 = f && fgetc(f) == 1;
        if (f) fclose(f);
        char dir[1024], dst[1200];
        snprintf(dir, sizeof(dir), "%s/%s/%s", home, ps4 ? "savedata" : "savedata_prospero", title);
        mkdirs(dir);
        snprintf(dst, sizeof(dst), "%s/%s", dir, name);
        usleep(600000); /* the real import takes a moment too */
        ok = copy_file(staged, dst) == 0;
        if (ok && ps4 && !strncmp(name, "sdimg_", 6)) {
            char bsrc[1200], bdst[1200];
            snprintf(bsrc, sizeof(bsrc), "%.*s/%s.bin", (int)(name - staged - 1), staged, name + 6);
            snprintf(bdst, sizeof(bdst), "%s/%s.bin", dir, name + 6);
            copy_file(bsrc, bdst);
        }
    }
    mc_import_result(id, uid, ok, ok ? "" : "the simulator only imports save images");
    mc_import_cleanup();
    if (ok) snprintf(json, sizeof(json), "{\"ok\":true,\"title_id\":\"%s\",\"dir_name\":\"%s\",\"main_title\":\"\",\"warning\":\"\"}", title, name);
    else snprintf(json, sizeof(json), "{\"error\":\"The simulator only imports save images\"}");
    reply_json(fd, json);
}

static void *client(void *arg) {
    int fd = (int)(intptr_t)arg;
    char req[16384];
    size_t n = 0;
    char *end = NULL;
    while (n < sizeof(req) - 1) {
        ssize_t r = read(fd, req + n, sizeof(req) - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        req[n] = 0;
        if ((end = strstr(req, "\r\n\r\n"))) break;
    }
    if (!end) {
        close(fd);
        return NULL;
    }
    char method[8] = {0}, url[2048] = {0};
    sscanf(req, "%7s %2047s", method, url);
    char *body = end + 4;
    char *cl = strcasestr(req, "Content-Length:");
    size_t want = cl ? strtoul(cl + 15, NULL, 10) : 0, have = n - (size_t)(body - req);
    while (have < want && n < sizeof(req) - 1) {
        ssize_t r = read(fd, req + n, sizeof(req) - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        have += (size_t)r;
        req[n] = 0;
    }

    if (!strcmp(method, "GET") && (!strcmp(url, "/") || !strcmp(url, "/index.html"))) {
        FILE *f = fopen(g_ui, "rb");
        if (!f) {
            reply(fd, "500 Error", "text/plain", "ui missing", 10);
        } else {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            rewind(f);
            char *page = malloc((size_t)len + 1);
            len = (long)fread(page, 1, (size_t)len, f);
            fclose(f);
            reply(fd, "200 OK", "text/html", page, (size_t)len);
            free(page);
        }
    } else if (!strncmp(url, "/api/mc/icon", 12)) {
        char title[64], path[1024];
        mc_url_param(url, "title", title, sizeof(title));
        FILE *f = mc_icon_path(title, path, sizeof(path)) == 0 ? fopen(path, "rb") : NULL;
        if (!f) {
            reply(fd, "404 Not Found", "text/plain", "No icon", 7);
        } else {
            static char png[1 << 20];
            size_t len = fread(png, 1, sizeof(png), f);
            fclose(f);
            reply(fd, "200 OK", "image/png", png, len);
        }
    } else if (!strncmp(url, "/api/mc/import", 14)) {
        if (strcmp(method, "POST")) reply_json(fd, "{\"error\":\"Use POST\"}");
        else fake_import(fd, url);
    } else if (!strncmp(url, "/api/mc/", 8)) {
        char *json = mc_http(method, url, want ? body : NULL);
        if (json) reply_json(fd, json);
        else reply(fd, "404 Not Found", "application/json", "{\"error\":\"Not found\"}", 21);
        free(json);
    } else if (!strncmp(url, "/sim/usb", 8)) {
        char a[8], b[8];
        mc_url_param(url, "n", a, sizeof(a));
        mc_url_param(url, "on", b, sizeof(b));
        int idx = atoi(a);
        if (idx >= 0 && idx < MC_MAX_USB) g_mounted[idx] = atoi(b);
        mc_wake();
        reply_json(fd, "{\"ok\":true}");
    } else if (!strncmp(url, "/sim/running", 12)) {
        pthread_mutex_lock(&g_lock);
        mc_url_param(url, "title", g_running, sizeof(g_running));
        pthread_mutex_unlock(&g_lock);
        mc_wake();
        reply_json(fd, "{\"ok\":true}");
    } else {
        reply(fd, "404 Not Found", "text/plain", "Not found", 9);
    }
    close(fd);
    return NULL;
}

static void *watcher(void *arg) {
    (void)arg;
    for (;;) {
        mc_tick();
        if (mc_wait() < 0) break;
    }
    return NULL;
}

int main(int argc, char **argv) {
    int port = 8083, usb = 1, empty = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ui") && i + 1 < argc) snprintf(g_ui, sizeof(g_ui), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) snprintf(g_root, sizeof(g_root), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--no-usb")) usb = 0;
        else if (!strcmp(argv[i], "--empty")) empty = 1;
    }
    signal(SIGPIPE, SIG_IGN);
    if (!g_root[0]) {
        snprintf(g_root, sizeof(g_root), "/tmp/memcard-sim-XXXXXX");
        if (!mkdtemp(g_root)) return 1;
    }
    mkdirs(g_root);
    char probe[1024];
    snprintf(probe, sizeof(probe), "%s/home", g_root);
    struct stat st;
    if (stat(probe, &st) < 0) {
        if (empty) {
            snprintf(probe, sizeof(probe), "%s/home/10000000", g_root);
            mkdirs(probe);
            snprintf(probe, sizeof(probe), "%s/usb0", g_root);
            mkdirs(probe);
        } else {
            seed();
        }
    }
    g_mounted[0] = usb;

    static char home[600], usbfmt[600], cfg[600], stage[600], meta[600];
    snprintf(home, sizeof(home), "%s/home", g_root);
    snprintf(usbfmt, sizeof(usbfmt), "%s/usb%%d", g_root);
    snprintf(cfg, sizeof(cfg), "%s/data/config.json", g_root);
    snprintf(stage, sizeof(stage), "%s/data/import", g_root);
    snprintf(meta, sizeof(meta), "%s/appmeta", g_root);
    mc_env_t env = {0};
    env.home_root = home;
    env.usb_fmt = usbfmt;
    env.config_path = cfg;
    env.stage_dir = stage;
    env.appmeta_root = meta;
    env.version = "sim";
    env.is_mounted = is_mounted;
    env.space = space;
    env.running_title = running_title;
    env.title_name = title_name;
    env.user_name = user_name;
    env.save_info = save_info;
    env.notify = notify;
    env.log = logger;
    mc_init(&env);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(srv, 32) < 0) {
        perror("listen");
        return 1;
    }
    printf("ps5-memcard simulator: http://127.0.0.1:%d/  (fake console in %s)\n", port, g_root);
    fflush(stdout);
    pthread_t t;
    pthread_create(&t, NULL, watcher, NULL);
    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        pthread_t c;
        if (pthread_create(&c, NULL, client, (void *)(intptr_t)fd) == 0) pthread_detach(c);
        else close(fd);
    }
}
