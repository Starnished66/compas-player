#include "plugin_store_internal.h"
#include "store_common.h"
#include "../../../ui/text/i18n/i18n.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* The Plugin Store downloads its catalog and release assets from the
 * compas-plugins GitHub repository. Assets are verified by size and SHA-256
 * before they are installed. The installed record lives at
 * SD/.plugins/.store_installed and stores plugin versions plus each file's
 * destination, hash, and keep flag. Replacing a local file requires caller
 * confirmation. Network and card operations run on detached workers. */

pthread_mutex_t store_mutex = PTHREAD_MUTEX_INITIALIZER;
plugin_store_status_t store_status;
bool store_worker_running;
plugin_store_result_t * store_results;
plugin_store_plugin_t * last_index;
size_t last_index_count;
char last_tag[64];

/* Catalog entry by plugin id; the caller holds store_mutex. */
const plugin_store_plugin_t * ps_index_find_locked(const char * id) {
    for (size_t i = 0; i < last_index_count; i++) {
        if (strcmp(last_index[i].id, id) == 0) {
            return &last_index[i];
        }
    }
    return NULL;
}

static void reject_store_operation(const char * error) {
    pthread_mutex_lock(&store_mutex);
    if (!store_worker_running) {
        store_status.state = PLUGIN_STORE_FAILED;
        store_status.percent = 0;
        store_status.changed = false;
        snprintf(store_status.error, sizeof(store_status.error), "%s", error ? error : TR("Plugin storage is unavailable."));
    }
    pthread_mutex_unlock(&store_mutex);
}

void ps_finish(plugin_store_state_t state, const char * error, bool changed) {
    pthread_mutex_lock(&store_mutex);
    store_status.state = state;
    store_status.changed = changed;
    if (changed) store_status.refresh_required = true;
    if (error) {
        snprintf(store_status.error, sizeof(store_status.error), "%s", error);
    }
    store_worker_running = false;
    pthread_mutex_unlock(&store_mutex);
}

/* Shared message for an unreadable installed record. */
const char * ps_record_error(ps_record_status_t status) {
    if (status == PS_RECORD_DAMAGED) {
        return TR("The installed plugins record is damaged. Remove or repair .plugins/.store_installed on the SD card.");
    }
    return TR("Could not read installed plugins.");
}

/* One row per catalog entry, with the state against the installed record.
 * Returns the row count. */
static size_t results_from_index(plugin_store_result_t * result, const plugin_store_plugin_t * rec,
                                 size_t rn, size_t index_count) {
    size_t count = 0;
    for (size_t i = 0; i < index_count && count < PLUGIN_STORE_MAX_RESULTS; i++) {
        plugin_store_plugin_t plugin;
        pthread_mutex_lock(&store_mutex);
        bool available = last_index && i < last_index_count;
        if (available) {
            plugin = last_index[i];
        }
        pthread_mutex_unlock(&store_mutex);
        if (!available) {
            break;
        }

        int match = ps_record_find(rec, rn, plugin.id);
        plugin_store_result_t * row = &result[count++];
        snprintf(row->id, sizeof(row->id), "%s", plugin.id);
        snprintf(row->name, sizeof(row->name), "%s", plugin.name);
        snprintf(row->version, sizeof(row->version), "%s", plugin.version);
        ps_plugin_kind(&plugin, &row->player_layout, &row->theme_pack);
        row->state = plugin_store_compute_state(&plugin, match >= 0 ? rec[match].version : NULL,
                                                STORE_SD_ROOT "/.plugins", &row->incompatible);
    }
    return count;
}

/* Appends a REMOVED row for each installed entry missing from the catalog.
 * Returns the new row count. */
static size_t results_add_removed(plugin_store_result_t * result, size_t count,
                                  const plugin_store_plugin_t * rec, size_t rn) {
    for (size_t r = 0; r < rn && count < PLUGIN_STORE_MAX_RESULTS; r++) {
        pthread_mutex_lock(&store_mutex);
        bool found = ps_index_find_locked(rec[r].id) != NULL;
        pthread_mutex_unlock(&store_mutex);
        if (!found) {
            plugin_store_result_t * row = &result[count++];
            snprintf(row->id, sizeof(row->id), "%s", rec[r].id);
            snprintf(row->version, sizeof(row->version), "%s", rec[r].version);
            row->state = PLUGIN_STORE_PLUGIN_REMOVED;
            ps_plugin_kind(&rec[r], &row->player_layout, &row->theme_pack);
        }
    }
    return count;
}

