#include "emulator_store.h"
#include "../../../core/system/runtime/board_config.h"
#include "../../../core/device/emulator_registry.h"
#include "../plugins/store_common.h"
#include "../../../ui/text/i18n/i18n.h"
#include "../../../ui/library/browse/gui_library.h" /* sd_card_root_is_mounted() */
#include "../../../hardware/usb/usb_mode_control.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

/* See emulator_store.h. The catalog is the compas-emulators latest release
 * index.json; its assets come from the release tag the index names. */

#ifndef EMULATOR_STORE_INDEX_URL
#define EMULATOR_STORE_INDEX_URL "https://github.com/Starnished66/compas-emulators/releases/latest/download/index.json"
#endif
#define STORE_INDEX_LIMIT (64u << 10)
#define STORE_FILE_LIMIT (64u << 20) /* bundled game data such as Freedoom's WADs */
#define STORE_MARGIN (4u << 20)
#define STAGE_PREFIX ".stage-"
#define ASIDE_PREFIX ".old-"

static pthread_mutex_t store_mutex = PTHREAD_MUTEX_INITIALIZER;
static plugin_store_status_t store_status;
static bool store_worker_running;
static emulator_store_result_t * store_results;
static emulator_store_entry_t * last_index;
static size_t last_index_count;
static char last_tag[64];

static void join_systems(char * out, size_t size, const char * name, bool first) {
    size_t len = strlen(out);
    if (len < size) snprintf(out + len, size - len, "%s%s", first ? "" : ", ", name);
}

