/* Library scanning: rescans, boot and update prompts, metadata and cover refresh, and cache loading. */
#include "../../text/i18n/i18n.h"
#include "gui_library_internal.h"
#include "gui_folder_actions.h"
#include "../../../library/metadata/durations/duration_prune.h"
#include "../../../library/metadata/durations/duration_cache.h"
#include "../../../library/metadata/scan/library_scan.h"
#include "../library_az_index.h"
#include "../../network/gui_network.h"
#include "../../../hardware/indicators/backlight.h"
#include "../../text/rendering/gui_text_input.h"
#include "../collections/gui_books.h"
#include "../../../library/metadata/catalog/metadata_refresh_targets.h"
#include "../../../library/playlists/playlist_files.h"
#include "../../../core/system/logging/db_log.h"
#include "../../../core/device/device_info.h"
#include "../../../core/persistence/storage_paths.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sched.h>
/* SCHED_BATCH below is glibc's own <bits/sched.h> policy constant, gated
 * behind _GNU_SOURCE there -- this file deliberately doesn't define that
 * (see search_matches()'s own comment on strcasestr() for why). The raw
 * kernel UAPI header defines the same numeric policy constants ungated on
 * both glibc and musl (confirmed in both toolchains' linux/sched.h), so
 * this alone is enough on host and target alike without touching feature-
 * test macros anywhere in this file. */
#include <linux/sched.h>
#include <sys/resource.h>

gui_busy_handle_t library_rescan_token = 0;

static pthread_t library_rescan_thread;
_Atomic bool library_rescan_active = false;
void (*library_setup_scan_complete)(bool success);
bool library_rescan_success_pending = false;
static atomic_bool library_rescan_done_flag = false;
library_rescan_mode_t library_rescan_mode;
gui_popup_t refresh_all_metadata_popup;
gui_popup_t refresh_all_covers_popup;
/* Published to the UI by library_rescan_done_flag's release/acquire pair. */
/* Set when the walk could not read part of the tree, so the toast does not
 * claim a full update. Rows whose file is missing are still removed. */
static const char library_recovered_message[] = N_("Library recovered. Use Settings > Update Music Database to save");
static void rescan_playlists(void);

static void library_scan_copy_result(library_scan_result_t *out) {
    if (!out) return;
    library_scan_get_result(out);
}

/* Cache reports are owned by the UI thread. */
static metadata_db_load_outcome_t library_cache_load_outcome;
static bool library_cache_load_pending;
/* "Library loaded" is shown only for an SD hot swap, not at boot. */
static bool library_cache_load_announce_loaded;
static uint32_t library_cache_load_card_generation;
static uint32_t library_cache_load_generation;
static uint32_t library_prompt_card_generation;
static uint32_t library_boot_ready_tick;
static bool library_boot_ready_tick_set;
bool boot_warmup_requested;
bool boot_warmup_pending;

typedef enum {
    LIBRARY_PROMPT_NONE,
    LIBRARY_PROMPT_NO_DATABASE,
    LIBRARY_PROMPT_LOAD_FAILED,
    LIBRARY_PROMPT_FILES_CHANGED,
} library_prompt_reason_t;

typedef enum {
    LIBRARY_PROMPT_REQUEST_NONE,
    LIBRARY_PROMPT_REQUEST_PENDING,
    LIBRARY_PROMPT_REQUEST_VISIBLE,
} library_prompt_request_state_t;

typedef struct {
    library_prompt_reason_t reason;
    library_prompt_request_state_t state;
    uint32_t card_generation;
    uint32_t load_generation;
} library_prompt_request_t;

static library_prompt_request_t library_prompt_request;
static library_prompt_request_t library_prompt_queued_request;
gui_popup_t library_update_prompt_popup;
lv_obj_t * library_update_prompt_title;
lv_obj_t * library_update_prompt_body;
lv_obj_t * library_update_prompt_confirm_label;

static void library_update_prompt_confirm_cb(lv_event_t * e);
static void library_update_prompt_later_cb(lv_event_t * e);
static void library_update_prompt_backdrop_cb(lv_event_t * e);

static bool library_prompt_request_is_current(const library_prompt_request_t * request) {
    if (!request || request->state == LIBRARY_PROMPT_REQUEST_NONE ||
        request->card_generation != library_prompt_card_generation) return false;
    if (request->reason == LIBRARY_PROMPT_FILES_CHANGED) return true;
    if (request->load_generation != library_cache_load_generation) return false;
    if (request->reason == LIBRARY_PROMPT_NO_DATABASE)
        return library_cache_load_outcome == METADATA_DB_LOAD_SUCCESS_FRESH;
    if (request->reason == LIBRARY_PROMPT_LOAD_FAILED)
        return library_cache_load_outcome == METADATA_DB_LOAD_FAILED;
    return false;
}

static void library_prompt_clear_request(library_prompt_request_t * request) {
    memset(request, 0, sizeof(*request));
}

static void library_prompt_activate_queued_request(void) {
    if (library_prompt_request.state != LIBRARY_PROMPT_REQUEST_NONE ||
        library_prompt_queued_request.state == LIBRARY_PROMPT_REQUEST_NONE) return;
    library_prompt_request = library_prompt_queued_request;
    library_prompt_clear_request(&library_prompt_queued_request);
}

static void library_prompt_queue_request(const library_prompt_request_t * request) {
    if (library_prompt_queued_request.reason == LIBRARY_PROMPT_FILES_CHANGED &&
        request->reason != LIBRARY_PROMPT_FILES_CHANGED) return;
    library_prompt_queued_request = *request;
}