/* Build the compact UI rows from the current index and installed record. */
bool ps_compute_results(void) {
    size_t index_count;
    pthread_mutex_lock(&store_mutex);
    index_count = last_index_count;
    pthread_mutex_unlock(&store_mutex);

    plugin_store_plugin_t * rec = NULL;
    size_t rn = 0;
    ps_record_status_t status = ps_read_record_alloc(&rec, &rn);
    if (status == PS_RECORD_ERROR) {
        return false;
    }
    if (status == PS_RECORD_DAMAGED) {
        /* Keep the catalog browsable and carry the warning on the READY state. */
        rec = NULL;
        rn = 0;
        pthread_mutex_lock(&store_mutex);
        snprintf(store_status.error, sizeof(store_status.error), "%s", ps_record_error(status));
        pthread_mutex_unlock(&store_mutex);
    }
    size_t capacity = index_count + rn;
    if (capacity > PLUGIN_STORE_MAX_RESULTS) {
        capacity = PLUGIN_STORE_MAX_RESULTS;
    }
    plugin_store_result_t * result = calloc(capacity ? capacity : 1, sizeof(*result));
    if (!result) {
        free(rec);
        return false;
    }
    size_t count = results_from_index(result, rec, rn, index_count);
    count = results_add_removed(result, count, rec, rn);
    pthread_mutex_lock(&store_mutex);
    free(store_results);
    store_results = result;
    store_status.result_count = count;
    store_status.result_generation++;
    pthread_mutex_unlock(&store_mutex);
    free(rec);
    return true;
}

/* Rebuild the rows and finish READY, or FAILED when they cannot be built so
 * stale rows are never reported as current. */
bool ps_finish_ready(bool changed) {
    if (ps_compute_results()) {
        ps_finish(PLUGIN_STORE_READY, NULL, changed);
        return true;
    }
    ps_finish(PLUGIN_STORE_FAILED, ps_record_error(PS_RECORD_ERROR), changed);
    return false;
}

void plugin_store_classify_loaders(const char * const * filenames, bool * layout_flags,
                                   bool * theme_flags, size_t count) {
    if (!filenames || (!layout_flags && !theme_flags) || !count) return;
    plugin_store_plugin_t * records = NULL;
    size_t record_count = 0;
    if (ps_read_record_alloc(&records, &record_count) != PS_RECORD_OK) return;

    for (size_t r = 0; r < record_count; r++) {
        const plugin_store_plugin_t * record = &records[r];
        bool is_layout, is_theme;
        ps_plugin_kind(record, &is_layout, &is_theme);
        is_layout = is_layout && layout_flags;
        is_theme = is_theme && theme_flags;
        if (!is_layout && !is_theme) continue;
        for (int f = 0; f < record->file_count; f++) {
            const char * dest = record->files[f].dest;
            if (strncmp(dest, ".plugins/", 9) != 0 || strchr(dest + 9, '/')) continue;
            size_t len = strlen(dest + 9);
            if (len <= 4 || strcasecmp(dest + 9 + len - 4, ".lua") != 0) continue;
            for (size_t i = 0; i < count; i++) {
                if (filenames[i] && strcmp(filenames[i], dest + 9) == 0) {
                    if (is_layout) layout_flags[i] = true;
                    if (is_theme) theme_flags[i] = true;
                }
            }
        }
    }
    free(records);
}

void ps_progress_set(int percent, const char * name) {
    pthread_mutex_lock(&store_mutex);
    store_status.percent = percent;
    if (name) {
        snprintf(store_status.current_plugin, sizeof(store_status.current_plugin), "%.64s", name);
    }
    pthread_mutex_unlock(&store_mutex);
}

/* Claims the worker slot and clears the previous operation's status; the
 * caller holds store_mutex and has checked that no worker runs. */
static void status_begin_locked(plugin_store_state_t state) {
    store_worker_running = true;
    store_status.state = state;
    store_status.percent = 0;
    store_status.changed = false;
    store_status.error[0] = 0;
    store_status.current_plugin[0] = 0;
}

