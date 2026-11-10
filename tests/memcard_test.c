/* Host test for the memcard core: builds a fake console + USB tree and walks
 * the watcher through the situations it has to get right.
 *   cc -O1 -g -Isrc tests/memcard_test.c src/memcard.c -lpthread -o /tmp/memcard_test */
#include "memcard.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static char g_root[512];
static int g_mounted[MC_MAX_USB];
static char g_running[32];
static int64_t g_clock = 1800000000; /* fake wall clock, also drives the monotonic one */
static int g_notifications;
static int g_failures, g_checks;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        g_checks++;                                                                      \
        if (!(cond)) {                                                                   \
            g_failures++;                                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
        }                                                                                \
    } while (0)

static const char *P(const char *fmt, ...) {
    static char bufs[8][1024];
    static int next;
    char *out = bufs[next++ % 8];
    char rel[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rel, sizeof(rel), fmt, ap);
    va_end(ap);
    snprintf(out, 1024, "%s/%s", g_root, rel);
    return out;
}

static void mkdirs(const char *path) {
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
    if (system(cmd) != 0) abort();
}

static void put(const char *path, const char *data, size_t len, int64_t mtime) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    *strrchr(dir, '/') = 0;
    mkdirs(dir);
    FILE *f = fopen(path, "wb");
    if (!f) abort();
    fwrite(data, 1, len, f);
    fclose(f);
    struct timeval tv[2] = {{mtime, 0}, {mtime, 0}};
    utimes(path, tv);
}

static char *slurp(const char *path, size_t *len) {
    static char buf[1 << 16];
    FILE *f = fopen(path, "rb");
    if (!f) {
        *len = 0;
        return NULL;
    }
    *len = fread(buf, 1, sizeof(buf) - 1, f);
    buf[*len] = 0;
    fclose(f);
    return buf;
}

static int same(const char *path, const char *data, size_t len) {
    size_t n;
    char *got = slurp(path, &n);
    return got && n == len && !memcmp(got, data, len);
}