static void library_prompt_enqueue(library_prompt_reason_t reason, uint32_t card_generation) {
    library_prompt_request_t request = {
        .reason = reason,
        .state = LIBRARY_PROMPT_REQUEST_PENDING,
        .card_generation = card_generation,
        .load_generation = library_cache_load_generation,
    };
    if (library_prompt_request.state == LIBRARY_PROMPT_REQUEST_VISIBLE) {
        if (library_prompt_request.reason == LIBRARY_PROMPT_FILES_CHANGED &&
            reason == LIBRARY_PROMPT_FILES_CHANGED) return;
        library_prompt_queue_request(&request);
    } else if (library_prompt_request.state == LIBRARY_PROMPT_REQUEST_PENDING &&
               library_prompt_request.reason == LIBRARY_PROMPT_FILES_CHANGED &&
               reason != LIBRARY_PROMPT_FILES_CHANGED) {
        library_prompt_queue_request(&request);
    } else {
        library_prompt_request = request;
        library_prompt_clear_request(&library_prompt_queued_request);
    }
}

static void library_prompt_drop_all(void) {
    gui_popup_hide(&library_update_prompt_popup);
    library_prompt_clear_request(&library_prompt_request);
    library_prompt_clear_request(&library_prompt_queued_request);
}

void gui_library_invalidate_boot_prompt(void) {
    library_prompt_drop_all();
}

void gui_library_suspend_boot_prompt(void) {
    library_prompt_requeue_visible();
}

void library_prompt_card_changed(void) {
    library_prompt_card_generation++;
    boot_warmup_pending = false;
    library_prompt_drop_all();
}

void gui_library_request_files_changed_prompt(void) {
    library_prompt_card_status_t card_status = library_prompt_card_status();
    if (card_status == LIBRARY_PROMPT_CARD_CHANGED) {
        library_prompt_card_changed();
        return;
    }
    library_prompt_enqueue(LIBRARY_PROMPT_FILES_CHANGED, library_prompt_card_generation);
}

static bool library_prompt_has_manual_overlay(void) {
    return library_az_index_overlay_visible() ||
           gui_playlists_menu_visible() ||
           library_collection_menu_visible() ||
           gui_folder_actions_menu_visible();
}

#define BOOT_SETTLE_MS 8000
static bool library_prompt_environment_idle(void) {
    if (!current_settings.setup_complete || !library_boot_ready_tick_set || lv_tick_elaps(library_boot_ready_tick) < BOOT_SETTLE_MS ||
        !backlight_screen_is_on() || gui_navigation_transition_in_progress() ||
        lv_screen_active() == gui_busy_get_screen() || gui_library_navigation_blocked() ||
        gui_network_boot_prompt_blocked() || gui_shell_boot_prompt_blocked() ||
        gui_lock_screen_is_showing() || gui_player_boot_prompt_blocked() ||
        gui_queue_boot_prompt_blocked() || library_prompt_has_manual_overlay() ||
        gui_notifications_toast_visible()) return false;

    lv_obj_t * active = lv_screen_active();
    if (t9_keypad_is_inline_active() ||
        (gui_text_input_get_screen() && active == gui_text_input_get_screen())) return false;

    return gui_popup_visible_count() == 0;
}

/* A visible prompt yields only to something that takes over the screen.
 * Touches on the prompt itself latch the shell's gesture candidates, and a
 * toast can appear under it, so the settle checks above do not apply. */
static bool library_prompt_taken_over(void) {
    return !backlight_screen_is_on() || lv_screen_active() == gui_busy_get_screen() ||
           gui_lock_screen_is_showing() || gui_network_boot_prompt_blocked() ||
           gui_popup_visible_count() != 1;
}

static void library_prompt_update_labels(library_prompt_reason_t reason) {
    switch (reason) {
        case LIBRARY_PROMPT_NO_DATABASE:
            lv_label_set_text(library_update_prompt_title, TR("No music database"));
            lv_label_set_text(library_update_prompt_body,
                              TR("Build it now? Large libraries can take several minutes."));
            lv_label_set_text(library_update_prompt_confirm_label, TR("Build"));
            break;
        case LIBRARY_PROMPT_LOAD_FAILED:
            lv_label_set_text(library_update_prompt_title, TR("Music database unavailable"));
            lv_label_set_text(library_update_prompt_body, TR("It could not be loaded. Rebuild it now?"));
            lv_label_set_text(library_update_prompt_confirm_label, TR("Rebuild"));
            break;
        case LIBRARY_PROMPT_FILES_CHANGED:
            lv_label_set_text(library_update_prompt_title, TR("Update music database?"));
            lv_label_set_text(library_update_prompt_body, TR("Files on the card may have changed."));
            lv_label_set_text(library_update_prompt_confirm_label, TR("Update"));
            break;
        case LIBRARY_PROMPT_NONE:
            break;
    }
}

static void library_update_prompt_close(bool show_settings_toast) {
    gui_popup_hide(&library_update_prompt_popup);
    library_prompt_clear_request(&library_prompt_request);
    library_prompt_activate_queued_request();

    if (!show_settings_toast) return;
    show_info_toast(TR("Use Settings > Update Music Database"));
}

/* The settle checks only decide when to show the prompt. Once the user taps
 * the action, revalidate what makes the scan unsafe or stale; the tap's own
 * press still has the shell's gesture candidates latched at this point. */
