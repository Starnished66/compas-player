#pragma once
#include <lvgl/lvgl.h>

/* Screen accessors */
lv_obj_t * gui_settings_get_screen(void);
lv_obj_t * gui_settings_get_music_screen(void);
lv_obj_t * gui_settings_get_display_screen(void);
lv_obj_t * gui_settings_get_power_screen(void);
lv_obj_t * gui_settings_get_system_screen(void);
lv_obj_t * gui_settings_get_about_screen(void);
lv_obj_t * gui_settings_get_accent_screen(void);
/* Refreshes the Accent Color screen after any accent change (gui_theme.c). */
void gui_settings_accent_changed(void);
lv_obj_t * gui_settings_get_custom_font_screen(void);
lv_obj_t * gui_settings_get_eq_screen(void);

void gui_settings_init(void);
/* Deletes every screen this module owns (not build_home_screen()'s result --
 * that's gui_shell.c's own static) so gui_reload.c's in-process UI reload
 * can call gui_settings_init() again from a clean slate. */
void gui_settings_teardown(void);
void gui_settings_refresh_font_geometry(void);

lv_obj_t * build_home_screen(void);
lv_obj_t * build_dac_home_screen(void);

void gui_settings_sync_crossfade_toggle(void);
void gui_settings_sync_gapless_toggle(void);
void gui_settings_sync_sleep_timer_toggle(void);
void gui_settings_sync_car_mode(void);
void gui_settings_open_car_mode(void);
void gui_settings_sync_adb_toggle(void);
/* Drives the online firmware update UI (firmware_ota.h); call every tick. */
void poll_firmware_ota(void);
void gui_display_apply_rotation(bool upside_down);
