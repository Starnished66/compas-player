#ifndef EMULATOR_REGISTRY_H
#define EMULATOR_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

/* Installed emulators (compas-emulators contract, protocol 1). The root is on
 * the vfat SD card, so file modes are not stored: every file reads as
 * executable and only access(X_OK) is checked. Each lives in
 * EMULATOR_ROOT/<id>/ with an emulator.json manifest and a static executable;
 * its games live in <SD>/Games/<games_dir>/, plus any it bundles in
 * EMULATOR_ROOT/<id>/games/<games_dir>/. */

#ifdef HOST_BUILD
#define EMULATOR_GAMES_SD_ROOT "./music"
#else
#define EMULATOR_GAMES_SD_ROOT "/data/mnt/sd_0"
#endif
/* On the SD card, like the plugins' ".plugins" folder. */
#define EMULATOR_ROOT EMULATOR_GAMES_SD_ROOT "/.emulators"

#define EMULATOR_PROTOCOL 1
#define EMULATOR_MAX_SYSTEMS 8
#define EMULATOR_MAX_EXTENSIONS 8
#define EMULATOR_MAX_INSTALLED 32 /* folders beyond this are ignored */

typedef struct {
    char id[16];
    char name[33];
    char games_dir[64];
    char extensions[EMULATOR_MAX_EXTENSIONS][8];
    int extension_count;
} emulator_system_t;

typedef struct {
    char id[33];
    char name[65];
    char version[32];
    char exec[64];
    char author[65];
    char license[65];
    char description[401];
    emulator_system_t systems[EMULATOR_MAX_SYSTEMS];
    int system_count;
    /* "free_player_memory": true; the player drops its rebuildable caches
     * before launching this emulator (for memory-hungry ones such as Doom). */
    bool free_player_memory;
} emulator_manifest_t;

typedef struct {
    char path[600];
    char label[256]; /* file name without extension */
} emulator_game_t;

/* The id rule shared with the store catalog: ^[a-z0-9][a-z0-9_-]{0,31}$ */
bool emulator_id_valid(const char * id);

/* Parses and validates a manifest. The error is a short English reason for
 * the log, not for the UI. */
bool emulator_manifest_parse(const char * json, size_t length, emulator_manifest_t * out,
                             char * error, size_t error_size);

/* Loads dir/emulator.json and also requires the manifest id to equal
 * expected_id and the exec file to be an executable regular file. */
bool emulator_manifest_load_dir(const char * dir, const char * expected_id, emulator_manifest_t * out,
                                char * error, size_t error_size);

/* Loads root/<folder>/emulator.json and also requires the manifest id to
 * equal the folder name and the exec file to be an executable regular file. */
bool emulator_manifest_load(const char * root, const char * folder, emulator_manifest_t * out,
                            char * error, size_t error_size);

/* Valid emulators below root, sorted by name. Invalid folders are skipped
 * with one log line each. Caller frees the array; NULL with *count 0 when
 * there are none. */
emulator_manifest_t * emulator_registry_scan(const char * root, int * count);

/* "<sd_root>/Games/<games_dir>" for a system. */
bool emulator_games_dir(char * out, size_t size, const char * sd_root, const emulator_system_t * system);

/* Regular files whose extension matches (case-insensitive) in the system's
 * games folder on the SD card and in the games the emulator bundles,
 * <emulator_dir>/games/<games_dir> (emulator_dir may be NULL), sorted by
 * label. A missing folder is the same as an empty one. Dot files are
 * skipped. Caller frees the array. */
emulator_game_t * emulator_registry_list_games(const char * sd_root, const char * emulator_dir,
                                               const emulator_system_t * system, int * count);

#endif
