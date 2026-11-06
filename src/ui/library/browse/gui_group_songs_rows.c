/* The Songs page rows: the incremental row builder, populate, covers and the now-playing marker. */
#include "../../text/i18n/i18n.h"
#include "gui_album_header.h"
#include "gui_album_view.h"
#include "../../widgets/builders/gui_play_bar.h"
#include "../../assets/assets.h"
#include "../../../hardware/indicators/backlight.h"
#include "../../../core/ui/display/ui_wake.h"
#include "../../diagnostics/ui_frame_trace.h"
#include <limits.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gui_group_songs_internal.h"

static bool group_songs_rows_rebuilding;

static void group_songs_prev_page_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || group_songs_page_start <= 0) return;
    group_songs_page_start -= GROUP_SONGS_PAGE_SIZE;
    if (group_songs_page_start < 0) group_songs_page_start = 0;
    populate_group_songs_rows();
}

static void group_songs_next_page_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int next = group_songs_page_start + GROUP_SONGS_PAGE_SIZE;
    if (next >= group_songs_count) return;
    group_songs_page_start = next;
    populate_group_songs_rows();
}

static lv_obj_t * add_group_songs_page_row(const char * text, lv_event_cb_t cb) {
    lv_obj_t * row = lv_label_create(group_songs_list);
    lv_obj_add_style(row, &list_row_style, 0);
    if (group_songs_music_submenu) lv_obj_set_width(row, lv_pct(100));
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    row_label_enable_marquee(row);
    int32_t row_height = ui_list_row_height();
    lv_obj_set_style_height(row, row_height, LV_PART_MAIN);
    lv_obj_add_style(row, &native_row_min_style, 0);
    lv_obj_set_style_pad_top(row,
        (row_height - lv_font_get_line_height(&LIST_ROW_FONT)) / 2, LV_PART_MAIN);
    lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(row, text);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    return row;
}

/* A Previous / Next row; `fmt` takes the first and last entry shown and the total. */
static lv_obj_t * group_songs_add_page_nav_row(const char * fmt, int page_end, lv_event_cb_t cb) {
    char page_text[96];
    snprintf(page_text, sizeof(page_text), fmt, group_songs_page_start + 1, page_end, group_songs_count);
    return add_group_songs_page_row(page_text, cb);
}

/* Music's Artist/Album drill-down uses a deliberate two-line identity.
 * Playlist and Queue rows intentionally retain their established shared
 * builder geometry. */
static void layout_music_submenu_row_text(lv_obj_t * row) {
    if (!row || lv_obj_get_child_count(row) < 2) return;
    lv_obj_t * primary = lv_obj_get_child(row, 0);
    lv_obj_t * secondary = lv_obj_get_child(row, 1);
    int32_t height = lv_obj_get_style_height(row, LV_PART_MAIN);
    int32_t title_height = row_label_bounded_height(lv_obj_get_style_text_font(primary, 0));
    bool has_detail = !lv_obj_has_flag(secondary, LV_OBJ_FLAG_HIDDEN);
    int32_t detail_height = has_detail
        ? row_label_bounded_height(lv_obj_get_style_text_font(secondary, 0)) : 0;
    int32_t gap = has_detail ? BOARD_SCALE_PX(4) : 0;
    int32_t top = (height - title_height - gap - detail_height) / 2;
    if (top < 0) top = 0;
    lv_obj_set_y(primary, top);
    if (has_detail) lv_obj_set_y(secondary, top + title_height + gap);
}

/* Puts the detail line ("4:32 · FLAC") under the row's title. */
void group_song_row_show_detail(lv_obj_t * row, const char * detail) {
    lv_obj_t * secondary = lv_obj_get_child(row, 1);
    lv_label_set_text(secondary, detail);
    lv_obj_remove_flag(secondary, LV_OBJ_FLAG_HIDDEN);
    layout_music_submenu_row_text(row);
}

/* A finger is down or a gesture (drag, fling) is running on any pointer device. */
bool landing_pointer_busy(void) {
    for (lv_indev_t * indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) continue;
        if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED || lv_indev_get_scroll_obj(indev)) return true;
    }
    return false;
}

/* ---- Long song pages are built in pieces ------------------------------------
 * A page of up to GROUP_SONGS_PAGE_SIZE rows (an artist's "All songs", a genre, a big album)
 * costs about 5 ms a row on the R1, so building them all before the page appears kept the
 * first frame waiting for a second. The header and the rows of the first screens are built
 * at once; one spacer child, exactly as tall as everything still to come, keeps the content
 * height (scroll range, scrollbar) what the finished page has. The rest is built by a timer,
 * a few milliseconds per rendered frame (less during a gesture, none while the screen is off
 * or another screen is shown), plus the rows the next frame of a scroll shows. Editing builds
 * every row; the anchor and scroll restore only need the rows they point at (the spacer
 * keeps the scroll range exact); a repopulate forgets the rest. The tail
 * (spacer, Next row, now-playing bar) is recreated after each piece so the child order of the
 * finished list is the one a single pass produces. */
