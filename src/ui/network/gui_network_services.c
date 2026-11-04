#include "../application/gui.h"
#include "../text/i18n/i18n.h"
#include "gui_network.h"
#include "../../network/remote/import/remote_import_files.h"
#include "../../network/remote/discovery/remote_control_mdns.h"
#include "../widgets/builders/screen_builders.h"
#include "subsonic/gui_subsonic.h"
#include "../../network/airplay/airplay_control.h"
#include "../../network/services/dlna_control.h"
#include "../../network/remote/control/remote_control.h"
#include "../../network/bluetooth/media/bt_remote_control.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "gui_network_internal.h"

typedef enum {
    NETWORK_SERVICE_IMPORT,
    NETWORK_SERVICE_REMOTE,
    NETWORK_SERVICE_DLNA,
    NETWORK_SERVICE_COUNT
} network_service_t;

static bool network_service_enqueue(network_service_t service, bool start, bool force_stop);
#ifndef HOST_BUILD
static void handle_remote_import_request(bool start);
#endif
static lv_obj_t * dlna_screen;
lv_obj_t * remote_control_screen;

/* The import file API runs only as a session requested through Remote
 * Control; the player has no local Import screen. True from an accepted
 * remote start until its stop is enqueued, so Wi-Fi loss knows whether a
 * teardown is still needed. */
static bool remote_import_active = false;
static bool remote_import_stop_pending = false;
/* Wi-Fi-loss stops skip the automatic rescan: nothing finished importing. */
static bool remote_import_stop_suppress_rescan = false;
/* UI-thread-only transient radio-sleep state. This never changes the saved RC
 * preference; it records whether this sleep cycle accepted an RC stop. */
static bool remote_radio_sleep_prepared = false;
static bool remote_radio_sleep_stop_queued = false;
/* Worker completions must not be inferred from the public state snapshot:
 * an authenticated request can replace that snapshot before the UI polls. */
enum {
    REMOTE_IMPORT_WORKER_START_FAILED = 1u << 0,
    REMOTE_IMPORT_WORKER_STOP_DONE = 1u << 1,
};
static atomic_uint remote_import_worker_events = 0;

/* All service lifecycle calls below are process/socket teardown or setup and
 * must not run on the LVGL thread.  Keep one bounded, joinable worker and a
 * tiny FIFO so Wi-Fi-loss cleanup can serialize several stops without ever
 * overlapping a start/stop for the same service.  The worker contains no UI
 * code; completion is joined and consumed by poll_network_service_ops(). */
typedef struct {
    network_service_t service;
    bool start;
} network_service_job_t;

#define NETWORK_SERVICE_QUEUE_CAPACITY 8
static pthread_mutex_t network_service_mutex = PTHREAD_MUTEX_INITIALIZER;
static network_service_job_t network_service_queue[NETWORK_SERVICE_QUEUE_CAPACITY];
static unsigned network_service_queue_head;
static unsigned network_service_queue_count;
static bool network_service_busy_flags[NETWORK_SERVICE_COUNT];
static bool network_service_stop_queued[NETWORK_SERVICE_COUNT];
static bool network_service_worker_active;
static pthread_t network_service_thread;
static atomic_bool network_service_done = false;
/* Set while gui_network_services_cancel() is shutting the worker down: queued
 * start jobs are dropped (only stops still run) so shutdown never brings a
 * service up, nor skips the worker path's remote-control check and rollback. */
static atomic_bool network_service_cancelling = false;

static void poll_network_service_ops(void);

/* Removes the oldest queued job; the caller holds network_service_mutex. */
static bool network_service_pop_locked(network_service_job_t * out) {
    if (network_service_queue_count == 0) return false;
    *out = network_service_queue[network_service_queue_head];
    network_service_queue_head = (network_service_queue_head + 1) % NETWORK_SERVICE_QUEUE_CAPACITY;
    network_service_queue_count--;
    return true;
}

/* Remote Control: the HTTP listener plus the Bluetooth RFCOMM service. */
static void network_service_run_remote(bool start) {
    if (start) {
        remote_control_start();
#ifndef HOST_BUILD
        bt_remote_control_start();
#endif
    } else {
        remote_control_stop();
#ifndef HOST_BUILD
        bt_remote_control_stop();
#endif
    }
}