bool emulator_store_parse_index(const char * json, size_t length, emulator_store_entry_t ** out,
                                size_t * out_count, char tag_out[64], char * error, size_t error_size) {
    if (out) *out = NULL;
    if (out_count) *out_count = 0;
    if (tag_out) tag_out[0] = '\0';
    store_error_text(error, error_size, TR("Unexpected reply from GitHub."));
    if (!json || !out || !tag_out || length > STORE_INDEX_LIMIT) return false;

    cJSON * root = cJSON_ParseWithLength(json, length);
    emulator_store_entry_t * parsed = NULL;
    uint32_t schema;
    const char * tag = store_json_string(root, "tag");
    const cJSON * emulators = store_json_field(root, "emulators");
    if (!cJSON_IsObject(root) || !store_json_uint(root, "schema", 1, 1, &schema) || !tag ||
        !store_token_valid(tag, 63, true) || !cJSON_IsArray(emulators) ||
        cJSON_GetArraySize(emulators) > EMULATOR_STORE_MAX_EMULATORS) {
        goto fail;
    }

    size_t count = (size_t) cJSON_GetArraySize(emulators);
    parsed = calloc(count ? count : 1, sizeof(*parsed));
    if (!parsed) goto fail;
    for (int i = 0; i < (int) count; i++) {
        const cJSON * e = cJSON_GetArrayItem(emulators, i);
        emulator_store_entry_t * d = &parsed[i];
        const char * id = store_json_string(e, "id");
        const char * name = store_json_string(e, "name");
        const char * ver = store_json_string(e, "version");
        const char * desc = store_json_string(e, "description");
        const char * author = store_json_string(e, "author");
        const char * license = store_json_string(e, "license");
        if (!author) author = "";
        if (!license) license = "";
        const cJSON * systems = store_json_field(e, "systems");
        if (!cJSON_IsObject(e) || !emulator_id_valid(id) || !store_safe_text(name, 64, false) ||
            !store_version_valid(ver) || !store_safe_text(desc, 400, true) ||
            !store_safe_text(author, 64, true) || !store_safe_text(license, 64, true) ||
            !store_json_uint(e, "protocol", 1, UINT32_MAX, &d->protocol) ||
            !store_json_uint(e, "size", 0, UINT32_MAX, &d->size) || !cJSON_IsArray(systems) ||
            cJSON_GetArraySize(systems) < 1 || cJSON_GetArraySize(systems) > EMULATOR_MAX_SYSTEMS) {
            goto fail;
        }
        snprintf(d->id, sizeof(d->id), "%s", id);
        snprintf(d->name, sizeof(d->name), "%s", name);
        snprintf(d->version, sizeof(d->version), "%s", ver);
        snprintf(d->description, sizeof(d->description), "%s", desc);
        snprintf(d->author, sizeof(d->author), "%s", author);
        snprintf(d->license, sizeof(d->license), "%s", license);
        for (int s = 0; s < cJSON_GetArraySize(systems); s++) {
            const cJSON * system = cJSON_GetArrayItem(systems, s);
            if (!cJSON_IsString(system) || !store_safe_text(system->valuestring, 32, false)) goto fail;
            join_systems(d->systems, sizeof(d->systems), system->valuestring, s == 0);
        }

        const cJSON * files = store_json_field(e, "files");
        int file_count = cJSON_GetArraySize(files);
        if (!cJSON_IsArray(files) || file_count < 1 || file_count > EMULATOR_STORE_MAX_FILES) goto fail;
        d->file_count = file_count;
        char prefix[40];
        snprintf(prefix, sizeof(prefix), "%s/", d->id);
        size_t prefix_len = strlen(prefix);
        bool has_manifest = false, has_executable = false;
        uint64_t total = 0;
        for (int j = 0; j < file_count; j++) {
            const cJSON * f = cJSON_GetArrayItem(files, j);
            emulator_store_file_t * df = &d->files[j];
            const char * asset = store_json_string(f, "asset");
            const char * dest = store_json_string(f, "dest");
            const char * sha = store_json_string(f, "sha256");
            const cJSON * executable = store_json_field(f, "executable");
            uint32_t size;
            /* dest stays inside this emulator's own folder. */
            if (!cJSON_IsObject(f) || !store_token_valid(asset, 127, true) || !store_dest_path_valid(dest) ||
                strncmp(dest, prefix, prefix_len) != 0 || !dest[prefix_len] || !store_sha_valid(sha) ||
                !store_json_uint(f, "size", 1, STORE_FILE_LIMIT, &size) ||
                (executable && !cJSON_IsBool(executable))) {
                goto fail;
            }
            snprintf(df->asset, sizeof(df->asset), "%s", asset);
            snprintf(df->dest, sizeof(df->dest), "%s", dest);
            snprintf(df->sha256, sizeof(df->sha256), "%s", sha);
            df->size = size;
            df->executable = cJSON_IsTrue(executable);
            total += size;
            has_manifest = has_manifest || strcmp(dest + prefix_len, "emulator.json") == 0;
            has_executable = has_executable || df->executable;
            for (int k = 0; k < j; k++) {
                if (store_dests_collide(df->dest, d->files[k].dest)) goto fail;
            }
        }
        if (!has_manifest || !has_executable || total != d->size) goto fail;
        for (int x = 0; x < i; x++) {
            if (strcmp(parsed[x].id, d->id) == 0) goto fail;
        }
    }
    if (out_count) *out_count = count;
    snprintf(tag_out, 64, "%s", tag);
    *out = parsed;
    cJSON_Delete(root);
    store_error_text(error, error_size, "");
    return true;
fail:
    free(parsed);
    cJSON_Delete(root);
    return false;
}

emulator_store_entry_state_t emulator_store_compute_state(const emulator_store_entry_t * entry,
                                                          const char * installed_version) {
    bool incompatible = entry->protocol != EMULATOR_PROTOCOL;
    if (installed_version && *installed_version) {
        if (store_version_compare(entry->version, installed_version) <= 0 || incompatible) {
            return EMULATOR_STORE_INSTALLED;
        }
        return EMULATOR_STORE_UPDATE;
    }
    return incompatible ? EMULATOR_STORE_INCOMPATIBLE : EMULATOR_STORE_AVAILABLE;
}

/* Deletes a folder tree without following links. */
static bool remove_tree(const char * path, int depth) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
    if (depth > 8) return false;
    DIR * dir = opendir(path);
    if (!dir) return false;
    bool ok = true;
    struct dirent * de;
    while (ok && (de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        ok = snprintf(child, sizeof(child), "%s/%s", path, de->d_name) < (int) sizeof(child) &&
             remove_tree(child, depth + 1);
    }
    closedir(dir);
    return ok && rmdir(path) == 0;
}

