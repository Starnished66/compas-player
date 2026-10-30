#ifndef GUI_PLAY_BAR_H
#define GUI_PLAY_BAR_H

#include "lvgl.h"

/* The Play / Shuffle pill pair shared by album pages, artist pages and the song lists.
 * Play is filled with the accent colour, Shuffle is an outline. Pressing a pill dims it
 * (opacity) and a click calls its callback with its own user data. */
lv_obj_t * gui_pill_button(lv_obj_t * parent, int32_t w, int32_t h, const char * text, bool filled,
                           lv_event_cb_t cb, void * user_data);

/* The row of the two pills, row_w wide (LV_SYMBOL_PLAY "  Play" and LV_SYMBOL_SHUFFLE "  Shuffle"). */
lv_obj_t * gui_play_buttons_build(lv_obj_t * parent, int32_t row_w, lv_event_cb_t play_cb, void * play_data,
                                  lv_event_cb_t shuffle_cb, void * shuffle_data);

/* A small muted count line ("12 tracks") above the pill row: the header of a song list. Its height
 * is LV_SIZE_CONTENT, so call lv_obj_update_layout() before reading it. */
lv_obj_t * gui_play_bar_build(lv_obj_t * parent, int32_t row_w, const char * count_text,
                              lv_event_cb_t play_cb, lv_event_cb_t shuffle_cb);

/* True for a bar made by gui_play_bar_build(). */
bool gui_play_bar_is(const lv_obj_t * obj);

/* Re-applies the fonts, pill heights and the Play label colour of a long-lived bar after a text size, font
 * or accent change. The caller re-reads the bar's height afterwards. */
void gui_play_bar_restyle(lv_obj_t * root);

#endif
