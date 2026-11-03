#include "../application/gui.h"
#include "../text/i18n/i18n.h"
#include "gui_network.h"
#include "../text/rendering/gui_text_input.h"
#include "../widgets/builders/screen_builders.h"
#include "../assets/assets.h"
#include "../../network/wifi/wifi_status.h"
#include "../../network/bluetooth/media/bt_remote_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "gui_network_internal.h"


static gui_busy_handle_t wifi_connect_saved_token = 0;
static gui_busy_handle_t wifi_connect_token = 0;
wifi_network_t wifi_scan_results[WIFI_MAX_RESULTS];
int wifi_scan_result_count = 0;
/* The scan worker stages results privately; the UI owns wifi_scan_results and
 * publishes the staged array only after joining, so snapshot refreshes cannot
 * observe partially written SSIDs or reorder data under the worker. */
static wifi_network_t wifi_scan_worker_results[WIFI_MAX_RESULTS];
static int wifi_scan_worker_result_count;
wifi_saved_network_t wifi_saved_results[WIFI_MAX_SAVED];
int wifi_saved_result_count = 0;
lv_obj_t * wifi_list;
lv_obj_t * wifi_rescan_btn;
bool wifi_enable_pending_feedback = false;
static char wifi_connect_pending_ssid[WIFI_MAX_SSID_LEN];
static lv_obj_t * wifi_setup_parent;
static bool wifi_setup_requested;
static bool wifi_setup_scan_started;
static bool wifi_setup_scan_finished;
static bool wifi_setup_enable_failed;

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    bool secured;
    bool connected;
    int saved_id;
} wifi_setup_row_t;

static void wifi_setup_render(void);
static void wifi_setup_start_scan_once(void);
static int wifi_setup_saved_id(const char * ssid);

/* Saved-network/configuration reads are deliberately kept separate from the
 * scan buffers.  The worker owns its staging snapshot; the UI copies it only
 * after joining the worker, so no LVGL code and no partially-written strings
 * can cross the thread boundary. */
typedef struct {
    wifi_saved_network_t saved[WIFI_MAX_SAVED];
    int saved_count;
    wifi_info_t info;
    bool info_connected;
    unsigned generation;
} wifi_settings_snapshot_t;

static pthread_t wifi_settings_snapshot_thread;
bool wifi_settings_snapshot_active = false;
static atomic_bool wifi_settings_snapshot_done_flag = false;
static atomic_uint wifi_settings_snapshot_generation = 1;
static wifi_settings_snapshot_t wifi_settings_snapshot_worker;
static unsigned wifi_settings_snapshot_cached_generation = 0;
static bool wifi_settings_snapshot_cached = false;
static uint32_t wifi_settings_snapshot_tick;
bool wifi_settings_snapshot_enabled = false;
static bool wifi_settings_snapshot_visible_changed;
wifi_info_t wifi_cached_info;
bool wifi_cached_info_connected = false;
static bool wifi_last_connected_saved_valid;
static int wifi_last_connected_saved_id = -1;
static char wifi_last_connected_saved_ssid[WIFI_MAX_SSID_LEN];
#ifndef HOST_BUILD
static bool remote_control_last_bt_powered;
static bt_remote_control_status_t remote_control_last_bt_status = BT_REMOTE_CONTROL_DISABLED;
#endif

static pthread_t wifi_connect_thread;
static bool wifi_connect_active = false;
static bool wifi_connect_saved_active = false;
static bool wifi_disconnect_active = false;
static bool wifi_scan_active;
static atomic_bool wifi_connect_done_flag = false;
static volatile bool wifi_connect_succeeded = false;

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    char password[256];
} wifi_connect_request_t;

/* True while a connect, saved-connect or disconnect worker owns the link. */
bool wifi_link_op_busy(void) {
    return wifi_connect_active || wifi_connect_saved_active || wifi_disconnect_active;
}

/* Re-read saved networks, link info and scan results after the link changed. */
static void wifi_refresh_after_link_change(void) {
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_start();
    start_wifi_scan();
}

/* wifi_control_connect*() returning true only confirms wpa_cli accepted the
 * configuration, not that association happened. Poll the real link state for
 * a few seconds before calling it a success. */
static bool wifi_wait_for_association(bool accepted) {
    bool associated = false;
    if (accepted) {
        for (int i = 0; i < 10 && !associated; i++) {
            int level;
            associated = wifi_get_status(&level);
            if (!associated) usleep(500000);
        }
    }
    return accepted && associated;
}

static void * wifi_connect_thread_func(void * arg) {
    wifi_connect_request_t * req = (wifi_connect_request_t *) arg;
    bool accepted = wifi_control_connect(req->ssid, req->password[0] != '\0' ? req->password : NULL);
    free(req);

    wifi_connect_succeeded = wifi_wait_for_association(accepted);
    atomic_store_explicit(&wifi_connect_done_flag, true, memory_order_release); /* written last -- poll_wifi_connect only checks this flag */
    return NULL;
}