static void aside_path(char * out, size_t size, const char * root, const char * id) {
    static unsigned counter;
    snprintf(out, size, "%s/" ASIDE_PREFIX "%.32s.%lu.%u", root, id, (unsigned long) time(NULL), ++counter);
}

bool emulator_store_commit_staged(const char * root, const char * stage, const char * id) {
    emulator_manifest_t manifest;
    char error[64];
    if (!emulator_id_valid(id) || !emulator_manifest_load_dir(stage, id, &manifest, error, sizeof(error))) {
        fprintf(stderr, "[emulators] staged %s rejected: %s\n", id ? id : "?", error);
        return false;
    }
    char final_path[PATH_MAX], aside[PATH_MAX];
    struct stat st;
    if (snprintf(final_path, sizeof(final_path), "%s/%s", root, id) >= (int) sizeof(final_path)) return false;
    bool had_old = lstat(final_path, &st) == 0;
    aside_path(aside, sizeof(aside), root, id);
    if (had_old && rename(final_path, aside) != 0) return false;
    if (rename(stage, final_path) != 0) {
        if (had_old) (void) rename(aside, final_path);
        return false;
    }
    if (had_old) (void) remove_tree(aside, 0);
    sync();
    return true;
}

bool emulator_store_remove(const char * root, const char * id) {
    char final_path[PATH_MAX], aside[PATH_MAX];
    struct stat st;
    if (!emulator_id_valid(id) || snprintf(final_path, sizeof(final_path), "%s/%s", root, id) >= (int) sizeof(final_path) ||
        lstat(final_path, &st) != 0) {
        return false;
    }
    aside_path(aside, sizeof(aside), root, id);
    if (rename(final_path, aside) != 0) return false;
    bool ok = remove_tree(aside, 0);
    sync();
    return ok;
}

/* Staging and set-aside folders a crashed or failed operation left behind. */
static void remove_leftovers(const char * root) {
    DIR * dir = opendir(root);
    if (!dir) return;
    struct dirent * de;
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, STAGE_PREFIX, strlen(STAGE_PREFIX)) != 0 &&
            strncmp(de->d_name, ASIDE_PREFIX, strlen(ASIDE_PREFIX)) != 0) continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", root, de->d_name) < (int) sizeof(path)) (void) remove_tree(path, 0);
    }
    closedir(dir);
}

bool emulator_store_sd_usable(void) {
#ifndef HOST_BUILD
    if (!sd_card_root_is_mounted()) return false;
#endif
    return !usb_mode_control_storage_write_blocked();
}

/* The emulator folder must exist, be writable and have room for the install. */
static bool root_ready(const char * root, uint64_t need, char * error, size_t error_size) {
    struct stat st;
    if (!emulator_store_sd_usable()) {
        store_error_text(error, error_size, TR("Insert an SD card to change emulators."));
        return false;
    }
    if (!plugin_store_storage_writable(error, error_size)) return false;
    if ((mkdir(root, 0755) != 0 && errno != EEXIST) || stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        store_error_text(error, error_size, TR("Cannot create the emulator folder."));
        return false;
    }
    struct statvfs fs;
    if (statvfs(root, &fs) != 0 || (fs.f_flag & ST_RDONLY) || access(root, W_OK) != 0) {
        store_error_text(error, error_size, TR("The emulator folder is not writable."));
        return false;
    }
    if ((uint64_t) fs.f_bavail * fs.f_frsize < need + STORE_MARGIN) {
        store_error_text(error, error_size, TR("Not enough free space on the player."));
        return false;
    }
    return true;
}

static void finish(plugin_store_state_t state, const char * error, bool changed) {
    pthread_mutex_lock(&store_mutex);
    store_status.state = state;
    store_status.changed = changed;
    if (error) snprintf(store_status.error, sizeof(store_status.error), "%s", error);
    store_worker_running = false;
    pthread_mutex_unlock(&store_mutex);
}

static bool begin(plugin_store_state_t state) {
    pthread_mutex_lock(&store_mutex);
    if (store_worker_running) {
        pthread_mutex_unlock(&store_mutex);
        return false;
    }
    store_worker_running = true;
    memset(&store_status, 0, sizeof(store_status));
    store_status.state = state;
    pthread_mutex_unlock(&store_mutex);
    return true;
}

