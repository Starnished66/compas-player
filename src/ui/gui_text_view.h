#ifndef GUI_TEXT_VIEW_H
#define GUI_TEXT_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl/lvgl.h"

/* The view uses the current body font; a later Lua font_px is ignored on purpose.
 * There is no API to build an arbitrary pixel size, so font_px is not part of this C API. */

#define GUI_TEXT_VIEW_MAX_BYTES (256u * 1024u)
#define GUI_TEXT_VIEW_MAX_PAGES 8192

/* Picture pages: the text may contain "<ESC><index><ESC>" (index 0-based,
 * 1 to 3 digits) naming images[index], an LVGL-readable file path. Such a
 * marker gets a page of its own showing the picture scaled to fit; text
 * pages end before it. An ESC that is not a valid marker is plain text. */
#define GUI_TEXT_VIEW_MAX_IMAGES 64

typedef struct {
    bool has_page;     /* page is 1-based */
    int page;
    bool has_offset;   /* byte offset into the copied text */
    size_t offset;
    const char * const * images; /* copied; may be NULL */
    int image_count;             /* at most GUI_TEXT_VIEW_MAX_IMAGES */
} gui_text_view_opts;

typedef void (*gui_text_view_turn_fn)(int page, int pages, size_t byte_offset, void * user);
typedef void (*gui_text_view_close_fn)(int page, size_t byte_offset, void * user);

bool gui_text_view_init(void);
void gui_text_view_teardown(void);
lv_obj_t * gui_text_view_get_screen(void);
/* Navigation dropped this screen. A covered view has already been unloaded,
 * so it will not receive another unload event. on_close is queued until any
 * slide transition has finished. */
void gui_text_view_note_nav_removed(void);
bool gui_text_view_is_open(void);

/* Copies title and text. Returns false if a view is already open, the
 * screen was not inited, or a slide transition is animating. Callbacks never
 * run inside this call: the first on_turn comes from the pagination timer,
 * and on_close from a timer after the back transition finishes.
 * NULL opts means page 1. If both has_page and has_offset, page wins.
 * Callbacks may be NULL. user is not freed. */
bool gui_text_view_show(const char * title, const void * text, size_t text_len,
                        const gui_text_view_opts * opts,
                        gui_text_view_turn_fn on_turn, gui_text_view_close_fn on_close,
                        void * user);

#endif /* GUI_TEXT_VIEW_H */
