#ifndef GUI_NATIVE_AUDIO_H
#define GUI_NATIVE_AUDIO_H
#include <lvgl/lvgl.h>
void gui_native_audio_add_effect_rows(lv_obj_t * list, lv_obj_t ** mseb_row,
                                     lv_obj_t ** soundstage_row);
void gui_native_audio_add_play_through_rows(lv_obj_t * list);
void gui_native_audio_deinit(void);
bool gui_native_audio_quick_toggle_value(int kind);
void gui_native_audio_quick_toggle_set(int kind, bool value);
void gui_native_audio_quick_toggle_open(int kind);
void gui_native_audio_refresh_gain(void);
lv_obj_t * gui_native_audio_add_gain_row(lv_obj_t * list);
#endif