static void compute_results(void) {
    int installed_count;
    emulator_manifest_t * installed = emulator_registry_scan(EMULATOR_ROOT, &installed_count);
    pthread_mutex_lock(&store_mutex);
    size_t capacity = last_index_count + (size_t) installed_count;
    if (capacity > EMULATOR_STORE_MAX_RESULTS) capacity = EMULATOR_STORE_MAX_RESULTS;
    emulator_store_result_t * rows = calloc(capacity ? capacity : 1, sizeof(*rows));
    size_t count = 0;
    for (size_t i = 0; rows && i < last_index_count && count < capacity; i++) {
        const emulator_store_entry_t * entry = &last_index[i];
        const char * version = NULL;
        for (int r = 0; r < installed_count; r++) {
            if (strcmp(installed[r].id, entry->id) == 0) version = installed[r].version;
        }
        emulator_store_result_t * row = &rows[count++];
        snprintf(row->id, sizeof(row->id), "%s", entry->id);
        snprintf(row->name, sizeof(row->name), "%s", entry->name);
        snprintf(row->version, sizeof(row->version), "%s", entry->version);
        snprintf(row->systems, sizeof(row->systems), "%s", entry->systems);
        row->state = emulator_store_compute_state(entry, version);
    }
    /* Installed emulators the catalog no longer lists stay removable. */
    for (int r = 0; rows && r < installed_count && count < capacity; r++) {
        bool listed = false;
        for (size_t i = 0; i < last_index_count; i++) listed = listed || strcmp(last_index[i].id, installed[r].id) == 0;
        if (listed) continue;
        emulator_store_result_t * row = &rows[count++];
        snprintf(row->id, sizeof(row->id), "%s", installed[r].id);
        snprintf(row->name, sizeof(row->name), "%s", installed[r].name);
        snprintf(row->version, sizeof(row->version), "%s", installed[r].version);
        for (int s = 0; s < installed[r].system_count; s++)
            join_systems(row->systems, sizeof(row->systems), installed[r].systems[s].name, s == 0);
        row->state = EMULATOR_STORE_INSTALLED;
    }
    free(store_results);
    store_results = rows;
    store_status.result_count = rows ? count : 0;
    pthread_mutex_unlock(&store_mutex);
    free(installed);
}

static void * refresh_worker(void * unused) {
    (void) unused;
    http_response_t response;
    const char * fetch_error;
    if (!store_fetch_index(EMULATOR_STORE_INDEX_URL, STORE_INDEX_LIMIT,
                           TR("Could not read the emulator list from GitHub."), &response, &fetch_error)) {
        finish(PLUGIN_STORE_FAILED, fetch_error, false);
        return NULL;
    }
    emulator_store_entry_t * parsed = NULL;
    size_t count = 0;
    char tag[64], err[128];
    bool valid = emulator_store_parse_index((char *) response.body, response.body_len, &parsed, &count, tag,
                                            err, sizeof(err));
    http_response_free(&response);
    if (!valid) {
        finish(PLUGIN_STORE_FAILED, err, false);
        return NULL;
    }
    pthread_mutex_lock(&store_mutex);
    free(last_index);
    last_index = parsed;
    last_index_count = count;
    snprintf(last_tag, sizeof(last_tag), "%s", tag);
    pthread_mutex_unlock(&store_mutex);
    compute_results();
    finish(PLUGIN_STORE_READY, NULL, false);
    return NULL;
}

typedef struct {
    emulator_store_entry_t entry;
    char tag[64];
} install_job_t;

typedef struct {
    uint64_t done_before, total;
} progress_t;

static bool download_progress(uint64_t downloaded, uint64_t file_total, void * context) {
    (void) file_total;
    const progress_t * progress = context;
    int percent = progress->total ? (int) ((progress->done_before + downloaded) * 100 / progress->total) : 0;
    pthread_mutex_lock(&store_mutex);
    store_status.percent = percent > 100 ? 100 : percent;
    pthread_mutex_unlock(&store_mutex);
    return true;
}

