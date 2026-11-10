/* ps5-memcard on the PS5 home screen.
 *
 * Shortcut: a title directory under /user/app with a param.json whose
 * deeplinkUri is the page, registered with sceAppInstUtilAppInstallTitleDir.
 * That is how ftpsrv (John Tornblom), ps5-payload-manager and
 * ps5-web-file-manager add their Media tab entries.
 *
 * Toasts: sceNotificationSend with the ToastTemplateA layout, which takes an
 * icon from a file. The system UI cannot see /data, only /user/data, so the
 * icon path is given in that form.
 */
#include "homescreen.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#define ASSET(name, file)                                  \
    __asm__(".pushsection .rodata\n"                       \
            ".global " #name "\n" #name ":\n"              \
            ".incbin \"" file "\"\n"                       \
            ".global " #name "_end\n" #name "_end:\n"      \
            ".popsection\n");                              \
    extern const uint8_t name[], name##_end[]

ASSET(hs_param_json, "assets/param.json");
ASSET(hs_icon_png, "assets/icon0.png");

#define APP_DIR "/user/app/" HOMESCREEN_TITLE_ID
/* Same file, as this process and as the system UI see it. */
#define ICON_FILE "/data/memcard/icon0.png"
#define ICON_FOR_SHELL "/user/data/memcard/icon0.png"

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void *reserved);
int sceNotificationSend(int user_id, bool logged, const char *payload);
#define NOTIFICATION_USER_SYSTEM 0xFE

void logprintf(const char *fmt, ...);
void notify(const char *fmt, ...);

/* 1 when path does not already hold exactly these bytes. */
static int differs(const char *path, const uint8_t *data, size_t size) {
    struct stat st;
    if (stat(path, &st) != 0 || (size_t)st.st_size != size) return 1;
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    uint8_t *have = malloc(size ? size : 1);
    int changed = !have || fread(have, 1, size, f) != size || memcmp(have, data, size) != 0;
    free(have);
    fclose(f);
    return changed;
}

static int put_file(const char *path, const uint8_t *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

/* Register one title directory. Not every firmware exports
 * sceAppInstUtilAppInstallTitleDir by name, so it is looked up by its NID (as
 * ftpsrv and ps5-payload-manager do); failing that, the system rescans all. */
static int register_title(const char *title_id, const char *parent) {
    int (*install_dir)(const char *, const char *, void *) = NULL;
    uint32_t handle = 0;
    if (!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle))
        install_dir = (void *)kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE");
    if (install_dir) return install_dir(title_id, parent, 0);
    logprintf("[memcard] shortcut: no per-title install on this firmware, rescanning all titles\n");
    return sceAppInstUtilAppInstallAll(0);
}

static void install_shortcut(void) {
    const size_t param_size = (size_t)(hs_param_json_end - hs_param_json);
    const size_t icon_size = (size_t)(hs_icon_png_end - hs_icon_png);
    const char *param = APP_DIR "/sce_sys/param.json", *icon = APP_DIR "/sce_sys/icon0.png";
    if (!differs(param, hs_param_json, param_size) && !differs(icon, hs_icon_png, icon_size)) {
        logprintf("[memcard] home screen shortcut " HOMESCREEN_TITLE_ID " is in place\n");
        return;
    }
    mkdir("/user/app", 0755);
    mkdir(APP_DIR, 0755);
    mkdir(APP_DIR "/sce_sys", 0755);
    if (put_file(param, hs_param_json, param_size) < 0 || put_file(icon, hs_icon_png, icon_size) < 0) {
        logprintf("[memcard] shortcut: cannot write %s (errno %d)\n", APP_DIR, errno);
        return;
    }
    int err = sceAppInstUtilInitialize();
    if (err) {
        logprintf("[memcard] shortcut: sceAppInstUtilInitialize failed: 0x%08x\n", err);
        return;
    }
    err = register_title(HOMESCREEN_TITLE_ID, "/user/app/");
    sceAppInstUtilTerminate();
    if (err) {
        logprintf("[memcard] shortcut: install failed: 0x%08x\n", err);
        /* Leave nothing that looks installed, so the next start tries again. */
        unlink(param);
        return;
    }
    logprintf("[memcard] home screen shortcut " HOMESCREEN_TITLE_ID " installed (Media tab)\n");
}

void homescreen_setup(void) {
    const size_t icon_size = (size_t)(hs_icon_png_end - hs_icon_png);
    if (differs(ICON_FILE, hs_icon_png, icon_size) && put_file(ICON_FILE, hs_icon_png, icon_size) < 0)
        logprintf("[memcard] cannot write %s (errno %d)\n", ICON_FILE, errno);
    install_shortcut();
}

/* Append s to out as the inside of a JSON string. */
static void json_text(char *out, size_t cap, const char *s) {
    size_t n = strlen(out);
    for (; *s && n + 8 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c == '\n') { out[n++] = ' '; }
        else if (c < 0x20) { n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c); }
        else out[n++] = (char)c;
    }
    out[n] = 0;
}

void homescreen_toast(const char *title, const char *fmt, ...) {
    static unsigned serial;
    char text[512], json[4096], stamp[40];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S.000Z", &tm);

    /* ToastTemplateA: icon, a title line and a smaller line under it. The
     * previewDisabled view is what is shown when previews are turned off. */
    snprintf(json, sizeof(json),
             "{\"rawData\":{\"viewTemplateType\":\"ToastTemplateA\",\"channelType\":\"Downloads\","
             "\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,"
             "\"viewData\":{\"icon\":{\"type\":\"Url\",\"parameters\":{\"url\":\"" ICON_FOR_SHELL "\"}},"
             "\"message\":{\"body\":\"");
    json_text(json, sizeof(json) - 600, title);
    strncat(json, "\"},\"subMessage\":{\"body\":\"", sizeof(json) - strlen(json) - 1);
    json_text(json, sizeof(json) - 500, text);
    strncat(json,
            "\"}},\"platformViews\":{\"previewDisabled\":{\"viewData\":{\"icon\":{\"type\":\"Predefined\","
            "\"parameters\":{\"icon\":\"download\"}},\"message\":{\"body\":\"",
            sizeof(json) - strlen(json) - 1);
    json_text(json, sizeof(json) - 200, title);
    size_t n = strlen(json);
    snprintf(json + n, sizeof(json) - n,
             "\"}}}}},\"createdDateTime\":\"%s\",\"localNotificationId\":\"%u\"}", stamp,
             588193127u + serial++);

    int err = sceNotificationSend(NOTIFICATION_USER_SYSTEM, true, json);
    if (err) {
        logprintf("[memcard] toast refused (0x%08x), using a plain notification\n", err);
        notify("%s\n%s", title, text);
        return;
    }
    logprintf("[memcard] %s: %s\n", title, text);
}
