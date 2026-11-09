#ifndef GUI_NATIVE_AUDIO_H
#define GUI_NATIVE_AUDIO_H
#include <lvgl/lvgl.h>
void gui_native_audio_add_sound_rows(lv_obj_t * list);
void gui_native_audio_add_play_through_rows(lv_obj_t * list);
void gui_native_audio_deinit(void);
bool gui_native_audio_quick_toggle_value(int kind);
void gui_native_audio_quick_toggle_set(int kind, bool value);
void gui_native_audio_quick_toggle_open(int kind);
void gui_native_audio_refresh_gain(void);
void gui_native_audio_show_gain(lv_obj_t * dropdown);
#endif