static bool library_prompt_can_run_action(const library_prompt_request_t * request) {
    return library_prompt_request_is_current(request) &&
           library_prompt_card_status() == LIBRARY_PROMPT_CARD_CURRENT &&
           !gui_library_navigation_blocked() && !gui_network_usb_prompt_invalidated();
}

static void library_update_prompt_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED ||
        !gui_popup_is_visible(&library_update_prompt_popup)) return;
    library_prompt_request_t handled = library_prompt_request;
    gui_popup_hide(&library_update_prompt_popup);
    library_prompt_clear_request(&library_prompt_request);
    if (!library_prompt_can_run_action(&handled)) {
        library_prompt_card_status_t card_status = library_prompt_card_status();
        if (card_status == LIBRARY_PROMPT_CARD_CHANGED) library_prompt_card_changed();
        else if (gui_library_navigation_blocked())
            library_prompt_drop_all();
        else if (library_prompt_request_is_current(&handled)) {
            handled.state = LIBRARY_PROMPT_REQUEST_PENDING;
            library_prompt_request = handled;
        }
        return;
    }
    if (handled.reason == LIBRARY_PROMPT_LOAD_FAILED) start_library_rescan();
    else start_library_auto_rescan();
}

static void library_update_prompt_later_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    library_update_prompt_close(true);
}

static void library_update_prompt_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    library_update_prompt_close(true);
}

void build_library_update_prompt_popup(void) {
    library_update_prompt_popup.popup = build_confirm_popup_with_labels(
        "", LV_LABEL_LONG_WRAP, &library_update_prompt_title, "", &library_update_prompt_body,
        "", &library_update_prompt_confirm_label, accent_lv_color(), library_update_prompt_confirm_cb, NULL,
        TR("Later"), lv_color_white(), library_update_prompt_later_cb, NULL,
        library_update_prompt_backdrop_cb, &library_update_prompt_popup.backdrop);
}

void library_prompt_requeue_visible(void) {
    if (!gui_popup_is_visible(&library_update_prompt_popup)) return;
    gui_popup_hide(&library_update_prompt_popup);
    if (library_prompt_request_is_current(&library_prompt_request))
        library_prompt_request.state = LIBRARY_PROMPT_REQUEST_PENDING;
    else
        library_prompt_clear_request(&library_prompt_request);
}

void gui_library_boot_ready(uint32_t tick) {
    if (library_boot_ready_tick_set) return;
    library_boot_ready_tick = tick;
    library_boot_ready_tick_set = true;
}

void gui_library_poll_boot_prompt(void) {
    bool has_request = library_prompt_request.state != LIBRARY_PROMPT_REQUEST_NONE ||
                       library_prompt_queued_request.state != LIBRARY_PROMPT_REQUEST_NONE;
    library_prompt_card_status_t card_status = LIBRARY_PROMPT_CARD_CURRENT;
    if (has_request || boot_warmup_pending) card_status = library_prompt_card_status();
    if ((has_request || boot_warmup_pending) && card_status == LIBRARY_PROMPT_CARD_CHANGED) {
        library_prompt_card_changed();
        return;
    }
    if (card_status == LIBRARY_PROMPT_CARD_UNKNOWN) {
        library_prompt_requeue_visible();
        return;
    }
    if (has_request && gui_network_usb_prompt_invalidated()) {
        gui_library_suspend_boot_prompt();
        return;
    }
    if (has_request && gui_library_navigation_blocked()) {
        library_prompt_drop_all();
        return;
    }

    if (library_prompt_request.state != LIBRARY_PROMPT_REQUEST_NONE &&
        !library_prompt_request_is_current(&library_prompt_request)) {
        gui_popup_hide(&library_update_prompt_popup);
        library_prompt_clear_request(&library_prompt_request);
    }
    if (library_prompt_queued_request.state != LIBRARY_PROMPT_REQUEST_NONE &&
        !library_prompt_request_is_current(&library_prompt_queued_request))
        library_prompt_clear_request(&library_prompt_queued_request);
    library_prompt_activate_queued_request();

    if (boot_warmup_pending && library_prompt_environment_idle()) {
        int64_t song_count = metadata_db_get_song_count();
        library_thumbnail_status_t thumb_status;
        library_thumbnails_get_status(&thumb_status);
        if (song_count <= 0 || thumb_status.warmup_active) {
            boot_warmup_pending = false;
        } else if (!thumb_status.lazy_active) {
            boot_warmup_pending = false;
            library_thumbnails_start_warmup();
        }
    }

    if (library_prompt_request.state == LIBRARY_PROMPT_REQUEST_VISIBLE) {
        if (!gui_popup_is_visible(&library_update_prompt_popup) ||
            library_prompt_taken_over()) library_prompt_requeue_visible();
        return;
    }
    if (library_prompt_request.state != LIBRARY_PROMPT_REQUEST_PENDING ||
        !library_update_prompt_popup.popup || !library_prompt_environment_idle()) return;

    library_prompt_update_labels(library_prompt_request.reason);
    library_prompt_request.state = LIBRARY_PROMPT_REQUEST_VISIBLE;
    gui_popup_show(&library_update_prompt_popup);
}

/* Initialized before pthread_create and published by library_rescan_done_flag. */
static metadata_db_load_outcome_t reload_library_cache(void);

