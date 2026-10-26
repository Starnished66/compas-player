#ifndef COMPACT_LIST_H
#define COMPACT_LIST_H

#include "lvgl/lvgl.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char * label;
    int64_t identity;              /* optional stable DB/item identity for row decorators */
    const char * trailing_asset;   /* optional theme-relative badge asset */
    const char * subtitle;         /* optional; same borrowed lifetime as label */
    bool is_action;                /* explicit action affordance, not a song */
    uint64_t artwork_key;           /* optional stable thumbnail key, separate from row identity */
    const char * artwork_name;      /* optional artist name for artist-image resolution */
} compact_list_item_t;

/* Fixed size of a paged-mode row label buffer (compact_list_fetch_page_cb_t
 * below) -- matches cached_tags_t.title's own size (metadata_db.h), the
 * largest display string a paged provider actually needs to carry. */
#define COMPACT_LIST_LABEL_MAX 384

typedef struct {
    char label[COMPACT_LIST_LABEL_MAX];
    int64_t identity;
    char trailing_asset[64];
    char subtitle[256];
    bool is_action;
    uint64_t artwork_key;
    char artwork_name[128];
} compact_list_page_row_t;

/* Fired when a row is tapped, with the index into the `items` array passed
 * to build_compact_list_screen() -- not an lv_event_cb_t, since the
 * virtualized row widgets underneath (see compact_list.c) are reused
 * across many different indices over the list's lifetime, so there's no
 * single fixed LVGL event user_data to bind an index to at row-creation
 * time the way a one-widget-per-item list could. */
typedef void (*compact_list_click_cb_t)(int index);

/* Titled screen: real back-arrow button and a vertically scrollable,
 * virtualized list of compact shared-height rows (no icon, no accessory -- just a
 * label). Virtualized because a flat list can contain thousands of songs:
 * only a small pool of row widgets exists at once, repositioned and
 * relabeled as the list scrolls, avoiding the memory and layout overhead
 * of allocating an LVGL object per song. `items` (and the strings its `label`
 * pointers reference) must stay valid for the screen's entire lifetime --
 * this function keeps its own copy of the `items` array itself (freed
 * automatically when the screen is deleted), but not of the label strings
 * themselves, matching how every existing caller already sources labels
 * from long-lived backing arrays (song tags, artist/album group names)
 * that only go away alongside a full library rescan, at which point these
 * screens get torn down and rebuilt too. */
/* on_long_press: fired on LV_EVENT_LONG_PRESSED, same index-resolution
 * shape as on_click -- NULL for a list whose rows aren't individual songs
 * (Artists/Albums/Album Artist name rows, the Time Zone city list), so
 * there's nothing sensible to long-press into a context menu. Passed
 * straight through to build_compact_list_widget() below. */
/* out_title_label: NULL if the caller never needs to change the title text
 * after construction (every existing caller so far) -- pass a real pointer
 * only when the screen's title changes at runtime (e.g. a Subsonic list
 * screen reused for different artists/albums, whose title reflects
 * whichever one is currently shown). */
lv_obj_t * build_compact_list_screen(const char * title, lv_event_cb_t back_btn_cb,
                                      const compact_list_item_t * items, int item_count,
                                      compact_list_click_cb_t on_click, compact_list_click_cb_t on_long_press,
                                      lv_obj_t ** out_list, lv_obj_t ** out_title_label, int32_t row_width,
                                      bool enable_now_playing, lv_style_t * now_playing_style);

/* The virtualized list widget itself (what build_compact_list_screen()
 * builds internally), with no screen/back-button/title wrapper -- for a
 * caller that already has its own screen and wants to attach a compact
 * list directly onto it (e.g. a search-results overlay layered on top of
 * an existing screen's own UI). Returns the list object directly (not a
 * screen) -- caller owns positioning it via lv_obj_align()/lv_obj_set_size().
 * row_width overrides list_row_style's own LIST_ROW_WIDTH per-instance (pass
 * LIST_ROW_WIDTH for the shared default, or LIST_ROW_WIDTH_WIDE to match a
 * widened parent screen -- e.g. the Files search overlay matching Files'
 * own now-wider rows). enable_now_playing/now_playing_style: pass true and
 * a shared style when the list needs a now-playing marker, or false/NULL
 * when it does not (e.g. the Files search overlay or timezone city list).
 * The style pointer is attached to the marker and can remain live as its
 * properties change; compact_list.c never resolves its color itself. */
lv_obj_t * build_compact_list_widget(lv_obj_t * parent, const compact_list_item_t * items, int item_count,
                                      compact_list_click_cb_t on_click, compact_list_click_cb_t on_long_press,
                                      int32_t row_width, bool enable_now_playing, lv_style_t * now_playing_style);

/* Shows/moves/hides a thin accent-colored bar flush against the screen's
 * far-left edge (independent of row_width/any row's own inset -- this is a
 * "far left of the screen" indicator, not "far left of the row"), at the
 * same y as the row for `item_index` (an index into whatever `items` array
 * this list was built/last set with) -- item_index < 0 hides it. No-op if
 * this list wasn't built with enable_now_playing=true. Position tracks the
 * item directly (not the visible scroll window), so LVGL's own list
 * scrolling/clipping naturally shows or hides it as the matching row
 * scrolls in and out of view -- no separate scroll-event wiring needed. */