static void network_service_run_job(network_service_job_t job) {
    if (job.service == NETWORK_SERVICE_IMPORT) {
        if (job.start) {
            bool control_ready = remote_control_is_running();
            bool ready = control_ready && remote_import_files_start();
            if (!ready) remote_import_files_stop();
            remote_control_set_import_state(ready ? REMOTE_IMPORT_READY : REMOTE_IMPORT_ERROR,
                                            ready ? NULL : control_ready ? "import_start_failed" : "remote_control_unavailable");
            if (!ready)
                atomic_fetch_or_explicit(&remote_import_worker_events, REMOTE_IMPORT_WORKER_START_FAILED,
                                         memory_order_release);
        } else {
            remote_import_files_stop();
            remote_control_set_import_state(REMOTE_IMPORT_STOPPED, NULL);
            atomic_fetch_or_explicit(&remote_import_worker_events, REMOTE_IMPORT_WORKER_STOP_DONE,
                                     memory_order_release);
        }
    } else if (job.service == NETWORK_SERVICE_REMOTE) {
        network_service_run_remote(job.start);
    } else if (job.service == NETWORK_SERVICE_DLNA) {
        if (job.start) dlna_control_start();
        else dlna_control_stop();
    }
}

static void * network_service_worker_func(void * arg) {
    (void) arg;
    for (;;) {
        network_service_job_t job;
        pthread_mutex_lock(&network_service_mutex);
        bool have_job = network_service_pop_locked(&job);
        pthread_mutex_unlock(&network_service_mutex);
        if (!have_job) break;

        if (!job.start || !atomic_load_explicit(&network_service_cancelling, memory_order_acquire))
            network_service_run_job(job);
        pthread_mutex_lock(&network_service_mutex);
        bool same_service_queued = false;
        for (unsigned i = 0; i < network_service_queue_count; i++) {
            unsigned index = (network_service_queue_head + i) % NETWORK_SERVICE_QUEUE_CAPACITY;
            if (network_service_queue[index].service == job.service) {
                same_service_queued = true;
                break;
            }
        }
        if (!same_service_queued) network_service_busy_flags[job.service] = false;
        if (!job.start) network_service_stop_queued[job.service] = false;
        pthread_mutex_unlock(&network_service_mutex);
    }
    atomic_store_explicit(&network_service_done, true, memory_order_release);
    return NULL;
}

static bool network_service_is_busy(network_service_t service) {
    pthread_mutex_lock(&network_service_mutex);
    bool busy = network_service_busy_flags[service];
    pthread_mutex_unlock(&network_service_mutex);
    return busy;
}

static bool network_service_launch_locked(void) {
    atomic_store_explicit(&network_service_done, false, memory_order_relaxed);
    network_service_worker_active = true;
    if (pthread_create(&network_service_thread, NULL, network_service_worker_func, NULL) != 0) {
        network_service_worker_active = false;
        return false;
    }
    return true;
}

static bool network_service_enqueue(network_service_t service, bool start, bool force_stop) {
    /* A completed worker may still await the next UI poll. Reap it before
     * appending work, otherwise a new job could be stranded behind a thread
     * that has already exited. */
    poll_network_service_ops();
    pthread_mutex_lock(&network_service_mutex);
    if ((!force_stop && network_service_busy_flags[service]) ||
        (force_stop && network_service_stop_queued[service]) ||
        network_service_queue_count >= NETWORK_SERVICE_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&network_service_mutex);
        return false;
    }
    unsigned tail = (network_service_queue_head + network_service_queue_count) % NETWORK_SERVICE_QUEUE_CAPACITY;
    bool was_busy = network_service_busy_flags[service];
    bool had_stop = network_service_stop_queued[service];
    network_service_queue[tail] = (network_service_job_t){ .service = service, .start = start };
    network_service_queue_count++;
    network_service_busy_flags[service] = true;
    if (!start) network_service_stop_queued[service] = true;
    if (!network_service_worker_active) {
        if (!network_service_launch_locked()) {
            network_service_queue_count--;
            network_service_busy_flags[service] = was_busy;
            network_service_stop_queued[service] = had_stop;
            network_service_worker_active = false;
            pthread_mutex_unlock(&network_service_mutex);
            return false;
        }
    }
    pthread_mutex_unlock(&network_service_mutex);
    return true;
}

static void poll_network_service_ops(void) {
    pthread_mutex_lock(&network_service_mutex);
    bool active = network_service_worker_active;
    pthread_mutex_unlock(&network_service_mutex);
    if (active && !atomic_load_explicit(&network_service_done, memory_order_acquire)) return;
    if (active) pthread_join(network_service_thread, NULL);
    pthread_mutex_lock(&network_service_mutex);
    if (active) network_service_worker_active = false;
    /* The worker may have observed an empty queue just before an enqueue
     * published a job and set done. Re-launch any such job while holding the
     * lifecycle lock; this closes the exit/enqueue handshake race. */
    if (network_service_queue_count > 0) {
        /* Retry creation on every UI poll. A transient pthread resource
         * failure must not strand a busy flag or an Import teardown. */
        (void) network_service_launch_locked();
    }
    pthread_mutex_unlock(&network_service_mutex);
}

