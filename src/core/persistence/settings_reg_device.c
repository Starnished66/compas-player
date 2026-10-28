#include "settings_registry.h"
#include "settings.h"

/* Display, Power, Connections and Library entries. Wi-Fi, Remote Control,
 * the hostname and the USB mode are listed read-only: changing them can cut
 * the owner off from the player. The PIN is deliberately not here at all. */

#define FIELD(f) offsetof(player_settings_t, f), sizeof(((player_settings_t *) 0)->f)

#define BOOL_ENTRY(k, grp, lbl, field, ap, ...) \
    { .key = k, .group = grp, .label = lbl, .type = SET_BOOL, FIELD(field), .apply = ap, ##__VA_ARGS__ }
#define PRESET_ENTRY(k, grp, lbl, field, table, count, u, ap) \
    { .key = k, .group = grp, .label = lbl, .type = SET_PRESET, FIELD(field), .presets = table, \
      .preset_count = count, .unit = u, .apply = ap }

static const int animation_presets[] = { 0, 25, 50, 75, 100 };

const setting_desc_t settings_reg_device[] = {
    /* Brightness has a floor so a remote change can never black out the screen. */
    { .key = "brightness_percent", .group = "display", .label = "Brightness", .type = SET_INT,
      FIELD(brightness_percent), .min = 5, .max = 100, .unit = "%", .apply = SET_APPLY_LIVE },
    BOOL_ENTRY("screen_timeout_enabled", "display", "Screen Timeout", screen_timeout_enabled, SET_APPLY_LIVE),
    PRESET_ENTRY("screen_timeout_seconds", "display", "Screen Timeout", screen_timeout_seconds,
                 SCREEN_TIMEOUT_STEPS, SCREEN_TIMEOUT_STEP_COUNT, "s", SET_APPLY_LIVE),
    BOOL_ENTRY("screen_dimming_enabled", "display", "Screen Dimming", screen_dimming_enabled, SET_APPLY_LIVE),
    PRESET_ENTRY("screen_dim_delay_seconds", "display", "Screen Dimming", screen_dim_delay_seconds,
                 SCREEN_DIM_DELAY_STEPS, SCREEN_DIM_DELAY_STEP_COUNT, "s", SET_APPLY_LIVE),
    BOOL_ENTRY("show_battery_percent", "display", "Battery Percentage", show_battery_percent, SET_APPLY_LIVE),
    PRESET_ENTRY("animation_scale", "display", "Animation Speed", animation_scale, animation_presets,
                 5, "%", SET_APPLY_LIVE),

    /* Idle shutdown takes presets only; a tiny value would power the player off. */
    PRESET_ENTRY("sleep_timer_minutes", "power", "Sleep Timer", sleep_timer_minutes, SLEEP_TIMER_STEPS,
                 SLEEP_TIMER_STEP_COUNT, "min", SET_APPLY_NEXT_USE),
    BOOL_ENTRY("idle_shutdown_enabled", "power", "Idle Shutdown", idle_shutdown_enabled, SET_APPLY_LIVE),
    PRESET_ENTRY("idle_shutdown_minutes", "power", "Idle Shutdown", idle_shutdown_minutes, IDLE_SHUTDOWN_STEPS,
                 IDLE_SHUTDOWN_STEP_COUNT, "min", SET_APPLY_LIVE),
    BOOL_ENTRY("idle_suspend_enabled", "power", "Suspend to RAM", idle_suspend_enabled,
               SET_APPLY_LIVE),
    /* Charge settings are only the two booleans the device offers, never a level. */
    BOOL_ENTRY("charge_limiter_enabled", "power", "Charge Limit (85%)", charge_limiter_enabled, SET_APPLY_LIVE),
    BOOL_ENTRY("safe_charging_enabled", "power", "Safe Charging (500mA)", safe_charging_enabled, SET_APPLY_LIVE),
    BOOL_ENTRY("led_indicator_enabled", "power", "LED charge indicator", led_indicator_enabled, SET_APPLY_LIVE),

    BOOL_ENTRY("bt_hide_unnamed_devices", "connections", "Hide Unnamed Devices", bt_hide_unnamed_devices,
               SET_APPLY_LIVE),
    BOOL_ENTRY("bt_volume_sync_enabled", "connections", "Bluetooth Volume Sync", bt_volume_sync_enabled,
               SET_APPLY_LIVE, .flags = SET_F_READONLY, .reason = "device_only"),
    BOOL_ENTRY("bt_dac_mode_enabled", "connections", "Bluetooth DAC mode", bt_dac_mode_enabled,
               SET_APPLY_LIVE, .flags = SET_F_READONLY, .reason = "device_only"),
    { .key = "bt_codec", .group = "connections", .label = "Bluetooth Codec", .type = SET_STRING,
      FIELD(bt_codec), .flags = SET_F_READONLY, .reason = "device_only" },
    { .key = "wifi_enabled", .group = "connections", .label = "Wi-Fi", .type = SET_BOOL, .size = 1,
      .flags = SET_F_READONLY | SET_F_RUNTIME, .reason = "would_lock_out_remote" },
    BOOL_ENTRY("remote_control_enabled", "connections", "Remote Control", remote_control_enabled, SET_APPLY_LIVE,
               .flags = SET_F_READONLY, .reason = "would_lock_out_remote"),
    { .key = "hostname", .group = "connections", .label = "Hostname", .type = SET_STRING, FIELD(hostname),
      .flags = SET_F_READONLY, .reason = "would_lock_out_remote" },
    { .key = "usb_mode", .group = "connections", .label = "USB Mode", .type = SET_INT, FIELD(usb_mode),
      .min = 0, .max = 2, .flags = SET_F_READONLY, .reason = "would_lock_out_remote" },
};
const size_t settings_reg_device_count = sizeof(settings_reg_device) / sizeof(settings_reg_device[0]);
