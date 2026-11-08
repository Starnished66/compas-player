#include "../application/gui.h"
#include "../text/i18n/i18n.h"
#include "gui_network.h"
#include "../shell/gui_shell.h"
#include "../settings/gui_settings.h"
#include "../widgets/builders/screen_builders.h"
#include "../../hardware/power/battery.h"
#include "../storage/usb_storage_session.h"
#include "../../hardware/usb/usb_dac_bridge.h"
#include "../../core/updates/firmware_update.h"
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include "gui_network_internal.h"

static lv_obj_t * usb_mode_screen;
/* See the mode-switch success path for why this exists rather than reading
 * current_settings.usb_mode. */
static bool usb_dac_mode_active = false;
static pthread_t usb_dac_charging_thread;
static bool usb_dac_charging_active;
static atomic_bool usb_dac_charging_done = false;
static volatile bool usb_dac_charging_succeeded;
static bool usb_dac_charging_target_disabled;

/* ---- USB device mode (Settings > USB Mode) -- single-select list with an
 * accent-colored border on the active option. Applying a mode modifies
 * configfs/sysfs in a worker thread polled from update_timer_cb to prevent
 * blocking the UI thread. ---- */

typedef struct {
    usb_mode_t mode;
    const char * label;
} usb_mode_option_t;

static const usb_mode_option_t usb_mode_options[] = {
    { USB_MODE_STORAGE, N_("Storage") },
    { USB_MODE_DAC, N_("USB DAC") },
    { USB_MODE_ADB, N_("ADB") },
};
#define USB_MODE_OPTION_COUNT (sizeof(usb_mode_options) / sizeof(usb_mode_options[0]))

static lv_obj_t * usb_mode_list;

static pthread_t usb_mode_switch_thread;
static bool usb_mode_switch_active = false;
static bool usb_mode_switch_queued = false;
/* UI-owned guard; the detached worker only publishes completion. It may
 * wait indefinitely for an old bridge writer, so never join it on the UI. */
static bool usb_bridge_restart_active = false;
static atomic_bool usb_bridge_restart_done = false;
static atomic_bool usb_mode_switch_done_flag = false;
static volatile bool usb_mode_switch_succeeded = false;
static bool usb_mode_switch_lease_deferred;
static usb_mode_t usb_mode_switch_target;
static bool usb_cable_state_initialized;
static bool usb_cable_was_connected;
static bool usb_storage_rebind_pending;
static usb_storage_session_t usb_storage_session;
static void flush_sd_view_after_usb_storage(void);
static uint32_t usb_storage_host_check_tick;

static void usb_mode_option_row_cb(lv_event_t * e);
static void finish_usb_dac_charging_update(bool show_failure, bool thread_already_joined);
/* ADB is a separate toggle, not a third equal option alongside Storage/DAC
 * -- matches the stock firmware's own USB device mode screen (confirmed by
 * the user against a real stock device: ADB overrides Storage/DAC while
 * on, turning it off falls back to the other two). This also matches the
 * actual gadget architecture: ADB lives under its own "adb_demo" configfs
 * directory, entirely separate from the "android0" one Storage and DAC
 * share (see usb_mode_control.c), so it really is orthogonal rather than a
 * third position in the same rotation. */
static bool adb_owns_usb_port(void);

static void populate_usb_mode_screen(void) {
    lv_obj_clean(usb_mode_list);

    /* ADB itself is toggled from Developer Options, not here, but it still
     * owns the port while on, so this screen keeps showing that the other
     * modes are unavailable. */
    bool adb_active = adb_owns_usb_port();

    for (size_t i = 0; i < USB_MODE_OPTION_COUNT; i++) {
        if (usb_mode_options[i].mode == USB_MODE_ADB) continue; /* handled by the toggle above */

        bool selected = !adb_active && current_settings.usb_mode == (int) usb_mode_options[i].mode;
        lv_obj_t * row = add_pill_option_row(usb_mode_list, TR(usb_mode_options[i].label),
                                            selected, usb_mode_option_row_cb, (void *) (intptr_t) i);

        if (adb_active) {
            /* Storage/DAC aren't meaningful choices while ADB owns the USB
             * port -- dimmed and inert rather than removed, so the row
             * stays put (no layout jump) and it's visually clear these
             * exist but aren't the current mode. */
            lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(row, LV_OPA_50, 0);
        }
    }
}