static int there(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int is_mounted(const char *path) {
    int n = path[strlen(path) - 1] - '0';
    return n >= 0 && n < MC_MAX_USB && g_mounted[n];
}
static int space(const char *path, uint64_t *f, uint64_t *t) {
    (void)path;
    *f = 1ull << 30;
    *t = 4ull << 30;
    return 0;
}
static int running_title(char *out, size_t size) {
    snprintf(out, size, "%s", g_running);
    return g_running[0] != 0;
}
static void title_name(const char *id, char *out, size_t size) {
    snprintf(out, size, "%s", !strcmp(id, "PPSA01234") ? "Astro's \"Test\" Room" : "");
}
static void user_name(const char *uid, char *out, size_t size) {
    snprintf(out, size, "%s", !strcmp(uid, "10000000") ? "heyde" : "guest");
}
static void save_info(const char *uid, const char *title, const char *name, int ps4, char *m,
                      size_t ms, char *s, size_t ss) {
    (void)uid;
    (void)title;
    (void)ps4;
    snprintf(m, ms, "Main of %s", name);
    snprintf(s, ss, "Slot\t1");
}
static void notify(const char *msg) {
    (void)msg;
    g_notifications++;
}
static void logger(const char *msg) {
    if (getenv("MC_TEST_VERBOSE")) fputs(msg, stdout);
}
static int64_t wall(void) { return g_clock; }
static int64_t mono(void) { return g_clock - 1700000000; }

static char *call(const char *method, const char *url, const char *body) {
    static char out[1 << 16];
    char *r = mc_http(method, url, body);
    snprintf(out, sizeof(out), "%s", r ? r : "(null)");
    free(r);
    return out;
}

#define SAVE1 "10000000/savedata_prospero/PPSA01234/sdimg_SAVE0001"
#define CARD1 "usb0/memcard/10000000/PPSA01234/sdimg_SAVE0001"

int main(void) {
    snprintf(g_root, sizeof(g_root), "/tmp/memcard-test-XXXXXX");
    if (!mkdtemp(g_root)) return 2;
    static char home[600], usb[600], cfg[600], stage[600], meta[600];
    snprintf(home, sizeof(home), "%s/home", g_root);
    snprintf(usb, sizeof(usb), "%s/usb%%d", g_root);
    snprintf(cfg, sizeof(cfg), "%s/data/config.json", g_root);
    snprintf(stage, sizeof(stage), "%s/data/import", g_root);
    snprintf(meta, sizeof(meta), "%s/appmeta", g_root);

    int64_t t0 = g_clock - 5000;
    put(P("home/" SAVE1), "\x02ps5-save-v1", 12, t0);
    put(P("home/10000000/savedata_prospero/PPSA01234/sdimg_sce_bu_SAVE0001"), "\x02" "bu", 3, t0);
    put(P("home/10000000/savedata/CUSA00001/sdimg_PS4SAVE"), "\x01ps4-image", 10, t0);
    put(P("home/10000000/savedata/CUSA00001/PS4SAVE.bin"), "sealedkey", 9, t0);
    put(P("home/10000000/savedata/CUSA00001/notasave"), "junk", 4, t0);
    put(P("home/1a2b3c4d/savedata_prospero/PPSA01234/sdimg_SAVE0001"), "\x02other-user", 11, t0);
    mkdirs(P("usb0"));
    mkdirs(P("usb1"));

    mc_env_t env = {0};
    env.home_root = home;
    env.usb_fmt = usb;
    env.config_path = cfg;
    env.stage_dir = stage;
    env.appmeta_root = meta;
    env.version = "test";
    env.is_mounted = is_mounted;
    env.space = space;
    env.running_title = running_title;
    env.title_name = title_name;
    env.user_name = user_name;
    env.save_info = save_info;
    env.notify = notify;
    env.log = logger;
    env.now_wall = wall;
    env.now_mono = mono;
    mc_init(&env);

    printf("1. no drive mounted: nothing is written\n");
    mc_tick();
    CHECK(!there(P("usb0/memcard")));
    CHECK(strstr(call("GET", "/api/mc/status", NULL), "\"present\":false"));
    CHECK(strstr(call("GET", "/api/mc/console", NULL), "\"state\":\"no_card\""));
    CHECK(strstr(call("POST", "/api/mc/backup?all=1", NULL), "No USB drive"));

    printf("2. drive appears: every save is mirrored\n");
    g_mounted[0] = 1;
    mc_tick();
    CHECK(same(P(CARD1), "\x02ps5-save-v1", 12));
    CHECK(same(P("usb0/memcard/1a2b3c4d/PPSA01234/sdimg_SAVE0001"), "\x02other-user", 11));
    CHECK(same(P("usb0/memcard/10000000/CUSA00001/sdimg_PS4SAVE"), "\x01ps4-image", 10));
    CHECK(same(P("usb0/memcard/10000000/CUSA00001/PS4SAVE.bin"), "sealedkey", 9));
    CHECK(!there(P("usb0/memcard/10000000/CUSA00001/notasave")));
    CHECK(!there(P("usb0/memcard/10000000/PPSA01234/sdimg_sce_bu_SAVE0001")));
    CHECK(same(P("usb0/memcard/10000000/profile.txt"), "heyde", 5));
    CHECK(there(P("usb0/memcard/index.tsv")));
    CHECK(!there(P(CARD1 ".mctmp")));
    CHECK(!there(P("usb1/memcard")));
    CHECK(g_notifications == 1); /* one popup for the whole pass */
    char *st = call("GET", "/api/mc/status", NULL);
    CHECK(strstr(st, "\"present\":true") && strstr(st, "\"usb\":0"));
    CHECK(strstr(st, "\"console_saves\":3,\"synced\":3,\"waiting\":0"));
    char *card = call("GET", "/api/mc/card", NULL);
    CHECK(strstr(card, "\"id\":\"10000000/PPSA01234/sdimg_SAVE0001\""));
    CHECK(strstr(card, "\"title_name\":\"Astro's \\\"Test\\\" Room\""));
    CHECK(strstr(card, "\"user_name\":\"heyde\""));
    CHECK(strstr(card, "\"sub_title\":\"Slot 1\""));
    CHECK(strstr(card, "\"type\":\"ps4\"") && strstr(card, "\"type\":\"ps5\""));
    CHECK(!strstr(card, "PS4SAVE.bin\"") && !strstr(card, "profile.txt"));
    CHECK(strstr(call("GET", "/api/mc/console", NULL), "\"state\":\"synced\""));
    CHECK(!strstr(call("GET", "/api/mc/console", NULL), "\"state\":\"pending\""));

    call("POST", "/api/mc/backup?all=1", NULL);
    mc_tick();
    CHECK(!there(P(CARD1 ".prev"))); /* asking again for an identical save copies nothing */
    CHECK(strstr(call("GET", "/api/mc/status", NULL), "3 copies already up to date"));

    printf("3. a changed save waits for the settle time, the old copy is kept\n");
    g_clock += 100;
    put(P("home/" SAVE1), "\x02ps5-save-v2!", 13, g_clock);
    mc_tick();
    CHECK(same(P(CARD1), "\x02ps5-save-v1", 12));
    CHECK(strstr(call("GET", "/api/mc/console", NULL), "\"state\":\"pending\""));
    g_clock += 10;
    mc_tick();
    CHECK(same(P(CARD1), "\x02ps5-save-v1", 12));
    g_clock += 10;
    mc_tick();
    CHECK(same(P(CARD1), "\x02ps5-save-v2!", 13));
    CHECK(same(P(CARD1 ".prev"), "\x02ps5-save-v1", 12));
    CHECK(strstr(call("GET", "/api/mc/card", NULL), "\"has_previous\":true"));

    printf("4. while the game runs, one save is copied at most once per interval\n");
    snprintf(g_running, sizeof(g_running), "PPSA01234");
    int before = g_notifications;
    g_clock += 30;
    put(P("home/" SAVE1), "\x02ps5-save-v3!", 13, g_clock);
    g_clock += 20;
    mc_tick();
    CHECK(same(P(CARD1), "\x02ps5-save-v2!", 13));
    g_clock += 300;
    mc_tick();
    CHECK(same(P(CARD1), "\x02ps5-save-v3!", 13));
    CHECK(g_notifications == before); /* no popup over the running game */
    CHECK(strstr(call("GET", "/api/mc/status", NULL), "\"running_title\":\"PPSA01234\""));

    printf("5. closing the game triggers a content check even without a new timestamp\n");
    {
        struct stat keep;
        stat(P("home/" SAVE1), &keep);
        put(P("home/" SAVE1), "\x02ps5-save-v4!", 13, keep.st_mtime);
    }
    g_clock += 400;
    mc_tick();
    CHECK(same(P(CARD1), "\x02ps5-save-v3!", 13));
    g_running[0] = 0;
    mc_tick(); /* notices the close, starts the settle timer */
    CHECK(same(P(CARD1), "\x02ps5-save-v3!", 13));
    g_clock += 16;
    mc_tick();
    CHECK(same(P(CARD1), "\x02ps5-save-v4!", 13));

    printf("6. an older console save never overwrites a newer card copy by itself\n");
    put(P("home/" SAVE1), "\x02old-console", 12, t0 - 999);
    g_clock += 100;
    mc_tick();
    CHECK(same(P(CARD1), "\x02ps5-save-v4!", 13));
    CHECK(strstr(call("GET", "/api/mc/console", NULL), "\"state\":\"card_newer\""));
    CHECK(strstr(call("POST", "/api/mc/backup?uid=10000000&title=PPSA01234&save=sdimg_SAVE0001", NULL),
                 "\"queued\":true"));
    mc_tick();
    CHECK(same(P(CARD1), "\x02old-console", 12));
    CHECK(same(P(CARD1 ".prev"), "\x02ps5-save-v4!", 13));

    printf("7. import staging copies from the card and leaves the card alone\n");
    char staged[1024], title[32], err[256];
    CHECK(mc_import_prepare("10000000/PPSA01234/sdimg_SAVE0001", -1, 0, staged, sizeof(staged), title,
                            sizeof(title), err, sizeof(err)) == 0);
    CHECK(!strcmp(staged, P("data/import/PPSA01234/sdimg_SAVE0001")));
    CHECK(!strcmp(title, "PPSA01234"));
    CHECK(same(staged, "\x02old-console", 12));
    CHECK(same(P(CARD1), "\x02old-console", 12));
    CHECK(mc_import_prepare("10000000/PPSA01234/sdimg_SAVE0001", -1, 1, staged, sizeof(staged), title,
                            sizeof(title), err, sizeof(err)) == 0);
    CHECK(same(staged, "\x02ps5-save-v4!", 13));
    CHECK(mc_import_prepare("10000000/CUSA00001/sdimg_PS4SAVE", -1, 0, staged, sizeof(staged), title,
                            sizeof(title), err, sizeof(err)) == 0);
    CHECK(same(P("data/import/CUSA00001/sdimg_PS4SAVE"), "\x01ps4-image", 10));
    CHECK(same(P("data/import/CUSA00001/PS4SAVE.bin"), "sealedkey", 9));
    CHECK(!there(P("data/import/PPSA01234"))); /* the staging dir holds one save at a time */
    mc_import_result("10000000/CUSA00001/sdimg_PS4SAVE", "1a2b3c4d", 1, "");
    CHECK(strstr(call("GET", "/api/mc/status", NULL), "imported to guest"));

    printf("8. hostile references are refused\n");
    CHECK(mc_import_prepare("../../home/10000000", -1, 0, staged, sizeof(staged), title, sizeof(title),
                            err, sizeof(err)) < 0);
    CHECK(mc_import_prepare("10000000/PPSA01234/..", -1, 0, staged, sizeof(staged), title, sizeof(title),
                            err, sizeof(err)) < 0);
    CHECK(mc_import_prepare("10000000/../sdimg_SAVE0001", -1, 0, staged, sizeof(staged), title,
                            sizeof(title), err, sizeof(err)) < 0);
    CHECK(mc_import_prepare("10000000/PPSA01234/a/b", -1, 0, staged, sizeof(staged), title, sizeof(title),
                            err, sizeof(err)) < 0);
    CHECK(mc_import_prepare("10000000/PPSA01234/sdimg_SAVE0001.prev", -1, 0, staged, sizeof(staged),
                            title, sizeof(title), err, sizeof(err)) < 0);
    CHECK(strstr(call("POST", "/api/mc/delete?id=..%2F..%2Fhome%2Fx", NULL), "error"));
    CHECK(strstr(call("POST", "/api/mc/delete?id=10000000%2FPPSA01234%2F..", NULL), "error"));
    CHECK(strstr(call("POST", "/api/mc/backup?uid=..&title=PPSA01234", NULL), "error"));
    CHECK(strstr(call("POST", "/api/mc/backup", NULL), "error"));
    CHECK(there(P("home/" SAVE1)));
    char icon[1024];
    CHECK(mc_icon_path("../../etc", icon, sizeof(icon)) < 0);

    printf("9. settings: turning auto backup off stops mirroring, manual still works\n");
    char *set = call("POST", "/api/mc/settings",
                     "{\"enabled\":false,\"settle_seconds\":3,\"keep_previous\":false,\"poll_seconds\":1}");
    CHECK(strstr(set, "\"enabled\":false") && strstr(set, "\"settle_seconds\":3"));
    CHECK(strstr(set, "\"poll_seconds\":2")); /* clamped */
    size_t n;
    CHECK(strstr(slurp(cfg, &n), "\"enabled\":false"));
    g_clock += 50;
    put(P("home/" SAVE1), "\x02ps5-save-v5!", 13, g_clock);
    g_clock += 50;
    mc_tick();
    CHECK(same(P(CARD1), "\x02old-console", 12));
    call("POST", "/api/mc/backup?all=1", NULL);
    mc_tick();
    CHECK(same(P(CARD1), "\x02ps5-save-v5!", 13));
    CHECK(!there(P(CARD1 ".prev"))); /* keep_previous off: no generations are kept */
    call("POST", "/api/mc/settings", "{\"enabled\":true}");

    printf("10. deleting a card save removes its companions and the index entry\n");
    CHECK(strstr(call("POST", "/api/mc/delete?id=10000000%2FCUSA00001%2Fsdimg_PS4SAVE", NULL), "\"ok\":true"));
    CHECK(!there(P("usb0/memcard/10000000/CUSA00001/sdimg_PS4SAVE")));
    CHECK(!there(P("usb0/memcard/10000000/CUSA00001/PS4SAVE.bin")));
    CHECK(!there(P("usb0/memcard/10000000/CUSA00001")));
    CHECK(!strstr(slurp(P("usb0/memcard/index.tsv"), &n), "CUSA00001"));
    CHECK(strstr(call("GET", "/api/mc/console", NULL), "\"state\":\"missing\""));
    mc_tick(); /* and the watcher puts it back because the console still has it */
    CHECK(there(P("usb0/memcard/10000000/CUSA00001/sdimg_PS4SAVE")));

    printf("11. the index survives a restart and a drive swap\n");
    g_mounted[0] = 0;
    mc_tick();
    CHECK(strstr(call("GET", "/api/mc/status", NULL), "memory card removed"));
    CHECK(strstr(call("GET", "/api/mc/card", NULL), "\"present\":false"));
    g_mounted[0] = 1;
    mc_tick();
    st = call("GET", "/api/mc/status", NULL);
    CHECK(strstr(st, "\"console_saves\":3,\"synced\":3,\"waiting\":0"));
    CHECK(!there(P(CARD1 ".prev"))); /* nothing was copied again */

    printf("12. a decrypted folder dropped on the card is listed and can be staged\n");
    put(P("usb0/memcard/10000000/PPSA09999/MYSAVE/sce_sys/param.sfo"), "sfo", 3, t0);
    put(P("usb0/memcard/10000000/PPSA09999/MYSAVE/data.bin"), "payload", 7, t0);
    card = call("GET", "/api/mc/card", NULL);
    CHECK(strstr(card, "\"id\":\"10000000/PPSA09999/MYSAVE\"") && strstr(card, "\"format\":\"folder\""));
    CHECK(mc_import_prepare("10000000/PPSA09999/MYSAVE", -1, 0, staged, sizeof(staged), title,
                            sizeof(title), err, sizeof(err)) == 0);
    CHECK(same(P("data/import/PPSA09999/MYSAVE/sce_sys/param.sfo"), "sfo", 3));
    CHECK(same(P("data/import/PPSA09999/MYSAVE/data.bin"), "payload", 7));
    mc_import_cleanup();
    CHECK(!there(P("data/import")));

    printf("13. pinning a usb port\n");
    g_mounted[1] = 1;
    call("POST", "/api/mc/settings", "{\"usb\":1}");
    mc_tick();
    CHECK(there(P("usb1/memcard/10000000/PPSA01234/sdimg_SAVE0001")));
    st = call("GET", "/api/mc/status", NULL);
    CHECK(strstr(st, "{\"usb\":1,\"card\":true") && strstr(st, "{\"usb\":0,\"card\":false"));
    CHECK(strstr(st, "memory card removed from usb0") && strstr(st, "\"count\":1"));

#define SAVEID "uid=10000000&title=PPSA01234&save=sdimg_SAVE0001"
#define ONCARD(n) P("usb%d/memcard/10000000/PPSA01234/sdimg_SAVE0001", n)
    printf("14. two memory cards: every save goes to both\n");
    call("POST", "/api/mc/settings", "{\"usb\":-1}");
    mc_tick();
    st = call("GET", "/api/mc/status", NULL);
    CHECK(strstr(st, "{\"usb\":0,\"card\":true") && strstr(st, "{\"usb\":1,\"card\":true"));
    CHECK(strstr(st, "\"count\":2"));
    before = g_notifications;
    g_clock += 10;
    put(P("home/" SAVE1), "\x02ps5-save-v6!", 13, g_clock);
    g_clock += 10;
    mc_tick();
    CHECK(same(ONCARD(0), "\x02ps5-save-v6!", 13));
    CHECK(same(ONCARD(1), "\x02ps5-save-v6!", 13));
    CHECK(g_notifications == before + 1); /* one save, one popup, however many cards */
    st = call("GET", "/api/mc/status", NULL);
    CHECK(strstr(st, "sdimg_SAVE0001 -> usb0") && strstr(st, "sdimg_SAVE0001 -> usb1"));
    CHECK(strstr(st, "\"console_saves\":3,\"synced\":3,\"waiting\":0"));
    char *con = call("GET", "/api/mc/console", NULL);
    CHECK(strstr(con, "\"cards\":[{\"usb\":0,\"state\":\"synced\""));
    CHECK(strstr(con, ",{\"usb\":1,\"state\":\"synced\""));
    card = call("GET", "/api/mc/card", NULL);
    CHECK(strstr(card, "{\"usb\":0,\"id\":\"10000000/PPSA01234/sdimg_SAVE0001\""));
    CHECK(strstr(card, "{\"usb\":1,\"id\":\"10000000/PPSA01234/sdimg_SAVE0001\""));
    CHECK(!strstr(call("GET", "/api/mc/card?usb=1", NULL), "{\"usb\":0,\"id\""));

    printf("15. a blank third drive is left alone until it is made a card\n");
    mkdirs(P("usb2"));
    mkdirs(P("usb3"));
    g_mounted[2] = 1;
    mc_tick();
    CHECK(!there(P("usb2/memcard")));
    CHECK(strstr(call("GET", "/api/mc/status", NULL), "{\"usb\":2,\"card\":false"));
    CHECK(strstr(call("POST", "/api/mc/card_init?usb=5", NULL), "No drive is connected"));
    CHECK(strstr(call("POST", "/api/mc/card_init", NULL), "error"));
    CHECK(strstr(call("POST", "/api/mc/card_init?usb=2", NULL), "\"ok\":true"));
    mc_tick();
    CHECK(same(ONCARD(2), "\x02ps5-save-v6!", 13));
    CHECK(there(P("usb2/memcard/10000000/CUSA00001/PS4SAVE.bin")));
    CHECK(there(P("usb2/memcard/index.tsv")));
    CHECK(strstr(call("GET", "/api/mc/status", NULL), "\"count\":3"));

    printf("16. a manual backup can target one card\n");
    call("POST", "/api/mc/settings", "{\"enabled\":false}");
    g_clock += 10;
    put(P("home/" SAVE1), "\x02ps5-save-v7!", 13, g_clock);
    g_clock += 10;
    mc_tick();
    CHECK(same(ONCARD(1), "\x02ps5-save-v6!", 13));
    CHECK(strstr(call("POST", "/api/mc/backup?usb=5&" SAVEID, NULL), "usb5 is not a memory card"));
    CHECK(strstr(call("POST", "/api/mc/backup?usb=9&" SAVEID, NULL), "Invalid"));
    CHECK(strstr(call("POST", "/api/mc/backup?usb=x&" SAVEID, NULL), "Invalid"));
    CHECK(strstr(call("POST", "/api/mc/backup?usb=1&" SAVEID, NULL), "\"queued\":true"));
    mc_tick();
    CHECK(same(ONCARD(1), "\x02ps5-save-v7!", 13));
    CHECK(same(ONCARD(0), "\x02ps5-save-v6!", 13));
    CHECK(same(ONCARD(2), "\x02ps5-save-v6!", 13));
    con = call("GET", "/api/mc/console", NULL);
    CHECK(strstr(con, "\"state\":\"pending\",\"backed_at\"")); /* overall: the card furthest behind */
    CHECK(strstr(con, "{\"usb\":0,\"state\":\"pending\"") && strstr(con, "{\"usb\":1,\"state\":\"synced\""));
    st = call("GET", "/api/mc/status", NULL);
    CHECK(strstr(st, "\"console_saves\":3,\"synced\":2,\"waiting\":1"));

    printf("17. import and delete act on the card that was named\n");
    CHECK(mc_import_prepare("10000000/PPSA01234/sdimg_SAVE0001", 1, 0, staged, sizeof(staged), title,
                            sizeof(title), err, sizeof(err)) == 0);
    CHECK(same(staged, "\x02ps5-save-v7!", 13));
    CHECK(mc_import_prepare("10000000/PPSA01234/sdimg_SAVE0001", 0, 0, staged, sizeof(staged), title,
                            sizeof(title), err, sizeof(err)) == 0);
    CHECK(same(staged, "\x02ps5-save-v6!", 13));
    CHECK(mc_import_prepare("10000000/PPSA01234/sdimg_SAVE0001", 5, 0, staged, sizeof(staged), title,
                            sizeof(title), err, sizeof(err)) < 0);
    CHECK(mc_import_prepare("10000000/PPSA01234/sdimg_SAVE0001", 42, 0, staged, sizeof(staged), title,
                            sizeof(title), err, sizeof(err)) < 0);
    /* the folder from step 12 only exists on usb0: without a card it is found there */
    CHECK(mc_import_prepare("10000000/PPSA09999/MYSAVE", -1, 0, staged, sizeof(staged), title,
                            sizeof(title), err, sizeof(err)) == 0);
    CHECK(mc_import_prepare("10000000/PPSA09999/MYSAVE", 1, 0, staged, sizeof(staged), title,
                            sizeof(title), err, sizeof(err)) < 0);
    mc_import_cleanup();
    CHECK(strstr(call("POST", "/api/mc/delete?usb=2&id=10000000%2FPPSA01234%2Fsdimg_SAVE0001", NULL), "\"ok\":true"));
    CHECK(!there(ONCARD(2)) && there(ONCARD(0)) && there(ONCARD(1)));
    CHECK(!strstr(slurp(P("usb2/memcard/index.tsv"), &n), "10000000\tPPSA01234\tsdimg_SAVE0001"));
    CHECK(strstr(slurp(P("usb1/memcard/index.tsv"), &n), "10000000\tPPSA01234\tsdimg_SAVE0001"));
    CHECK(strstr(call("POST", "/api/mc/delete?usb=5&id=10000000%2FPPSA01234%2Fsdimg_SAVE0001", NULL), "error"));

    printf("18. a card keeps its name, wherever it is plugged in\n");
    CHECK(strstr(call("POST", "/api/mc/label?usb=1&name=Travel%20card", NULL), "\"ok\":true"));
    CHECK(same(P("usb1/memcard/label.txt"), "Travel card", 11));
    CHECK(strstr(call("GET", "/api/mc/status", NULL), "{\"usb\":1,\"card\":true,\"label\":\"Travel card\""));
    CHECK(strstr(call("POST", "/api/mc/label?usb=5&name=x", NULL), "error"));
    CHECK(strstr(call("POST", "/api/mc/label?usb=2&name=%0A%20evil%09name%20", NULL), "\"ok\":true"));
    CHECK(same(P("usb2/memcard/label.txt"), "evilname", 8));
    CHECK(strstr(call("POST", "/api/mc/label?usb=2&name=", NULL), "\"ok\":true"));
    CHECK(!there(P("usb2/memcard/label.txt")));
    g_mounted[1] = 0;
    mc_tick();
    CHECK(strstr(call("GET", "/api/mc/status", NULL), "\"count\":2"));
    g_mounted[1] = 1;
    call("POST", "/api/mc/settings", "{\"enabled\":true}");
    mc_tick();
    st = call("GET", "/api/mc/status", NULL);
    CHECK(strstr(st, "\"label\":\"Travel card\"") && strstr(st, "\"count\":3"));
    CHECK(same(ONCARD(0), "\x02ps5-save-v7!", 13) && same(ONCARD(2), "\x02ps5-save-v7!", 13));
    g_clock += 10;
    put(P("home/" SAVE1), "\x02ps5-save-v8!", 13, g_clock);
    g_clock += 10;
    mc_tick();
    CHECK(strstr(call("GET", "/api/mc/status", NULL), "sdimg_SAVE0001 -> Travel card (usb1)"));
    CHECK(!strstr(call("GET", "/api/mc/card", NULL), "label.txt"));

    printf("19. with \"every drive\" on, a blank drive becomes a card by itself\n");
    g_mounted[3] = 1;
    mc_tick();
    CHECK(!there(P("usb3/memcard")));
    set = call("POST", "/api/mc/settings", "{\"all_drives\":true}");
    CHECK(strstr(set, "\"all_drives\":true"));
    mc_tick();
    CHECK(same(ONCARD(3), "\x02ps5-save-v8!", 13));
    CHECK(strstr(slurp(cfg, &n), "\"all_drives\":true"));
    call("POST", "/api/mc/settings", "{\"all_drives\":false}");

    printf("20. pulling one card does not stop the others, and it catches up later\n");
    g_mounted[0] = 0;
    g_clock += 10;
    put(P("home/" SAVE1), "\x02ps5-save-v9!", 13, g_clock);
    g_clock += 10;
    mc_tick();
    CHECK(same(ONCARD(1), "\x02ps5-save-v9!", 13) && same(ONCARD(2), "\x02ps5-save-v9!", 13) &&
          same(ONCARD(3), "\x02ps5-save-v9!", 13));
    CHECK(same(ONCARD(0), "\x02ps5-save-v8!", 13));
    st = call("GET", "/api/mc/status", NULL);
    CHECK(strstr(st, "\"count\":3") && strstr(st, "\"console_saves\":3,\"synced\":3,\"waiting\":0"));
    CHECK(!strstr(call("GET", "/api/mc/card", NULL), "{\"usb\":0,\"id\""));
    g_mounted[0] = 1;
    mc_tick();
    CHECK(same(ONCARD(0), "\x02ps5-save-v9!", 13));
    st = call("GET", "/api/mc/status", NULL);
    CHECK(strstr(st, "\"count\":4") && strstr(st, "\"console_saves\":3,\"synced\":3,\"waiting\":0"));

    printf("21. no card at all\n");
    g_mounted[0] = g_mounted[1] = g_mounted[2] = g_mounted[3] = 0;
    mc_tick();
    st = call("GET", "/api/mc/status", NULL);
    CHECK(strstr(st, "\"drives\":[]") && strstr(st, "\"present\":false,\"count\":0"));
    CHECK(strstr(call("GET", "/api/mc/console", NULL), "\"state\":\"no_card\",\"backed_at\":0,\"cards\":[]"));
    CHECK(strstr(call("POST", "/api/mc/backup?all=1", NULL), "No USB drive"));

    printf("%d checks, %d failed\n", g_checks, g_failures);
    if (!g_failures) {
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_root);
        if (system(cmd) != 0) return 1;
    } else {
        printf("tree kept in %s\n", g_root);
    }
    return g_failures ? 1 : 0;
}