static void report_library_cache_load(void) {
    if (!library_cache_load_pending) return;
    library_cache_load_pending = false;
    switch (library_cache_load_outcome) {
        case METADATA_DB_LOAD_FAILED:
            library_prompt_enqueue(LIBRARY_PROMPT_LOAD_FAILED, library_cache_load_card_generation);
            break;
        case METADATA_DB_LOAD_UNMOUNTED: show_error_toast(TR("No SD card")); break;
        case METADATA_DB_LOAD_SUCCESS_RECOVERED: show_info_toast(TR(library_recovered_message)); break;
        case METADATA_DB_LOAD_SUCCESS_FRESH:
            library_prompt_enqueue(LIBRARY_PROMPT_NO_DATABASE, library_cache_load_card_generation);
            break;
        case METADATA_DB_LOAD_SUCCESS_NORMAL:
            if (library_cache_load_announce_loaded) show_info_toast(TR("Library loaded"));
            break;
    }
}

/* Keep the metadata scan worker at its existing background priority. The
 * thumbnail component has its own private copy for its three independent
 * worker lifetimes. */
static void library_worker_lower_priority(int nice_value) {
#ifdef __linux__
    errno = 0;
    int inherited = getpriority(PRIO_PROCESS, 0);
    if (errno == 0 && inherited < nice_value)
        (void)setpriority(PRIO_PROCESS, 0, nice_value);
#else
    (void)nice_value;
#endif
}

static void * library_rescan_thread_func(void * arg) {
    install_thread_crash_altstack(); /* see its own comment (main.c) */
#ifdef __linux__
    struct sched_param sp = { 0 };
    sched_setscheduler(0, SCHED_BATCH, &sp);
    library_worker_lower_priority(5);
#endif
    (void)arg;
    uint64_t started_ms = db_log_enabled() ? db_log_now_ms() : 0;
    DB_LOG("DB", "rescan_thread_begin rss_kb=%ld", db_log_rss_kb());
    library_scan_run();
    DB_LOG("DB", "rescan_thread_end songs=%lld elapsed_ms=%llu rss_kb=%ld",
           (long long) metadata_db_get_song_count(), (unsigned long long) (db_log_now_ms() - started_ms),
           db_log_rss_kb());
    atomic_store_explicit(&library_rescan_done_flag, true, memory_order_release); /* written last -- update_timer_cb only checks this flag */
    return NULL;
}

/* Controls automatic library rescanning on boot / SD mount. */
#define GUI_LIBRARY_AUTO_RESCAN_ENABLED true
bool gui_library_auto_rescan_enabled(void) {
    return current_settings.setup_complete && GUI_LIBRARY_AUTO_RESCAN_ENABLED;
}

/* 4MB stack size configured for library rescan thread to accommodate deep
 * call chains through metadata parsers and tagcache indexing. */
#define LIBRARY_RESCAN_THREAD_STACK_SIZE (4 * 1024 * 1024)

void library_rescan_free_target_paths(void) {
    library_scan_release();
}

static bool start_library_rescan_job(bool allow_rebuild, library_rescan_mode_t mode,
                                     char ** target_paths, int target_count,
                                     library_scan_scope_t scope, const char * scope_name,
                                     const char * scope_album_artist) {
    gui_library_invalidate_boot_prompt();
    boot_warmup_pending = false;
    /* Ignore request if a rescan is already running. */
    if (library_rescan_active) {
        for (int i = 0; i < target_count; i++) free(target_paths[i]);
        free(target_paths);
        return false;
    }
    DB_LOG("DB", "rescan_requested existing_songs=%lld rss_kb=%ld",
           (long long) metadata_db_get_song_count(), db_log_rss_kb());
    /* A metadata parser child is already a meaningful peak on this 56 MiB
     * target. Do not overlap it with a previous warmer or lazy cover decode;
     * cancellation is cooperative and bounded by the artwork timeout. */
    quiesce_thumbnails();
    library_rescan_mode = mode;
    library_scan_request_t scan_request = {
        .mode = mode == LIBRARY_RESCAN_FORCE_METADATA ? LIBRARY_SCAN_MODE_FORCE_METADATA
             : mode == LIBRARY_RESCAN_TARGETED ? LIBRARY_SCAN_MODE_TARGETED : LIBRARY_SCAN_MODE_NORMAL,
        .allow_rebuild = allow_rebuild,
        .join_albums = current_settings.join_albums,
        .target_paths = target_paths,
        .target_count = target_count,
        .scope = scope,
        .scope_name = scope_name,
        .scope_album_artist = scope_album_artist,
        .outcome_seed = !allow_rebuild && library_cache_load_pending ? library_cache_load_outcome
                                                                    : METADATA_DB_LOAD_SUCCESS_NORMAL,
        .books_rescan = gui_books_rescan,
        .playlists_rescan = rescan_playlists,
    };
    /* The card may have been swapped since the setting last changed: tools on
     * a computer read the join setting this scan applies from device.json. */
    if (sd_card_root_is_mounted()) (void) device_info_publish(SD_COMPAS_ROOT);
    library_scan_reset_cancel();
    library_scan_prepare(&scan_request);
    atomic_store_explicit(&library_rescan_done_flag, false, memory_order_relaxed);
    library_rescan_active = true;
    const char * busy_title = mode == LIBRARY_RESCAN_FORCE_METADATA ? TR("Refreshing\nall metadata...")
        : mode == LIBRARY_RESCAN_TARGETED ? TR("Refreshing\nmetadata...")
        : TR("Updating\nmusic database...");
    const char * busy_detail = mode == LIBRARY_RESCAN_TARGETED || mode == LIBRARY_RESCAN_FORCE_METADATA
            ? TR("Preparing metadata refresh...") : TR("Preparing music library...");
    library_rescan_token = gui_busy_show(busy_title, busy_detail);
    gui_busy_set_progress(library_rescan_token, 0);

    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    bool attr_initialized = pthread_attr_init(&attr) == 0;
    if (attr_initialized) {
        if (pthread_attr_setstacksize(&attr, LIBRARY_RESCAN_THREAD_STACK_SIZE) == 0) attr_ptr = &attr;
    }
    bool created = pthread_create(&library_rescan_thread, attr_ptr, library_rescan_thread_func, NULL) == 0;
    if (attr_initialized) pthread_attr_destroy(&attr);
    if (created) library_cache_load_pending = false;
    if (!created) {
        library_rescan_active = false;
        gui_busy_hide(library_rescan_token);
        library_scan_record_start_failure();
        library_rescan_free_target_paths();
        library_rescan_mode = LIBRARY_RESCAN_NORMAL;
        show_error_toast(TR("Thread launch failed"));
    }
    return created;
}

