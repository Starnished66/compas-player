#include "plugin_store_internal.h"
#include "store_common.h"
#include "../../../core/files/file_integrity.h"
#include "../../../ui/text/i18n/i18n.h"
#include "../../http/client/http_client.h"
#include "../../../plugins/manager/plugin_api_version.h"

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#ifndef PLUGIN_STORE_INDEX_URL
#define PLUGIN_STORE_INDEX_URL "https://github.com/Starnished66/compas-plugins/releases/latest/download/index.json"
#endif
#ifdef HOST_BUILD
#define STORE_WORK STORE_SD_ROOT "/.compas/store"
#else
#define STORE_WORK SD_COMPAS_ROOT "/store"
#endif
#define STORE_MARGIN (2u << 20)

/* Detached workers of the Plugin Store: catalog refresh, install/update,
 * uninstall, and update-all. Each owns the store's worker slot until it
 * reports through ps_finish(). */

typedef struct {
    const store_card_t * card;
    char error[160];
} download_guard_t;

static bool download_progress(uint64_t downloaded, uint64_t total, void * context) {
    download_guard_t * guard = context;
    char error[160] = {0};
    if (!ps_storage_path_writable(STORE_SD_ROOT, error, sizeof(error)) ||
        (guard && guard->card && !ps_card_still(guard->card))) {
        if (guard) snprintf(guard->error, sizeof(guard->error), "%s",
                            error[0] ? error : TR("The SD card changed during the operation."));
        return false;
    }
    int percent = total ? (int) (downloaded * 100 / total) : 0;
    if (percent > 100) {
        percent = 100;
    }
    pthread_mutex_lock(&store_mutex);
    store_status.percent = percent;
    pthread_mutex_unlock(&store_mutex);
    return true;
}

/* A keep file already on the card is neither downloaded nor replaced. */
static bool keep_present(const plugin_store_file_t * f) {
    char path[PATH_MAX];
    struct stat st;
    return f->keep && snprintf(path, sizeof(path), "%s/%s", STORE_SD_ROOT, f->dest) > 0 &&
           lstat(path, &st) == 0;
}

static uint64_t bytes_to_fetch(const plugin_store_plugin_t * p) {
    uint64_t total = 0;
    for (int i = 0; i < p->file_count; i++) {
        if (!keep_present(&p->files[i])) {
            total += p->files[i].size;
        }
    }
    return total;
}

static bool download_plugin_files(const plugin_store_plugin_t * p, const char * tag,
                                 const char * stage, download_guard_t * guard) {
    if (!ps_storage_path_writable(STORE_SD_ROOT, guard ? guard->error : NULL, guard ? sizeof(guard->error) : 0) ||
        !ps_mkdir_tree(STORE_SD_ROOT, stage)) {
        return false;
    }
    for (int i = 0; i < p->file_count; i++) {
        const plugin_store_file_t * f = &p->files[i];
        char dest[PATH_MAX], url[512];
        ps_stage_file(dest, sizeof(dest), stage, (size_t) i, "part");
        if (keep_present(f)) {
            continue;
        }
        if (!ps_make_asset_url(url, sizeof(url), tag, f->asset)) {
            return false;
        }
        if (!store_download_verified(url, dest, f->size, f->sha256, STORE_FILE_LIMIT, download_progress, guard)) {
            return false;
        }
    }
    return true;
}

void * ps_refresh_worker(void * unused) {
    (void) unused;
    http_response_t response;
    const char * fetch_error;
    if (!store_fetch_index(PLUGIN_STORE_INDEX_URL, STORE_INDEX_LIMIT,
                           TR("Could not read the plugin list from GitHub."), &response, &fetch_error)) {
        ps_finish(PLUGIN_STORE_FAILED, fetch_error, false);
        return NULL;
    }
    plugin_store_plugin_t * parsed = NULL;
    size_t count = 0;
    char tag[64];
    char err[128];
    bool valid = plugin_store_parse_index((char *) response.body, response.body_len, &parsed,
                                          &count, tag, err, sizeof(err));
    http_response_free(&response);
    if (!valid) {
        ps_finish(PLUGIN_STORE_FAILED, err, false);
        return NULL;
    }
    pthread_mutex_lock(&store_mutex);
    free(last_index);
    last_index = parsed;
    last_index_count = count;
    plugin_store_preview_invalidate_locked();
    snprintf(last_tag, sizeof(last_tag), "%s", tag);
    pthread_mutex_unlock(&store_mutex);
    ps_finish_ready(false);
    return NULL;
}

