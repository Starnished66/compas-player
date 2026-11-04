#ifndef PLUGIN_STORE_INTERNAL_H
#define PLUGIN_STORE_INTERNAL_H

/* Shared by the Plugin Store's source files only (plugin_store.c,
 * plugin_store_catalog.c, plugin_store_card.c, plugin_store_workers.c,
 * plugin_store_preview.c). Callers outside the store use plugin_store.h. */

#include "plugin_store.h"
#include "../../../core/persistence/storage_paths.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <strings.h>
#include <sys/types.h>

#ifdef HOST_BUILD
#define STORE_SD_ROOT "./music"
#else
#define STORE_SD_ROOT SD_DEVICE_MOUNT
#endif
#define STORE_INDEX_LIMIT (256u << 10)
#define STORE_FILE_LIMIT (1u << 20)
/* Installed-record entries. Larger than the catalog cap so retired plugins
 * still installed do not make the record unreadable after catalog turnover;
 * new installs stop at this limit instead. */
#define STORE_MAX_RECORD 400
#define STORE_PREVIEW_LIMIT 65536U

#define PLUGIN_STORE_MAX_PLUGINS 200
#define PLUGIN_STORE_MAX_FILES 32

typedef struct {
    char asset[128];
    char sha256[65];
    uint32_t size;
    uint32_t width;
    uint32_t height;
} plugin_store_preview_t;

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
    plugin_store_preview_t preview; /* Optional, independently fetched; never installed. */
    int file_count;
    plugin_store_file_t files[PLUGIN_STORE_MAX_FILES];
} plugin_store_plugin_t;

/* Which card is mounted: its device number, plus the card's own CID where
 * the kernel exposes it. A different card can mount on the same block
 * device, so the device number alone cannot detect a swap. */
typedef struct {
    dev_t dev;
    char cid[48];
} store_card_t;

typedef struct {
    plugin_store_plugin_t plugin;
    char tag[64];
    bool force;
    plugin_store_state_t state;
} store_job_t;

/* Shared state, defined in plugin_store.c. store_mutex guards all of it and
 * all preview cache state in plugin_store_preview.c. One store worker runs
 * at a time (store_worker_running). */
extern pthread_mutex_t store_mutex;
extern plugin_store_status_t store_status;
extern bool store_worker_running;
extern plugin_store_result_t * store_results;
extern plugin_store_plugin_t * last_index;
extern size_t last_index_count;
extern char last_tag[64];

/* The card a running worker started on; see plugin_store_card.c. */
extern const store_card_t * ps_commit_card;

/* plugin_store.c: status and shared helpers */
void ps_finish(plugin_store_state_t state, const char * error, bool changed);
void ps_progress_set(int percent, const char * name);
/* Outcome of reading the installed record: a damaged record exists but is
 * invalid; an error is any other failure (I/O, memory). */
typedef enum { PS_RECORD_OK, PS_RECORD_DAMAGED, PS_RECORD_ERROR } ps_record_status_t;
/* Translated message for a failed (not OK) record read. */
const char * ps_record_error(ps_record_status_t status);
bool ps_compute_results(void);
bool ps_finish_ready(bool changed);
const plugin_store_plugin_t * ps_index_find_locked(const char * id);

/* plugin_store_catalog.c: catalog parsing and classification */
bool ps_regex_id(const char * s);
bool plugin_store_dest_valid(const char * dest);
bool plugin_store_parse_index(const char * json, size_t length, plugin_store_plugin_t ** out,
                              size_t * out_count, char tag_out[64], char * error,
                              size_t error_size);
plugin_store_plugin_state_t plugin_store_compute_state(const plugin_store_plugin_t * p,
                                                       const char * installed,
                                                       const char * plugins_dir,
                                                       bool * incompatible);
void ps_plugin_kind(const plugin_store_plugin_t * plugin, bool * layout, bool * theme);
bool ps_preview_eligible(const plugin_store_plugin_t * plugin);
void store_preview_bounds(int * width, int * height);
bool ps_make_asset_url(char * out, size_t cap, const char * tag, const char * asset);
bool ps_layout_id_from_dest(const char * dest, char * out, size_t size);

/* plugin_store_card.c: card guards, installed record, commit */
bool ps_storage_path_writable(const char * path, char * error, size_t error_size);
bool ps_mkdir_tree(const char * root, const char * path);
bool ps_card_mounted(store_card_t * card);
bool ps_card_still(const store_card_t * card);
bool ps_path_for(char * out, size_t cap, const char * root, const char * dest);
void ps_stage_file(char * out, size_t size, const char * stage, size_t i, const char * ext);
bool ps_parent_dirs_safe(const char * root, const char * dest, bool create);
int ps_record_find(const plugin_store_plugin_t * rec, size_t rn, const char * id);
ps_record_status_t ps_read_record_alloc(plugin_store_plugin_t ** out, size_t * count);
bool ps_write_record_without(const char * root, const plugin_store_plugin_t * rec,
                             size_t rn, const char * remove_id);
bool ps_conflict_exists(const char * root, const plugin_store_plugin_t * p,
                        const plugin_store_plugin_t * rec, size_t rn);
bool ps_commit_staged(const char * root, const char * stage,
                      const plugin_store_plugin_t * p, bool force,
                      const plugin_store_plugin_t * rec, size_t rn,
                      bool * preserve_stage);

/* plugin_store_workers.c: detached worker bodies */
void * ps_refresh_worker(void * unused);
void * ps_install_worker(void * arg);
void * ps_uninstall_worker(void * arg);
void * ps_update_all_worker(void * unused);

/* plugin_store_preview.c */
void plugin_store_preview_invalidate_locked(void);

#endif