static void start_library_rescan_with_repair(bool allow_rebuild) {
    (void) start_library_rescan_job(allow_rebuild, LIBRARY_RESCAN_NORMAL, NULL, 0,
                                   LIBRARY_SCAN_SCOPE_NONE, NULL, NULL);
}

void start_library_rescan(void) {
    start_library_rescan_with_repair(true);
}

void start_library_auto_rescan(void) {
    start_library_rescan_with_repair(false);
}

void library_scan_once(void) {
    library_scan_run_full_current(gui_books_rescan, rescan_playlists);
}

bool gui_library_start_setup_scan(void (*on_complete)(bool success)) {
    if (!on_complete || library_setup_scan_complete || library_rescan_active ||
        library_rescan_success_pending || gui_library_sd_format_is_active()) return false;
    if (!start_library_rescan_job(true, LIBRARY_RESCAN_NORMAL, NULL, 0,
                                 LIBRARY_SCAN_SCOPE_NONE, NULL, NULL)) return false;
    library_setup_scan_complete = on_complete;
    return true;
}

static void finish_setup_scan(bool success) {
    void (*done)(bool) = library_setup_scan_complete;
    library_setup_scan_complete = NULL;
    if (done) done(success);
}

static int copy_scan_target_paths(const char * const * paths, int count, char *** out_paths) {
    return metadata_refresh_copy_unique_paths(paths, count, out_paths);
}

void start_library_metadata_refresh_paths(const char * const * paths, int count) {
    char ** copied = NULL;
    int unique_count = copy_scan_target_paths(paths, count, &copied);
    if (unique_count <= 0) {
        show_error_toast(TR("No songs to refresh"));
        return;
    }
    if (!start_library_rescan_job(true, LIBRARY_RESCAN_TARGETED, copied, unique_count,
                                  LIBRARY_SCAN_SCOPE_PATHS, NULL, NULL))
        show_info_toast(TR("Library is busy"));
}

void start_library_metadata_refresh_album(const char * album, const char * album_artist) {
    if (!album || !album[0] || !album_artist) {
        show_info_toast(TR("No songs to refresh"));
        return;
    }
    if (!start_library_rescan_job(true, LIBRARY_RESCAN_TARGETED, NULL, 0,
                                  LIBRARY_SCAN_SCOPE_ALBUM, album, album_artist))
        show_info_toast(TR("Library is busy"));
}

void start_library_metadata_refresh_artist(metadata_db_group_kind_t kind, const char * name) {
    if (!name || !name[0] || (kind != METADATA_DB_GROUP_ARTIST &&
                              kind != METADATA_DB_GROUP_ALBUM_ARTIST)) {
        show_info_toast(TR("No songs to refresh"));
        return;
    }
    library_scan_scope_t scope = kind == METADATA_DB_GROUP_ALBUM_ARTIST
        ? LIBRARY_SCAN_SCOPE_ALBUM_ARTIST : LIBRARY_SCAN_SCOPE_ARTIST;
    if (!start_library_rescan_job(true, LIBRARY_RESCAN_TARGETED, NULL, 0, scope, name, NULL))
        show_info_toast(TR("Library is busy"));
}

static void refresh_all_metadata_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&refresh_all_metadata_popup);
    (void) start_library_rescan_job(true, LIBRARY_RESCAN_FORCE_METADATA, NULL, 0,
                                   LIBRARY_SCAN_SCOPE_NONE, NULL, NULL);
}

static void refresh_all_metadata_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_popup_hide(&refresh_all_metadata_popup);
}

static void refresh_all_metadata_backdrop_cb(lv_event_t * e) {
    refresh_all_metadata_cancel_cb(e);
}

static bool library_cover_reload_storage_available(void) {
    if (!metadata_db_storage_current()) return false;
#ifndef HOST_BUILD
    if (!sd_card_root_is_mounted()) return false;
#endif
    return true;
}