void poll_remote_import(void) {
    poll_network_service_ops();
    if (!network_service_is_busy(NETWORK_SERVICE_IMPORT)) {
        unsigned worker_events = atomic_exchange_explicit(&remote_import_worker_events, 0, memory_order_acquire);
        if (worker_events & REMOTE_IMPORT_WORKER_START_FAILED) remote_import_active = false;
        if ((worker_events & REMOTE_IMPORT_WORKER_STOP_DONE) && remote_import_stop_pending) {
            bool suppress_rescan = remote_import_stop_suppress_rescan;
            remote_import_stop_pending = false;
            remote_import_stop_suppress_rescan = false;
            remote_import_active = false;
            if (!suppress_rescan && gui_library_auto_rescan_enabled()) start_library_auto_rescan();
        }
    }
#ifndef HOST_BUILD
    bool remote_start = false;
    if (remote_control_consume_import_request(&remote_start)) {
        handle_remote_import_request(remote_start);
    }
#endif
}

/* Enqueues the service-worker stop for the running remote session. */
static void remote_import_stop(bool suppress_rescan) {
    atomic_fetch_and_explicit(&remote_import_worker_events, ~REMOTE_IMPORT_WORKER_STOP_DONE, memory_order_acq_rel);
    remote_import_stop_pending = true;
    remote_import_stop_suppress_rescan = suppress_rescan;
    remote_control_set_import_state(REMOTE_IMPORT_STOPPING, NULL);
    if (!network_service_enqueue(NETWORK_SERVICE_IMPORT, false, true)) {
        remote_import_stop_pending = false;
        remote_import_stop_suppress_rescan = false;
        remote_control_set_import_state(REMOTE_IMPORT_ERROR, "file_import_stop_failed");
        return;
    }
    remote_import_active = false;
}

/* Called by gui_network_handle_wifi_disabled(): stops a running remote
 * session without the rescan, or marks an in-flight stop to skip it. */
static void remote_import_handle_wifi_disabled(void) {
    if (remote_import_stop_pending) {
        remote_import_stop_suppress_rescan = true;
        return;
    }
    if (!remote_import_active) return; /* not running */
    remote_import_stop(true);
}

#ifndef HOST_BUILD
/* Called only by poll_remote_import_files_stop() on the LVGL thread. Remote starts
 * share the daemon without pushing or replacing a screen. */
static void handle_remote_import_request(bool start) {
    if (start) {
        if (remote_radio_sleep_prepared) {
            remote_control_set_import_state(REMOTE_IMPORT_ERROR, "radio_sleep");
            return;
        }
        if (remote_import_active || remote_import_stop_pending || network_service_is_busy(NETWORK_SERVICE_IMPORT)) {
            remote_control_set_import_state(REMOTE_IMPORT_ERROR, "file_import_busy");
            return;
        }
        if (!gui_shell_wifi_effective_enabled()) {
            remote_control_set_import_state(REMOTE_IMPORT_ERROR, "wifi_disabled");
            return;
        }
        if (!sd_card_root_is_mounted()) {
            remote_control_set_import_state(REMOTE_IMPORT_ERROR, "sd_card_unavailable");
            return;
        }
        if (gui_network_usb_storage_session_active() || gui_library_navigation_blocked()) {
            remote_control_set_import_state(REMOTE_IMPORT_ERROR, "player_storage_busy");
            return;
        }
        remote_control_set_import_state(REMOTE_IMPORT_STARTING, NULL);
        if (!network_service_enqueue(NETWORK_SERVICE_IMPORT, true, false)) {
            remote_control_set_import_state(REMOTE_IMPORT_ERROR, "file_import_busy");
            return;
        }
        remote_import_active = true;
        return;
    }

    if (remote_import_stop_pending) {
        /* An explicit stop while a Wi-Fi-loss stop is in flight still wants the rescan. */
        remote_import_stop_suppress_rescan = false;
        return;
    }
    if (!remote_import_active) {
        /* Nothing is running (e.g. a failed start already cleaned up). */
        remote_import_state_t current_state;
        remote_control_get_import_state(&current_state, NULL, 0);
        if (current_state != REMOTE_IMPORT_STOPPED) remote_control_set_import_state(REMOTE_IMPORT_STOPPED, NULL);
        return;
    }
    remote_import_stop(false);
}
#endif

