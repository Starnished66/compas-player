#include "gui_remote_settings.h"

#include <stdio.h>
#include <string.h>

#include "../../application/gui.h"
#include "../../library/browse/gui_library.h"
#include "../../player/screen/gui_player.h"
#include "../../shell/gui_notifications.h"
#include "../../shell/gui_shell.h"
#include "../../text/i18n/i18n.h"
#include "../../../audio/engine/audio.h"
#include "../../../core/persistence/settings.h"
#include "../../../core/persistence/settings_registry.h"
#include "../../../core/system/logging/db_log.h"
#include "../../../hardware/device/device_config.h"
#include "../../../hardware/indicators/backlight.h"
#include "../../../hardware/input/hw_buttons.h"
#include "../../../hardware/indicators/led_control.h"
#include "../../../hardware/power/charge_limiter.h"
#include "../../../network/remote/control/remote_settings.h"
#include "../../../network/remote/control/remote_settings_gate.h"
#include "../../../network/wifi/wifi_control.h"
#ifndef HOST_BUILD
#include "../../../audio/output/audio_output.h"
#endif

static lv_obj_t * allow_switch;
extern bool library_rescan_active; /* gui_library.c: set while a scan runs */

/* ---- Registry hooks ---- */

static bool feature_present(unsigned flag) {
#ifdef HOST_BUILD
    (void) flag;
    return false;
#else
    return flag == SET_F_DRE ? audio_output_dac_dre_supported() : audio_output_dac_controls_supported();
#endif
}

static bool runtime_value(const setting_desc_t * desc, int * out) {
    if (strcmp(desc->key, "wifi_enabled") != 0) return false;
    *out = wifi_control_is_enabled() ? 1 : 0;
    return true;
}

static bool rescan_running(void) {
    return __atomic_load_n(&library_rescan_active, __ATOMIC_RELAXED);
}

void gui_remote_settings_init(void) {
    int loud = 0;
    if (!device_config_get_volume_warn_threshold(&loud)) loud = 0;
    settings_registry_hooks_t hooks = { .feature_present = feature_present, .runtime_value = runtime_value,
                                        .loud_volume_percent = loud };
    settings_registry_set_hooks(&hooks);
    remote_settings_set_rescan_probe(rescan_running);
    settings_registry_note_save(&current_settings);
}

/* ---- The permission switch ---- */

bool gui_remote_settings_allowed(void) {
    return remote_settings_gate_open(remote_settings_now_ms());
}

static void allow_switch_deleted_cb(lv_event_t * e) {
    if (allow_switch == lv_event_get_target(e)) allow_switch = NULL;
}

void gui_remote_settings_bind_switch(lv_obj_t * sw) {
    allow_switch = sw;
    if (sw) lv_obj_add_event_cb(sw, allow_switch_deleted_cb, LV_EVENT_DELETE, NULL);
}

static void show_allow_state(bool on) {
    if (!allow_switch) return;
    if (on) lv_obj_add_state(allow_switch, LV_STATE_CHECKED);
    else lv_obj_clear_state(allow_switch, LV_STATE_CHECKED);
}

void gui_remote_settings_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool on = !gui_remote_settings_allowed();
    remote_settings_gate_set(on, remote_settings_now_ms());
    show_allow_state(on);
}

/* ---- Applying a change ---- */

static int stored_value(const setting_desc_t * d) {
    const unsigned char * base = (const unsigned char *) &current_settings;
    int value = 0;
    if (d->size == 1) return *(const bool *) (base + d->offset) ? 1 : 0;
    memcpy(&value, base + d->offset, sizeof(int));
    return value;
}

static void store_value(const setting_desc_t * d, int value) {
    unsigned char * base = (unsigned char *) &current_settings;
    if (d->size == 1) *(bool *) (base + d->offset) = value != 0;
    else memcpy(base + d->offset, &value, sizeof(int));
}

/* Same sequence as the Max Volume slider on the device. */
static void apply_max_volume(int percent) {
    float previous_volume = audio_get_volume();
    current_settings.max_volume_percent = percent;
    audio_set_max_volume_percent(percent);
    float capped_volume = audio_get_volume();
    if (capped_volume < previous_volume) {
        int32_t current_percent = (int32_t) (capped_volume * 100.0f + 0.5f);
        gui_player_set_volume_percent(current_percent);
        gui_player_remember_volume_percent(current_percent);
        refresh_volume_topbar(current_percent);
    }
}

static void apply_dac(void) {
#ifndef HOST_BUILD
    audio_output_set_dac_preferences(current_settings.dac_filter, current_settings.dac_nos,
                                     current_settings.dac_dre);
#endif
}

/* Settings with a device-side setter use it; the rest are a plain store plus
 * the same live call their screen makes. */
