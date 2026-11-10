/*
 * ps5-memcard core: keeps a mirror of every console save on a USB drive under
 * /memcard, lists what is on the card and stages card saves for import.
 *
 * Every connected USB drive can be a memory card (one per port, usb0..usb7);
 * saves are mirrored to all of them and each keeps its own index.
 *
 * This file is plain POSIX on purpose. The PS5 specifics (USB mount detection,
 * running title, title names, notifications) come in through mc_env_t, so the
 * same code runs on a PC against a fake tree (tests/sim).
 *
 * Card layout:
 *   <usb>/memcard/index.tsv                      what was backed up and from what
 *   <usb>/memcard/label.txt                      optional name of the card
 *   <usb>/memcard/<user id>/profile.txt          name of the profile it came from
 *   <usb>/memcard/<user id>/<TITLE>/<save>       raw save image (same bytes as the console)
 *   <usb>/memcard/<user id>/<TITLE>/<save>.prev  previous generation (optional)
 *   <usb>/memcard/<user id>/<TITLE>/<name>.bin   PS4 sealed-key companion
 */
#ifndef MEMCARD_H
#define MEMCARD_H

#include <stddef.h>
#include <stdint.h>

#define MC_PATH 1024
#define MC_CARD_DIR "memcard"
#define MC_MAX_USB 8

typedef struct mc_env {
    const char *home_root;    /* /user/home */
    const char *usb_fmt;      /* /mnt/usb%d */
    const char *config_path;  /* /data/memcard/config.json */
    const char *stage_dir;    /* /data/memcard/import */
    const char *appmeta_root; /* /user/appmeta */
    const char *version;

    /* 1 when path is the root of a mounted drive (never write to a bare /mnt/usbN dir). */
    int (*is_mounted)(const char *path);
    int (*space)(const char *path, uint64_t *free_bytes, uint64_t *total_bytes);
    /* 1 and the title id when a game is running. */
    int (*running_title)(char *title_id, size_t size);
    void (*title_name)(const char *title_id, char *out, size_t size);
    void (*user_name)(const char *uid, char *out, size_t size);
    /* Optional save description (main title / sub title). Never called with a memcard lock held. */
    void (*save_info)(const char *uid, const char *title_id, const char *save_name, int is_ps4,
                      char *main_title, size_t main_size, char *sub_title, size_t sub_size);
    void (*notify)(const char *message);
    void (*log)(const char *message);
    int (*stopping)(void);
    /* Optional clocks, for tests. */
    int64_t (*now_wall)(void);
    int64_t (*now_mono)(void);
} mc_env_t;

void mc_init(const mc_env_t *env);

/* Look at the USB ports now. Returns how many memory cards are connected. */
int mc_detect_cards(void);

/* One watcher pass: find the card, run queued manual backups, mirror changed saves. */
void mc_tick(void);
/* Sleep until the next pass is due or somebody asks for one. -1 when stopping. */
int mc_wait(void);
void mc_wake(void);
void mc_note_resume(void);

/* /api/mc/{status,card,console,backup,delete,card_init,label,settings,rescan}. Returns a malloc'd
 * JSON body, or NULL when the URL is not one of ours. */
char *mc_http(const char *method, const char *url, const char *body);

/* Copy a card save to the staging dir so the importer never touches the card.
 * id is "<uid>/<TITLE>/<save>", usb the card it is on (-1: the first card that
 * has it). On success staged holds <stage_dir>/<TITLE>/<save>. */
int mc_import_prepare(const char *id, int usb, int previous, char *staged, size_t staged_size,
                      char *title_id, size_t title_size, char *error, size_t error_size);
void mc_import_result(const char *id, const char *uid, int ok, const char *detail);
void mc_import_cleanup(void);

/* Path of a title's icon, 0 on success. */
int mc_icon_path(const char *title_id, char *out, size_t size);

/* Query-string helper shared with the HTTP glue. 1 when found. */
int mc_url_param(const char *url, const char *key, char *out, size_t size);

#endif
