#pragma once
#include <lvgl/lvgl.h>
#include <stdbool.h>

/* Screen accessors */
lv_obj_t * gui_settings_get_screen(void);
lv_obj_t * gui_settings_get_system_screen(void);
lv_obj_t * gui_settings_get_about_screen(void);
/* Refreshes the Accent Color screen after any accent change (gui_theme.c). */
void gui_settings_accent_changed(void);
/* The keyboard layouts offered in Settings -> Keyboard, in display order.
 * _at() returns the KEYBOARD_LAYOUT_* value and its untranslated label (pass
 * it through TR()), or -1 past the end. */
size_t gui_settings_keyboard_layout_count(void);
int gui_settings_keyboard_layout_at(size_t index, const char ** label);
lv_obj_t * gui_settings_get_eq_screen(void);
/* Direct drawer/navigation entry points. */
void gui_settings_open_eq(void);
void gui_settings_open_sleep_timer(void);
void gui_settings_open_sound(void);
void gui_settings_open_playback(void);
void gui_settings_open_library(void);

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
void gui_settings_sync_usb_dac_charging_toggle(void);
void gui_display_apply_rotation(bool upside_down);