static void apply_change(const setting_desc_t * d, int value) {
    const char * key = d->key;
    if (strcmp(key, "replaygain_mode") == 0) gui_player_set_replaygain_mode(value);
    else if (strcmp(key, "gapless_enabled") == 0) gui_player_set_gapless_enabled(value != 0);
    else if (strcmp(key, "crossfade_enabled") == 0) gui_player_set_crossfade_enabled(value != 0);
    else if (strcmp(key, "play_mode") == 0) gui_player_set_play_mode(value);
    else if (strcmp(key, "car_mode_enabled") == 0) gui_player_set_car_mode_enabled(value != 0);
    else if (strcmp(key, "max_volume_percent") == 0) apply_max_volume(value);
    else {
        store_value(d, value);
        if (strncmp(key, "dac_", 4) == 0) apply_dac();
#ifndef HOST_BUILD
        else if (strcmp(key, "usb_dsd_output") == 0) audio_output_set_usb_dsd_mode(value);
#endif
        else if (strcmp(key, "brightness_percent") == 0) backlight_request_normal_percent(value);
        else if (strcmp(key, "screen_dimming_enabled") == 0 && !value) backlight_set_dimmed(false);
        else if (strcmp(key, "show_battery_percent") == 0) gui_shell_apply_topbar_settings();
        else if (strcmp(key, "volume_lock_screen_off") == 0) hw_buttons_set_volume_lock_screen_off(value != 0);
        else if (strcmp(key, "led_indicator_enabled") == 0) led_control_apply(value != 0);
        else if (strcmp(key, "charge_limiter_enabled") == 0) charge_limiter_poll(value != 0, true);
        else if (strcmp(key, "safe_charging_enabled") == 0) safe_charging_poll(value != 0, true);
    }
}

/* The device keeps the dim delay shorter than the screen timeout. */
static void keep_dim_delay_below_timeout(void) {
    if (!current_settings.screen_timeout_enabled) return;
    if (current_settings.screen_dim_delay_seconds < current_settings.screen_timeout_seconds) return;
    int best = SCREEN_DIM_DELAY_STEPS[0];
    for (int i = 0; i < SCREEN_DIM_DELAY_STEP_COUNT; i++)
        if (SCREEN_DIM_DELAY_STEPS[i] < current_settings.screen_timeout_seconds) best = SCREEN_DIM_DELAY_STEPS[i];
    current_settings.screen_dim_delay_seconds = best;
}

static void apply_job(const remote_settings_job_t * job) {
    bool touched_timing = false;
    for (int i = 0; i < job->count; i++) {
        const setting_desc_t * d = job->changes[i].desc;
        int old_value = stored_value(d);
        touched_timing |= strncmp(d->key, "screen_", 7) == 0;
        apply_change(d, job->changes[i].value);
        DB_LOG("SETTINGS", "remote change key=%s old=%d new=%d via=%s", d->key, old_value, job->changes[i].value,
               job->via);
    }
    if (touched_timing) {
        int before = current_settings.screen_dim_delay_seconds;
        keep_dim_delay_below_timeout();
        if (before != current_settings.screen_dim_delay_seconds)
            DB_LOG("SETTINGS", "remote change key=screen_dim_delay_seconds old=%d new=%d via=%s (kept below timeout)",
                   before, current_settings.screen_dim_delay_seconds, job->via);
    }
    settings_save_async(&current_settings);
    settings_registry_note_save(&current_settings);
    if (job->count == 1) {
        char text[160];
        snprintf(text, sizeof(text), TR("%s changed from the remote"), TR(job->changes[0].desc->label));
        show_info_toast(text);
    } else if (job->count > 1) {
        show_info_toast(TR("Settings changed from the remote"));
    }
}

void gui_remote_settings_poll(void) {
    uint64_t now = remote_settings_now_ms();
    if (remote_settings_gate_consume_expired(now)) show_info_toast(TR("Remote settings changes turned off"));
    /* Expiry, or the permission being revoked when Remote Control stops, can happen off this thread. */
    bool open = remote_settings_gate_open(now);
    if (allow_switch && lv_obj_has_state(allow_switch, LV_STATE_CHECKED) != open) show_allow_state(open);
    if (remote_settings_consume_rescan()) {
        /* Same entry point as Settings > Library > Update Music Database,
         * which ignores the request while a scan is already running. */
        bool was_running = rescan_running();
        if (!was_running) start_library_rescan();
        bool started = !was_running && rescan_running();
        DB_LOG("SETTINGS", "remote rescan %s", started ? "started" : "not started (already running or blocked)");
        if (started) show_info_toast(TR("Music scan started from the remote"));
    }
    remote_settings_job_t * job = NULL;
    static remote_settings_job_t job_storage;
    job = &job_storage;
    if (remote_settings_consume_job(job)) {
        apply_job(job);
        remote_settings_finish_job(settings_registry_revision());
    }
}