bool gui_network_prepare_radio_sleep(void) {
    remote_radio_sleep_prepared = true;

    /* Preserve Wi-Fi-loss import teardown semantics, including suppressing
     * its automatic library rescan while radios are going down. */
    remote_import_handle_wifi_disabled();

    if (current_settings.remote_control_enabled && !remote_radio_sleep_stop_queued) {
        if (network_service_enqueue(NETWORK_SERVICE_REMOTE, false, true)) {
            remote_radio_sleep_stop_queued = true;
        } else {
            pthread_mutex_lock(&network_service_mutex);
            remote_radio_sleep_stop_queued = network_service_stop_queued[NETWORK_SERVICE_REMOTE];
            pthread_mutex_unlock(&network_service_mutex);
        }
    }

    bool import_busy = network_service_is_busy(NETWORK_SERVICE_IMPORT) ||
                       remote_import_active || remote_import_stop_pending;
    bool remote_busy = network_service_is_busy(NETWORK_SERVICE_REMOTE);
    if (import_busy || remote_busy) return false;
    return !current_settings.remote_control_enabled || remote_radio_sleep_stop_queued;
}

void gui_network_resume_radio_services(void) {
    if (!remote_radio_sleep_prepared) return;

    /* A wake can cancel preparation before the RC stop was accepted (for
     * example, pthread creation failed). Let any import teardown finish, then
     * discard the transient gate without inventing a service restart. */
    if (!remote_radio_sleep_stop_queued) {
        if (network_service_is_busy(NETWORK_SERVICE_IMPORT) ||
            remote_import_active || remote_import_stop_pending) return;
        remote_radio_sleep_prepared = false;
        return;
    }

    if (!current_settings.remote_control_enabled) {
        remote_radio_sleep_prepared = false;
        remote_radio_sleep_stop_queued = false;
        return;
    }
    if (!remote_radio_sleep_stop_queued ||
        network_service_is_busy(NETWORK_SERVICE_REMOTE) ||
        network_service_is_busy(NETWORK_SERVICE_IMPORT) ||
        remote_import_active || remote_import_stop_pending) return;

    if (network_service_enqueue(NETWORK_SERVICE_REMOTE, true, false)) {
        remote_radio_sleep_prepared = false;
        remote_radio_sleep_stop_queued = false;
    }
}

/* ---- DLNA screen -- toggles the DLNA DMR service (dmrd) to allow UPnP/DLNA
 * controllers on the local Wi-Fi network to stream to this device. ---- */

static lv_obj_t * dlna_list;

static void dlna_toggle_cb(lv_event_t * e);

static void populate_dlna_screen(void) {
    lv_obj_clean(dlna_list);
    add_pill_toggle_row(dlna_list, TR("DLNA Renderer"), current_settings.dlna_renderer_enabled, dlna_toggle_cb);

    add_network_explanation(dlna_list,
                      TR("When on, this device is visible to DLNA/UPnP controller apps on your Wi-Fi network -- cast "
                      "a track from one to play it here. Pause, mute, volume, and seek from the controller app "
                      "aren't supported; use this device's own controls instead once a track starts."));
}

static void refresh_dlna_screen_if_built(void) {
    if (dlna_list) populate_dlna_screen();
}

/* gui_network_toggle_airplay()'s twin for DLNA -- same reason the drawer
 * routes through it rather than setting the flag itself. */
bool gui_network_toggle_dlna(void) {
    bool turning_on = !current_settings.dlna_renderer_enabled;
    /* Same defensive enable guard as airplay_toggle_cb() -- this screen can
     * already be open when Wi-Fi is disabled elsewhere, so the tile guard
     * alone (dlna_tile_cb() below) isn't sufficient. */
    if (turning_on && !wifi_feature_guard()) {
        refresh_dlna_screen_if_built();
        return current_settings.dlna_renderer_enabled;
    }
    if (network_service_is_busy(NETWORK_SERVICE_DLNA) ||
        !network_service_enqueue(NETWORK_SERVICE_DLNA, turning_on, false)) {
        show_info_toast(TR("Service is busy"));
        refresh_dlna_screen_if_built();
        return current_settings.dlna_renderer_enabled;
    }
    current_settings.dlna_renderer_enabled = turning_on;
    settings_save_async(&current_settings);

    refresh_dlna_screen_if_built();
    return current_settings.dlna_renderer_enabled;
}