/* Creates the folders above stage/rel (stage itself exists). */
static bool make_parents(const char * stage, const char * rel) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", stage, rel) >= (int) sizeof(path)) return false;
    for (char * p = path + strlen(stage) + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(path, 0755) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return true;
}

static bool download_entry(const emulator_store_entry_t * entry, const char * tag, const char * stage) {
    progress_t progress = { 0, entry->size };
    for (int i = 0; i < entry->file_count; i++) {
        const emulator_store_file_t * f = &entry->files[i];
        const char * rel = f->dest + strlen(entry->id) + 1; /* parse guaranteed "<id>/" */
        char path[PATH_MAX], url[512];
        if (!make_parents(stage, rel) || snprintf(path, sizeof(path), "%s/%s", stage, rel) >= (int) sizeof(path) ||
            snprintf(url, sizeof(url), "https://github.com/Starnished66/compas-emulators/releases/download/%s/%s",
                     tag, f->asset) >= (int) sizeof(url) ||
            !store_download_verified(url, path, f->size, f->sha256, STORE_FILE_LIMIT, download_progress, &progress)) {
            return false;
        }
        /* vfat keeps no modes (everything reads as executable), so a failed
         * chmod is not an error. */
        (void) chmod(path, f->executable ? 0755 : 0644);
        progress.done_before += f->size;
    }
    return true;
}

static void * install_worker(void * arg) {
    install_job_t * job = arg;
    const emulator_store_entry_t * entry = &job->entry;
    char error[160];
    pthread_mutex_lock(&store_mutex);
    snprintf(store_status.current_plugin, sizeof(store_status.current_plugin), "%.64s", entry->name);
    pthread_mutex_unlock(&store_mutex);

    plugin_store_state_t final_state = PLUGIN_STORE_FAILED;
    const char * final_error = NULL;
    bool changed = false;
    bool leased = false;
    if (entry->protocol != EMULATOR_PROTOCOL) {
        final_error = TR("This emulator needs a newer player version.");
    } else if (!(leased = usb_mode_control_storage_write_begin())) {
        final_error = TR("Disconnect USB storage from the host before changing emulators.");
    } else if (!root_ready(EMULATOR_ROOT, entry->size, error, sizeof(error))) {
        final_error = error;
    } else {
        /* Keep the SD write lease through staging, downloads, replacement,
         * cleanup and the final scan so USB mass storage cannot expose the
         * card while this worker is changing it. */
        remove_leftovers(EMULATOR_ROOT);
        static unsigned stage_counter;
        char stage[PATH_MAX];
        snprintf(stage, sizeof(stage), "%s/" STAGE_PREFIX "%.32s.%lu.%u", EMULATOR_ROOT, entry->id,
                 (unsigned long) time(NULL), ++stage_counter);
        if (mkdir(stage, 0755) != 0) {
            final_error = TR("Could not install the emulator.");
        } else if (!download_entry(entry, job->tag, stage)) {
            (void) remove_tree(stage, 0);
            final_error = TR("The emulator download failed verification. Try again.");
        } else if (!emulator_store_commit_staged(EMULATOR_ROOT, stage, entry->id)) {
            (void) remove_tree(stage, 0);
            final_error = TR("Could not install the emulator.");
        } else {
            compute_results();
            final_state = PLUGIN_STORE_READY;
            changed = true;
        }
    }
    if (leased) usb_mode_control_storage_write_end();
    finish(final_state, final_error, changed);
    free(job);
    return NULL;
}

/* Snapshots the catalog entry and claims the worker slot atomically. */
static bool start_install(const char * id, plugin_store_state_t state) {
#if !BOARD_SUPPORTS_EMULATORS
    (void) id;
    (void) state;
    return false;
#endif
    if (!emulator_id_valid(id)) return false;
    install_job_t * job = calloc(1, sizeof(*job));
    if (!job) return false;
    bool found = false;
    pthread_mutex_lock(&store_mutex);
    if (!store_worker_running && last_index) {
        for (size_t i = 0; i < last_index_count && !found; i++) {
            if (strcmp(last_index[i].id, id) != 0) continue;
            job->entry = last_index[i];
            snprintf(job->tag, sizeof(job->tag), "%s", last_tag);
            found = true;
        }
        if (found) {
            store_worker_running = true;
            memset(&store_status, 0, sizeof(store_status));
            store_status.state = state;
        }
    }
    pthread_mutex_unlock(&store_mutex);
    if (!found) {
        free(job);
        return false;
    }
    if (!store_start_thread(install_worker, job)) {
        free(job);
        finish(PLUGIN_STORE_FAILED, TR("Could not start the emulator operation"), false);
        return false;
    }
    return true;
}