static bool start_library_cover_reload_job(bool all_albums, const song_row_t * target_song,
                                           const char * album, const char * album_artist_key) {
    gui_library_invalidate_boot_prompt();
    boot_warmup_pending = false;
    if (library_rescan_active) {
        show_info_toast(TR("Library is busy"));
        return false;
    }
    if (!all_albums && (!target_song || !album || !album_artist_key)) {
        show_error_toast(TR("Album unavailable"));
        return false;
    }
    if (!library_cover_reload_storage_available()) {
        show_error_toast(TR("No SD card"));
        return false;
    }

    uint64_t target_key = 0;
    int64_t target_song_id = 0;
    if (!all_albums) {
        target_key = song_album_thumbnail_key(target_song);
        target_song_id = target_song->id;
    }

    quiesce_thumbnails();
    gui_player_begin_cover_reload();

    library_rescan_mode = all_albums ? LIBRARY_RESCAN_REFRESH_COVERS_ALL
                                     : LIBRARY_RESCAN_REFRESH_COVERS_ALBUM;
    library_scan_reset_cancel();
    library_rescan_active = true;

    if (all_albums) {
        library_thumbnails_clear_cache();
    } else {
        /* Any artist may alias this album or hold a "no art" entry that it
         * could now resolve; those records are cheap to re-derive. */
        library_thumbnails_invalidate_album(target_key);
    }
    gui_player_invalidate_cover_negatives();

    library_thumbnail_cover_request_t request = {
        .scope = all_albums ? LIBRARY_THUMB_COVER_ALL : LIBRARY_THUMB_COVER_ALBUM,
        .target_song_id = target_song_id,
        /* The cover worker forgets this album's cached failures before it reloads. */
        .album = all_albums ? NULL : album,
        .album_artist = all_albums ? NULL : album_artist_key
    };
    bool created = library_thumbnails_start_cover_reload(&request, library_thumbnail_show_busy);
    if (!created) {
        library_rescan_active = false;
        gui_player_finish_cover_reload(); /* releases any held player request */
        gui_busy_hide(library_rescan_token);
        library_rescan_mode = LIBRARY_RESCAN_NORMAL;
        show_error_toast(TR("Thread launch failed"));
    }
    return created;
}

void start_library_cover_reload_album(const char * album, const char * album_artist) {
    if (!album || !album[0] || !album_artist) {
        show_error_toast(TR("Album unavailable"));
        return;
    }
    song_row_t target_song;
    if (metadata_db_get_album_songs(album, album_artist, 0, &target_song, 1) != 1) {
        show_error_toast(TR("Album unavailable"));
        return;
    }
    (void) start_library_cover_reload_job(false, &target_song, album, album_artist);
}

static void start_library_cover_reload_all(void) {
    (void) start_library_cover_reload_job(true, NULL, NULL, NULL);
}

static void refresh_all_covers_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&refresh_all_covers_popup);
    start_library_cover_reload_all();
}

static void refresh_all_covers_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_popup_hide(&refresh_all_covers_popup);
}

static void refresh_all_covers_backdrop_cb(lv_event_t * e) {
    refresh_all_covers_cancel_cb(e);
}

void show_library_refresh_all_covers_prompt(void) {
    if (!refresh_all_covers_popup.popup) {
        refresh_all_covers_popup.popup = build_confirm_popup(
            TR("Refresh all covers?"), LV_LABEL_LONG_WRAP, NULL,
            TR("Removes saved covers and extracts them again. This may take a while."),
            TR("Refresh"), accent_lv_color(), refresh_all_covers_confirm_cb, NULL,
            TR("Cancel"), accent_lv_color(), refresh_all_covers_cancel_cb, NULL,
            refresh_all_covers_backdrop_cb, &refresh_all_covers_popup.backdrop);
    }
    gui_popup_show(&refresh_all_covers_popup);
}

void show_library_refresh_all_metadata_prompt(void) {
    if (!refresh_all_metadata_popup.popup) {
        refresh_all_metadata_popup.popup = build_confirm_popup(
            TR("Refresh all metadata?"), LV_LABEL_LONG_WRAP, NULL,
            TR("Reads the tags of every song again. This may take a while."),
            TR("Refresh"), accent_lv_color(), refresh_all_metadata_confirm_cb, NULL,
            TR("Cancel"), accent_lv_color(), refresh_all_metadata_cancel_cb, NULL,
            refresh_all_metadata_backdrop_cb, &refresh_all_metadata_popup.backdrop);
    }
    gui_popup_show(&refresh_all_metadata_popup);
}

/* SD reinsertion loads an existing saved database and queue without walking
 * the whole tree again. A card with no database still needs its first scan;
 * users can explicitly update a saved database after changing files. */
void reload_library_on_sd_reinsert(bool announce_loaded) {
    quiesce_thumbnails();
    library_thumbnails_clear_cache();
    playlist_files_refresh_async(PLAYLISTS_DIR);
    library_search_invalidate_catalog();
    library_search_invalidate_bindings();
    library_load_from_cache_only();
    library_cache_load_announce_loaded = announce_loaded;
    refresh_library_screens_after_reload(false); /* cache already cleared above, before the reload */
    report_library_cache_load();
    if (library_cache_load_outcome != METADATA_DB_LOAD_SUCCESS_FRESH &&
        metadata_db_get_song_count() > 0) {
        boot_warmup_pending = false;
        library_thumbnails_start_warmup();
    }
}

/* How long the "Library updated" success message stays up once a rescan
 * finishes, before falling back to Home -- purely so the user gets a
 * moment to actually read it, not a wait for anything real. */
#define LIBRARY_RESCAN_SUCCESS_MS 1500

static uint32_t library_rescan_success_since_tick = 0;

static void finish_library_cover_reload(bool cancelled) {
    library_rescan_mode_t completed_mode = library_rescan_mode;
    bool all_albums = completed_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL;
    library_thumbnail_status_t status;
    library_thumbnails_get_status(&status);
    int error_count = status.cover_error_count;
    library_rescan_active = false;
    library_thumbnails_finish_cover_reload();
    gui_player_finish_cover_reload();
    /* Pops back to the screen the reload was started from. */
    gui_busy_hide(library_rescan_token);
    library_rescan_free_target_paths();
    library_rescan_mode = LIBRARY_RESCAN_NORMAL;
    if (cancelled) return;
    if (error_count > 0) {
        if (all_albums) show_error_toast(TR("Some covers could not be refreshed"));
        else show_error_toast(TR("Could not reload cover"));
    } else if (all_albums) {
        show_info_toast(TR("Covers refreshed"));
    } else {
        show_info_toast(TR("Cover reloaded"));
    }
}