static void start_wifi_connect(const char * ssid, const char * password) {
    if (wifi_link_op_busy()) {
        show_info_toast(TR("Wi-Fi is busy"));
        return;
    }
    wifi_connect_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    snprintf(req->ssid, sizeof(req->ssid), "%s", ssid);
    snprintf(req->password, sizeof(req->password), "%s", password ? password : "");

    atomic_store_explicit(&wifi_connect_done_flag, false, memory_order_relaxed);
    wifi_connect_active = true;

    wifi_connect_token = gui_busy_show(TR("Connecting to"), ssid);

        if (pthread_create(&wifi_connect_thread, NULL, wifi_connect_thread_func, req) != 0) {
        wifi_connect_active = false;
        free(req);
        gui_busy_hide(wifi_connect_token);
        show_error_toast(TR("Thread launch failed"));
    }
}

void poll_wifi_connect(void) {
    if (!wifi_connect_active || !atomic_load_explicit(&wifi_connect_done_flag, memory_order_acquire)) return;

    wifi_connect_active = false;
    pthread_join(wifi_connect_thread, NULL);
    gui_busy_hide(wifi_connect_token);
    if (!wifi_connect_succeeded) {
        show_error_toast(TR("Couldn't connect to Wi-Fi network"));
    }
    wifi_refresh_after_link_change();
}

/* Reconnecting to an already-memorized network -- deliberately a separate
 * thread/flag pair from the plain connect above (not a shared "mode" on the
 * same one) rather than reusing wifi_connect_active's state, matching this
 * file's existing convention of one dedicated pair per background Wi-Fi
 * operation (scan/connect/disconnect/forget already each have their own).
 * See wifi_control_connect_saved()'s own doc comment for why this can't
 * just call start_wifi_connect(ssid, NULL) instead. */
static pthread_t wifi_connect_saved_thread;
static atomic_bool wifi_connect_saved_done_flag = false;
static volatile bool wifi_connect_saved_succeeded = false;

typedef struct {
    int id;
} wifi_connect_saved_request_t;

static void * wifi_connect_saved_thread_func(void * arg) {
    wifi_connect_saved_request_t * req = (wifi_connect_saved_request_t *) arg;
    bool accepted = wifi_control_connect_saved(req->id);
    free(req);

    wifi_connect_saved_succeeded = wifi_wait_for_association(accepted);
    atomic_store_explicit(&wifi_connect_saved_done_flag, true, memory_order_release); /* written last -- poll_wifi_connect_saved only checks this flag */
    return NULL;
}

void start_wifi_connect_saved(int id, const char * ssid) {
    if (wifi_link_op_busy()) {
        show_info_toast(TR("Wi-Fi is busy"));
        return;
    }
    wifi_connect_saved_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    req->id = id;

    atomic_store_explicit(&wifi_connect_saved_done_flag, false, memory_order_relaxed);
    wifi_connect_saved_active = true;

    wifi_connect_saved_token = gui_busy_show(TR("Connecting to"), ssid);

        if (pthread_create(&wifi_connect_saved_thread, NULL, wifi_connect_saved_thread_func, req) != 0) {
        wifi_connect_saved_active = false;
        free(req);
        gui_busy_hide(wifi_connect_saved_token);
        show_error_toast(TR("Thread launch failed"));
    }
}

void poll_wifi_connect_saved(void) {
    if (!wifi_connect_saved_active || !atomic_load_explicit(&wifi_connect_saved_done_flag, memory_order_acquire)) return;

    wifi_connect_saved_active = false;
    pthread_join(wifi_connect_saved_thread, NULL);
    gui_busy_hide(wifi_connect_saved_token);
    if (!wifi_connect_saved_succeeded) {
        show_error_toast(TR("Couldn't connect to Wi-Fi network"));
    }
    wifi_refresh_after_link_change();
}

static pthread_t wifi_disconnect_thread;
static atomic_bool wifi_disconnect_done_flag = false;

static void * wifi_disconnect_thread_func(void * arg) {
    (void) arg;
    wifi_control_disconnect();
    atomic_store_explicit(&wifi_disconnect_done_flag, true, memory_order_release); /* written last -- poll_wifi_disconnect only checks this flag */
    return NULL;
}

void start_wifi_disconnect(void) {
    /* A second launch would overwrite the joinable handle of the worker still
     * running (leaking it, and later joining the wrong job). */
    if (wifi_link_op_busy()) {
        show_info_toast(TR("Wi-Fi is busy"));
        return;
    }
    atomic_store_explicit(&wifi_disconnect_done_flag, false, memory_order_relaxed);
    wifi_disconnect_active = true;
        if (pthread_create(&wifi_disconnect_thread, NULL, wifi_disconnect_thread_func, NULL) != 0) {
        wifi_disconnect_active = false;
        show_error_toast(TR("Thread launch failed"));
    }
}

