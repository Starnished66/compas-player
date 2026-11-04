#ifndef PLUGIN_STORE_H
#define PLUGIN_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PLUGIN_STORE_MAX_RESULTS 600 /* 200 catalog plugins plus up to 400 installed ones no longer offered */

/* Check SD write access before the setup batch or its following scan. */
bool plugin_store_storage_writable(char * error, size_t error_size);

/* GitHub plugin catalog and SD-card installer. The catalog comes from
 * Starnished66/compas-plugins' latest release index over verified HTTPS;
 * assets come from that release tag and are checked against their declared
 * size and SHA-256 before installation.
 * The installed record is SD/.plugins/.store_installed, with P <id> <version>
 * lines followed by F<TAB><dest><TAB><sha256><TAB><keep:0|1> lines. Paths
 * cannot contain tabs or newlines, so the fields need no escaping. Replacing
 * a local file requires a confirmation result; callers retry with force=true.
 * Network requests and card I/O run on detached workers. UI calls only copy
 * status, result rows, or index metadata while holding the store mutex. */
typedef enum {
    PLUGIN_STORE_IDLE,
    PLUGIN_STORE_REFRESHING,
    PLUGIN_STORE_INSTALLING,
    PLUGIN_STORE_UPDATING,
    PLUGIN_STORE_UPDATING_ALL,
    PLUGIN_STORE_UNINSTALLING,
    PLUGIN_STORE_READY,
    PLUGIN_STORE_NEEDS_CONFIRM,
    PLUGIN_STORE_FAILED
} plugin_store_state_t;

typedef enum {
    PLUGIN_STORE_PLUGIN_AVAILABLE,
    PLUGIN_STORE_PLUGIN_INSTALLED,
    PLUGIN_STORE_PLUGIN_UPDATE,
    PLUGIN_STORE_PLUGIN_MANUAL,
    PLUGIN_STORE_PLUGIN_INCOMPATIBLE,
    PLUGIN_STORE_PLUGIN_REMOVED
} plugin_store_plugin_state_t;

/* Rows are about 170 bytes; details are fetched on demand. The UI can receive
 * at most PLUGIN_STORE_MAX_RESULTS rows in its caller-owned result buffer. */
typedef struct {
    char id[64];
    char name[65];
    char version[32];
    plugin_store_plugin_state_t state;
    bool incompatible;
    bool needs_confirmation;
    bool player_layout;
    bool theme_pack;
} plugin_store_result_t;

typedef struct {
    char description[401];
    char category[33];
    char author[65];
    uint32_t size;
} plugin_store_details_t;

typedef struct {
    plugin_store_state_t state;
    int percent;
    size_t result_count;
    uint64_t result_generation; /* Changes with the rows, including across reset. */
    bool changed;
    /* Sticky until plugin_store_reset() runs after a full UI teardown. */
    bool refresh_required;
    char current_plugin[65];
    char error[256];
} plugin_store_status_t;

bool plugin_store_refresh(void);
bool plugin_store_install(const char * id, bool force);
bool plugin_store_update(const char * id, bool force);
bool plugin_store_update_all(void);
bool plugin_store_uninstall(const char * id);
void plugin_store_get_status(plugin_store_status_t * out, plugin_store_result_t * results,
                             size_t result_capacity);
bool plugin_store_get_details(const char * id, plugin_store_details_t * out);
/* Resolves a selected catalog plugin to its installed primary XML layout ID. */
bool plugin_store_get_player_layout_id(const char * plugin_id, char * out, size_t size);
/* Marks installed Lua files whose records own player layouts or native
 * themes. Existing true flags are preserved. Either output may be NULL. */
void plugin_store_classify_loaders(const char * const * filenames, bool * layout_flags,
                                   bool * theme_flags, size_t count);
/* Non-blocking, verified layout previews. Missing/failed previews use a placeholder. */
#define PLUGIN_STORE_PREVIEW_CAPACITY 16
/* Requests visible/nearby IDs; offscreen previews may be evicted. */
bool plugin_store_prepare_previews(const char * const * ids, size_t count);
bool plugin_store_get_preview(const char * id, char * out, size_t size);
/* Returns the hash-verified JPEG source (not the LVGL RGB565 thumbnail). */
bool plugin_store_get_preview_jpeg(const char * id, char * out, size_t size);
typedef enum {
    PLUGIN_STORE_PREVIEW_UNKNOWN,
    PLUGIN_STORE_PREVIEW_NONE,
    PLUGIN_STORE_PREVIEW_READY,
    PLUGIN_STORE_PREVIEW_PENDING,
    PLUGIN_STORE_PREVIEW_STARTABLE,
    PLUGIN_STORE_PREVIEW_FAILED
} plugin_store_preview_state_t;
/* Bounded status only: never exposes an asset URL or cache path. */
plugin_store_preview_state_t plugin_store_preview_state(const char * id);
uint64_t plugin_store_preview_generation(void);
/* Stop pending preview transfers when the layout browser is left. */
void plugin_store_cancel_previews(void);
bool plugin_store_busy(void);
void plugin_store_reset(void);

#endif
