#ifndef GUI_LYRICS_H
#define GUI_LYRICS_H
#include <stdbool.h>
#include "lvgl/lvgl.h"

void gui_lyrics_init(void);
/* Deletes both screens this module owns so gui_reload.c's in-process UI
 * reload can call gui_lyrics_init() again from a clean slate. */
void gui_lyrics_teardown(void);
lv_obj_t * gui_lyrics_get_screen(void);

void gui_lyrics_poll_load(void);
void gui_lyrics_poll_backdrop(void);
void gui_lyrics_on_cover_changed(int current_playlist_index);
void gui_lyrics_load_track(int index, const char * path);
void gui_lyrics_open_screen(void);
/* Precomputes or updates line geometry without changing list visibility,
 * parenting, scroll position, or row-pool state. */
void gui_lyrics_prepare_layout(void);

/* Look of the embedded pane, from the player layout's roles. Colors apply to
 * every embedded pane where has_normal / has_active is set; align and
 * on_empty_tap only to a pane placed by gui_lyrics_show_embedded_area().
 * The struct is copied. NULL restores the defaults. */
typedef struct {
    bool has_normal;
    lv_color_t normal;
    bool has_active;
    lv_color_t active;
    bool has_align;
    lv_text_align_t align;
    bool active_marker; /* Accent rule beside the current embedded lyric. */
    void (*on_empty_tap)(void);
} gui_lyrics_look_t;
void gui_lyrics_set_look(const gui_lyrics_look_t * look);
/* Same as gui_lyrics_prepare_layout() for an embedded pane `width` pixels wide. */
void gui_lyrics_prepare_layout_for_width(int32_t width);
/* Like gui_lyrics_show_embedded() for the exact rectangle x, y, w, h of parent
 * (a layout's own lyrics area). */
void gui_lyrics_show_embedded_area(lv_obj_t * parent, int32_t x, int32_t y, int32_t w, int32_t h);
/* Borrows parent for content only; no navigation or player chrome changes.
 * Caller must hide before deleting parent (including player teardown). */
void gui_lyrics_show_embedded(lv_obj_t * parent, int32_t top, int32_t height);
/* Prepares the same full-width or layout-area pane hidden before motion. */
void gui_lyrics_prepare_embedded(lv_obj_t * parent, int32_t x, int32_t y,
                                int32_t width, int32_t height, bool area_mode);
void gui_lyrics_hide_embedded(void);
void lyrics_font_size_settings_row_cb(lv_event_t * e);

bool gui_lyrics_has_background_work(void);
void gui_lyrics_cancel_background_work(void);
void gui_lyrics_refresh_layout(void);
/* Pauses the per-tick timer and hides the backdrop -- everything this
 * screen needs done before its own nav-stack entry is popped, whether via
 * close_lyrics_screen()'s own nav_pop() or gui_shell.c's live back-swipe
 * (which does its own stack-only pop and defers the actual screen load to
 * the settle animation's completion). */
void gui_lyrics_prepare_exit(void);

#endif