void poll_wifi_disconnect(void) {
    if (!wifi_disconnect_active || !atomic_load_explicit(&wifi_disconnect_done_flag, memory_order_acquire)) return;
    wifi_disconnect_active = false;
    pthread_join(wifi_disconnect_thread, NULL);
    wifi_refresh_after_link_change();
}

void wifi_password_entered_cb(const char * text, void * user_data) {
    (void) user_data;
    start_wifi_connect(wifi_connect_pending_ssid, text);
}

void wifi_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    wifi_network_t * net = &wifi_scan_results[index];
    if (wifi_network_is_connected(net)) {
        start_wifi_disconnect();
        return;
    }

    snprintf(wifi_connect_pending_ssid, sizeof(wifi_connect_pending_ssid), "%s", net->ssid);
    if (net->secured) {
        show_text_entry(TR("Wi-Fi Password"), NULL, true, false, wifi_password_entered_cb, NULL);
    } else {
        start_wifi_connect(net->ssid, NULL);
    }
}

void wifi_rescan_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* An explicit rescan is a new discovery snapshot, not an append to the
     * previous one. Clear stale access points immediately so the screen
     * cannot imply that an old result is still currently visible while the
     * radio is collecting its replacement set. Saved networks remain in
     * their own section because they are configuration, not scan results. */
    wifi_scan_result_count = 0;
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_enabled = true;
    populate_wifi_screen(wifi_control_is_enabled());
    start_wifi_scan();
}

static void * wifi_settings_snapshot_thread_func(void * arg) {
    unsigned generation = (unsigned) (uintptr_t) arg;
    wifi_settings_snapshot_t * snapshot = &wifi_settings_snapshot_worker;

    snapshot->generation = generation;
    snapshot->saved_count = wifi_control_list_saved(snapshot->saved, WIFI_MAX_SAVED);
    snapshot->info_connected = wifi_control_get_info(&snapshot->info);
    atomic_store_explicit(&wifi_settings_snapshot_done_flag, true, memory_order_release);
    return NULL;
}

void wifi_settings_snapshot_invalidate(void) {
    atomic_fetch_add_explicit(&wifi_settings_snapshot_generation, 1, memory_order_acq_rel);
    wifi_settings_snapshot_cached = false;
    wifi_settings_snapshot_cached_generation = 0;
    wifi_cached_info_connected = false;
    wifi_saved_result_count = 0;
}

void wifi_settings_snapshot_start(void) {
    if (wifi_settings_snapshot_active || wifi_enable_pending_feedback || !wifi_settings_snapshot_enabled ||
        !gui_shell_wifi_effective_enabled()) return;

    unsigned generation = atomic_load_explicit(&wifi_settings_snapshot_generation, memory_order_acquire);
    if (wifi_settings_snapshot_cached && wifi_settings_snapshot_cached_generation == generation &&
        lv_tick_elaps(wifi_settings_snapshot_tick) < 5000) return;

    atomic_store_explicit(&wifi_settings_snapshot_done_flag, false, memory_order_relaxed);
    wifi_settings_snapshot_active = true;
    if (pthread_create(&wifi_settings_snapshot_thread, NULL, wifi_settings_snapshot_thread_func,
                       (void *) (uintptr_t) generation) != 0) {
        wifi_settings_snapshot_active = false;
    }
}

/* Index of the saved profile that is the live connection to `ssid`, or -1. */
int wifi_saved_current_index(const char * ssid) {
    for (int i = 0; i < wifi_saved_result_count; ++i) {
        if (wifi_saved_results[i].current && strcmp(wifi_saved_results[i].ssid, ssid) == 0)
            return i;
    }
    return -1;
}

static void wifi_remember_confirmed_saved_profile(void) {
    if (!wifi_cached_info_connected) return;
    int i = wifi_saved_current_index(wifi_cached_info.ssid);
    if (i < 0) return;
    wifi_last_connected_saved_valid = true;
    wifi_last_connected_saved_id = wifi_saved_results[i].id;
    snprintf(wifi_last_connected_saved_ssid, sizeof(wifi_last_connected_saved_ssid), "%s",
             wifi_saved_results[i].ssid);
}

/* Returns true when a completed snapshot was published.  Rendering is kept
 * in the UI poller, never in the worker, and a generation mismatch discards
 * results collected across an enable/disable transition. */