void compact_list_set_now_playing(lv_obj_t * list, int item_index);

/* Scrolls a build_compact_list_screen() list so item `index` lands at the
 * top of the viewport -- exposed as a semantic helper (rather than exposing
 * COMPACT_LIST_TOP_PAD/COMPACT_LIST_ROW_STRIDE themselves, private to
 * compact_list.c) so callers like gui.c's A-Z browse index don't need to
 * know this list's internal row geometry. */
void compact_list_scroll_to_index(lv_obj_t * list, int index);

/* Swaps a build_compact_list_screen() list's contents in place -- used for
 * live search filtering -- without the delete()+rebuild this project
 * otherwise uses for changing a screen's list contents. */
void compact_list_set_items(lv_obj_t * list, const compact_list_item_t * items, int item_count);

/* ---- Paged mode -- Rockbox-style DB-backed lists. An alternative to
 * compact_list_set_items() for a list whose full content is too large to
 * ever materialize at once (the local library's own All Songs/Artists/
 * Albums/Album Artist screens): instead of a caller-owned items[] array,
 * rows are fetched on demand, a bounded page at a time, from a callback --
 * the list still only ever holds ~20 real LVGL row objects (same
 * virtualization as eager mode), but now the underlying DATA is bounded
 * too, not just the widget count. ---- */

/* Fills out_labels[0..count) with the display label for logical rows
 * [offset, offset+count) (0-indexed into the full, currently-sorted/
 * filtered result set) -- e.g. a thin wrapper around metadata_db_get_
 * songs_page()/get_groups_page() that formats each row's own display
 * title/name into the fixed COMPACT_LIST_LABEL_MAX-sized buffer. Returns
 * the number of rows actually written (less than `count` only at the very
 * end of the result set -- ctx knows its own real total already via
 * whatever populated it, this isn't how the list learns the total; see
 * compact_list_set_paged_provider()'s own total_count argument for that).
 * Called from the LVGL/main thread only (same thread every other compact_
 * list callback runs on), so it's safe to call straight into metadata_db.c
 * (its own METADATA_DB_GUARD) or any other main-thread-only state. */
typedef int (*compact_list_fetch_page_cb_t)(void * ctx, int offset, int count,
                                             compact_list_page_row_t out_rows[]);

/* Optional visible-row decoration hook. Called only on the LVGL thread for
 * the fixed recycled row pool. `leading_image` is owned by the list and may
 * be assigned a static asset or a caller-owned descriptor whose lifetime
 * exceeds the assignment. */
typedef void (*compact_list_row_decorator_cb_t)(lv_obj_t * list, lv_obj_t * row,
                                                lv_obj_t * leading_image, int logical_index,
                                                int pool_slot, int64_t identity, uint64_t artwork_key,
                                                const char * artwork_name, void * ctx);
void compact_list_set_row_decorator(lv_obj_t * list, compact_list_row_decorator_cb_t cb, void * ctx);
/* Clears every leading image currently showing `src` (pointer compare only,
 * no decorator run). For owners about to free a borrowed image descriptor. */
/* Returns true when at least one row image was using src. */
bool compact_list_detach_image_src(lv_obj_t * list, const void * src);
/* True when a row currently in the list's recycled pool shows this artwork key. */
bool compact_list_shows_artwork_key(lv_obj_t * list, uint64_t key);
/* Makes each visible trailing_asset a separate tappable accessory. The
 * callback receives the row's current logical index, including after the
 * virtual row has been recycled during scrolling. NULL disables it. */
void compact_list_set_trailing_click(lv_obj_t * list, compact_list_click_cb_t cb);
void compact_list_refresh_visible(lv_obj_t * list);
void compact_list_refresh_all(void);
/* Re-runs only the visible row decorators and image alignment for already
 * bound compact-list rows. Does not fetch a paged provider, move the virtual
 * window, or rewrite row labels/identity; use for artwork/cache changes. */
void compact_list_refresh_decorations(lv_obj_t * list);
/* Rebinds artwork on entry, retrying an uncovered failed page when needed. */
void compact_list_refresh_on_show(lv_obj_t * list);

/* Re-runs label/accessory decoration for one currently visible logical row.
 * A row that has already scrolled out is intentionally ignored. */
void compact_list_refresh_item(lv_obj_t * list, int logical_index);

/* Changes only this compact list's row height. Useful for denser/roomier
 * feature areas without changing Settings and every other shared row. */
void compact_list_set_row_height(lv_obj_t * list, int32_t row_height);

/* Switches list to (or refreshes) paged mode: total_count is the logical
 * row count (drives the scrollbar range and every fetch_page() bound
 * above) -- call again (passing the same fetch_page/ctx) after the
 * underlying data changes (e.g. a library rescan, or a live search's
 * filter changing) to refresh the count and invalidate the row cache.
 * Pass fetch_page == NULL to switch back to eager mode -- the list then
 * shows whatever compact_list_set_items() last gave it (empty if that was
 * never called); total_count/ctx are ignored in that case. */
void compact_list_set_paged_provider(lv_obj_t * list, compact_list_fetch_page_cb_t fetch_page, void * ctx,
                                      int total_count);


#endif /* COMPACT_LIST_H */