static void * usb_mode_switch_thread_func(void * arg) {
    (void) arg;
    bool storage_busy = false;
    usb_mode_switch_succeeded = usb_mode_control_apply_with_status(usb_mode_switch_target, &storage_busy);
    usb_mode_switch_lease_deferred = storage_busy && usb_mode_switch_target == USB_MODE_STORAGE;
    atomic_store_explicit(&usb_mode_switch_done_flag, true, memory_order_release); /* written last -- poll_usb_mode_switch only checks this flag */
    return NULL;
}

static void * usb_bridge_restart_thread_func(void * arg) {
    (void) arg;
    usb_dac_bridge_start();
    atomic_store_explicit(&usb_bridge_restart_done, true, memory_order_release);
    return NULL;
}

/* Returns whether the worker actually started. A launch that fails runs no
 * poll, so nothing else will ever correct a control left flipped in
 * anticipation of it -- the caller has to know. */
static bool launch_usb_mode_switch(void) {
    usb_mode_switch_queued = false;
    atomic_store_explicit(&usb_mode_switch_done_flag, false, memory_order_relaxed);
    int rc = pthread_create(&usb_mode_switch_thread, NULL, usb_mode_switch_thread_func, NULL);
    if (rc != 0) {
        /* Without this rollback, a transient low-memory/thread-creation
         * failure leaves the guard latched forever: every later tap is
         * ignored until the player or device is restarted. */
        usb_mode_switch_active = false;
        show_error_toast(TR("Could not start USB mode switch"));
        return false;
    }
    return true;
}

/* Returns whether the request was accepted. A caller that flips its own UI
 * ahead of the hardware needs to know when a tap was dropped, or the control
 * ends up showing a state nothing is working toward. */
bool start_usb_mode_switch(usb_mode_t target) {
    if (firmware_update_busy()) {
        show_error_toast(TR("An update is already in progress"));
        return false;
    }
    if (usb_mode_switch_active) return false;
    gui_library_suspend_boot_prompt();
    usb_mode_switch_target = target;
    usb_mode_switch_active = true;
    /* Preserve the explicit request without letting gadget teardown race a
     * watchdog start already in flight. Polling launches it once safe. */
    if (target == USB_MODE_DAC) {
        if (current_settings.wifi_dac_mode_enabled || gui_network_airplay_enable_pending())
            disable_airplay_mode();
        if (current_settings.bt_dac_mode_enabled || gui_shell_bt_dac_busy())
            disable_bt_dac_mode();
    }
    usb_mode_switch_queued = usb_bridge_restart_active ||
        (target == USB_MODE_DAC && gui_shell_bt_dac_busy());
    if (!usb_mode_switch_queued) return launch_usb_mode_switch();
    return true;
}

bool gui_network_usb_dac_pending(void) {
    return (usb_mode_switch_active && usb_mode_switch_target == USB_MODE_DAC) ||
           usb_bridge_restart_active;
}

/* The state the ADB control should show: the target while a switch is in
 * flight, otherwise the recorded mode. */
bool gui_network_adb_active(void) {
    if (usb_mode_switch_active) return usb_mode_switch_target == USB_MODE_ADB;
    return current_settings.usb_mode == (int) USB_MODE_ADB;
}

/* Whether ADB still has a claim on the port, which is deliberately more
 * cautious than gui_network_adb_active(). Leaving ADB does not free the port
 * until the switch actually lands, so the modes that cannot be chosen yet stay
 * dimmed for the whole transition in both directions rather than offering rows
 * whose taps the in-flight guard would only reject. */