static bool poll_wifi_settings_snapshot(void) {
    if (!wifi_settings_snapshot_active ||
        !atomic_load_explicit(&wifi_settings_snapshot_done_flag, memory_order_acquire)) return false;

    wifi_settings_snapshot_active = false;
    pthread_join(wifi_settings_snapshot_thread, NULL);

    unsigned generation = atomic_load_explicit(&wifi_settings_snapshot_generation, memory_order_acquire);
    if (wifi_settings_snapshot_worker.generation != generation) {
        wifi_settings_snapshot_start();
        return false;
    }

    bool previous_snapshot_valid = wifi_settings_snapshot_cached &&
        wifi_settings_snapshot_cached_generation == generation;
    bool visible_changed = !previous_snapshot_valid ||
        wifi_saved_result_count != wifi_settings_snapshot_worker.saved_count ||
        wifi_cached_info_connected != wifi_settings_snapshot_worker.info_connected;
    if (!visible_changed) {
        for (int i = 0; i < wifi_settings_snapshot_worker.saved_count; ++i) {
            if (wifi_saved_results[i].id != wifi_settings_snapshot_worker.saved[i].id ||
                strcmp(wifi_saved_results[i].ssid, wifi_settings_snapshot_worker.saved[i].ssid) != 0 ||
                wifi_saved_results[i].current != wifi_settings_snapshot_worker.saved[i].current ||
                wifi_saved_results[i].disabled != wifi_settings_snapshot_worker.saved[i].disabled ||
                wifi_saved_results[i].temp_disabled != wifi_settings_snapshot_worker.saved[i].temp_disabled) {
                visible_changed = true;
                break;
            }
        }
    }
    /* Only association affects these rows. IP, gateway and signal changes
     * still update the info screen, but must not rebuild a scrolling list. */
    if (!visible_changed && wifi_settings_snapshot_worker.info_connected &&
        strcmp(wifi_cached_info.ssid, wifi_settings_snapshot_worker.info.ssid) != 0)
        visible_changed = true;

    wifi_saved_result_count = wifi_settings_snapshot_worker.saved_count;
    memcpy(wifi_saved_results, wifi_settings_snapshot_worker.saved,
           sizeof(wifi_saved_results[0]) * (size_t) wifi_saved_result_count);
    wifi_cached_info = wifi_settings_snapshot_worker.info;
    wifi_cached_info_connected = wifi_settings_snapshot_worker.info_connected;
    /* Remember only a profile that this completed snapshot confirms as both
     * associated and CURRENT. After disconnect, CURRENT may be cleared. */
    wifi_remember_confirmed_saved_profile();
    wifi_settings_snapshot_cached_generation = generation;
    wifi_settings_snapshot_cached = true;
    wifi_settings_snapshot_tick = lv_tick_get();
    wifi_settings_snapshot_visible_changed = visible_changed;
    return true;
}

bool wifi_cached_info_is_current(void) {
    return wifi_settings_snapshot_cached &&
           wifi_settings_snapshot_cached_generation ==
               atomic_load_explicit(&wifi_settings_snapshot_generation, memory_order_acquire);
}

/* A fresh status snapshot is authoritative, including its disconnected state.
 * Scan's is_current bit is only a fallback before any status snapshot exists. */
bool wifi_network_is_connected(const wifi_network_t * net) {
    if (!net) return false;
    if (wifi_cached_info_is_current())
        return wifi_cached_info_connected && strcmp(net->ssid, wifi_cached_info.ssid) == 0;
    return net->is_current;
}

static void set_wifi_rescan_active(bool active) {
    if (!wifi_rescan_btn) return;
    set_header_refresh_action_busy(wifi_rescan_btn, active);
}

static void wifi_setup_row_delete_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_DELETE) free(lv_event_get_user_data(e));
}

static void wifi_setup_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    wifi_setup_row_t * row = lv_event_get_user_data(e);
    if (!row || row->connected) return;
    if (wifi_link_op_busy()) {
        show_info_toast(TR("Wi-Fi is busy"));
        return;
    }
    snprintf(wifi_connect_pending_ssid, sizeof(wifi_connect_pending_ssid), "%s", row->ssid);
    int saved_id = wifi_setup_saved_id(row->ssid);
    if (saved_id < 0) saved_id = row->saved_id;
    if (saved_id >= 0) {
        start_wifi_connect_saved(saved_id, row->ssid);
    } else if (row->secured) {
        show_text_entry(TR("Wi-Fi Password"), NULL, true, false, wifi_password_entered_cb, NULL);
    } else {
        start_wifi_connect(row->ssid, NULL);
    }
}

static int wifi_setup_saved_id(const char * ssid) {
    if (!wifi_cached_info_is_current()) return -1;
    for (int i = 0; i < wifi_saved_result_count; ++i) {
        if (strcmp(wifi_saved_results[i].ssid, ssid) != 0) continue;
        int preferred = wifi_saved_display_index(i);
        return wifi_saved_results[preferred].id;
    }
    return -1;
}

bool wifi_ssid_is_saved(const char * ssid) {
    if (!ssid) return false;
    for (int i = 0; i < wifi_saved_result_count; ++i)
        if (strcmp(wifi_saved_results[i].ssid, ssid) == 0) return true;
    return false;
}