static void dlna_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_network_toggle_dlna();
}

/* ---- Remote Control screen (Wireless -> "Open Link") -- exposes the
 * HTTP address over Wi-Fi and the RFCOMM service name over Bluetooth.
 * Built on the same flex list as the DLNA/Wi-Fi screens: standard toggle
 * row, a "Connection PIN" row with a refresh icon to replace the PIN, then the
 * QR/URL lines. ---- */

static lv_obj_t * remote_control_toggle_img;
static lv_obj_t * remote_control_pin_label;
static char remote_control_displayed_pin[REMOTE_CONTROL_PIN_MAX_LENGTH + 1];
static bool remote_control_displayed_pin_valid;
static lv_obj_t * remote_control_status_label;
static lv_obj_t * remote_control_url_label;
static lv_obj_t * remote_control_bluetooth_label;
#if LV_USE_QRCODE
static lv_obj_t * remote_control_qrcode;
#endif

/* The screen is a flex column (build_subsonic_list_screen()), so positions
 * follow the content automatically. An empty label still occupies a text
 * line there, though, so hide the address lines whose text is empty instead
 * of leaving blank gaps. Call after every text change below. */
static void remote_control_hide_empty_lines(void) {
    lv_obj_t * lines[] = { remote_control_url_label, remote_control_bluetooth_label };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        const char * text = lv_label_get_text(lines[i]);
        if (text && text[0]) lv_obj_remove_flag(lines[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(lines[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void remote_control_refresh_pin_label(void) {
    if (!remote_control_pin_label) return;
    char pin[REMOTE_CONTROL_PIN_MAX_LENGTH + 1] = {0};
    remote_control_get_pin(pin, sizeof(pin));
    if (remote_control_displayed_pin_valid && strcmp(remote_control_displayed_pin, pin) == 0) return;
    /* Empty only if the system random source failed; the server refuses to
     * start in that state too, so say so rather than show a fake PIN. */
    lv_label_set_text(remote_control_pin_label, pin[0] ? pin : TR("Unavailable"));
    snprintf(remote_control_displayed_pin, sizeof(remote_control_displayed_pin), "%s", pin);
    remote_control_displayed_pin_valid = true;
}

/* Bluetooth route line of the Remote Control screen. */
static const char * remote_control_bt_line(bool powered, bt_remote_control_status_t status,
                                           bool available) {
    if (available) return TR("Bluetooth: Compas Remote Control");
    if (powered && status == BT_REMOTE_CONTROL_FAILED) return TR("Bluetooth: registration failed; retrying");
    if (powered) return TR("Bluetooth: waiting for service registration");
    return "";
}

void remote_control_refresh_address(void) {
    remote_control_refresh_pin_label();
    if (!current_settings.remote_control_enabled) {
        lv_label_set_text(remote_control_status_label,
                           TR("Turn this on to see the address here."));
        lv_label_set_text(remote_control_url_label, "");
        lv_label_set_text(remote_control_bluetooth_label, "");
#if LV_USE_QRCODE
        lv_obj_add_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
#endif
        remote_control_hide_empty_lines();
        return;
    }

    wifi_settings_snapshot_start();
    bool wifi_available = gui_shell_wifi_effective_enabled() && wifi_cached_info_is_current() &&
                          wifi_cached_info_connected && wifi_cached_info.ip[0] != '\0';
    bool bluetooth_available = false;
    bool bluetooth_powered = false;
    bt_remote_control_status_t bluetooth_status = BT_REMOTE_CONTROL_DISABLED;
#ifndef HOST_BUILD
    bluetooth_powered = bt_is_powered_cached;
    bluetooth_status = bt_remote_control_status();
    bluetooth_available = bluetooth_powered && bt_remote_control_is_advertising();
#endif
    if (!wifi_available && !bluetooth_available) {
        if (bluetooth_powered && bluetooth_status == BT_REMOTE_CONTROL_FAILED)
            lv_label_set_text(remote_control_status_label, TR("Bluetooth remote service is unavailable; retrying."));
        else if (bluetooth_powered)
            lv_label_set_text(remote_control_status_label, TR("Bluetooth remote service is starting…"));
        else
            lv_label_set_text(remote_control_status_label, TR("Enable Wi-Fi or Bluetooth to connect."));
        lv_label_set_text(remote_control_url_label, "");
        lv_label_set_text(remote_control_bluetooth_label,
                          remote_control_bt_line(bluetooth_powered, bluetooth_status, false));
#if LV_USE_QRCODE
        lv_obj_add_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
#endif
        remote_control_hide_empty_lines();
        return;
    }

    lv_label_set_text(remote_control_status_label, TR("Connect using either available route:"));
    if (wifi_available) {
        char url[160], dns_name[64];
        if (remote_control_mdns_hostname(dns_name, sizeof(dns_name)))
            snprintf(url, sizeof(url), "Wi-Fi: http://%s%s\nhttp://%s:8899", dns_name,
                     remote_control_serves_default_port() ? "" : ":8899", wifi_cached_info.ip);
        else
            snprintf(url, sizeof(url), "Wi-Fi: http://%s:8899", wifi_cached_info.ip);
        lv_label_set_text(remote_control_url_label, url);
    } else {
        lv_label_set_text(remote_control_url_label, "");
    }
    lv_label_set_text(remote_control_bluetooth_label,
                      remote_control_bt_line(bluetooth_powered, bluetooth_status, bluetooth_available));
#if LV_USE_QRCODE
    if (wifi_available) {
        char url[64];
        snprintf(url, sizeof(url), "http://%s:8899", wifi_cached_info.ip);
        lv_obj_remove_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
        lv_qrcode_update(remote_control_qrcode, url, strlen(url));
    } else {
        lv_obj_add_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    remote_control_hide_empty_lines();
}

static void refresh_remote_control_screen_if_built(void) {
    if (remote_control_toggle_img) remote_control_refresh_address();
}

/* gui_network_toggle_airplay()'s twin for the phone remote-control server --
 * shown as "RC" on the quick drawer's own tile, where there is no room for
 * the full name. */
bool gui_network_toggle_remote_control(void) {
    bool turning_on = !current_settings.remote_control_enabled;
    if (network_service_is_busy(NETWORK_SERVICE_REMOTE) ||
        !network_service_enqueue(NETWORK_SERVICE_REMOTE, turning_on, false)) {
        show_info_toast(TR("Service is busy"));
        refresh_remote_control_screen_if_built();
        return current_settings.remote_control_enabled;
    }
    current_settings.remote_control_enabled = turning_on;
    settings_save_async(&current_settings);

    /* Real lv_switch now (see build_remote_control_screen()'s own comment)
     * -- CHECKED state alone drives its visual, no sprite swap needed. */
    if (remote_control_toggle_img) {
        if (current_settings.remote_control_enabled) lv_obj_add_state(remote_control_toggle_img, LV_STATE_CHECKED);
        else lv_obj_clear_state(remote_control_toggle_img, LV_STATE_CHECKED);
    }
    refresh_remote_control_screen_if_built();
    if (turning_on) gui_network_extend_bt_pairing_window();
    return current_settings.remote_control_enabled;
}

static void remote_control_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_network_toggle_remote_control();
}

/* ---- New-PIN confirmation for the PIN row's refresh icon. Regenerating
 * immediately locks out every app and browser still using the old PIN, so it
 * asks first, like the other disruptive actions in this file
 * (bt_dac_leave_popup). Built once in
 * gui_network_init() on lv_layer_top(), torn down in gui_network_teardown(). */
static gui_popup_t remote_control_new_pin_popup;

static void remote_control_new_pin_popup_dismiss_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&remote_control_new_pin_popup);
}

static void remote_control_new_pin_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&remote_control_new_pin_popup);
    if (!remote_control_generate_new_pin()) {
        show_info_toast(TR("Could not generate a new PIN"));
        return;
    }
    remote_control_refresh_pin_label();
    show_info_toast(TR("New PIN generated"));
}

static void remote_control_regenerate_pin_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_show(&remote_control_new_pin_popup);
}