static bool ps_begin(plugin_store_state_t state) {
    pthread_mutex_lock(&store_mutex);
    if (store_worker_running) {
        pthread_mutex_unlock(&store_mutex);
        return false;
    }
    status_begin_locked(state);
    pthread_mutex_unlock(&store_mutex);
    return true;
}

/* Entry-point check: a mounted, writable card, reported on the store status. */
static bool store_precheck(const char * no_card_message) {
    char guard_error[160];
    if (!ps_card_mounted(NULL)) {
        reject_store_operation(no_card_message);
        return false;
    }
    if (!ps_storage_path_writable(STORE_SD_ROOT, guard_error, sizeof(guard_error))) {
        reject_store_operation(guard_error);
        return false;
    }
    return true;
}

/* Snapshot the selected catalog entry and claim the worker slot atomically. */
static bool start_install(const char * id, bool force, plugin_store_state_t state) {
    if (!id || !ps_regex_id(id)) {
        return false;
    }
    if (!store_precheck(TR("Insert an SD card to change plugins."))) {
        return false;
    }
    store_job_t * job = calloc(1, sizeof(*job));
    if (!job) {
        return false;
    }
    bool found = false;
    pthread_mutex_lock(&store_mutex);
    if (!store_worker_running && last_index) {
        const plugin_store_plugin_t * entry = ps_index_find_locked(id);
        if (entry) {
            job->plugin = *entry;
            snprintf(job->tag, sizeof(job->tag), "%s", last_tag);
            status_begin_locked(state);
            found = true;
        }
    }
    pthread_mutex_unlock(&store_mutex);
    if (!found) {
        free(job);
        return false;
    }
    job->force = force;
    job->state = state;
    if (!store_start_thread(ps_install_worker, job)) {
        free(job);
        ps_finish(PLUGIN_STORE_FAILED, TR("Could not start the plugin operation."), false);
        return false;
    }
    return true;
}
bool plugin_store_refresh(void) {
    if (!ps_begin(PLUGIN_STORE_REFRESHING)) {
        return false;
    }
    if (!store_start_thread(ps_refresh_worker, NULL)) {
        ps_finish(PLUGIN_STORE_FAILED, TR("Could not start the plugin refresh."), false);
        return false;
    }
    return true;
}

bool plugin_store_install(const char * id, bool force) {
    return start_install(id, force, PLUGIN_STORE_INSTALLING);
}

bool plugin_store_update(const char * id, bool force) {
    return start_install(id, force, PLUGIN_STORE_UPDATING);
}

bool plugin_store_uninstall(const char * id) {
    if (!id || !ps_regex_id(id)) return false;
    if (!store_precheck(TR("Insert an SD card to remove plugins."))) {
        return false;
    }
    if (!ps_begin(PLUGIN_STORE_UNINSTALLING)) {
        return false;
    }
    char * copy = strdup(id);
    if (!copy) {
        ps_finish(PLUGIN_STORE_FAILED, TR("Could not start the plugin operation."), false);
        return false;
    }
    if (!store_start_thread(ps_uninstall_worker, copy)) {
        free(copy);
        ps_finish(PLUGIN_STORE_FAILED, TR("Could not start the plugin operation."), false);
        return false;
    }
    return true;
}

bool plugin_store_update_all(void) {
    if (!store_precheck(TR("Insert an SD card to update plugins."))) {
        return false;
    }
    pthread_mutex_lock(&store_mutex);
    bool have_index = last_index != NULL;
    pthread_mutex_unlock(&store_mutex);
    if (!have_index) {
        return false;
    }
    if (!ps_begin(PLUGIN_STORE_UPDATING_ALL)) {
        return false;
    }
    if (!store_start_thread(ps_update_all_worker, NULL)) {
        ps_finish(PLUGIN_STORE_FAILED, TR("Could not start the plugin update."), false);
        return false;
    }
    return true;
}
void plugin_store_get_status(plugin_store_status_t * out, plugin_store_result_t * results,
                             size_t cap) {
    if (!out) {
        return;
    }
    pthread_mutex_lock(&store_mutex);
    *out=store_status;
    size_t n=store_status.result_count;
    if (n > cap) {
        n = cap;
    }
    if (results && store_results && n) {
        memcpy(results, store_results, n * sizeof(*results));
    }
    pthread_mutex_unlock(&store_mutex);
}