/* 3 = active profile, 2 = last confirmed profile, 1 = usable, 0 = disabled. */
static int wifi_saved_rank(const wifi_saved_network_t * n) {
    bool remembered = wifi_last_connected_saved_valid &&
        n->id == wifi_last_connected_saved_id &&
        strcmp(n->ssid, wifi_last_connected_saved_ssid) == 0;
    return n->current ? 3 : remembered ? 2 : (!n->disabled && !n->temp_disabled) ? 1 : 0;
}

/* Keep saved-profile entries (and their callback IDs) untouched, but render
 * only one row per SSID. Prefer the active profile, the last confirmed
 * profile, then a profile that is not disabled. */
int wifi_saved_display_index(int candidate) {
    if (candidate < 0 || candidate >= wifi_saved_result_count) return candidate;
    const char * ssid = wifi_saved_results[candidate].ssid;
    int best = candidate;
    int best_rank = wifi_saved_rank(&wifi_saved_results[candidate]);
    for (int i = 0; i < wifi_saved_result_count; ++i) {
        if (strcmp(wifi_saved_results[i].ssid, ssid) != 0) continue;
        int rank = wifi_saved_rank(&wifi_saved_results[i]);
        if (rank > best_rank || (rank == best_rank && i < best)) {
            best = i;
            best_rank = rank;
        }
    }
    return best;
}

/* Row subtitle for a scanned network; `saved` only applies to unconnected rows. */
const char * wifi_network_status_text(const wifi_network_t * net, bool connected, bool saved) {
    if (connected) return net->secured ? TR("Secured · Connected") : TR("Open · Connected");
    if (saved) return TR("Saved network");
    return net->secured ? TR("Secured network") : TR("Open network");
}

/* Signal-strength icon (level clamped to the 0..3 asset range). */
const char * wifi_signal_icon(int level) {
    char icon_path[40];
    int clamped = level < 0 ? 0 : level > 3 ? 3 : level;
    snprintf(icon_path, sizeof(icon_path), "topbar/wifi_connect_%d.png", clamped);
    return asset_path(icon_path);
}

/* Index of the first scan result that is the live connection, or -1. */
int wifi_scan_connected_index(void) {
    for (int i = 0; i < wifi_scan_result_count; ++i) {
        if (wifi_network_is_connected(&wifi_scan_results[i])) return i;
    }
    return -1;
}

