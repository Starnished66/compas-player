#ifndef PLUGIN_STORE_H
#define PLUGIN_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PLUGIN_STORE_MAX_PLUGINS 200
#define PLUGIN_STORE_MAX_FILES 32
#define PLUGIN_STORE_MAX_RESULTS 600 /* 200 catalog plugins plus up to 400 installed ones no longer offered */

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
    bool changed;
    char current_plugin[65];
    char error[160];
} plugin_store_status_t;

typedef struct {
    char asset[128];
    char dest[256];
    char sha256[65];
    uint32_t size;
    bool keep;
} plugin_store_file_t;

typedef struct {
    char id[64];
    char name[65];
    char version[32];
    uint32_t api_min;
    char description[401];
    char category[33];
    char author[65];
    uint32_t size;
    int file_count;
    plugin_store_file_t files[PLUGIN_STORE_MAX_FILES];
} plugin_store_plugin_t;

bool plugin_store_refresh(void);
bool plugin_store_install(const char * id, bool force);
bool plugin_store_update(const char * id, bool force);
bool plugin_store_update_all(void);
bool plugin_store_uninstall(const char * id);
void plugin_store_get_status(plugin_store_status_t * out, plugin_store_result_t * results,
                             size_t result_capacity);
bool plugin_store_get_details(const char * id, plugin_store_details_t * out);
bool plugin_store_busy(void);
void plugin_store_reset(void);

/* Pure helpers and local filesystem hooks for the no-network host selftest.
 * Index parsing allocates exactly the validated plugin count; caller frees it. */
bool plugin_store_parse_index(const char * json, size_t length, plugin_store_plugin_t ** out,
                              size_t * out_count, char tag[64], char * error, size_t error_size);
bool plugin_store_dest_valid(const char * dest);
int plugin_store_version_compare(const char * a, const char * b);
/* Evaluate an index entry against record metadata and a .plugins directory. */
plugin_store_plugin_state_t plugin_store_compute_state(const plugin_store_plugin_t * plugin,
                                                       const char * installed_version,
                                                       const char * plugins_dir,
                                                       bool * incompatible);
/* Testable record operations. Root is the SD card root, not .compas. */
bool plugin_store_record_write(const char * root, const plugin_store_plugin_t * plugins,
                               const int * plugin_indices, size_t count);
size_t plugin_store_record_read(const char * root, plugin_store_plugin_t * plugins,
                                char (*versions)[32], size_t capacity);
/* Installs verified local staged files named <stage_dir>/<i>.part. */
bool plugin_store_commit_local(const char * root, const char * stage_dir,
                               const plugin_store_plugin_t * plugin, bool force,
                               const plugin_store_plugin_t * record_plugins,
                               const char (*record_versions)[32], size_t record_count);

#endif