#define GROUP_ROWS_INCREMENTAL_MIN 48
#define GROUP_ROWS_CHUNK_MS 6          /* one chunk of an idle page */
#define GROUP_ROWS_GESTURE_CHUNK_MS 3  /* one chunk while a finger or a fling moves the list */
#define GROUP_ROWS_TIMER_MS 16         /* one chunk per rendered frame at most */
#define GROUP_ROWS_TIMER_OFF_MS 500    /* backlight off: nothing is rendered */
static struct {
    bool active;
    bool album_layout;
    uint8_t bare_mask[(GROUP_SONGS_PAGE_SIZE + 7) / 8]; /* entries that had no detail line when the page began */
    bool layout_dirty;
    int next, end;           /* entries still to build: [next, end) */
    int rows_bare_left, rows_detail_left, headers_left; /* rows without / with a detail line, disc headers */
    int32_t row_h_bare, row_h_detail, header_h;         /* what each takes, measured on the built ones */
    uint32_t overhead_us;    /* the last chunk's layout and tail work, kept inside the next chunk's budget */
    lv_obj_t * spacer;
    lv_obj_t * next_row;
    lv_timer_t * timer;
    uint32_t timer_period;
} gsb;

static bool gsb_slot_bare(int slot) {
    return (gsb.bare_mask[slot >> 3] >> (slot & 7)) & 1;
}

static int group_songs_initial_rows(void) {
    int32_t stride = ui_music_row_height();
    if (stride > BOARD_SCALE_PX(76)) stride = BOARD_SCALE_PX(76); /* album rows are the shortest */
    stride += GUI_ROW_GAP;
    int32_t screen_h = lv_display_get_vertical_resolution(lv_display_get_default());
    return (int) ((screen_h * 3 / 2) / stride) + 2;
}

/* The "Disc N" header above the entry that starts a disc. */
void group_songs_add_disc_header(int i) {
    if (!group_songs_entries[i].show_disc_header) return;
    char header[32];
    snprintf(header, sizeof(header), TR("Disc %d"), group_songs_entries[i].disc_number);
    add_section_header(group_songs_list, header);
}

/* A music-list row for entry `i`, registered in the visible-row table. */
lv_obj_t * group_songs_new_row(int i, const char * title, int32_t reserve) {
    lv_obj_t * row = build_music_list_row(group_songs_list, title, NULL, reserve);
    if (group_songs_music_submenu) lv_obj_set_width(row, lv_pct(100));
    group_songs_visible_rows[i - group_songs_page_start] = row;
    if (group_songs_music_submenu) layout_music_submenu_row_text(row);
    return row;
}

/* One entry of the plain (not editing) page: its disc header when it starts a disc, then the row. */
static void group_songs_add_plain_entry(int i, bool album_layout, bool was_bare) {
    group_songs_add_disc_header(i);
    /* One lv_label via the shared list_row_style, not a container +
     * child label each with their own local style properties -- see
     * list_row_style's own doc comment (screen_builders.h). */
    char album_title[384];
    /* Most pages carry "title\nFLAC" from the start and every row takes that path. An entry that
     * had no detail line when the page began (`was_bare`) and holds "title\ndetail" now, because a
     * probe or the duration cache landed meanwhile, is built bare and gets the detail the way the
     * probe applies it (below): its height and text positions are those a single pass would have
     * given it (a row with a subtitle can be taller than one without in the larger fonts). Every
     * other entry takes the builder a single pass would. */
    const char * title = group_songs_entries[i].title;
    const char * detail = album_layout || !was_bare ? NULL : strchr(title, '\n');
    char * bare = NULL;
    if (detail) {
        size_t len = (size_t) (detail - title);
        bare = malloc(len + 1);
        if (bare) { memcpy(bare, title, len); bare[len] = '\0'; title = bare; }
        detail++;
        if (!bare) detail = NULL;
    }
    lv_obj_t * row = group_songs_new_row(i,
        album_layout ? gui_album_header_row_title(title, album_title, sizeof(album_title)) : title,
        BOARD_SCALE_PX(70));
    free(bare);
    /* Room for the cover from the first frame: the cover (or its
     * placeholder) is added later, and moving the text then made
     * every row jump sideways. */
    if (group_song_has_cover(i)) group_song_row_text_offset(row, true);

    gui_album_view_row_badge(row, song_quality_asset_for_path(group_songs_entries[i].path));
    if (album_layout) gui_album_header_decorate_row(row, i);
    if (detail && detail[0]) group_song_row_show_detail(row, detail); /* as apply_group_probe_row() does */

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, ui_clear_flag_on_press_cb, LV_EVENT_PRESSED, &group_song_row_long_press_fired);
    lv_obj_add_event_cb(row, group_song_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    lv_obj_add_event_cb(row, group_song_row_long_press_cb, LV_EVENT_LONG_PRESSED, (void *) (intptr_t) i);
}

static lv_obj_t * group_songs_add_next_row(int page_end) {
    if (page_end >= group_songs_count) return NULL;
    return group_songs_add_page_nav_row(TR("Next  •  %d–%d of %d"), page_end, group_songs_next_page_cb);
}