static bool adb_owns_usb_port(void) {
    bool recorded_adb = current_settings.usb_mode == (int) USB_MODE_ADB;
    if (usb_mode_switch_active) return recorded_adb || usb_mode_switch_target == USB_MODE_ADB;
    return recorded_adb;
}

void poll_usb_mode_switch(void) {
    finish_usb_dac_charging_update(true, false);
    if (usb_bridge_restart_active &&
        atomic_load_explicit(&usb_bridge_restart_done, memory_order_acquire)) {
        usb_bridge_restart_active = false;
    }
    if (usb_mode_switch_queued && !usb_bridge_restart_active &&
        (usb_mode_switch_target != USB_MODE_DAC || !gui_shell_bt_dac_busy())) {
        /* Deferred launch: same failure consequence as the immediate one, but
         * the original caller has long since returned, so the control it
         * flipped has to be corrected from here. */
        if (!launch_usb_mode_switch()) gui_settings_sync_adb_toggle();
    }
    /* If set to USB DAC mode but the audio bridge is not running, restart it.
     * Skipped while a mode switch is in flight to avoid racing
     * usb_mode_control_apply(). */
    if (!usb_mode_switch_active && !usb_bridge_restart_active &&
        current_settings.usb_mode == (int) USB_MODE_DAC) {
        usb_dac_stream_info_t info;
        usb_dac_bridge_get_stream_info(&info);
        if (!info.bridge_running) {
            if (current_settings.wifi_dac_mode_enabled || gui_network_airplay_enable_pending())
                disable_airplay_mode();
            if (current_settings.bt_dac_mode_enabled) disable_bt_dac_mode();
            if (gui_shell_bt_dac_busy()) return;
            pthread_attr_t attr;
            if (pthread_attr_init(&attr) == 0) {
                if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0) {
                    pthread_t thread;
                    atomic_store_explicit(&usb_bridge_restart_done, false, memory_order_relaxed);
                    usb_bridge_restart_active = true;
                    if (pthread_create(&thread, &attr, usb_bridge_restart_thread_func, NULL) != 0)
                        usb_bridge_restart_active = false;
                }
                pthread_attr_destroy(&attr);
            }
        }
    }

    if (!usb_mode_switch_active || usb_mode_switch_queued || !atomic_load_explicit(&usb_mode_switch_done_flag, memory_order_acquire)) return;
    usb_mode_switch_active = false;
    pthread_join(usb_mode_switch_thread, NULL);

    /* usb_mode_control_apply() only reports success once the target
     * gadget is actually verified bound, not just "the script ran" -- on
     * failure current_settings.usb_mode is deliberately left untouched
     * (the switch genuinely didn't happen) rather than optimistically
     * recording what was merely requested. */
    if (!usb_mode_switch_succeeded) {
        if (usb_mode_switch_lease_deferred) {
            usb_storage_rebind_pending = true;
            return; /* Retry quietly after setup releases the card. */
        }
        const char * label = TR("USB mode");
        for (size_t i = 0; i < USB_MODE_OPTION_COUNT; i++) {
            if (usb_mode_options[i].mode == usb_mode_switch_target) { label = TR(usb_mode_options[i].label); break; }
        }
        char msg[128];
        snprintf(msg, sizeof(msg), TR("Failed to switch to %s"), label);
        show_error_toast(msg);
        /* apply() tears the old gadget down before bringing the target up, so
         * a failure can leave neither bound -- the recorded mode is then not
         * merely unchanged, it is wrong. Re-read the hardware so the rollback
         * below reflects what is actually there. */
        usb_mode_t actual_usb_mode;
        if (usb_mode_control_detect_current(&actual_usb_mode))
            current_settings.usb_mode = (int) actual_usb_mode;
        populate_usb_mode_screen(); /* in case current_settings.usb_mode's actual row needs re-highlighting */
        gui_settings_sync_adb_toggle(); /* roll the optimistic flip back to what the hardware actually did */
        return;
    }

    /* Leaving Storage while the PC is still attached ends its access too. */
    if (current_settings.usb_mode == (int) USB_MODE_STORAGE && usb_mode_switch_target != USB_MODE_STORAGE &&
        usb_storage_session.host_seen)
        flush_sd_view_after_usb_storage();
    current_settings.usb_mode = (int) usb_mode_switch_target;
    settings_save_async(&current_settings);
    /* Process-lifetime truth for "the DAC overlay owns the UI", as opposed to
     * current_settings.usb_mode, which settings.h documents as only a UI hint
     * and which silently keeps a stale DAC value across a restart with no
     * cable attached (gui.c's startup correction only applies when
     * usb_mode_control_detect_current() can see a bound gadget, and it can't
     * when nothing is plugged in). Set here rather than next to the nav_push
     * below so leaving DAC mode clears it just as reliably. */
    usb_dac_mode_active = (usb_mode_switch_target == USB_MODE_DAC);
    populate_usb_mode_screen(); /* refresh which row shows the accent border */
    gui_settings_sync_adb_toggle();

    /* DAC mode takes over the whole screen with its own overlay (matching
     * the stock firmware's hiby_usb.view) -- the device can't sensibly be
     * used as a normal music player while it's presenting itself to a PC
     * as a USB sound card. Storage/ADB have no such conflict (the app
     * keeps running normally), so they don't get an overlay. */
    if (usb_mode_switch_target == USB_MODE_DAC) {
        /* Same three-way mutual exclusion as the Bluetooth DAC toggle/
         * airplay_toggle_cb() -- all three (local playback, Bluetooth DAC,
         * AirPlay) fight over the same hw:0,0 output. usb_dac_bridge_start()
         * (called from usb_mode_control_apply(), already run on the
         * background thread by this point) already called audio_stop()
         * itself, but that only stops the backend -- the play button icon
         * needs its own explicit refresh, same as every other call site
         * that stops playback out from under the UI. */
        stop_local_playback();
        nav_push(usb_dac_overlay_screen);
    }
}