static bool disk_space(uint64_t bytes) {
    struct statvfs fs;
    uint64_t available;
    return statvfs(STORE_SD_ROOT, &fs) == 0 && !(fs.f_flag & ST_RDONLY) &&
           filesystem_available_bytes(STORE_SD_ROOT, &available) && available >= bytes + STORE_MARGIN;
}

/* A staging folder of this operation's own. The name is new each time, so
 * a failed download (the download module removes its temporary file)
 * or a cleanup can never hit files another card left at a fixed path. */
static void make_stage_path(char * out, size_t size, const char * kind, const char * id) {
    static unsigned stage_counter;
    snprintf(out, size, "%s/%s%.63s.%lu.%u", STORE_WORK, kind, id, (unsigned long) time(NULL),
             ++stage_counter);
}

/* Removes staged files, but only from a real directory on the card the
 * operation started on: a symlinked stage must not make this delete
 * elsewhere, and a swapped card's files are not ours. */
static void cleanup_stage(const store_card_t * card, const char * path, int count) {
    if (!ps_card_still(card) || !ps_mkdir_tree(STORE_SD_ROOT, path)) {
        return;
    }
    for (int i = 0; i < count; i++) {
        if (!ps_card_still(card) || !ps_storage_path_writable(STORE_SD_ROOT, NULL, 0)) return;
        char p[PATH_MAX + 32];
        ps_stage_file(p, sizeof(p), path, (size_t) i, "part");
        unlink(p);
        ps_stage_file(p, sizeof(p), path, (size_t) i, "backup");
        unlink(p);
    }
    if (ps_card_still(card) && ps_storage_path_writable(STORE_SD_ROOT, NULL, 0)) rmdir(path); /* only succeeds once empty */
}
/* Worker error exit: drops the installed-record copy and reports. */
static void * worker_fail(plugin_store_plugin_t * rec, plugin_store_state_t state,
                          const char * error, bool changed) {
    free(rec);
    ps_finish(state, error, changed);
    return NULL;
}

/* NULL while the card is writable and still the one the worker started on;
 * otherwise the message to report (error is the writer's scratch buffer). */
static const char * worker_guard_error(const store_card_t * card, char * error, size_t size) {
    if (ps_storage_path_writable(STORE_SD_ROOT, error, size) && ps_card_still(card)) {
        return NULL;
    }
    return error[0] ? error : TR("The SD card changed during the operation.");
}

/* Card present and writable, or the operation is finished as FAILED. */
static bool begin_operation(store_card_t * card, const char * no_card_error) {
    char error[160] = {0};
    if (!ps_card_mounted(card)) {
        ps_finish(PLUGIN_STORE_FAILED, no_card_error, false);
        return false;
    }
    if (!ps_storage_path_writable(STORE_SD_ROOT, error, sizeof(error))) {
        ps_finish(PLUGIN_STORE_FAILED, error, false);
        return false;
    }
    return true;
}

/* State of one install or update. */
typedef struct {
    const store_job_t * job;
    const plugin_store_plugin_t * p;
    store_card_t card;
    plugin_store_plugin_t * rec; /* installed record */
    size_t rn;
    char stage[PATH_MAX];
    char error[160];
    plugin_store_state_t fail_state; /* state to report when an error is returned */
} install_t;

/* NULL when the job may go ahead, otherwise the reason it may not. */
static const char * install_precheck(const install_t * s) {
    const plugin_store_plugin_t * p = s->p;
    int installed = ps_record_find(s->rec, s->rn, p->id);
    bool updateable = installed >= 0 &&
                      store_version_compare(p->version, s->rec[installed].version) > 0;
    if (p->api_min > PLUGIN_API_VERSION) return TR("This plugin needs a newer player version.");
    if (installed < 0 && s->rn >= STORE_MAX_RECORD) {
        return TR("Too many plugins are installed. Remove one and try again.");
    }
    if (s->job->state == PLUGIN_STORE_INSTALLING && installed >= 0 && !s->job->force) {
        return TR("This plugin is already installed by the store.");
    }
    if (s->job->state == PLUGIN_STORE_UPDATING && !updateable) return TR("This plugin has no update available.");
    if (!disk_space(bytes_to_fetch(p))) return TR("Not enough free space on the SD card.");
    return NULL;
}

