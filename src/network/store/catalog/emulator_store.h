#ifndef EMULATOR_STORE_H
#define EMULATOR_STORE_H

#include "../plugins/plugin_store.h" /* plugin_store_state_t and plugin_store_status_t are shared */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EMULATOR_STORE_MAX_EMULATORS 32
#define EMULATOR_STORE_MAX_FILES 16
#define EMULATOR_STORE_MAX_RESULTS 64 /* catalog entries plus installed ones no longer offered */

/* GitHub emulator catalog and installer (compas-emulators contract). Mirrors
 * the Plugin Store: the catalog is the latest release's index.json, every
 * file is checked against its declared size and SHA-256, and network and
 * flash I/O run on one detached worker whose state the UI polls. The shared
 * validation, hashing and download helpers live in store_common.c.
 *
 * Unlike plugins, an emulator is a self-contained folder in EMULATOR_ROOT:
 * an install downloads into a hidden staging folder, validates it as an
 * emulator and renames it over the old folder. The manifest on disk is the
 * installed record, so there is no separate record file. */

typedef enum {
    EMULATOR_STORE_AVAILABLE,
    EMULATOR_STORE_INSTALLED,
    EMULATOR_STORE_UPDATE,
    EMULATOR_STORE_INCOMPATIBLE
} emulator_store_entry_state_t;

typedef struct {
    char id[33];
    char name[65];
    char version[32];
    char systems[97]; /* display names joined with ", " */
    emulator_store_entry_state_t state;
} emulator_store_result_t;

typedef struct {
    char description[401];
    char author[65];
    char license[65];
    uint32_t size;
} emulator_store_details_t;

typedef struct {
    char asset[128];
    char dest[256];
    char sha256[65];
    uint32_t size;
    bool executable;
} emulator_store_file_t;

typedef struct {
    char id[33];
    char name[65];
    char version[32];
    uint32_t protocol;
    char systems[97];
    char description[401];
    char author[65];
    char license[65];
    uint32_t size;
    int file_count;
    emulator_store_file_t files[EMULATOR_STORE_MAX_FILES];
} emulator_store_entry_t;

bool emulator_store_refresh(void);
bool emulator_store_install(const char * id);
bool emulator_store_update(const char * id);
bool emulator_store_uninstall(const char * id);
void emulator_store_get_status(plugin_store_status_t * out, emulator_store_result_t * results,
                               size_t result_capacity);
bool emulator_store_get_details(const char * id, emulator_store_details_t * out);
bool emulator_store_busy(void);
/* The SD card is mounted and not exported over USB mass storage; installed
 * emulators are only listed and changed while this holds. */
bool emulator_store_sd_usable(void);
void emulator_store_reset(void);

/* Pure helpers and local filesystem hooks for the no-network host test.
 * Index parsing allocates exactly the validated entry count; caller frees. */
bool emulator_store_parse_index(const char * json, size_t length, emulator_store_entry_t ** out,
                                size_t * out_count, char tag[64], char * error, size_t error_size);
emulator_store_entry_state_t emulator_store_compute_state(const emulator_store_entry_t * entry,
                                                          const char * installed_version);
/* Validates the staged folder as an emulator with the given id and renames
 * it over root/<id>, restoring the old folder if the rename fails. */
bool emulator_store_commit_staged(const char * root, const char * stage, const char * id);
/* Removes root/<id> (renamed aside first, so it vanishes in one step). */
bool emulator_store_remove(const char * root, const char * id);

#endif