static void * uninstall_worker(void * arg) {
    char id[33];
    snprintf(id, sizeof(id), "%s", (char *) arg);
    free(arg);
    char error[160];
    plugin_store_state_t final_state = PLUGIN_STORE_FAILED;
    const char * final_error = NULL;
    bool changed = false;
    bool leased = usb_mode_control_storage_write_begin();
    if (!leased) {
        final_error = TR("Disconnect USB storage from the host before changing emulators.");
    } else if (!root_ready(EMULATOR_ROOT, 0, error, sizeof(error))) {
        final_error = error;
    } else if (!emulator_store_remove(EMULATOR_ROOT, id)) {
        final_error = TR("Could not remove the emulator.");
    } else {
        /* The lease also covers cleanup and the result scan after removal. */
        remove_leftovers(EMULATOR_ROOT);
        compute_results();
        final_state = PLUGIN_STORE_READY;
        changed = true;
    }
    if (leased) usb_mode_control_storage_write_end();
    finish(final_state, final_error, changed);
    return NULL;
}

bool emulator_store_refresh(void) {
    if (!begin(PLUGIN_STORE_REFRESHING)) return false;
    if (!store_start_thread(refresh_worker, NULL)) {
        finish(PLUGIN_STORE_FAILED, TR("Could not start the emulator operation"), false);
        return false;
    }
    return true;
}

bool emulator_store_install(const char * id) {
    return start_install(id, PLUGIN_STORE_INSTALLING);
}

bool emulator_store_update(const char * id) {
    return start_install(id, PLUGIN_STORE_UPDATING);
}

bool emulator_store_uninstall(const char * id) {
    if (!emulator_id_valid(id) || !begin(PLUGIN_STORE_UNINSTALLING)) return false;
    char * copy = strdup(id);
    if (!copy || !store_start_thread(uninstall_worker, copy)) {
        free(copy);
        finish(PLUGIN_STORE_FAILED, TR("Could not start the emulator operation"), false);
        return false;
    }
    return true;
}

void emulator_store_get_status(plugin_store_status_t * out, emulator_store_result_t * results,
                               size_t result_capacity) {
    pthread_mutex_lock(&store_mutex);
    if (out) *out = store_status;
    if (results && store_results) {
        size_t n = store_status.result_count < result_capacity ? store_status.result_count : result_capacity;
        memcpy(results, store_results, n * sizeof(*results));
        if (out) out->result_count = n;
    }
    pthread_mutex_unlock(&store_mutex);
}

bool emulator_store_get_details(const char * id, emulator_store_details_t * out) {
    if (!id || !out) return false;
    bool found = false;
    pthread_mutex_lock(&store_mutex);
    for (size_t i = 0; i < last_index_count; i++) {
        if (strcmp(last_index[i].id, id) != 0) continue;
        snprintf(out->description, sizeof(out->description), "%s", last_index[i].description);
        snprintf(out->author, sizeof(out->author), "%s", last_index[i].author);
        snprintf(out->license, sizeof(out->license), "%s", last_index[i].license);
        out->size = last_index[i].size;
        found = true;
        break;
    }
    pthread_mutex_unlock(&store_mutex);
    return found;
}

bool emulator_store_busy(void) {
    pthread_mutex_lock(&store_mutex);
    bool busy = store_worker_running;
    pthread_mutex_unlock(&store_mutex);
    return busy;
}

void emulator_store_reset(void) {
    pthread_mutex_lock(&store_mutex);
    if (!store_worker_running) {
        memset(&store_status, 0, sizeof(store_status));
        free(store_results);
        store_results = NULL;
        free(last_index);
        last_index = NULL;
        last_index_count = 0;
        last_tag[0] = '\0';
    }
    pthread_mutex_unlock(&store_mutex);
}
