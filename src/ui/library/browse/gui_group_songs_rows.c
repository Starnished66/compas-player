/* The Songs page rows: compact browsing, editable rows, covers and now-playing state. */
#include "../../text/i18n/i18n.h"
#include "gui_album_header.h"
#include "gui_album_view.h"
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

typedef enum { GROUP_COMPACT_SONG, GROUP_COMPACT_PREV, GROUP_COMPACT_NEXT, GROUP_COMPACT_DISC } group_compact_kind_t;
typedef struct {
    char * title;
    char * subtitle;
    size_t subtitle_capacity;
    int source_index;
    group_compact_kind_t kind;
} group_compact_row_t;
static group_compact_row_t * group_compact_rows;
static int group_compact_count;
static lv_obj_t * group_compact_header;
int group_songs_compact_row_count(void) { return group_compact_count; }
int group_songs_compact_row_source(int row) {
    return row >= 0 && row < group_compact_count && group_compact_rows[row].kind == GROUP_COMPACT_SONG
        ? group_compact_rows[row].source_index : -1;
}

void group_songs_compact_update_label(int source_index, const char * detail, unsigned seconds) {
    if (!group_compact_rows || !compact_list_is_live(group_songs_list)) return;
    for (int i = 0; i < group_compact_count; ++i) {
        group_compact_row_t * row = &group_compact_rows[i];
        if (row->kind != GROUP_COMPACT_SONG || row->source_index != source_index) continue;
        if (group_songs_album_layout()) {
            compact_list_refresh_item(group_songs_list, i);
        } else {
            if (row->subtitle) snprintf(row->subtitle, row->subtitle_capacity, "%s", detail ? detail : "");
            compact_list_refresh_item(group_songs_list, i);
        }
        (void) seconds;
        return;
    }
}

static void group_compact_release(void) {
    if (group_compact_rows)
        for (int i = 0; i < group_compact_count; ++i) {
            free(group_compact_rows[i].title);
            free(group_compact_rows[i].subtitle);
        }
    free(group_compact_rows); group_compact_rows = NULL;
    group_compact_count = 0;
}

void group_songs_compact_clear(void) {
    if (compact_list_is_live(group_songs_list)) compact_list_set_items(group_songs_list, NULL, 0);
    group_compact_release();
    group_compact_header = NULL;
}

static void group_compact_click(int index) {
    if (index < 0 || index >= group_compact_count) return;
    group_compact_row_t * row = &group_compact_rows[index];
    if (row->kind == GROUP_COMPACT_PREV) {
        group_songs_page_start -= GROUP_SONGS_PAGE_SIZE;
        if (group_songs_page_start < 0) group_songs_page_start = 0;
        populate_group_songs_rows(); return;
    }
    if (row->kind == GROUP_COMPACT_NEXT) {
        group_songs_page_start += GROUP_SONGS_PAGE_SIZE;
        populate_group_songs_rows(); return;
    }
    if (row->kind == GROUP_COMPACT_SONG) group_songs_compact_play(row->source_index);
}

static void group_compact_long_press(int index) {
    if (index < 0 || index >= group_compact_count) return;
    group_compact_row_t * row = &group_compact_rows[index];
    if (row->kind == GROUP_COMPACT_SONG) group_songs_compact_context(row->source_index);
}

static void group_compact_add(compact_list_item_t * items, group_compact_kind_t kind,
                              const char * title, const char * subtitle, int source) {
    group_compact_row_t * row = &group_compact_rows[group_compact_count];
    row->title = strdup(title ? title : "");
    size_t subtitle_len = subtitle ? strlen(subtitle) : 0;
    row->subtitle_capacity = subtitle_len + 1;
    if (row->subtitle_capacity < 256) row->subtitle_capacity = 256;
    row->subtitle = calloc(row->subtitle_capacity, 1);
    if (row->subtitle && subtitle) snprintf(row->subtitle, row->subtitle_capacity, "%s", subtitle);
    row->source_index = source;
    row->kind = kind;
    compact_list_item_t * item = &items[group_compact_count];
    item->label = row->title ? row->title : "";
    item->subtitle = row->subtitle ? row->subtitle : "";
    item->identity = kind == GROUP_COMPACT_SONG ? group_songs_entries[source].song_id : 0;
    item->artwork_key = kind == GROUP_COMPACT_SONG ? group_songs_entries[source].artwork_key : 0;
    item->trailing_asset = kind == GROUP_COMPACT_SONG
        ? song_quality_asset_for_path(group_songs_entries[source].path) : NULL;
    item->is_action = kind == GROUP_COMPACT_PREV || kind == GROUP_COMPACT_NEXT;
    item->is_section_header = kind == GROUP_COMPACT_DISC;
    group_compact_count++;
}