static void wifi_setup_add_row(const wifi_network_t * net) {
    lv_obj_t * parent = wifi_setup_parent;
    if (!parent) return;
    lv_obj_t * row_obj = lv_obj_create(parent);
    lv_obj_set_width(row_obj, LV_PCT(100));
    lv_obj_set_height(row_obj, BOARD_SCALE_PX(76));
    lv_obj_set_style_radius(row_obj, BOARD_SCALE_PX(16), 0);
    lv_obj_add_style(row_obj, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(row_obj, 0, 0);
    lv_obj_set_style_pad_all(row_obj, 0, 0);
    lv_obj_remove_flag(row_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row_obj, LV_OBJ_FLAG_CLICKABLE);

    wifi_setup_row_t * context = calloc(1, sizeof(*context));
    if (!context) { lv_obj_delete(row_obj); return; }
    size_t ssid_len = strnlen(net->ssid, sizeof(context->ssid) - 1);
    memcpy(context->ssid, net->ssid, ssid_len);
    context->ssid[ssid_len] = '\0';
    context->secured = net->secured;
    context->connected = wifi_network_is_connected(net);
    context->saved_id = wifi_setup_saved_id(net->ssid);
    lv_obj_add_event_cb(row_obj, wifi_setup_row_delete_cb, LV_EVENT_DELETE, context);
    lv_obj_add_event_cb(row_obj, wifi_setup_row_click_cb, LV_EVENT_CLICKED, context);

    lv_obj_t * title = lv_label_create(row_obj);
    lv_label_set_text(title, net->ssid);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(10), BOARD_SCALE_PX(8));

    lv_obj_t * status = lv_label_create(row_obj);
    lv_label_set_text(status, wifi_network_status_text(net, context->connected, context->saved_id >= 0));
    lv_obj_add_style(status, context->connected ? gui_theme_accent_style() : &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(status, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(status, LV_PCT(100));
    lv_obj_align(status, LV_ALIGN_BOTTOM_LEFT, BOARD_SCALE_PX(10), -BOARD_SCALE_PX(8));

    lv_obj_t * signal = lv_image_create(row_obj);
    lv_image_set_src(signal, wifi_signal_icon(net->signal_level));
    lv_obj_set_size(signal, BOARD_SCALE_PX(28), BOARD_SCALE_PX(28));
    lv_image_set_inner_align(signal, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(signal, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(10), 0);
    lv_obj_set_width(title, LV_PCT(72));
    lv_obj_set_width(status, LV_PCT(72));
}

static void wifi_setup_render(void) {
    lv_obj_t * parent = wifi_setup_parent;
    if (!parent) return;
    lv_obj_t * scroll_owner = parent;
    while (scroll_owner && !lv_obj_has_flag(scroll_owner, LV_OBJ_FLAG_SCROLLABLE))
        scroll_owner = lv_obj_get_parent(scroll_owner);
    lv_coord_t scroll_y = scroll_owner ? lv_obj_get_scroll_y(scroll_owner) : 0;
    lv_obj_clean(parent);
    if (wifi_setup_enable_failed) {
        build_list_message(parent, TR("Wi-Fi could not be enabled"), TR("Check the wireless switch and try again."));
        goto restore_scroll;
    }
    if (!gui_shell_wifi_enabled_and_settled()) {
        build_list_message(parent, TR("Turning Wi-Fi on"), TR("Nearby networks will appear when Wi-Fi is ready."));
        goto restore_scroll;
    }
    if (wifi_scan_active || (!wifi_setup_scan_finished && wifi_setup_scan_started)) {
        build_list_message(parent, TR("Searching for networks"), TR("This can take a few seconds."));
        goto restore_scroll;
    }
    if (wifi_scan_result_count == 0) {
        build_list_message(parent, TR("No networks found"), TR("Try scanning again or add a hidden network."));
        goto restore_scroll;
    }
    int connected_index = wifi_scan_connected_index();
    for (int i = 0; i < wifi_scan_result_count; ++i)
        wifi_setup_add_row(&wifi_scan_results[rotate_to_front(i, connected_index)]);
restore_scroll:
    if (scroll_owner) {
        lv_obj_update_layout(scroll_owner);
        lv_obj_scroll_to_y(scroll_owner, scroll_y, LV_ANIM_OFF);
    }
}

static void wifi_setup_start_scan_once(void) {
    if (!wifi_setup_requested || !gui_shell_wifi_enabled_and_settled()) return;
    wifi_settings_snapshot_enabled = true;
    wifi_settings_snapshot_start();
    if (wifi_setup_scan_started) return;
    if (wifi_scan_active) {
        wifi_setup_scan_started = true;
        wifi_setup_scan_finished = false;
        return;
    }
    wifi_setup_scan_finished = false;
    wifi_scan_result_count = 0;
    bool started = start_wifi_scan();
    wifi_setup_scan_started = true;
    wifi_setup_scan_finished = !started;
}

static void wifi_setup_parent_delete_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_DELETE && lv_event_get_target(e) == wifi_setup_parent) {
        wifi_setup_parent = NULL;
    }
}

void gui_network_setup_wifi_prepare(void) {
    wifi_setup_requested = true;
    wifi_setup_enable_failed = !gui_shell_wifi_ensure_enabled();
    if (!wifi_setup_enable_failed) wifi_setup_start_scan_once();
    wifi_setup_render();
}

void gui_network_setup_wifi_mount(lv_obj_t * parent) {
    if (wifi_setup_parent && wifi_setup_parent != parent) wifi_setup_parent = NULL;
    wifi_setup_parent = parent;
    if (parent) lv_obj_add_event_cb(parent, wifi_setup_parent_delete_cb, LV_EVENT_DELETE, NULL);
    wifi_setup_render();
}

void gui_network_setup_wifi_rescan(void) {
    if (!wifi_setup_requested) gui_network_setup_wifi_prepare();
    if (wifi_scan_active) {
        wifi_setup_scan_started = true;
        wifi_setup_scan_finished = false;
        wifi_setup_render();
        return;
    }
    if (!gui_shell_wifi_enabled_and_settled()) {
        wifi_setup_enable_failed = !gui_shell_wifi_ensure_enabled();
        wifi_setup_render();
        return;
    }
    wifi_setup_enable_failed = false;
    wifi_setup_scan_started = false;
    wifi_setup_scan_finished = false;
    wifi_scan_result_count = 0;
    wifi_setup_start_scan_once();
    wifi_setup_render();
}

bool wifi_pending_ssid_set(const char * text) {
    size_t len = text ? strlen(text) : 0;
    if (len == 0 || len >= WIFI_MAX_SSID_LEN) {
        show_error_toast(TR("SSID must be 1 to 32 bytes"));
        return false;
    }
    memcpy(wifi_connect_pending_ssid, text, len + 1);
    return true;
}

static void wifi_setup_hidden_ssid_cb(const char * text, void * user_data) {
    (void) user_data;
    if (wifi_link_op_busy()) {
        show_info_toast(TR("Wi-Fi is busy"));
        return;
    }
    if (!wifi_pending_ssid_set(text)) return;
    show_text_entry(TR("Wi-Fi Password"), NULL, true, false, wifi_password_entered_cb, NULL);
}

void gui_network_setup_wifi_add_hidden(void) {
    if (!gui_shell_wifi_enabled_and_settled()) {
        show_info_toast(TR("Wi-Fi is still starting"));
        return;
    }
    if (wifi_link_op_busy()) {
        show_info_toast(TR("Wi-Fi is busy"));
        return;
    }
    show_text_entry(TR("Network Name (SSID)"), NULL, false, false, wifi_setup_hidden_ssid_cb, NULL);
}

bool gui_network_setup_wifi_connected(void) {
    return gui_shell_wifi_enabled_and_settled() && wifi_cached_info_is_current() &&
           wifi_cached_info_connected && lv_tick_elaps(wifi_settings_snapshot_tick) < 10000;
}

static pthread_t wifi_scan_thread;
static atomic_bool wifi_scan_done_flag = false;

static void * wifi_scan_thread_func(void * arg) {
    (void) arg;
    /* No auto-enable here anymore -- the screen's own toggle row is now the
     * explicit way to turn Wi-Fi on, so a stray Rescan tap while it's off
     * should just find nothing rather than silently flipping the radio on
     * behind the toggle's back. */
    wifi_control_scan_start();
    sleep(3); /* wpa_cli's scan is async -- give the radio time before reading scan_results */
    wifi_scan_worker_result_count = wifi_control_get_results(wifi_scan_worker_results, WIFI_MAX_RESULTS);
    atomic_store_explicit(&wifi_scan_done_flag, true, memory_order_release); /* written last -- poll_wifi_scan only checks this flag */
    return NULL;
}

/* Runs fully in the background, same as the stock player -- no busy
 * screen, no navigation at all. The list just updates in place
 * (populate_wifi_screen()) once the scan finishes, whichever screen
 * the user happens to be on by then. An earlier version pushed a
 * "Scanning for networks..." interstitial shared with bt scan/the bt
 * toggle's own overlay -- see git history if that's ever needed again, but
 * it was also the source of a real, repeatedly-hit stuck-screen bug
 * (multiple uncoordinated users of one shared overlay, plus the overlay
 * getting buried under further navigation), which not having an overlay at
 * all sidesteps entirely. */
bool start_wifi_scan(void) {
    if (wifi_scan_active) return false;

    atomic_store_explicit(&wifi_scan_done_flag, false, memory_order_relaxed);
    wifi_scan_active = true;
    set_wifi_rescan_active(true);

    if (pthread_create(&wifi_scan_thread, NULL, wifi_scan_thread_func, NULL) != 0) {
        wifi_scan_active = false;
        set_wifi_rescan_active(false);
        return false;
    }
    return true;
}

void poll_wifi_scan(void) {
    /* Keep both the rendered rows and their indexed result arrays stable
     * throughout a touch/kinetic scroll. Completed workers remain staged
     * until release, rather than deleting LVGL's active event target. */
    if (wifi_screen && gui_navigation_is_top(wifi_screen)) {
        if (gui_navigation_transition_in_progress()) return;
        if (network_list_touch_active(wifi_list)) return;
    }
    if (wifi_settings_snapshot_enabled && !gui_shell_wifi_effective_enabled()) {
        wifi_settings_snapshot_enabled = false;
        wifi_settings_snapshot_invalidate();
    }
    bool snapshot_published = poll_wifi_settings_snapshot();
    bool scan_published = false;
    if (wifi_scan_active && atomic_load_explicit(&wifi_scan_done_flag, memory_order_acquire)) {
        wifi_scan_active = false;
        pthread_join(wifi_scan_thread, NULL);
        wifi_scan_result_count = wifi_scan_worker_result_count;
        memcpy(wifi_scan_results, wifi_scan_worker_results,
               sizeof(wifi_scan_results[0]) * (size_t) wifi_scan_result_count);
        set_wifi_rescan_active(false);
        scan_published = true;
    }
    if (scan_published && wifi_setup_scan_started) wifi_setup_scan_finished = true;

    bool snapshot_visible_changed = snapshot_published && wifi_settings_snapshot_visible_changed;
    if ((snapshot_visible_changed || scan_published) &&
        wifi_screen && gui_navigation_is_top(wifi_screen))
        populate_wifi_screen(wifi_control_is_enabled());
    if (snapshot_visible_changed || scan_published) wifi_setup_render();

    if (snapshot_published) {
        if (wifi_info_screen && gui_navigation_is_top(wifi_info_screen)) populate_wifi_info_screen();
        if (remote_control_screen && gui_navigation_is_top(remote_control_screen)) remote_control_refresh_address();
    }
    if (remote_control_screen && gui_navigation_is_top(remote_control_screen))
        remote_control_refresh_pin_label();
#ifndef HOST_BUILD
    /* These snapshots are updated by the service thread / regular shell poll;
     * never query bluetoothd from the UI thread. */
    bt_remote_control_status_t bt_status = bt_remote_control_status();
    if (remote_control_screen && gui_navigation_is_top(remote_control_screen) &&
        (remote_control_last_bt_powered != bt_is_powered_cached ||
         remote_control_last_bt_status != bt_status)) {
        remote_control_last_bt_powered = bt_is_powered_cached;
        remote_control_last_bt_status = bt_status;
        remote_control_refresh_address();
    }
#endif
    if ((wifi_screen && gui_navigation_is_top(wifi_screen)) ||
        (wifi_setup_requested && !current_settings.setup_complete &&
         gui_shell_wifi_enabled_and_settled()) ||
        (wifi_info_screen && gui_navigation_is_top(wifi_info_screen)) ||
        (remote_control_screen && gui_navigation_is_top(remote_control_screen)))
        wifi_settings_snapshot_start();
}

static void show_wifi_toggle_pending_async(void * user_data) {
    bool enabled = (bool) (intptr_t) user_data;
    if (!gui_navigation_is_top(gui_network_get_wifi_screen())) return;

    wifi_enable_pending_feedback = enabled;
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_enabled = enabled;
    if (enabled) wifi_scan_result_count = 0;
    populate_wifi_screen(enabled);
    set_wifi_rescan_active(wifi_scan_active);
}

void gui_network_show_wifi_toggle_pending(bool enabled) {
    /* Rebuilding wifi_list synchronously from its toggle row callback would
     * delete LVGL's active event target. Defer by one UI turn, matching the
     * safe pattern used elsewhere for destructive screen rebuilds. */
    lv_async_call(show_wifi_toggle_pending_async, (void *) (intptr_t) enabled);
}

void gui_network_wifi_toggle_completed(bool enabled) {
    wifi_enable_pending_feedback = false;
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_enabled = enabled;
    if (enabled) wifi_settings_snapshot_start();
    if (wifi_setup_requested) {
        wifi_setup_enable_failed = !enabled && !gui_shell_wifi_effective_enabled();
        if (enabled) wifi_setup_start_scan_once();
        wifi_setup_render();
    }
    if (!gui_navigation_is_top(gui_network_get_wifi_screen())) {
        set_wifi_rescan_active(wifi_scan_active);
        return;
    }

    populate_wifi_screen(enabled);
    if (enabled) start_wifi_scan();
    else set_wifi_rescan_active(wifi_scan_active);
}

static pthread_t wifi_forget_thread;
static bool wifi_forget_active = false;
static atomic_bool wifi_forget_done_flag = false;

typedef struct {
    int id;
} wifi_forget_request_t;

static void * wifi_forget_thread_func(void * arg) {
    wifi_forget_request_t * req = (wifi_forget_request_t *) arg;
    wifi_control_forget(req->id);
    free(req);
    atomic_store_explicit(&wifi_forget_done_flag, true, memory_order_release); /* written last -- poll_wifi_forget only checks this flag */
    return NULL;
}

void start_wifi_forget(int id) {
    /* Same handle-overwrite hazard as start_wifi_disconnect(). */
    if (wifi_forget_active) {
        show_info_toast(TR("Wi-Fi is busy"));
        return;
    }
    wifi_forget_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    req->id = id;
    atomic_store_explicit(&wifi_forget_done_flag, false, memory_order_relaxed);
    wifi_forget_active = true;
        if (pthread_create(&wifi_forget_thread, NULL, wifi_forget_thread_func, req) != 0) {
        wifi_forget_active = false;
        free(req);
    }
}

void poll_wifi_forget(void) {
    if (!wifi_forget_active || !atomic_load_explicit(&wifi_forget_done_flag, memory_order_acquire)) return;
    wifi_forget_active = false;
    pthread_join(wifi_forget_thread, NULL);
    wifi_refresh_after_link_change();
}

/* Lifecycle hooks used by gui_network_init/teardown/has_background_work/
 * cancel_background_work. */
void gui_network_wifi_teardown(void) {
    wifi_setup_parent = NULL;
    wifi_setup_requested = false;
    wifi_setup_scan_started = false;
    wifi_setup_scan_finished = false;
    wifi_setup_enable_failed = false;
    wifi_settings_snapshot_enabled = false;
    wifi_settings_snapshot_invalidate();
}

bool gui_network_wifi_has_work(void) {
    return wifi_link_op_busy() || wifi_scan_active || wifi_forget_active ||
           wifi_settings_snapshot_active;
}

void gui_network_wifi_cancel(void) {
    if (wifi_connect_active) {
        pthread_join(wifi_connect_thread, NULL);
        wifi_connect_active = false;
        gui_busy_hide(wifi_connect_token);
    }
    if (wifi_connect_saved_active) {
        pthread_join(wifi_connect_saved_thread, NULL);
        wifi_connect_saved_active = false;
        gui_busy_hide(wifi_connect_saved_token);
    }
    if (wifi_disconnect_active) {
        pthread_join(wifi_disconnect_thread, NULL);
        wifi_disconnect_active = false;
    }
    if (wifi_scan_active) {
        pthread_join(wifi_scan_thread, NULL);
        wifi_scan_active = false;
    }
    if (wifi_forget_active) {
        pthread_join(wifi_forget_thread, NULL);
        wifi_forget_active = false;
    }
    if (wifi_settings_snapshot_active) {
        pthread_join(wifi_settings_snapshot_thread, NULL);
        wifi_settings_snapshot_active = false;
    }
}