/* Downloads into the stage and runs the checks before the commit. NULL when
 * ready to commit, otherwise the message (and fail_state may be set). */
static const char * install_fetch(install_t * s) {
    const plugin_store_plugin_t * p = s->p;
    download_guard_t download_guard = { .card = &s->card };
    if (!download_plugin_files(p, s->job->tag, s->stage, &download_guard)) {
        cleanup_stage(&s->card, s->stage, p->file_count);
        snprintf(s->error, sizeof(s->error), "%s", download_guard.error[0] ? download_guard.error :
                 TR("The plugin download failed verification. Try again."));
        return s->error;
    }
    if (!ps_card_still(&s->card)) {
        /* No cleanup: the stage path now belongs to the other card. */
        return TR("The SD card changed during the download.");
    }
    if (ps_conflict_exists(STORE_SD_ROOT, p, s->rec, s->rn) && !s->job->force) {
        cleanup_stage(&s->card, s->stage, p->file_count);
        s->fail_state = PLUGIN_STORE_NEEDS_CONFIRM;
        return TR("A local plugin file will be replaced. Confirm to continue.");
    }
    const char * guard_message = worker_guard_error(&s->card, s->error, sizeof(s->error));
    if (guard_message) cleanup_stage(&s->card, s->stage, p->file_count);
    return guard_message;
}

/* Reports why a commit failed. */
static void install_report_failure(bool same_card, bool needs_confirmation) {
    if (!same_card) {
        ps_finish(PLUGIN_STORE_FAILED, TR("The SD card changed during the operation."), false);
    } else if (needs_confirmation) {
        ps_finish(PLUGIN_STORE_NEEDS_CONFIRM,
               TR("A local plugin file will be replaced. Confirm to continue."), false);
    } else {
        char write_error[160];
        ps_finish(PLUGIN_STORE_FAILED, !ps_storage_path_writable(STORE_SD_ROOT, write_error, sizeof(write_error)) ? write_error :
               TR("Could not install the plugin on the SD card."), false);
    }
}

/* Commits the staged files, cleans up and reports. Frees the record. */
static void install_commit(install_t * s) {
    bool preserve_stage = false;
    ps_commit_card = &s->card;
    bool ok = ps_commit_staged(STORE_SD_ROOT, s->stage, s->p, s->job->force, s->rec, s->rn, &preserve_stage);
    ps_commit_card = NULL;
    bool same = ps_card_still(&s->card);
    bool needs_confirmation = !ok && same && !s->job->force &&
                             ps_conflict_exists(STORE_SD_ROOT, s->p, s->rec, s->rn);
    if (same && !preserve_stage) {
        cleanup_stage(&s->card, s->stage, s->p->file_count); /* never on another card */
    }
    free(s->rec);
    if (!ok) {
        install_report_failure(same, needs_confirmation);
        return;
    }
    ps_finish_ready(true);
}

void * ps_install_worker(void * arg) {
    store_job_t job = *(store_job_t *) arg;
    free(arg);
    install_t s = { .job = &job, .p = &job.plugin, .fail_state = PLUGIN_STORE_FAILED };
    if (!begin_operation(&s.card, TR("Insert an SD card to install plugins."))) return NULL;
    ps_progress_set(0, job.plugin.name);
    ps_record_status_t record_status = ps_read_record_alloc(&s.rec, &s.rn);
    if (record_status != PS_RECORD_OK) {
        ps_finish(PLUGIN_STORE_FAILED, ps_record_error(record_status), false);
        return NULL;
    }
    const char * error = install_precheck(&s);
    if (!error) {
        make_stage_path(s.stage, sizeof(s.stage), "", job.plugin.id);
        error = install_fetch(&s);
    }
    if (error) return worker_fail(s.rec, s.fail_state, error, false);
    install_commit(&s);
    return NULL;
}