static void group_compact_populate(int page_end, bool album_layout, bool restore_scroll, int32_t old_scroll) {
    int start = group_songs_page_start;
    int count = page_end - start;
    /* A page can add one disc heading per song plus both navigation rows. */
    int capacity = count * 2 + 2;
    group_compact_release();
    group_compact_rows = calloc((size_t)capacity, sizeof(*group_compact_rows));
    compact_list_item_t * items = calloc((size_t)capacity, sizeof(*items));
    if (!group_compact_rows || !items) {
        free(items);
        group_compact_release(); return;
    }
    if (start > 0) {
        char nav[96]; snprintf(nav, sizeof(nav), TR("Previous  •  %d–%d of %d"), start + 1, page_end, group_songs_count);
        group_compact_add(items, GROUP_COMPACT_PREV, nav, NULL, -1);
    }
    for (int i = start; i < page_end; ++i) {
        int stars = group_songs_rating_section(i);
        if (stars) {
            group_compact_add(items, GROUP_COMPACT_DISC, "", NULL, -1);
            items[group_compact_count - 1].section_stars = stars;
        }
        if (group_songs_entries[i].show_disc_header) {
            char disc[32]; snprintf(disc, sizeof(disc), TR("Disc %d"), group_songs_entries[i].disc_number);
            group_compact_add(items, GROUP_COMPACT_DISC, disc, NULL, -1);
        }
        const char * full = group_songs_entries[i].title ? group_songs_entries[i].title : "";
        const char * detail = strchr(full, '\n');
        size_t n = detail ? (size_t)(detail - full) : strlen(full);
        char * title = malloc(n + 1);
        if (!title) continue;
        memcpy(title, full, n); title[n] = 0;
        if (detail) detail++;
        /* Compact-list item strings outlive this temporary split of the source title. */
        group_compact_add(items, GROUP_COMPACT_SONG, title, album_layout ? NULL : detail, i);
        free(title);
    }
    if (page_end < group_songs_count) {
        char nav[96]; snprintf(nav, sizeof(nav), TR("Next  •  %d–%d of %d"), start + 1, page_end, group_songs_count);
        group_compact_add(items, GROUP_COMPACT_NEXT, nav, NULL, -1);
    }
    compact_list_set_items(group_songs_list, items, group_compact_count);
    free(items);
    compact_list_set_row_height(group_songs_list, album_layout ? BOARD_SCALE_PX(76) : ui_music_row_height());
    compact_list_set_now_playing(group_songs_list, -1);
    if (group_songs_anchor_pending) {
        for (int i = 0; i < group_compact_count; ++i) {
            if (group_compact_rows[i].kind != GROUP_COMPACT_SONG ||
                group_compact_rows[i].source_index != group_songs_anchor_index) continue;
            int32_t top, bottom, viewport;
            if (compact_list_row_span(group_songs_list, i, &top, &bottom, &viewport))
                lv_obj_scroll_to_y(group_songs_list, top - group_songs_anchor_offset, LV_ANIM_OFF);
            (void)bottom; (void)viewport;
            group_songs_anchor_pending = false;
            restore_scroll = false;
            break;
        }
        group_songs_anchor_pending = false;
    }
    if (restore_scroll) lv_obj_scroll_to_y(group_songs_list, old_scroll, LV_ANIM_OFF);
}

