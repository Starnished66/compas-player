#ifndef GUI_TEXT_VIEW_H
#define GUI_TEXT_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl/lvgl.h"

/* The view uses the current body font unless opts->text_size names another
 * of the list row fonts; a Lua font_px is ignored on purpose. There is no
 * API to build an arbitrary pixel size, so font_px is not part of this C API. */

#define GUI_TEXT_VIEW_MAX_BYTES (256u * 1024u)
#define GUI_TEXT_VIEW_MAX_PAGES 8192

/* Picture pages: the text may contain "<ESC><index><ESC>" (index 0-based,
 * 1 to 3 digits) naming images[index], an LVGL-readable file path. Such a
 * marker gets a page of its own showing the picture scaled to fit; text
 * pages end before it. An ESC that is not a valid marker is plain text. */
#define GUI_TEXT_VIEW_MAX_IMAGES 64

/* Turning past either end of the text, when on_end is set: direction -1 is
 * back from page 1, +1 is forward from the last page once pagination is
 * complete. The callback may replace the view; the caller must not touch
 * per-content state after it returns. */
typedef void (*gui_text_view_end_fn)(int direction, void * user);

typedef struct {
    bool has_page;     /* page is 1-based */
    int page;
    bool has_offset;   /* byte offset into the copied text */
    size_t offset;
    const char * const * images; /* copied; may be NULL */
    int image_count;             /* at most GUI_TEXT_VIEW_MAX_IMAGES */
    const char * text_size;      /* "small", "medium", "large" or NULL for the body font */
    bool has_background;         /* screen background, 0xRRGGBB; else the theme */
    uint32_t background;
    bool has_text_color;         /* page text, title and footer, 0xRRGGBB; else the theme */
    uint32_t text_color;
    gui_text_view_end_fn on_end; /* NULL: page 1 back closes, last page forward does nothing */
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
 * screen was not inited, or a slide transition is animating. A page past the
 * last one lands on the last page. Callbacks never
 * run inside this call: the first on_turn comes from the pagination timer,
 * and on_close from a timer after the back transition finishes.
 * NULL opts means page 1. If both has_page and has_offset, page wins.
 * Callbacks may be NULL. user is not freed. */
bool gui_text_view_show(const char * title, const void * text, size_t text_len,
                        const gui_text_view_opts * opts,
                        gui_text_view_turn_fn on_turn, gui_text_view_close_fn on_close,
                        void * user);

/* The user pointer of the open view, or NULL when none is open. */
void * gui_text_view_user(void);

/* Swaps the open view's content in place: no navigation push or slide, and
 * the old on_turn/on_close are dropped without being called. Takes the same
 * arguments as gui_text_view_show(). Returns false, leaving the view as it
 * was, when no view is open or the new content cannot be built. Safe to call
 * from inside an on_turn or on_end callback. */
bool gui_text_view_replace(const char * title, const void * text, size_t text_len,
                           const gui_text_view_opts * opts,
                           gui_text_view_turn_fn on_turn, gui_text_view_close_fn on_close,
                           void * user);

#endif /* GUI_TEXT_VIEW_H */