/* Puts the first `upto` backed-up files of entry back where they were. */
static void restore_backups(const store_card_t * card, const char * stage,
                            const plugin_store_plugin_t * entry, const bool * backed, int upto) {
    for (int j = 0; j < upto; j++) {
        if (backed[j] && ps_storage_path_writable(STORE_SD_ROOT, NULL, 0) && ps_card_still(card)) {
            char dst[PATH_MAX], bak[PATH_MAX + 32];
            ps_path_for(dst, sizeof(dst), STORE_SD_ROOT, entry->files[j].dest);
            ps_stage_file(bak, sizeof(bak), stage, (size_t) j, "backup");
            rename(bak, dst);
        }
    }
}
/* State of one uninstall. */
typedef struct {
    store_card_t card;
    char stage[PATH_MAX];
    plugin_store_plugin_t * rec; /* installed record */
    size_t rn;
    int found; /* index of the plugin in rec */
    bool backed[PLUGIN_STORE_MAX_FILES];
    char error[160];
} uninstall_t;

/* Moves file i of the plugin into the stage. False when the move failed. */
static bool uninstall_backup_one(uninstall_t * u, int i) {
    const plugin_store_file_t * f = &u->rec[u->found].files[i];
    char path[PATH_MAX], backup[PATH_MAX + 32];
    struct stat st;
    if (!ps_path_for(path, sizeof(path), STORE_SD_ROOT, f->dest) ||
        !ps_parent_dirs_safe(STORE_SD_ROOT, f->dest, false)) {
        return true;
    }
    if (lstat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        ps_stage_file(backup, sizeof(backup), u->stage, (size_t) i, "backup");
        unlink(backup);
        if (rename(path, backup) != 0) return false;
        u->backed[i] = true;
    }
    return true;
}

/* Backs up every file the plugin owns. NULL on success, otherwise the message. */
static const char * uninstall_backup(uninstall_t * u) {
    for (int i = 0; i < u->rec[u->found].file_count; i++) {
        if (u->rec[u->found].files[i].keep) {
            continue;
        }
        /* Another card or a read-only/host-shared mount: leave backups untouched. */
        const char * guard_message = worker_guard_error(&u->card, u->error, sizeof(u->error));
        if (guard_message) return guard_message;
        if (!uninstall_backup_one(u, i)) {
            restore_backups(&u->card, u->stage, &u->rec[u->found], u->backed, i);
            return TR("Could not remove a plugin file.");
        }
    }
    return NULL;
}

/* The record no longer lists the plugin: the backups are not needed. */
static void uninstall_drop_backups(const uninstall_t * u) {
    for (int i = 0; i < u->rec[u->found].file_count; i++) {
        if (u->backed[i] && ps_storage_path_writable(STORE_SD_ROOT, NULL, 0) && ps_card_still(&u->card)) {
            char bak[PATH_MAX + 32];
            ps_stage_file(bak, sizeof(bak), u->stage, (size_t) i, "backup");
            unlink(bak);
        }
    }
    if (ps_storage_path_writable(STORE_SD_ROOT, NULL, 0) && ps_card_still(&u->card)) rmdir(u->stage);
}

/* Writes the record without the plugin, then drops or restores the backups. */
static bool uninstall_commit(uninstall_t * u, const char * id) {
    ps_commit_card = &u->card;
    bool ok = ps_write_record_without(STORE_SD_ROOT, u->rec, u->rn, id);
    ps_commit_card = NULL;
    if (!ps_card_still(&u->card)) return ok; /* a swapped card's files are left alone */
    if (!ok) restore_backups(&u->card, u->stage, &u->rec[u->found], u->backed, u->rec[u->found].file_count);
    else uninstall_drop_backups(u);
    return ok;
}