static bool group_songs_rows_rebuilding;
static bool group_songs_compact_mode;
static void group_compact_click(int index);
static void group_compact_long_press(int index);
static void group_compact_decorate(lv_obj_t * list, lv_obj_t * row, lv_obj_t * image,
                                   int logical, int slot, int64_t identity,
                                   uint64_t artwork_key, const char * artwork_name, void * ctx) {
    (void) artwork_name; (void) ctx;
    if (logical < 0 || logical >= group_compact_count) return;
    group_compact_row_t * item = &group_compact_rows[logical];
    if (item->kind != GROUP_COMPACT_SONG) {
        gui_album_view_clear_compact_row(row);
        lv_obj_set_style_pad_left(row, LIST_ROW_LABEL_INSET, 0);
        lv_image_set_src(image, NULL);
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (group_songs_album_layout()) {
        lv_image_set_src(image, NULL);
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        gui_album_header_decorate_row(row, item->source_index);
        return;
    }
    gui_album_view_clear_compact_row(row);
    if (identity > 0 && artwork_key)
        library_thumbnails_decorate_group_row(list, row, image, logical, slot, identity, artwork_key);
    else {
        lv_obj_set_style_pad_left(row, BOARD_SCALE_PX(14), 0);
        lv_image_set_src(image, NULL);
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t * group_songs_create_list(lv_obj_t * parent, bool compact) {
    group_songs_compact_mode = compact;
    lv_obj_t * list = compact
        ? build_compact_list_widget(parent, NULL, 0, group_compact_click, group_compact_long_press,
                                    LIST_ROW_WIDTH, true, gui_theme_accent_style())
        : build_song_list_body(parent);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(list, BOARD_SCALE_PX(3), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, BOARD_SCALE_PX(2), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_add_style(list, gui_theme_accent_style(), LV_PART_SCROLLBAR);
    if (compact) compact_list_set_row_decorator(list, group_compact_decorate, NULL);
    return list;
}

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

/* The Next row is retained by the real-row edit list. */
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

static void group_songs_tail_create(int page_end) {
    group_songs_add_next_row(page_end);
    group_songs_now_playing_bar = group_songs_create_now_playing_bar();
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
    if (group_songs_compact_mode) {
        compact_list_set_header(group_songs_list, NULL, 0);
        if (group_compact_header) lv_obj_delete(group_compact_header);
        group_compact_header = NULL;
        compact_list_set_items(group_songs_list, NULL, 0);
        group_compact_release();
    } else lv_obj_clean(group_songs_list);
}

static void group_songs_ensure_list_mode(bool compact) {
    if (!group_songs_list || group_songs_compact_mode == compact) return;
    lv_obj_t * parent = lv_obj_get_parent(group_songs_list);
    group_songs_reorder_detach();
    group_songs_forget_row_objects();
    gui_album_header_forget();
    cancel_group_song_probes();
    if (group_songs_compact_mode) group_songs_compact_clear();
    library_thumbnails_forget_list(group_songs_list);
    lv_obj_delete(group_songs_list);
    group_songs_list = group_songs_create_list(parent, compact);
    if (group_songs_screen) library_thumbnails_bind_list(group_songs_screen, group_songs_list,
        LIBRARY_THUMB_LIST_GROUP_SONGS_CUSTOM);
    gui_library_group_songs_list_changed();
    group_songs_populated_page_start = -1;
}

static void group_songs_empty_message(lv_obj_t *target) {
    if (group_songs_count == 0) {
        if (group_songs_loading)
            build_list_message(target, TR("Loading tracks…"), NULL);
        else if (group_songs_edit_m3u_path)
            build_list_message(target, TR("Playlist is empty"), TR("Add songs from a song menu."));
        else if (group_songs_refresh_source == GROUP_SONGS_REFRESH_FAVORITES)
            build_list_message(target, TR("Top rated"), TR("No songs rated 3 stars or more yet."));
        else if (group_songs_refresh_source == GROUP_SONGS_REFRESH_MOST_PLAYED)
            build_list_message(target, TR("No plays yet"), TR("Songs you play most will appear here."));
        else
            build_list_message(target, TR("No tracks found"), TR("Refresh the music database to update this list."));
    }
}

static lv_obj_t *group_songs_header_target(void) {
    lv_obj_t *target = group_songs_list;
    if (group_songs_compact_mode) {
        group_compact_header = lv_obj_create(group_songs_list);
        lv_obj_remove_style_all(group_compact_header);
        lv_obj_set_width(group_compact_header, LIST_ROW_WIDTH);
        lv_obj_set_height(group_compact_header, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(group_compact_header, 0, 0);
        lv_obj_set_style_pad_gap(group_compact_header, GUI_ROW_GAP, 0);
        lv_obj_set_flex_flow(group_compact_header, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(group_compact_header, LV_OBJ_FLAG_SCROLLABLE);
        target = group_compact_header;
    }
    return target;
}

static void group_songs_header_play_bar(lv_obj_t *target, bool album_layout, bool show_bar) {
    if (album_layout) {
        /* Albums get a header with Play and Shuffle. */
        gui_album_header_params_t header = {
            .album = group_songs_source_title, .album_artist = group_songs_refresh_album_artist,
            .entries = group_songs_entries, .count = group_songs_count,
            .show_buttons = !group_songs_loading && group_songs_count > 0,
            .play_cb = group_play_cb, .shuffle_cb = group_shuffle_cb,
            .quality_asset = song_quality_asset_for_path,
        };
        gui_album_header_build(target, &header);
    } else if (show_bar) {
        /* The track count above a Play / Shuffle pill pair (the same buttons as an album page). */
        gui_album_view_play_bar_build(target, LIST_ROW_WIDTH, group_songs_count,
                                      group_play_cb, group_shuffle_cb);
    }
}

/* The header of the page: edit actions, search button, album header or play bar, select-all row, empty message. */
static void group_songs_rows_header(bool editing, bool album_layout) {
    lv_obj_t * target = group_songs_header_target();
    if (group_songs_compact_mode) compact_list_set_empty_heading(group_songs_list, "");
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
    if (!group_songs_compact_mode)
        lv_obj_set_style_pad_hor(group_songs_list, GUI_LIST_SIDE_MARGIN, 0);
    group_songs_header_play_bar(target, album_layout, show_bar);
    if (editing) {
        add_group_songs_select_all_row();
        group_songs_update_edit_header();
    }

    group_songs_empty_message(target);
    if (group_songs_compact_mode) group_songs_compact_header_refresh();
}

void group_songs_compact_header_refresh(void) {
    if (!group_compact_header || !compact_list_is_live(group_songs_list)) return;
    lv_obj_update_layout(group_compact_header);
    int32_t height = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(group_compact_header); ++i)
        height += lv_obj_get_height(lv_obj_get_child(group_compact_header, (int32_t)i));
    uint32_t children = lv_obj_get_child_count(group_compact_header);
    if (height > 0) height += (int32_t)(children > 1 ? children - 1 : 1) * GUI_ROW_GAP;
    lv_obj_set_height(group_compact_header, height);
    compact_list_set_header(group_songs_list, group_compact_header, height);
}

/* A shrunk list can leave the page start past its last page. */
static void group_songs_clamp_page(void) {
    if (group_songs_count <= GROUP_SONGS_PAGE_SIZE) group_songs_page_start = 0;
    if (group_songs_page_start >= group_songs_count && group_songs_count > 0) {
        group_songs_page_start = ((group_songs_count - 1) / GROUP_SONGS_PAGE_SIZE) * GROUP_SONGS_PAGE_SIZE;
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
    group_songs_rows_rebuilding = true;
    bool editing = group_songs_edit_m3u_path && group_songs_edit_mode;
    group_songs_ensure_list_mode(!editing);
    int32_t restore_scroll_y = 0;
    bool restore_scroll = group_songs_list && group_songs_populated_page_start == group_songs_page_start;
    if (restore_scroll) restore_scroll_y = lv_obj_get_scroll_y(group_songs_list);
    group_songs_rows_detach();

    bool album_layout = group_songs_album_layout();
    group_songs_rows_header(editing, album_layout);

    group_songs_clamp_page();
    if (group_songs_page_start != group_songs_populated_page_start) restore_scroll = false;
    int page_end = group_songs_page_end();

    if (group_songs_compact_mode) {
        group_compact_populate(page_end, album_layout, restore_scroll, restore_scroll_y);
        group_songs_rows_rebuilding = false;
        refresh_group_songs_now_playing_indicator();
        refresh_group_thumbnail_binding();
        lv_obj_update_layout(group_songs_list);
        group_songs_populated_page_start = group_songs_page_start;
        if (group_songs_screen && lv_screen_active() == group_songs_screen && !album_layout) ui_wake_notify();
        UI_TRACE_SPAN_END(UI_TRACE_SPAN_GROUP_ROWS, perf_rows);
        return;
    }

    if (group_songs_page_start > 0) {
        group_songs_add_page_nav_row(TR("Previous  •  %d–%d of %d"), page_end, group_songs_prev_page_cb);
    }

    for (int i = group_songs_page_start; i < page_end; i++)
        group_songs_add_edit_entry(i);
    group_songs_tail_create(page_end);
    refresh_group_songs_now_playing_indicator();
    group_songs_rows_rebuilding = false;
    refresh_group_thumbnail_binding();
    lv_obj_update_layout(group_songs_list);
    if (restore_scroll) lv_obj_scroll_to_y(group_songs_list, restore_scroll_y, LV_ANIM_OFF);
    if (group_songs_anchor_pending) {
        int anchor = group_songs_anchor_index;
        if (anchor >= group_songs_page_start && anchor < page_end) {
            lv_obj_t * row = group_songs_visible_rows[anchor - group_songs_page_start];
            if (row) {
                lv_area_t list_area, row_area;
                lv_obj_get_coords(group_songs_list, &list_area);
                lv_obj_get_coords(row, &row_area);
                int32_t scroll = lv_obj_get_scroll_y(group_songs_list) + row_area.y1 - list_area.y1 - group_songs_anchor_offset;
                lv_obj_scroll_to_y(group_songs_list, scroll, LV_ANIM_OFF);
            }
        }
        group_songs_anchor_pending = false;
    }
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
        if (group_songs_edit_mode) lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    }
}

void refresh_group_thumbnail_binding(void) {
    if (!group_songs_list || group_songs_rows_rebuilding) return;
    if (compact_list_is_live(group_songs_list)) {
        compact_list_refresh_decorations(group_songs_list);
        if (group_songs_album_layout()) gui_album_header_refresh_cover(group_songs_list);
        return;
    }
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