/* True when the catalog file is recorded as installed and the file on the card
 * is a regular file under root_real whose size and hash still match. */
static bool layout_file_intact(const plugin_store_plugin_t * installed, const plugin_store_file_t * file,
                               const char * root_real) {
    char path[PATH_MAX], file_real[PATH_MAX], sha[65];
    uint64_t bytes = 0;
    bool recorded = false;
    for (int r = 0; r < installed->file_count; ++r)
        if (strcmp(installed->files[r].dest, file->dest) == 0 &&
            strcmp(installed->files[r].sha256, file->sha256) == 0) recorded = true;
    if (!recorded || !ps_path_for(path, sizeof(path), STORE_SD_ROOT, file->dest) ||
        !ps_parent_dirs_safe(STORE_SD_ROOT, file->dest, false)) return false;
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || !realpath(path, file_real)) return false;
    size_t root_len = strlen(root_real);
    if (strncmp(file_real, root_real, root_len) != 0 || file_real[root_len] != '/') return false;
    return store_hash_file(path, &bytes, sha) && bytes == file->size && strcmp(sha, file->sha256) == 0;
}

/* The first intact layout file of the installed plugin; false when none. */
static bool installed_layout_id(const plugin_store_plugin_t * catalog_plugin,
                                const plugin_store_plugin_t * installed, char * out, size_t size) {
    char root_real[PATH_MAX];
    if (realpath(STORE_SD_ROOT, root_real) == NULL) return false;
    for (int f = 0; f < catalog_plugin->file_count; ++f) {
        char layout_id[64];
        const plugin_store_file_t * file = &catalog_plugin->files[f];
        if (!ps_layout_id_from_dest(file->dest, layout_id, sizeof(layout_id))) continue;
        if (!layout_file_intact(installed, file, root_real)) continue;
        if (snprintf(out, size, "%s", layout_id) >= (int) size) { out[0] = '\0'; return false; }
        return true;
    }
    return false;
}

bool plugin_store_get_player_layout_id(const char * plugin_id, char * out, size_t size) {
    if (!out || !size) return false;
    out[0] = '\0';
    if (!plugin_id || !ps_regex_id(plugin_id)) return false;
    plugin_store_plugin_t catalog_plugin;
    bool found = false;
    pthread_mutex_lock(&store_mutex);
    const plugin_store_plugin_t * entry = ps_index_find_locked(plugin_id);
    if (entry) {
        catalog_plugin = *entry;
        found = true;
    }
    pthread_mutex_unlock(&store_mutex);
    if (!found) return false;

    plugin_store_plugin_t * records = NULL;
    size_t record_count = 0;
    if (ps_read_record_alloc(&records, &record_count) != PS_RECORD_OK) return false;
    int installed_index = ps_record_find(records, record_count, plugin_id);
    bool valid = installed_index >= 0 &&
                 installed_layout_id(&catalog_plugin, &records[installed_index], out, size);
    free(records);
    return valid;
}

bool plugin_store_get_details(const char * id, plugin_store_details_t * out) {
    if (!id || !out) {
        return false;
    }
    bool found = false;
    pthread_mutex_lock(&store_mutex);
    const plugin_store_plugin_t * entry = ps_index_find_locked(id);
    if (entry) {
        snprintf(out->description, sizeof(out->description), "%s", entry->description);
        snprintf(out->category, sizeof(out->category), "%s", entry->category);
        snprintf(out->author, sizeof(out->author), "%s", entry->author);
        out->size = entry->size;
        found = true;
    }
    pthread_mutex_unlock(&store_mutex);
    return found;
}
bool plugin_store_busy(void) {
    pthread_mutex_lock(&store_mutex);
    bool busy = store_worker_running;
    pthread_mutex_unlock(&store_mutex);
    return busy;
}

void plugin_store_reset(void) {
    pthread_mutex_lock(&store_mutex);
    if (!store_worker_running) {
        uint64_t generation = store_status.result_generation + 1;
        memset(&store_status, 0, sizeof(store_status));
        store_status.result_generation = generation;
        free(store_results);
        store_results = NULL;
        free(last_index);
        last_index = NULL;
        last_index_count = 0;
        plugin_store_preview_invalidate_locked();
        last_tag[0] = 0;
    }
    pthread_mutex_unlock(&store_mutex);
}