static lv_obj_t * group_songs_create_now_playing_bar(void) {
    /* Recreated fresh here (lv_obj_clean() above just destroyed whatever
     * was here before) rather than kept as a truly persistent object --
     * every row in this list gets rebuilt on every populate call already,
     * so this just follows the same pattern. LV_OBJ_FLAG_IGNORE_LAYOUT
     * keeps this list's own flex column layout from trying to stack it in
     * as another row, same trick build_icon_grid_screen() uses for its
     * divider lines. Positioned/shown by refresh_group_songs_now_playing_
     * indicator() below, not here -- that also runs standalone (no rebuild)
     * whenever now_playing_path changes while this screen stays
     * open, e.g. a gapless auto-advance to the next track in the group. */
    lv_obj_t * bar;
    bar = lv_obj_create(group_songs_list);
    lv_obj_remove_style_all(bar);
    /* No row minimum height: album rows can be shorter, and
     * group_songs_place_now_playing_bar() sizes the bar to the real row. */
    lv_obj_set_size(bar, BOARD_SCALE_PX(5), ui_list_row_height());
    lv_obj_add_style(bar, gui_theme_accent_style(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    return bar;
}

/* The spacer's height: what the entries still to build will take, gaps included. The gap
 * before the next row is the one already between the spacer and its neighbour. */
static int32_t group_songs_spacer_height(void) {
    int children = gsb.rows_bare_left + gsb.rows_detail_left + gsb.headers_left;
    if (children <= 0) return 0;
    int32_t gap = lv_obj_get_style_pad_row(group_songs_list, LV_PART_MAIN);
    return gsb.rows_bare_left * gsb.row_h_bare + gsb.rows_detail_left * gsb.row_h_detail +
           gsb.headers_left * gsb.header_h + (children - 1) * gap;
}

/* What the next row of the unbuilt rest is expected to take, gap included (the mean of what is left). */
static int32_t group_songs_stride_estimate(void) {
    int rows = gsb.rows_bare_left + gsb.rows_detail_left;
    int32_t gap = lv_obj_get_style_pad_row(group_songs_list, LV_PART_MAIN);
    int32_t rows_h = rows > 0 ? (gsb.rows_bare_left * gsb.row_h_bare + gsb.rows_detail_left * gsb.row_h_detail + rows / 2) / rows
                              : LV_MAX(gsb.row_h_bare, gsb.row_h_detail);
    return rows_h + gap;
}

static void group_songs_tail_create(int page_end) {
    gsb.spacer = NULL;
    if (gsb.active) {
        gsb.spacer = lv_obj_create(group_songs_list);
        lv_obj_remove_style_all(gsb.spacer);
        lv_obj_remove_flag(gsb.spacer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_size(gsb.spacer, 1, group_songs_spacer_height());
    }
    gsb.next_row = group_songs_add_next_row(page_end);
    group_songs_now_playing_bar = group_songs_create_now_playing_bar();
    gsb.layout_dirty = true;
}

/* The page is being replaced or left: forget the unbuilt rest. The objects go with lv_obj_clean(). */
void group_songs_build_reset(void) {
    gsb.active = false;
    gsb.spacer = NULL;
    gsb.next_row = NULL;
    if (gsb.timer) lv_timer_pause(gsb.timer);
}

/* The real clock. lv_tick_get() stands still for a whole handler pass (main.c hands LVGL its
 * ticks once per loop turn), so a budget measured with it never ran out and one chunk built
 * every row that was left. */
static uint64_t group_rows_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000u + (uint32_t) ts.tv_nsec / 1000u;
}

/* Builds up to `max_rows` more entries (and stops once `budget_ms` is used up when it is not 0,
 * always after at least one row). The budget covers the layout before and after the rows and the
 * tail work too: the last chunk's share of those is held back from this one. */
static void group_songs_build_more(int max_rows, uint32_t budget_ms) {
    if (!gsb.active || !group_songs_list) return;
    UI_TRACE_SPAN_BEGIN(perf_chunk);
    uint64_t started = group_rows_now_us();
    /* Work that is entirely below the viewport is not drawn: nothing visible changes, so no
     * invalidation is recorded for it (the new rows are created at the content origin and moved
     * by the layout, which would otherwise redraw the top of the list and its scrollbar every
     * chunk). The layout pending on the rest of the screen is flushed first, with invalidation
     * on, so only this chunk's own layout runs while it is off. */
    lv_display_t * disp = lv_obj_get_display(group_songs_list);
    bool quiet = false;
    int32_t content_before = 0;
    if (gsb.spacer && disp && lv_display_is_invalidation_enabled(disp)) {
        lv_obj_update_layout(group_songs_list);
        gsb.layout_dirty = false;
        quiet = lv_obj_get_y(gsb.spacer) >= lv_obj_get_scroll_y(group_songs_list) + lv_obj_get_height(group_songs_list);
        if (quiet) {
            content_before = lv_obj_get_scroll_bottom(group_songs_list);
            lv_display_enable_invalidation(disp, false);
        }
    }
    uint64_t rows_started = group_rows_now_us();
    int first = gsb.next, built = 0;
    while (gsb.next < gsb.end && built < max_rows) {
        if (group_songs_entries[gsb.next].show_disc_header) gsb.headers_left--;
        int slot = gsb.next - group_songs_page_start;
        bool bare = gsb_slot_bare(slot);
        if (bare) gsb.rows_bare_left--; else gsb.rows_detail_left--;
        group_songs_add_plain_entry(gsb.next++, gsb.album_layout, bare);
        built++;
        if (budget_ms && group_rows_now_us() - started + gsb.overhead_us >= (uint64_t) budget_ms * 1000u) break;
    }
    uint64_t rows_ended = group_rows_now_us();
    gsb.active = gsb.next < gsb.end;
    /* The tail objects (spacer, Next row, now-playing bar) stay: the new rows were created after
     * them, so the spacer shrinks in place and the tail goes back to the end of the list. The move
     * is not drawn (the objects keep their pixels; the layout moves what is visible and draws that). */
    if (gsb.spacer) {
        if (gsb.active) lv_obj_set_height(gsb.spacer, group_songs_spacer_height());
        else { lv_obj_delete(gsb.spacer); gsb.spacer = NULL; }
    }
    if (built) {
        bool was_enabled = disp && lv_display_is_invalidation_enabled(disp);
        if (was_enabled) lv_display_enable_invalidation(disp, false);
        if (gsb.spacer) lv_obj_move_foreground(gsb.spacer);
        if (gsb.next_row) lv_obj_move_foreground(gsb.next_row);
        if (group_songs_now_playing_bar) lv_obj_move_foreground(group_songs_now_playing_bar);
        if (was_enabled) lv_display_enable_invalidation(disp, true);
    }
    /* Laid out now, before anything else in this pass (a fling's next step reads the scroll
     * range) looks at the list. */
    lv_obj_update_layout(group_songs_list);
    gsb.layout_dirty = false;
    if (now_playing_path[0]) { /* the playing row may be one of the new ones */
        for (int i = first; i < gsb.next; i++)
            if (strcmp(group_songs_entries[i].path, now_playing_path) == 0) {
                group_songs_place_now_playing_bar(true);
                break;
            }
    }
    if (quiet) {
        lv_display_enable_invalidation(disp, true);
        /* The scroll range moved (a row took another height than the spacer assumed): the
         * scrollbar must be drawn again. */
        if (lv_obj_get_scroll_bottom(group_songs_list) != content_before) lv_obj_invalidate(group_songs_list);
    }
    if (!gsb.active && gsb.timer) lv_timer_pause(gsb.timer);
    gsb.overhead_us = (uint32_t) ((rows_started - started) + (group_rows_now_us() - rows_ended));
    UI_TRACE_SPAN_END(UI_TRACE_SPAN_GROUP_ROWS_CHUNK, perf_chunk);
}

static void group_songs_build_timer_cb(lv_timer_t * timer) {
    if (!gsb.active || !group_songs_list) { lv_timer_pause(timer); return; }
    /* Another screen is shown: stop until SCREEN_LOADED brings this one back (a page left
     * half built must not keep the main loop awake for the whole listening session). */
    if (lv_screen_active() != group_songs_screen) { lv_timer_pause(timer); return; }
    /* Screen off: nothing is drawn, so only an occasional look. */
    bool screen_on = backlight_screen_is_on();
    uint32_t period = screen_on ? GROUP_ROWS_TIMER_MS : GROUP_ROWS_TIMER_OFF_MS;
    if (gsb.timer_period != period) { gsb.timer_period = period; lv_timer_set_period(timer, period); }
    if (!screen_on || gui_navigation_transition_in_progress()) return; /* not under a slide either */
    /* A finger or a fling gets a smaller piece so its frames stay short. */
    group_songs_build_more(INT_MAX, landing_pointer_busy() ? GROUP_ROWS_GESTURE_CHUNK_MS : GROUP_ROWS_CHUNK_MS);
}

/* Builds the rows a list position up to `reach` (content coordinates) needs, nothing more. */
static void group_songs_build_to_reach(int32_t reach) {
    if (!gsb.active || !gsb.spacer || !group_songs_list) return;
    if (gsb.layout_dirty) { lv_obj_update_layout(group_songs_list); gsb.layout_dirty = false; }
    /* The estimate of what a row takes can be a little high (disc headers, rows of another
     * height): measure again after each piece, so the spacer never stays in view. */
    while (gsb.active && gsb.spacer) {
        int32_t top = lv_obj_get_y(gsb.spacer);
        if (top >= reach) return;
        int32_t stride = group_songs_stride_estimate();
        group_songs_build_more((int) ((reach - top) / (stride > 0 ? stride : 1)) + 1, 0);
    }
}

/* Every entry up to and including `index` exists when this returns. */
static void group_songs_build_through(int index) {
    if (gsb.active && index >= gsb.next) group_songs_build_more(index - gsb.next + 1, 0);
}

/* Scrolling toward rows that are not built yet: build the ones the next frame shows (plus one
 * for a fling's next step) and leave the margin beyond them to the chunk timer, which also
 * runs during gestures. */
void group_songs_scroll_cb(lv_event_t * e) {
    (void) e;
    if (!gsb.active || !gsb.spacer || !group_songs_list || group_songs_rows_rebuilding) return;
    if (gsb.timer && lv_timer_get_paused(gsb.timer)) lv_timer_resume(gsb.timer); /* a missed SCREEN_LOADED */
    int32_t stride = group_songs_stride_estimate();
    group_songs_build_to_reach(lv_obj_get_scroll_y(group_songs_list) + lv_obj_get_height(group_songs_list) + stride);
}

/* A touch after the screen was off: back to the normal period at once, not after up to 500 ms. */
void group_songs_wake_cb(lv_event_t * e) {
    (void) e;
    gui_library_screen_woke();
}

/* The screen came back on (any touch, key or other wake, not only a press of this list). */
void gui_library_screen_woke(void) {
    if (gsb.timer && gsb.timer_period != GROUP_ROWS_TIMER_MS && backlight_screen_is_on()) {
        gsb.timer_period = GROUP_ROWS_TIMER_MS;
        lv_timer_set_period(gsb.timer, GROUP_ROWS_TIMER_MS);
        if (gsb.active && !lv_timer_get_paused(gsb.timer)) lv_timer_ready(gsb.timer);
    }
}

/* What a row of one class (with / without a detail line) takes, measured on a row built the way
 * group_songs_add_plain_entry() builds it and deleted at once, unseen. Used when the first rows hold
 * no row of that class to measure: the larger fonts give the two classes different heights. */
static int32_t group_songs_probe_row_height(bool detail, bool album_layout) {
    lv_display_t * disp = lv_obj_get_display(group_songs_list);
    bool was_enabled = disp && lv_display_is_invalidation_enabled(disp);
    if (was_enabled) lv_display_enable_invalidation(disp, false);
    char album_title[384];
    const char * title = detail ? "Ag\nFLAC" : "Ag";
    lv_obj_t * row = build_music_list_row(group_songs_list,
        album_layout ? gui_album_header_row_title(title, album_title, sizeof(album_title)) : title,
        NULL, BOARD_SCALE_PX(70));
    if (group_songs_music_submenu) {
        lv_obj_set_width(row, lv_pct(100));
        layout_music_submenu_row_text(row);
    }
    /* Album pages size every real row in the decorator: measure the row the same way. */
    if (album_layout) gui_album_header_decorate_row(row, -1);
    lv_obj_update_layout(group_songs_list);
    int32_t height = lv_obj_get_height(row);
    lv_obj_delete(row);
    if (was_enabled) lv_display_enable_invalidation(disp, true);
    return height;
}

/* The first rows are built: measure what a row and a disc header take and queue the rest. */
static void group_songs_build_begin(int next, int end, bool album_layout) {
    gsb.active = next < end;
    if (!gsb.active) return;
    gsb.album_layout = album_layout;
    memset(gsb.bare_mask, 0, sizeof(gsb.bare_mask));
    for (int i = group_songs_page_start; i < end; i++)
        if (!strchr(group_songs_entries[i].title, '\n')) {
            int slot = i - group_songs_page_start;
            gsb.bare_mask[slot >> 3] |= (uint8_t) (1u << (slot & 7));
        }
    gsb.next = next;
    gsb.end = end;
    gsb.rows_bare_left = gsb.rows_detail_left = 0;
    gsb.headers_left = 0;
    for (int i = next; i < end; i++) {
        int slot = i - group_songs_page_start;
        if (gsb_slot_bare(slot)) gsb.rows_bare_left++; else gsb.rows_detail_left++;
        if (group_songs_entries[i].show_disc_header) gsb.headers_left++;
    }
    lv_obj_update_layout(group_songs_list);
    /* Rows with and without a detail line can differ in height (the larger fonts): what each kind
     * takes is the mean of the rows of that kind built so far, so the spacer is as tall as the rest
     * will be. A kind not built yet takes what the other one did. */
    int32_t sum[2] = {0, 0};
    int cnt[2] = {0, 0};
    for (int i = group_songs_page_start; i < next && i - group_songs_page_start < GROUP_SONGS_PAGE_SIZE; i++) {
        int slot = i - group_songs_page_start;
        lv_obj_t * row = group_songs_visible_rows[slot];
        if (!row) continue;
        int kind = gsb_slot_bare(slot);
        sum[kind] += lv_obj_get_height(row);
        cnt[kind]++;
    }
    int32_t fallback = group_songs_visible_rows[0] ? lv_obj_get_height(group_songs_visible_rows[0]) : ui_music_row_height();
    gsb.row_h_detail = cnt[0] ? (sum[0] + cnt[0] / 2) / cnt[0] : (cnt[1] ? (sum[1] + cnt[1] / 2) / cnt[1] : fallback);
    gsb.row_h_bare = cnt[1] ? (sum[1] + cnt[1] / 2) / cnt[1] : gsb.row_h_detail;
    /* A class the first rows hold none of, but the rest does, is measured exactly instead of
     * assumed to match the other one: the scroll range and the scrollbar then never change while
     * the page is built. */
    if (!cnt[0] && gsb.rows_detail_left) gsb.row_h_detail = group_songs_probe_row_height(true, album_layout);
    if (!cnt[1] && gsb.rows_bare_left) gsb.row_h_bare = group_songs_probe_row_height(false, album_layout);
    gsb.overhead_us = 0;
    gsb.header_h = 0;
    if (gsb.headers_left) {
        char probe_text[32];
        snprintf(probe_text, sizeof(probe_text), TR("Disc %d"), 1);
        lv_obj_t * probe = add_section_header(group_songs_list, probe_text);
        lv_obj_update_layout(group_songs_list);
        gsb.header_h = lv_obj_get_height(probe);
        lv_obj_delete(probe);
    }
    gsb.layout_dirty = false;
    if (!gsb.timer) {
        gsb.timer = lv_timer_create(group_songs_build_timer_cb, GROUP_ROWS_TIMER_MS, NULL);
        gsb.timer_period = GROUP_ROWS_TIMER_MS;
    }
    if (gsb.timer) lv_timer_resume(gsb.timer);
}

/* The row to keep in place after a rebuild: the anchor, or the last entry when the list got shorter. */
static int group_songs_clamped_anchor(void) {
    return group_songs_anchor_index < group_songs_count ? group_songs_anchor_index : group_songs_count - 1;
}

/* The rows of the page shown are about to be replaced: end a drag, let go of the borrowed row objects, clear the list. */
static void group_songs_rows_detach(void) {
    group_songs_reorder_rearm();
    library_thumbnails_group_rows_rebuilt(group_songs_list);
    cancel_group_song_probes();
    /* Cleaning a scrolled list can synchronously emit SCROLL_END. Detach
     * borrowed row pointers before deletion, and suppress decoration until
     * the replacement rows are complete. */
    group_songs_forget_row_objects();
    gui_album_header_forget();
    lv_obj_clean(group_songs_list);
}

/* The header of the page: edit actions, search button, album header or play bar, select-all row, empty message. */
static void group_songs_rows_header(bool editing, bool album_layout) {
    bool editable = group_songs_edit_m3u_path != NULL;
    if (editable) {
        group_songs_update_edit_header();
    } else {
        lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
        if (group_songs_delete_btn) lv_obj_add_flag(group_songs_delete_btn, LV_OBJ_FLAG_HIDDEN);
    }

    update_group_songs_search_button(editing || group_songs_loading);
    if (!album_layout) gui_album_header_release_cover();
    /* The play bar is hidden while loading, when empty and in M3U edit mode. */
    bool show_bar = !editing && !group_songs_loading && group_songs_count > 0;
    /* Every group-songs page keeps the app's side margins so the scrollbar clears the cards; they do not
     * depend on the bar, so M3U Edit / Done does not shift the cards. */
    lv_obj_set_style_pad_hor(group_songs_list, GUI_LIST_SIDE_MARGIN, 0);
    if (album_layout) {
        /* Albums get a header with Play and Shuffle. */
        gui_album_header_params_t header = {
            .album = group_songs_source_title, .album_artist = group_songs_refresh_album_artist,
            .entries = group_songs_entries, .count = group_songs_count,
            .show_buttons = !group_songs_loading && group_songs_count > 0,
            .play_cb = group_play_cb, .shuffle_cb = group_shuffle_cb,
            .quality_asset = song_quality_asset_for_path,
        };
        gui_album_header_build(group_songs_list, &header);
    } else if (show_bar) {
        /* The track count above a Play / Shuffle pill pair (the same buttons as an album page). */
        char count_text[48];
        snprintf(count_text, sizeof(count_text), TR_N("%d track", "%d tracks", group_songs_count),
                 group_songs_count);
        gui_play_bar_build(group_songs_list, LIST_ROW_WIDTH, count_text, group_play_cb, group_shuffle_cb);
    }
    if (editing) {
        add_group_songs_select_all_row();
        group_songs_update_edit_header();
    }

    if (group_songs_count == 0) {
        if (group_songs_loading)
            build_list_message(group_songs_list, TR("Loading tracks…"), NULL);
        else if (group_songs_edit_m3u_path)
            build_list_message(group_songs_list, TR("Playlist is empty"), TR("Add songs from a song menu."));
        else
            build_list_message(group_songs_list, TR("No tracks found"), TR("Refresh the music database to update this list."));
    }
}

/* A shrunk list can leave the page start past its last page. */
static void group_songs_clamp_page(void) {
    if (group_songs_count <= GROUP_SONGS_PAGE_SIZE) group_songs_page_start = 0;
    if (group_songs_page_start >= group_songs_count && group_songs_count > 0) {
        group_songs_page_start = ((group_songs_count - 1) / GROUP_SONGS_PAGE_SIZE) * GROUP_SONGS_PAGE_SIZE;
    }
}

/* Builds the rows the restored scroll position or anchor points into. */
static void group_songs_rows_build_for_restore(bool restore_scroll, int32_t restore_scroll_y) {
    /* The scroll position needs the rows it points into, not the rest: the spacer keeps
     * the content height exact and the scroll event builds what the new position shows. */
    if (gsb.active && restore_scroll && restore_scroll_y > 0)
        group_songs_build_to_reach(restore_scroll_y + lv_obj_get_height(group_songs_list));
    if (gsb.active && group_songs_anchor_pending) group_songs_build_through(group_songs_clamped_anchor());
}

/* Puts the list back where it was: the old scroll position, or the anchor row at its old offset. */
static void group_songs_rows_apply_restore(bool restore_scroll, int32_t restore_scroll_y, int page_end) {
    if (restore_scroll) lv_obj_scroll_to_y(group_songs_list, restore_scroll_y, LV_ANIM_OFF);
    if (group_songs_anchor_pending) {
        lv_area_t list_area;
        lv_obj_get_coords(group_songs_list, &list_area);
        int anchor_index = group_songs_clamped_anchor();
        if (anchor_index >= group_songs_page_start && anchor_index < page_end) {
            lv_obj_t * row = group_songs_visible_rows[anchor_index - group_songs_page_start];
            if (row) {
                lv_area_t row_area;
                lv_obj_get_coords(row, &row_area);
                int32_t scroll_y = lv_obj_get_scroll_y(group_songs_list) +
                    row_area.y1 - list_area.y1 - group_songs_anchor_offset;
                lv_obj_scroll_to_y(group_songs_list, scroll_y, LV_ANIM_OFF);
            }
        }
        group_songs_anchor_pending = false;
    }
}

/* Rebuilds group_songs_list's rows from whatever group_songs_entries/count
 * currently hold, in either of two shapes: a plain tappable-to-play label
 * (list_row_style, same as every other group -- Artists/Albums/Favorites/
 * Most Played always use this one) when not actively editing a playlist, or
 * rows with selection and reorder controls (no play-on-tap) when
 * group_songs_edit_mode is on for an
 * editable .m3u playlist. Split out from show_group_songs_editable() so the
 * selection, reorder, and Edit/Done actions can redraw in place without
 * re-deriving the group or nav_push()ing a second copy of this screen. */
void populate_group_songs_rows(void) {
    UI_TRACE_SPAN_BEGIN(perf_rows);
    group_songs_build_reset();
    group_songs_rows_rebuilding = true;
    int32_t restore_scroll_y = 0;
    bool restore_scroll = group_songs_list && group_songs_populated_page_start == group_songs_page_start;
    if (restore_scroll) restore_scroll_y = lv_obj_get_scroll_y(group_songs_list);
    group_songs_rows_detach();

    bool editing = group_songs_edit_m3u_path && group_songs_edit_mode;
    bool album_layout = group_songs_album_layout();
    group_songs_rows_header(editing, album_layout);

    group_songs_clamp_page();
    if (group_songs_page_start != group_songs_populated_page_start) restore_scroll = false;
    int page_end = group_songs_page_end();

    if (group_songs_page_start > 0) {
        group_songs_add_page_nav_row(TR("Previous  •  %d–%d of %d"), page_end, group_songs_prev_page_cb);
    }

    /* A long page is built in pieces (see group_songs_build_begin()); editing builds everything. */
    bool incremental = !editing && page_end - group_songs_page_start >= GROUP_ROWS_INCREMENTAL_MIN;
    int build_end = incremental ? group_songs_page_start + group_songs_initial_rows() : page_end;
    if (build_end > page_end) build_end = page_end;
    for (int i = group_songs_page_start; i < build_end; i++) {
        if (editing) group_songs_add_edit_entry(i);
        else group_songs_add_plain_entry(i, album_layout, false);
    }

    if (incremental) group_songs_build_begin(build_end, page_end, album_layout);
    group_songs_tail_create(page_end);
    group_songs_rows_build_for_restore(restore_scroll, restore_scroll_y);
    refresh_group_songs_now_playing_indicator();
    group_songs_rows_rebuilding = false;
    refresh_group_thumbnail_binding();
    lv_obj_update_layout(group_songs_list);
    group_songs_rows_apply_restore(restore_scroll, restore_scroll_y, page_end);
    group_songs_populated_page_start = group_songs_page_start;
    /* The page's duration lookup starts from gui_library_poll_track_probes(): ask the loop for that
     * pass now instead of leaving it to whichever worker notifies next or the 500 ms housekeeping.
     * Not on album pages: their rows built after the lookup lands take another (1 px different)
     * text layout than the ones it fills in, so the lookup keeps its old timing there. */
    if (group_songs_screen && lv_screen_active() == group_songs_screen && !group_songs_album_layout()) ui_wake_notify();
    UI_TRACE_SPAN_END(UI_TRACE_SPAN_GROUP_ROWS, perf_rows);
}

/* Forgets every borrowed pointer to a row object (before the rows are deleted). */
void group_songs_forget_row_objects(void) {
    memset(group_songs_selection_checks, 0, sizeof(group_songs_selection_checks));
    group_songs_select_all_row = NULL;
    group_songs_select_all_checkbox = NULL;
    group_songs_select_all_label = NULL;
    memset(group_songs_visible_rows, 0, sizeof(group_songs_visible_rows));
    memset(group_songs_thumbnail_images, 0, sizeof(group_songs_thumbnail_images));
}

/* Whether a group row gets a cover (or its placeholder) at all. */
bool group_song_has_cover(int index) {
    /* Album rows show the track number where the thumbnail was. */
    if (group_songs_album_layout()) return false;
    return index >= 0 && index < group_songs_count && group_songs_entries[index].song_id > 0 &&
           group_songs_entries[index].artwork_key != 0;
}

void group_song_row_text_offset(lv_obj_t * row, bool with_thumbnail) {
    if (!row || lv_obj_get_child_count(row) < 2 || group_songs_album_layout()) return;
    int32_t target_x = (with_thumbnail ? BOARD_LIBRARY_THUMB_TEXT_X : GUI_TEXT_INSET) +
        (group_songs_edit_mode ? group_songs_reorder_handle_width() : 0);
    /* New rows have explicit label geometry but no computed coordinates
     * until LVGL lays them out. Reading those coordinates here can turn a
     * full-width label into the 40px minimum permanently. Use the configured
     * pixel values, also keeping repeated thumbnail refreshes idempotent. */
    int32_t delta = target_x - lv_obj_get_style_x(lv_obj_get_child(row, 0), 0);
    if (delta == 0) return;
    for (int child_index = 0; child_index < 2; child_index++) {
        lv_obj_t * label = lv_obj_get_child(row, child_index);
        int32_t width = lv_obj_get_style_width(label, 0) - delta;
        if (width < 40) width = 40;
        lv_obj_set_x(label, target_x);
        lv_obj_set_width(label, width);
    }
}

void refresh_group_thumbnail_binding(void) {
    if (!group_songs_list || group_songs_rows_rebuilding) return;
    lv_obj_update_layout(group_songs_list);
    int page_end = group_songs_page_end();
    int32_t scroll_y = lv_obj_get_scroll_y(group_songs_list);
    int32_t view_bottom = scroll_y + lv_obj_get_height(group_songs_list);
    for (int index = group_songs_page_start; index < page_end; index++) {
        int slot = index - group_songs_page_start;
        lv_obj_t * row = group_songs_visible_rows[slot];
        if (!row) continue;
        int32_t row_y = lv_obj_get_y(row);
        bool in_viewport = row_y + lv_obj_get_height(row) >= scroll_y && row_y <= view_bottom;
        bool has_cover = group_song_has_cover(index);
        if (!in_viewport || !has_cover) {
            /* Off screen the image goes but the text keeps its place, so
             * scrolling back does not shift it. */
            if (group_songs_thumbnail_images[slot]) {
                lv_obj_delete(group_songs_thumbnail_images[slot]);
                group_songs_thumbnail_images[slot] = NULL;
            }
            group_song_row_text_offset(row, has_cover);
            continue;
        }
        if (!group_songs_thumbnail_images[slot]) {
            group_songs_thumbnail_images[slot] = lv_image_create(row);
            lv_obj_remove_flag(group_songs_thumbnail_images[slot], LV_OBJ_FLAG_CLICKABLE);
            group_song_row_text_offset(row, true);
        }
        library_thumbnails_decorate_group_row(group_songs_list, row, group_songs_thumbnail_images[slot],
                                              index, slot, group_songs_entries[index].song_id,
                                              group_songs_entries[index].artwork_key);
        /* The shared decorator pads compact-list rows for the cover;
         * these rows position their labels absolutely instead (see
         * group_song_row_text_offset()), so padding would shift the text twice. */
        lv_obj_set_style_pad_left(row, 0, 0);
        int32_t pad_left = lv_obj_get_style_pad_left(row, LV_PART_MAIN);
        int32_t pad_top = lv_obj_get_style_pad_top(row, LV_PART_MAIN);
        int32_t pad_bottom = lv_obj_get_style_pad_bottom(row, LV_PART_MAIN);
        lv_obj_set_size(group_songs_thumbnail_images[slot],
                        BOARD_LIBRARY_THUMB_PX, BOARD_LIBRARY_THUMB_PX);
        lv_image_set_inner_align(group_songs_thumbnail_images[slot], LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_align(group_songs_thumbnail_images[slot], LV_ALIGN_LEFT_MID,
                     BOARD_LIBRARY_THUMB_INSET +
                         (group_songs_edit_mode ? group_songs_reorder_handle_width() : 0) - pad_left,
                     (pad_top - pad_bottom) / -2);
    }
    if (group_songs_album_layout()) gui_album_header_refresh_cover(group_songs_list);
}

bool detach_group_song_thumbnail(const lv_image_dsc_t *dsc) {
    bool detached = false;
    for (int slot = 0; slot < GROUP_SONGS_PAGE_SIZE; ++slot) {
        lv_obj_t *image = group_songs_thumbnail_images[slot];
        if (!image || lv_image_get_src(image) != dsc) continue;
        lv_image_set_src(image, NULL);
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        detached = true;
    }
    if (gui_album_header_detach_cover(dsc)) detached = true;
    return detached;
}

/* The page is left for another screen: stop building until it is shown again. */
void group_songs_build_pause(void) {
    if (gsb.timer) lv_timer_pause(gsb.timer);
}

/* The page is shown again: carry on building what was left. */
void group_songs_build_resume(void) {
    if (gsb.active && gsb.timer) lv_timer_resume(gsb.timer);
}