/* A fresh boot intentionally has no gadget bound, but previously nothing
 * reacted when a PC was plugged in: Storage only appeared after manually
 * switching to DAC/ADB and back, because those taps happened to perform the
 * missing UDC bind. Reapply Storage on every physical connection edge so a
 * PC enumerates it immediately. Never override an active DAC/ADB session. */
/* The card stays mounted here while a PC writes it over USB storage, so the
 * kernel keeps cached names for folders and files the PC renamed or deleted:
 * a renamed Playlists folder still opened by its old name, listing files that
 * then failed to open. Once the PC is done, write back anything the player
 * wrote and drop the unused dentry/inode caches so every later lookup reads
 * the card again. No unmount; in-use entries are left alone by the kernel. */
static void flush_sd_view_after_usb_storage(void) {
#ifndef HOST_BUILD
    sync();
    int fd = open("/proc/sys/vm/drop_caches", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    if (write(fd, "2", 1) != 1) fprintf(stderr, "[usb] drop_caches failed after storage session\n");
    close(fd);
#endif
}

void poll_usb_storage_hotplug(void) {
    battery_external_power_state_t power = battery_get_external_power_state();
    /* A failed power-supply read is not an unplug. Preserve both the cable
     * edge and confirmed host session until power presence is known again. */
    if (power == BATTERY_EXTERNAL_POWER_UNKNOWN) return;
    bool connected = power == BATTERY_EXTERNAL_POWER_CONNECTED;
    bool storage_configured = false;
    if (connected && !usb_storage_session.host_seen && !usb_mode_switch_active &&
        (!usb_cable_state_initialized || !usb_cable_was_connected ||
         lv_tick_elaps(usb_storage_host_check_tick) >= 250)) {
        usb_storage_host_check_tick = lv_tick_get();
        storage_configured = usb_mode_control_storage_host_configured();
    }
    bool storage_was_active = gui_network_usb_storage_session_active();
    bool storage_session_ended = usb_storage_session_poll(&usb_storage_session,
                                                          connected, storage_configured);
    if (!usb_cable_state_initialized) {
        usb_cable_state_initialized = true;
        usb_cable_was_connected = connected;
        /* Storage is the default gadget when a PC is plugged in, including
         * a cable already present at boot. ADB and DAC stay opt-in from
         * Settings; usb_mode_control_apply() tears Storage down first. */
        usb_storage_rebind_pending = connected;
    } else if (connected && !usb_cable_was_connected) {
        usb_storage_rebind_pending = true;
    }
    usb_cable_was_connected = connected;
    if (!storage_was_active && gui_network_usb_storage_session_active())
        gui_library_suspend_boot_prompt();
    if (storage_session_ended) flush_sd_view_after_usb_storage();
    /* Not while the device is a USB sound card. host_seen latches for the
     * whole cable session, so a Storage phase before the user switched to
     * DAC (poll_usb_storage_hotplug() force-binds Storage on every fresh
     * connect) was still latched here and fired a rescan on unplug -- the
     * modal "Updating music database..." taking over the DAC overlay, long
     * after the host was done with the card. The latch itself is already
     * cleared by usb_storage_session_poll() above, so this only suppresses
     * the rescan; anything the PC wrote during that earlier Storage phase
     * is picked up by the next ordinary rescan trigger or a manual one from
     * Settings. */
    if (storage_session_ended && !usb_dac_mode_active && gui_library_auto_rescan_enabled()) {
        gui_library_request_files_changed_prompt();
    }

    if (!connected || !usb_storage_rebind_pending || usb_mode_switch_active ||
        usb_mode_control_storage_write_active()) return;

    usb_mode_t live_mode;
    bool have_live_mode = usb_mode_control_detect_current(&live_mode);
    if (have_live_mode && (live_mode == USB_MODE_DAC || live_mode == USB_MODE_ADB)) {
        usb_storage_rebind_pending = false;
        return;
    }

    if (start_usb_mode_switch(USB_MODE_STORAGE)) usb_storage_rebind_pending = false;
}

bool gui_network_usb_storage_session_active(void) {
    if (!usb_storage_session.host_seen || !usb_cable_was_connected) return false;

    bool adb_confirmed = false;
    if (!usb_mode_switch_active) {
        usb_mode_t live_mode;
        adb_confirmed = usb_mode_control_detect_current(&live_mode) &&
                        live_mode == USB_MODE_ADB;
    }
    return usb_storage_session_blocks_local_work(&usb_storage_session,
                                                  usb_cable_was_connected,
                                                  adb_confirmed);
}

static void usb_mode_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    usb_mode_t target = usb_mode_options[index].mode;
    if (current_settings.usb_mode == (int) target) return;
    start_usb_mode_switch(target);
}