static void build_remote_control_new_pin_popup(void) {
    remote_control_new_pin_popup.popup = build_confirm_popup(
        TR("Generate a new PIN?"), LV_LABEL_LONG_WRAP, NULL,
        TR("Apps and browsers using the current PIN will need the new one to reconnect."), TR("Generate"),
        lv_color_hex(GUI_BREEZE_NEGATIVE), remote_control_new_pin_confirm_cb, NULL, TR("Cancel"), accent_lv_color(),
        remote_control_new_pin_popup_dismiss_cb, NULL, remote_control_new_pin_popup_dismiss_cb,
        &remote_control_new_pin_popup.backdrop);
}

/* Standard settings-list layout (build_subsonic_list_screen(), the same
 * flex column as the Wi-Fi and Bluetooth screens): the on/off toggle row,
 * then the PIN row with a refresh icon to replace the PIN. The connection
 * details follow as wrapped text lines. */
static lv_obj_t * build_remote_control_screen(void) {
    lv_obj_t * list;
    lv_obj_t * scr = network_list_screen(TR("Remote Control"), &list);

    lv_obj_t * toggle_row = add_pill_toggle_row(list, TR("Remote Control"), current_settings.remote_control_enabled,
                                                remote_control_toggle_cb);
    /* add_pill_toggle_row() creates the label first and the switch last;
     * gui_network_toggle_remote_control() drives this switch's state. */
    remote_control_toggle_img = lv_obj_get_child(toggle_row, -1);

    lv_obj_t * pin_row = add_pill_row_base(list, TR("Connection PIN"));

    /* Refresh icon at the row's trailing edge (same -20 inset as the toggle
     * and chevron rows) regenerates the PIN; LV_SYMBOL_REFRESH comes from the
     * Montserrat fallback chain behind the app font. The glyph is small, so
     * widen its touch area. */
    lv_obj_t * regenerate_icon = lv_label_create(pin_row);
    lv_label_set_text(regenerate_icon, LV_SYMBOL_REFRESH);
    lv_obj_add_style(regenerate_icon, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(regenerate_icon, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(regenerate_icon, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_add_flag(regenerate_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(regenerate_icon, BOARD_SCALE_PX(20));
    lv_obj_add_event_cb(regenerate_icon, remote_control_regenerate_pin_cb, LV_EVENT_CLICKED, NULL);

    /* The PIN sits just left of the icon. lv_obj_align() (unlike
     * lv_obj_align_to()) keeps the right-edge anchor as the text changes
     * between a PIN and "Unavailable". */
    lv_obj_update_layout(regenerate_icon);
    int32_t icon_width = lv_obj_get_width(regenerate_icon);
    remote_control_pin_label = lv_label_create(pin_row);
    lv_obj_add_style(remote_control_pin_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(remote_control_pin_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(remote_control_pin_label, LV_ALIGN_RIGHT_MID, -20 - icon_width - BOARD_SCALE_PX(16), 0);
    remote_control_displayed_pin_valid = false;
    remote_control_refresh_pin_label();

    lv_obj_t * explanation = lv_label_create(list);
    lv_label_set_text(explanation,
                      TR("Connect over Wi-Fi or Bluetooth to see what's playing, control playback, and browse "
                      "your library. Enter this PIN when the app or browser asks for it; Bluetooth also "
                      "requires pairing."));
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(explanation, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_top(explanation, BOARD_SCALE_PX(12), 0);

    remote_control_status_label = lv_label_create(list);
    lv_obj_set_width(remote_control_status_label, lv_pct(90));
    lv_label_set_long_mode(remote_control_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(remote_control_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(remote_control_status_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(remote_control_status_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);

#if LV_USE_QRCODE
    remote_control_qrcode = lv_qrcode_create(list);
    lv_qrcode_set_size(remote_control_qrcode, BOARD_SCALE_PX(220));
    lv_qrcode_set_dark_color(remote_control_qrcode, lv_color_black());
    lv_qrcode_set_light_color(remote_control_qrcode, lv_color_white());
    lv_obj_set_style_border_width(remote_control_qrcode, 4, 0);
    lv_obj_set_style_border_color(remote_control_qrcode, lv_color_white(), 0);
    lv_obj_add_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
#endif

    remote_control_url_label = lv_label_create(list);
    lv_obj_set_width(remote_control_url_label, lv_pct(90));
    lv_label_set_long_mode(remote_control_url_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(remote_control_url_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(remote_control_url_label, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(remote_control_url_label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    remote_control_bluetooth_label = lv_label_create(list);
    lv_obj_set_width(remote_control_bluetooth_label, lv_pct(90));
    lv_label_set_long_mode(remote_control_bluetooth_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(remote_control_bluetooth_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(remote_control_bluetooth_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(remote_control_bluetooth_label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_set_style_pad_bottom(remote_control_bluetooth_label, BOARD_SCALE_PX(24), 0);
    /* remote_control_tile_cb() fills these in every time the screen
     * opens; hide the still-empty lines until then. */
    remote_control_hide_empty_lines();

    return scr;
}

void gui_network_open_remote_control(void) {
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_enabled = true;
    remote_control_refresh_address();
    nav_push(remote_control_screen);
    gui_network_extend_bt_pairing_window();
}

void remote_control_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_network_open_remote_control();
}

void gui_network_open_dlna(void) {
    if (!wifi_feature_guard()) return;
    populate_dlna_screen();
    nav_push(dlna_screen);
}

void dlna_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_network_open_dlna();
}

void gui_network_services_build(void) {
    dlna_screen = network_list_screen(TR("DLNA"), &dlna_list);
    remote_control_screen = build_remote_control_screen();
    build_remote_control_new_pin_popup();
}

void gui_network_services_teardown(void) {
    gui_popup_teardown(&remote_control_new_pin_popup);

    /* The Remote Control screen's widgets die with remote_control_screen
     * below; clear the pointers so the quick-drawer toggle and the periodic
     * poll (which check them) can't touch freed objects before a rebuild. */
    remote_control_toggle_img = NULL;
    remote_control_pin_label = NULL;
    remote_control_displayed_pin_valid = false;
    remote_control_status_label = NULL;
    remote_control_url_label = NULL;
    remote_control_bluetooth_label = NULL;
#if LV_USE_QRCODE
    remote_control_qrcode = NULL;
#endif

    network_delete_screen(&dlna_screen);
    network_delete_screen(&remote_control_screen);
    dlna_list = NULL;
}

bool gui_network_services_has_work(void) {
    pthread_mutex_lock(&network_service_mutex);
    bool network_service_work = network_service_worker_active || network_service_queue_count > 0;
    pthread_mutex_unlock(&network_service_mutex);
    return network_service_work;
}

void gui_network_services_cancel(void) {
    atomic_store_explicit(&network_service_cancelling, true, memory_order_release);
    /* Drain the serialized service worker before teardown. If a worker could
     * not be created, execute its bounded queue synchronously so cancellation
     * cannot leave lifecycle flags or an Import busy overlay permanently set. */
    pthread_mutex_lock(&network_service_mutex);
    bool service_worker_active = network_service_worker_active;
    pthread_t service_thread = network_service_thread;
    pthread_mutex_unlock(&network_service_mutex);
    if (service_worker_active) pthread_join(service_thread, NULL);
    pthread_mutex_lock(&network_service_mutex);
    network_service_worker_active = false;
    pthread_mutex_unlock(&network_service_mutex);
    for (;;) {
        pthread_mutex_lock(&network_service_mutex);
        network_service_job_t job;
        if (!network_service_pop_locked(&job)) {
            pthread_mutex_unlock(&network_service_mutex);
            break;
        }
        network_service_busy_flags[job.service] = false;
        if (!job.start) network_service_stop_queued[job.service] = false;
        pthread_mutex_unlock(&network_service_mutex);
        /* Shutdown: only stops run; a dropped start leaves the service off. */
        if (job.start) continue;
        if (job.service == NETWORK_SERVICE_IMPORT) {
            remote_import_files_stop();
        } else if (job.service == NETWORK_SERVICE_REMOTE) {
            network_service_run_remote(false);
        } else if (job.service == NETWORK_SERVICE_DLNA) {
            dlna_control_stop();
        }
    }
    atomic_store_explicit(&network_service_cancelling, false, memory_order_release);
}

/* Centralized Wi-Fi-disabled cleanup for Wi-Fi-dependent features
 * -- called by gui_shell_wifi.c's poll_wifi_toggle() only once a Wi-Fi disable is
 * authoritatively confirmed (see that call site's own comment). Stops
 * whichever of AirPlay/DLNA/remote file import is actually running and
 * corrects their persisted settings, refreshing any of their
 * screens/toggles already built so nothing shows stale "on" state. A single
 * settings_save_async() at the end covers every flag this pass touched, rather
 * than one write per feature. */
void gui_network_handle_wifi_disabled(void) {
    bool settings_changed = false;

    /* Subsonic requests are ordinary background sockets, not persistent
     * services with a settings toggle. Interrupt them before tearing down
     * the rest of the Wi-Fi-owned features so their active flags cannot
     * veto idle suspend after the radio is already off. */
    gui_subsonic_handle_wifi_disabled();

    if (current_settings.wifi_dac_mode_enabled) {
        airplay_control_stop();
        current_settings.wifi_dac_mode_enabled = false;
        settings_changed = true;
        populate_airplay_screen();
    }

    if (current_settings.dlna_renderer_enabled) {
        if (network_service_enqueue(NETWORK_SERVICE_DLNA, false, true)) {
            current_settings.dlna_renderer_enabled = false;
            settings_changed = true;
            populate_dlna_screen();
        }
    }

    /* Remote file import has no persisted enabled flag (session-only, started
     * through Remote Control) -- remote_import_handle_wifi_disabled() is
     * a cheap no-op whenever it isn't currently running. */
    remote_import_handle_wifi_disabled();

    if (settings_changed) settings_save_async(&current_settings);
}