void * ps_uninstall_worker(void * arg) {
    char id[64];
    snprintf(id, sizeof(id), "%s", (char *) arg);
    free(arg);
    uninstall_t u = {0};
    if (!begin_operation(&u.card, TR("Insert an SD card to remove plugins."))) return NULL;
    ps_record_status_t record_status = ps_read_record_alloc(&u.rec, &u.rn);
    if (record_status != PS_RECORD_OK) {
        ps_finish(PLUGIN_STORE_FAILED, ps_record_error(record_status), false);
        return NULL;
    }
    u.found = ps_record_find(u.rec, u.rn, id);
    if (u.found < 0) {
        return worker_fail(u.rec, PLUGIN_STORE_FAILED, TR("This plugin is not installed by the store."), false);
    }
    make_stage_path(u.stage, sizeof(u.stage), "uninstall-", id);
    if (!ps_mkdir_tree(STORE_SD_ROOT, u.stage)) {
        return worker_fail(u.rec, PLUGIN_STORE_FAILED, TR("Could not prepare plugin removal."), false);
    }
    const char * error = uninstall_backup(&u);
    /* Another card: restoring backups would write to it. */
    if (!error && !ps_card_still(&u.card)) error = TR("The SD card changed during the operation.");
    if (!error) error = worker_guard_error(&u.card, u.error, sizeof(u.error));
    if (error) return worker_fail(u.rec, PLUGIN_STORE_FAILED, error, false);
    bool ok = uninstall_commit(&u, id);
    free(u.rec);
    if (!ok) {
        ps_finish(PLUGIN_STORE_FAILED, TR("Could not update the installed plugin record."), false);
        return NULL;
    }
    sync();
    ps_finish_ready(true);
    return NULL;
}

/* State of one "update all" run. */
typedef struct {
    store_card_t card;
    char tag[64];
    plugin_store_plugin_t * rec; /* installed record, re-read after an install */
    size_t rn;
    bool changed;
    const char * failure; /* the last update that failed, reported at the end */
    char failure_buffer[160];
    char guard_error[160];
    char pending[65]; /* name of the last plugin that needs confirmation */
    char pending_ids[PLUGIN_STORE_MAX_PLUGINS][64];
    size_t pending_count;
} update_all_t;

static void update_pending_add(update_all_t * u, const plugin_store_plugin_t * p) {
    snprintf(u->pending, sizeof(u->pending), "%.64s", p->name);
    snprintf(u->pending_ids[u->pending_count++], 64, "%.63s", p->id);
}

/* Records why an update was not installed: a conflict to confirm, or a failure. */
static void update_note_not_installed(update_all_t * u, const plugin_store_plugin_t * p) {
    if (ps_conflict_exists(STORE_SD_ROOT, p, u->rec, u->rn) &&
        u->pending_count < PLUGIN_STORE_MAX_PLUGINS) {
        update_pending_add(u, p);
        return;
    }
    char write_error[160];
    if (!ps_storage_path_writable(STORE_SD_ROOT, write_error, sizeof(write_error))) {
        snprintf(u->failure_buffer, sizeof(u->failure_buffer), "%s", write_error);
        u->failure = u->failure_buffer;
    } else {
        u->failure = TR("Could not install a plugin on the SD card.");
    }
}

/* Commits the downloaded stage. False stops the whole run, with the reason in *abort_error. */
static bool update_commit(update_all_t * u, const plugin_store_plugin_t * p, const char * stage,
                          const char ** abort_error) {
    if (!ps_card_still(&u->card)) {
        *abort_error = TR("The SD card changed during the operation.");
        return false;
    }
    const char * guard_message = worker_guard_error(&u->card, u->guard_error, sizeof(u->guard_error));
    if (guard_message) {
        *abort_error = guard_message;
        return false;
    }
    bool preserve_stage = false;
    ps_commit_card = &u->card;
    bool installed = ps_commit_staged(STORE_SD_ROOT, stage, p, false, u->rec, u->rn, &preserve_stage);
    ps_commit_card = NULL;
    if (!installed && !ps_card_still(&u->card)) {
        *abort_error = TR("The SD card changed during the operation.");
        return false;
    }
    if (installed) {
        u->changed = true;
        free(u->rec);
        u->rec = NULL;
        ps_record_status_t record_status = ps_read_record_alloc(&u->rec, &u->rn);
        if (record_status != PS_RECORD_OK) {
            *abort_error = ps_record_error(record_status);
            if (!preserve_stage) cleanup_stage(&u->card, stage, p->file_count);
            return false;
        }
    } else {
        update_note_not_installed(u, p);
    }
    if (!preserve_stage) cleanup_stage(&u->card, stage, p->file_count);
    return true;
}