void usb_mode_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* current_settings.usb_mode is only a UI hint (see settings.h's own
     * comment: never re-applied to hardware on startup, so it can't be
     * trusted to reflect reality after a reboot or any change made outside
     * this app) -- correct it against live gadget state whenever that
     * state is unambiguous, rather than showing a stale/wrong row
     * highlighted, which is exactly what was happening before (always
     * defaulted to showing Storage selected regardless of what was
     * actually connected). Just sysfs reads, fast enough for the UI
     * thread. */
    usb_mode_t detected;
    if (usb_mode_control_detect_current(&detected) && current_settings.usb_mode != (int) detected) {
        current_settings.usb_mode = (int) detected;
        settings_save_async(&current_settings);
    }
    populate_usb_mode_screen();
    nav_push(usb_mode_screen);
}

void gui_network_usb_build(void) {
    usb_mode_screen = network_list_screen(TR("USB Mode"), &usb_mode_list);
}

void gui_network_usb_teardown(void) {
    network_delete_screen(&usb_mode_screen);
    usb_mode_list = NULL;
}

bool gui_network_usb_has_work(void) {
    return usb_mode_switch_active || usb_bridge_restart_active || usb_dac_charging_active;
}

void gui_network_usb_cancel(void) {
    if (usb_mode_switch_active) {
        if (!usb_mode_switch_queued) pthread_join(usb_mode_switch_thread, NULL);
        usb_mode_switch_queued = false;
        usb_mode_switch_active = false;
    }
    if (usb_dac_charging_active) {
        pthread_join(usb_dac_charging_thread, NULL);
        finish_usb_dac_charging_update(false, true);
    }
}