void poll_library_rescan(void) {
    library_thumbnails_retry_warmup();

    /* Keep the screen awake for the rescan progress display so the auto
     * screen-timeout does not trigger mid-rescan. */
    if (library_rescan_active || library_rescan_success_pending)
        lv_display_trigger_activity(NULL);

    if (library_rescan_success_pending) {
        if (lv_tick_elaps(library_rescan_success_since_tick) >= LIBRARY_RESCAN_SUCCESS_MS) {
            library_rescan_success_pending = false;
            if (!library_setup_scan_complete) nav_reset_to_home(); /* leaves the busy screen and discards any stale deeper screen */
        }
        return;
    }

    bool cover_reload = library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL ||
                        library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALBUM;
    if (cover_reload) {
        library_thumbnail_status_t status;
        library_thumbnails_get_status(&status);
        if (!status.cover_done) {
            int total = status.cover_total_count;
            int done = status.cover_done_count;
            if (total > 0) {
                if (done > total) done = total;
                int progress = (int)((int64_t)done * 100 / total);
                gui_busy_set_progress(library_rescan_token, progress);
                char detail[96];
                snprintf(detail, sizeof(detail),
                         library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL
                             ? TR("Refreshing covers\n%d of %d (%d%%)")
                             : TR("Reloading cover\n%d of %d (%d%%)"),
                         done, total, progress);
                gui_busy_set_detail(library_rescan_token, detail);
            } else {
                gui_busy_set_detail(library_rescan_token,
                    library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL
                        ? TR("Refreshing covers") : TR("Reloading cover"));
            }
            return;
        }
        finish_library_cover_reload(library_scan_cancel_pending() || status.cover_cancelled);
        return;
    }

    if (!library_rescan_active) {
        report_library_cache_load();
        return;
    }

    if (!atomic_load_explicit(&library_rescan_done_flag, memory_order_acquire)) {
        /* Still scanning -- total stays 0 until the initial file walk
         * finishes (see library_scan_once()), so there's nothing
         * meaningful to show yet in that window; the label just keeps
         * reading "Updating music database..." until then. */
        library_scan_progress_t progress;
        library_scan_get_progress(&progress);
        int total = progress.total;
        int done = progress.done;
        char detail[96];
        int phase = progress.phase;
        if (phase == LIBRARY_SCAN_PHASE_WALK) {
            int items = progress.walk_items;
            if (library_rescan_mode == LIBRARY_RESCAN_TARGETED)
                snprintf(detail, sizeof(detail), "%s", TR("Preparing metadata refresh"));
            else if (items > 0) snprintf(detail, sizeof(detail), TR("Looking for music files\n%d items checked"), items);
            else snprintf(detail, sizeof(detail), "%s", TR("Looking for music files"));
        } else if (phase == LIBRARY_SCAN_PHASE_TAGS && total > 0) {
            if (done > total) done = total;
            gui_busy_set_progress(library_rescan_token, (int32_t) ((int64_t) done * 100 / total));
            snprintf(detail, sizeof(detail), TR("Reading tags\n%d of %d songs (%d%%)"), done, total,
                     (int) ((int64_t) done * 100 / total));
        } else {
            gui_busy_set_progress(library_rescan_token, 100);
            if (library_rescan_mode == LIBRARY_RESCAN_TARGETED)
                snprintf(detail, sizeof(detail), "%s", TR("Saving music database"));
            else
                snprintf(detail, sizeof(detail), "%s", TR("Saving music database\nThis can take a while on large libraries"));
        }
        gui_busy_set_detail(library_rescan_token, detail);
        return;
    }

    library_rescan_mode_t completed_mode = library_rescan_mode;
    library_rescan_active = false;
    bool scan_cancelled = library_scan_cancel_pending();
    pthread_join(library_rescan_thread, NULL);

    library_scan_result_t scan_result;
    library_scan_copy_result(&scan_result);
    bool identity_changed = scan_result.identity_changed;
    bool published = scan_result.published;
    /* Cached track durations of files that are gone or changed are dropped. */
    if (published) duration_prune_request();
    if (published) invalidate_library_views_after_publication(completed_mode, identity_changed);
    if (scan_cancelled) {
        if (published) {
            if (completed_mode == LIBRARY_RESCAN_TARGETED) refresh_library_lists_in_place();
            else refresh_library_screens_after_reload(completed_mode == LIBRARY_RESCAN_NORMAL);
        }
        gui_busy_hide(library_rescan_token);
        if (!library_setup_scan_complete) nav_reset_to_home();
        library_rescan_free_target_paths();
        library_rescan_mode = LIBRARY_RESCAN_NORMAL;
        finish_setup_scan(false);
        return;
    }

    bool overall_success = scan_result.succeeded &&
                           scan_result.operation_outcome != METADATA_DB_LOAD_FAILED &&
                           scan_result.operation_outcome != METADATA_DB_LOAD_UNMOUNTED;
    if (published) {
        if (completed_mode == LIBRARY_RESCAN_TARGETED) refresh_library_lists_in_place();
        else refresh_library_screens_after_reload(completed_mode == LIBRARY_RESCAN_NORMAL);
    }

    /* Thumbnail generation is optional cache warming. It must not become a
     * second user-visible phase after a database scan -- no progress screen,
     * no toast. The worker yields when visible rows queue lazy work so those
     * decodes stay first. */
    if (!overall_success) {
        gui_busy_hide(library_rescan_token);
        if (!library_setup_scan_complete) nav_reset_to_home();
        if (scan_result.operation_outcome == METADATA_DB_LOAD_UNMOUNTED) {
            show_error_toast(TR("No SD card"));
        } else if (scan_result.load_blocked) {
            if (scan_result.operation_outcome == METADATA_DB_LOAD_SUCCESS_RECOVERED)
                show_info_toast(TR(library_recovered_message));
            else
                show_error_toast(TR("Library unavailable. Use Settings > Update Music Database to rebuild"));
        } else {
            show_error_toast(TR("Library update failed. Check SD card and retry"));
        }
        library_rescan_free_target_paths();
        library_rescan_mode = LIBRARY_RESCAN_NORMAL;
        finish_setup_scan(false);
        return;
    }
    if (published && completed_mode != LIBRARY_RESCAN_TARGETED) library_thumbnails_start_warmup();
    gui_busy_hide(library_rescan_token);

    if (completed_mode != LIBRARY_RESCAN_NORMAL) {
        /* Still set from the finished job; reset only when the next starts. */
        /* Parser failures, non-ENOENT stat errors and skipped folders all
         * mean some songs were not re-read. */
        if (scan_result.failed_count > 0 || scan_result.incomplete)
            show_error_toast(TR("Some songs could not be read"));
        else show_info_toast(TR("Metadata refreshed"));
    } else if (scan_result.operation_outcome == METADATA_DB_LOAD_SUCCESS_RECOVERED) {
        show_info_toast(scan_result.saved
                            ? (scan_result.incomplete ? TR("Library recovered and saved, some folders could not be read")
                                                         : TR("Library recovered and saved"))
                            : TR(library_recovered_message));
    } else {
        show_info_toast(scan_result.incomplete ? TR("Library updated, some folders could not be read")
                                                  : TR("Library updated"));
    }
    library_rescan_free_target_paths();
    library_rescan_mode = LIBRARY_RESCAN_NORMAL;
    /* A targeted refresh returns to the list it was started from, refreshed
     * in place; only full scans rebuild the library and reset to Home. */
    if (completed_mode == LIBRARY_RESCAN_TARGETED) return;
    if (library_setup_scan_complete) {
        finish_setup_scan(true);
        return;
    }
    library_rescan_success_pending = true;
    library_rescan_success_since_tick = lv_tick_get();
}