/* Updates one plugin. False stops the whole run, with the reason in *abort_error. */
static bool update_one(update_all_t * u, const plugin_store_plugin_t * p, int percent,
                       const char ** abort_error) {
    const char * guard_message = worker_guard_error(&u->card, u->guard_error, sizeof(u->guard_error));
    if (guard_message) {
        *abort_error = guard_message;
        return false;
    }
    ps_progress_set(percent, p->name);
    if (ps_conflict_exists(STORE_SD_ROOT, p, u->rec, u->rn)) {
        update_pending_add(u, p);
        return true;
    }
    if (!disk_space(bytes_to_fetch(p))) {
        u->failure = TR("Not enough free space on the SD card.");
        return true;
    }
    char stage[PATH_MAX];
    make_stage_path(stage, sizeof(stage), "", p->id);
    download_guard_t download_guard = { .card = &u->card };
    if (!download_plugin_files(p, u->tag, stage, &download_guard)) {
        cleanup_stage(&u->card, stage, p->file_count);
        snprintf(u->failure_buffer, sizeof(u->failure_buffer), "%s", download_guard.error[0]
                 ? download_guard.error : TR("A plugin download failed verification. Try again."));
        u->failure = u->failure_buffer;
        return true;
    }
    return update_commit(u, p, stage, abort_error);
}

/* Marks the plugins that need confirmation and reports the run's outcome. */
static void update_all_report(const update_all_t * u) {
    if (!u->pending[0] && !u->failure) {
        ps_finish(PLUGIN_STORE_READY, NULL, u->changed);
        return;
    }
    pthread_mutex_lock(&store_mutex);
    snprintf(store_status.current_plugin, sizeof(store_status.current_plugin), "%s", u->pending);
    for (size_t i = 0; i < store_status.result_count; i++) {
        for (size_t j = 0; j < u->pending_count; j++) {
            if (strcmp(store_results[i].id, u->pending_ids[j]) == 0) {
                store_results[i].needs_confirmation = true;
                store_status.result_generation++;
            }
        }
    }
    pthread_mutex_unlock(&store_mutex);
    /* A failure is the more urgent news; the plugins needing
     * confirmation stay marked and can be updated one by one. */
    if (u->failure) {
        ps_finish(PLUGIN_STORE_FAILED, u->failure, u->changed);
    } else {
        ps_finish(PLUGIN_STORE_NEEDS_CONFIRM,
               TR("Some updates need confirmation before replacing local files."), u->changed);
    }
}

/* Updates every plugin that has a newer version. NULL, or the reason the run
 * was aborted. */
static const char * update_all_run(update_all_t * u, size_t index_count) {
    for (size_t i = 0; i < index_count; i++) {
        plugin_store_plugin_t current;
        pthread_mutex_lock(&store_mutex);
        bool have_plugin = last_index && i < last_index_count;
        if (have_plugin) {
            current = last_index[i];
        }
        pthread_mutex_unlock(&store_mutex);
        if (!have_plugin) {
            break;
        }
        int r = ps_record_find(u->rec, u->rn, current.id);
        if (r < 0 || store_version_compare(current.version, u->rec[r].version) <= 0 ||
            current.api_min > PLUGIN_API_VERSION) {
            continue;
        }
        const char * abort_error = NULL;
        if (!update_one(u, &current, (int) (i * 100 / (index_count ? index_count : 1)), &abort_error)) {
            return abort_error;
        }
    }
    return NULL;
}

void * ps_update_all_worker(void * unused) {
    (void) unused;
    update_all_t u = {0};
    if (!begin_operation(&u.card, TR("Insert an SD card to update plugins."))) return NULL;
    ps_record_status_t record_status = ps_read_record_alloc(&u.rec, &u.rn);
    if (record_status != PS_RECORD_OK) {
        ps_finish(PLUGIN_STORE_FAILED, ps_record_error(record_status), false);
        return NULL;
    }
    pthread_mutex_lock(&store_mutex);
    size_t index_count = last_index_count;
    snprintf(u.tag, sizeof(u.tag), "%s", last_tag);
    pthread_mutex_unlock(&store_mutex);
    const char * abort_error = update_all_run(&u, index_count);
    if (abort_error) return worker_fail(u.rec, PLUGIN_STORE_FAILED, abort_error, u.changed);
    free(u.rec);
    if (ps_compute_results()) update_all_report(&u);
    else ps_finish(PLUGIN_STORE_FAILED, ps_record_error(PS_RECORD_ERROR), u.changed);
    return NULL;
}
