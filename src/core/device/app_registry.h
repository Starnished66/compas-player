#ifndef APP_REGISTRY_H
#define APP_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

/* Installed external apps (app protocol 1). Each lives in APP_ROOT/<id>/ on
 * the SD card with an app.json manifest and a static executable, and keeps
 * its own files in APP_DATA_ROOT/<id>/. The card is vfat, so file modes are
 * not stored: every file reads as executable and only access(X_OK) is
 * checked. Apps run as root with the framebuffer and input devices while the
 * player waits (app_session.c); the first run needs the user's approval
 * (app_approval.c). */

#ifdef HOST_BUILD
#define APP_SD_ROOT "./music"
#else
#define APP_SD_ROOT "/data/mnt/sd_0"
#endif
#define APP_ROOT APP_SD_ROOT "/.apps"
#define APP_DATA_ROOT APP_SD_ROOT "/.apps-data"

#define APP_PROTOCOL 1
#define APP_MAX_OPENS 8
#define APP_MAX_BOARDS 8
#define APP_MAX_INSTALLED 32 /* folders beyond this are ignored */

typedef struct {
    char id[33];
    char name[65];
    char version[32];
    char exec[64];
    char icon[64]; /* PNG file name in the app folder, "" when none */
    char opens[APP_MAX_OPENS][8]; /* lower-case file extensions, no dot */
    int opens_count;
    char boards[APP_MAX_BOARDS][16]; /* empty: every board */
    int boards_count;
    /* "free_player_memory": true; the player drops its rebuildable caches
     * before launching the app. */
    bool free_player_memory;
} app_manifest_t;

/* The board this build runs on: "r1", "r3proii", "r3ii_2025" or "tempotec_v1". */
const char * app_board_id(void);

/* The id rule shared with the emulators: ^[a-z0-9][a-z0-9_-]{0,31}$ */
bool app_id_valid(const char * id);

/* Parses and validates a manifest. The error is a short English reason for
 * the log, not for the UI. */
bool app_manifest_parse(const char * json, size_t length, app_manifest_t * out, char * error, size_t error_size);

/* True when the manifest has no "boards" list or the list names the board. */
bool app_manifest_for_board(const app_manifest_t * manifest, const char * board);

/* True when the path's extension (case-insensitive) is in "opens". */
bool app_manifest_opens_path(const app_manifest_t * manifest, const char * path);

/* The rules for the file passed to an app (plugin.launch_app): a canonical
 * path below sd_root (no ".", ".." or empty parts, not the root itself), at
 * most PATH_MAX-1 bytes, an extension the app opens, and an existing regular
 * file. NULL when the file is fine, otherwise a short English reason (a
 * literal). */
const char * app_launch_file_check(const char * sd_root, const app_manifest_t * manifest, const char * file);

/* Loads dir/app.json and also requires the manifest id to equal expected_id,
 * the exec to be an executable regular file that is a static ELF program,
 * and the icon (if any) to be a regular file; an unusable icon is dropped. */
bool app_manifest_load_dir(const char * dir, const char * expected_id, app_manifest_t * out, char * error,
                           size_t error_size);

/* Valid apps below root that run on board, sorted by name. Invalid folders
 * and apps for other boards are skipped (the former with one log line each).
 * Caller frees the array; NULL with *count 0 when there are none. */
app_manifest_t * app_registry_scan(const char * root, const char * board, int * count);

/* One installed app by id (folder root/<id>); false when it is missing,
 * invalid or not for the board. */
bool app_registry_find(const char * root, const char * board, const char * id, app_manifest_t * out);

/* "<root>/<id>" and "<root>/<id>/<exec>" (false when it does not fit). */
bool app_dir_path(char * out, size_t size, const char * root, const app_manifest_t * manifest);
bool app_exec_path(char * out, size_t size, const char * root, const app_manifest_t * manifest);

/* Lower-case hex SHA-256 of the app's executable. */
bool app_exec_sha256(const char * root, const app_manifest_t * manifest, char hex[65]);

#endif