#if BOARD_R1_HARDWARE
static void * usb_dac_charging_thread_func(void * arg) {
    (void) arg;
    usb_dac_charging_succeeded =
        usb_mode_control_set_dac_charging_disabled(usb_dac_charging_target_disabled);
    atomic_store_explicit(&usb_dac_charging_done, true, memory_order_release);
    return NULL;
}
#endif

static void finish_usb_dac_charging_update(bool show_failure, bool thread_already_joined) {
    if (!usb_dac_charging_active ||
        !atomic_load_explicit(&usb_dac_charging_done, memory_order_acquire)) return;
    usb_dac_charging_active = false;
    if (!thread_already_joined) pthread_join(usb_dac_charging_thread, NULL);
    if (usb_dac_charging_succeeded) {
        current_settings.usb_dac_disable_charging = usb_dac_charging_target_disabled;
        settings_save_async(&current_settings);
    }
    gui_settings_sync_usb_dac_charging_toggle();
    if (!usb_dac_charging_succeeded && show_failure)
        show_error_toast(TR("Failed to update USB charging"));
}

#if BOARD_R1_HARDWARE
void gui_network_set_usb_dac_charging_disabled(bool disabled) {
    if (usb_dac_charging_active) {
        show_info_toast(TR("USB charging setting is busy"));
        gui_settings_sync_usb_dac_charging_toggle();
        return;
    }

    usb_dac_charging_target_disabled = disabled;
    atomic_store_explicit(&usb_dac_charging_done, false, memory_order_relaxed);
    usb_dac_charging_active = true;
    int rc = pthread_create(&usb_dac_charging_thread, NULL, usb_dac_charging_thread_func, NULL);
    if (rc != 0) {
        usb_dac_charging_active = false;
        show_error_toast(TR("Could not start USB charging update"));
        gui_settings_sync_usb_dac_charging_toggle();
        return;
    }
}
#else
void gui_network_set_usb_dac_charging_disabled(bool disabled) {
    (void) disabled;
}
#endif

static bool usb_live_operation_blocks_update(void) {
    /* Check the live UDC state so a confirm click between polls cannot race
     * a newly-configured USB storage host. */
    return usb_mode_switch_active || usb_mode_switch_queued || usb_bridge_restart_active ||
           usb_dac_mode_active || usb_mode_control_storage_host_configured();
}

bool gui_network_usb_prompt_invalidated(void) {
    return usb_live_operation_blocks_update() || gui_network_usb_storage_session_active();
}

bool gui_network_firmware_update_usb_blocked(void) {
    if (usb_live_operation_blocks_update()) return true;
    if (!gui_network_usb_storage_session_active()) return false;

    /* The historical latch stays set until unplug so storage cache flush and
     * rescan semantics remain intact. Permit updates on this cable only when
     * the currently configured gadget is positively identified as ADB. */
    usb_mode_t live_mode;
    return !usb_mode_control_detect_current(&live_mode) || live_mode != USB_MODE_ADB;
}

bool gui_network_usb_dac_mode_active(void) { return usb_dac_mode_active; }
