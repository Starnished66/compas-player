#pragma once
#include <lvgl/lvgl.h>
#include "../../shell/gui_notifications.h"

void gui_queue_init(void);
/* Deletes every screen/popup this module owns so gui_reload.c's in-process
 * UI reload can call gui_queue_init() again from a clean slate. */
void gui_queue_teardown(void);
void open_queue_screen(void);
void populate_queue_screen(void);
void gui_queue_poll(void);
bool gui_queue_boot_prompt_blocked(void);
bool gui_queue_song_context_menu_visible(void);
void open_song_context_menu(const char * path);
void open_song_context_menu_stream(const char * path, void (*queue_cb)(bool next));
void open_song_context_menu_for(const char * path, unsigned int capabilities);
void open_song_context_menu_with_rows(const char * path, unsigned int capabilities,
                                      const menu_popup_row_t * extra_rows, int extra_row_count);
enum {
    SONG_MENU_PLAYBACK = 1u << 0,
    SONG_MENU_PLAYLIST = 1u << 1,
    SONG_MENU_RATING = 1u << 2,
    SONG_MENU_NAVIGATION = 1u << 3,
    SONG_MENU_INFORMATION = 1u << 4,
    SONG_MENU_REFRESH = 1u << 5,
    SONG_MENU_ALL = (1u << 6) - 1,
};
void hide_song_context_menu_popup(void);