void gui_library_cancel_scan(void) {
    library_scan_request_cancel();
    if (library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL ||
        library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALBUM)
        library_thumbnails_cancel_cover_reload();
}

/* Overall scan progress, polled by update_timer_cb while library_rescan_active
 * is true to drive the "Updating music database..." screen's progress bar
 * (Settings > Update Music Database) -- purely cosmetic, for the user's
 * peace of mind that a rescan is actually moving rather than stuck, since
 * the underlying incremental scan (metadata_db.c's mtime/size cache) is
 * already fast on an unchanged library. _total is set once discovered_count
 * is known (library_scan_once(), before its spool-reading loop starts);
 * _done is advanced by that same loop, one file at a time (see
 * scan_one_song_into_db()'s own caller in library_scan_once()). */

/* Refreshes the persistent playlist cache (metadata_db.c) from
 * PLAYLISTS_DIR only (the SD card's Playlists folder), not a walk of the
 * whole music tree. Folded into library_scan_once() so it runs on Update
 * Music Database. Ordinary in-app create/delete update the cache with a
 * single insert/delete instead of calling this. */
void gui_library_poll_playlists(void) {
    gui_playlists_poll();
}

static void rescan_playlists(void) {
    /* Runs before library_scan_once()'s own mount check: never prune or
     * create folders against an unmounted, empty mount point. */
    bool mounted = sd_card_root_is_mounted();
    if (mounted) ensure_playlists_dir();
    playlist_files_reconcile(PLAYLISTS_DIR, mounted);
}

static metadata_db_load_outcome_t reload_library_cache(void) {
    library_scan_reset_progress();
    return metadata_db_reload();
}

/* Loads saved metadata without scanning and queues one UI report. */
void library_load_from_cache_only(void) {
    library_prompt_prepare_cache_load_card();
    /* A (re)opened library may be another card's: durations measured before this
     * point are not stored, and the cache reloads from the card that is there now. */
    duration_cache_set_mount_check(sd_card_root_is_mounted);
    duration_cache_new_storage_epoch();
    library_cache_load_card_generation = library_prompt_card_generation;
    library_cache_load_announce_loaded = false;
    library_cache_load_outcome = reload_library_cache();
    library_cache_load_generation++;
    library_cache_load_pending = true;
}

void gui_library_scan_join_for_shutdown(void) {
    if (library_rescan_active) {
        bool cover_reload = library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL ||
                            library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALBUM;
        if (cover_reload) gui_library_cancel_scan();
        if (cover_reload) library_thumbnails_join_cover_abort();
        else pthread_join(library_rescan_thread, NULL);
        library_rescan_active = false;
        gui_busy_hide(library_rescan_token);
        if (cover_reload) {
            gui_player_cancel_cover_reload();
            library_rescan_mode = LIBRARY_RESCAN_NORMAL;
        }
    }
}
