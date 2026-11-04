/* The Songs page playlist editing: edit mode, selection, drag reorder, delete and reload of an .m3u playlist. */
#include "../../text/i18n/i18n.h"
#include "../../../library/playlists/playlist_files.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gui_group_songs_internal.h"

static void reload_edited_playlist(void);

static bool * group_songs_selected;
static int group_songs_selected_count;
static lv_obj_t * group_songs_reorder_handles[GROUP_SONGS_PAGE_SIZE];
static gui_list_reorder_state_t group_songs_reorder_state = { .drag_id = -1 };
static lv_obj_t * group_songs_drag_original_children[GROUP_SONGS_PAGE_SIZE * 2 + 8];
static int group_songs_drag_original_child_count;
static int group_songs_drag_source_id = -1;

static bool group_playlist_unchanged(void) {
    struct stat st;
    return group_songs_edit_m3u_path && group_songs_file_stat_valid &&
        stat(group_songs_edit_m3u_path, &st) == 0 && st.st_ino == group_songs_file_stat.st_ino &&
        st.st_dev == group_songs_file_stat.st_dev && st.st_size == group_songs_file_stat.st_size &&
        st.st_mtim.tv_sec == group_songs_file_stat.st_mtim.tv_sec &&
        st.st_mtim.tv_nsec == group_songs_file_stat.st_mtim.tv_nsec;
}

/* Reloads the playlist and tells the user when its file changed under the page; true then. */
bool group_songs_playlist_stale(void) {
    if (group_playlist_unchanged()) return false;
    reload_edited_playlist();
    show_info_toast(TR("Playlist changed. Select a song again."));
    return true;
}

static void group_songs_drag_forget(void) {
    group_songs_drag_original_child_count = 0;
    group_songs_drag_source_id = -1;
}

int32_t group_songs_reorder_handle_width(void) {
    int32_t width = BOARD_SCALE_PX(44);
    return width < 48 ? 48 : width;
}

static void group_song_selection_row_cb(lv_event_t * e);
static void group_songs_select_all_row_cb(lv_event_t * e);
static void group_songs_reorder_handle_cb(lv_event_t * e);
static void group_songs_reorder_prepare(gui_list_reorder_state_t * state, int id);
static void group_songs_reorder_commit(gui_list_reorder_state_t * state);
static void group_songs_reorder_restore(gui_list_reorder_state_t * state);
static bool group_songs_playlist_check(void * context, char * const * paths, int count);

static void group_songs_restore_drag_child_order(void) {
    if (!group_songs_list) return;
    for (int i = 0; i < GROUP_SONGS_PAGE_SIZE; ++i)
        gui_list_reorder_row_stop_translate(group_songs_visible_rows[i]);
    for (int i = 0; i < group_songs_drag_original_child_count; ++i) {
        lv_obj_t * child = group_songs_drag_original_children[i];
        if (child && lv_obj_get_parent(child) == group_songs_list)
            lv_obj_move_to_index(child, i);
    }
    group_songs_drag_forget();
}

static void group_songs_unregister_reorder_handles(void) {
    for (int i = 0; i < GROUP_SONGS_PAGE_SIZE; ++i) {
        if (group_songs_reorder_handles[i]) {
            unregister_swipe_dead_zone(group_songs_reorder_handles[i]);
            group_songs_reorder_handles[i] = NULL;
        }
    }
}

/* The page's rows are going away or the screen is left: end a drag in progress and let go of the rows. */
void group_songs_reorder_detach(void) {
    gui_list_reorder_finish(&group_songs_reorder_state, false);
    group_songs_unregister_reorder_handles();
    group_songs_reorder_state.rows = NULL;
    group_songs_reorder_state.list = NULL;
}

static void group_songs_reorder_prepare(gui_list_reorder_state_t * state, int id) {
    if (!state) return;
    state->count = 0;
    group_songs_drag_forget();
    if (!group_songs_edit_mode || !group_songs_edit_m3u_path ||
        !group_playlist_unchanged() || id < 0 || id >= GROUP_SONGS_PAGE_SIZE) {
        show_info_toast(TR("Playlist changed. Try again."));
        return;
    }
    int visible_count = group_songs_visible_count();
    if (visible_count < 2 || id >= visible_count) return;
    uint32_t child_count = lv_obj_get_child_count(group_songs_list);
    if (child_count > sizeof(group_songs_drag_original_children) /
                      sizeof(group_songs_drag_original_children[0])) {
        show_error_toast(TR("Cannot reorder playlist"));
        return;
    }
    for (uint32_t i = 0; i < child_count; ++i)
        group_songs_drag_original_children[i] = lv_obj_get_child(group_songs_list, i);
    group_songs_drag_original_child_count = (int) child_count;
    group_songs_drag_source_id = id;
    for (int i = 0; i < visible_count; ++i) {
        if (!group_songs_visible_rows[i]) {
            group_songs_drag_forget();
            return;
        }
        state->ids[i] = i;
    }
    state->count = visible_count;
}

static void group_songs_reorder_restore(gui_list_reorder_state_t * state) {
    (void) state;
    group_songs_restore_drag_child_order();
    group_songs_drag_source_id = -1;
}

static void group_songs_reorder_commit(gui_list_reorder_state_t * state) {
    int source_id = group_songs_drag_source_id;
    int destination_id = -1;
    if (state && source_id >= 0) {
        for (int i = 0; i < state->drag_count; ++i) {
            if (state->ids[i] == source_id) { destination_id = i; break; }
        }
    }
    if (source_id < 0 || destination_id < 0 || source_id == destination_id) {
        group_songs_restore_drag_child_order();
        group_songs_drag_source_id = -1;
        return;
    }
    int from = group_songs_page_start + source_id;
    int to = group_songs_page_start + destination_id;
    group_songs_restore_drag_child_order();
    playlist_edit_status_t status = playlist_files_edit_entry_checked(
        group_songs_edit_m3u_path, from, to, group_songs_playlist_check, NULL);
    group_songs_drag_source_id = -1;
    if (status == PLAYLIST_EDIT_OK) {
        reload_edited_playlist();
    } else if (status == PLAYLIST_EDIT_REFUSED) {
        reload_edited_playlist();
        show_info_toast(TR("Playlist changed. Try again."));
    } else {
        populate_group_songs_rows();
        show_error_toast(TR("Cannot reorder playlist"));
    }
}

static void group_songs_reorder_handle_cb(lv_event_t * e) {
    int id = (int) (intptr_t) lv_event_get_user_data(e);
    gui_list_reorder_event(&group_songs_reorder_state, id, e);
}

void group_songs_clear_selection(void) {
    free(group_songs_selected);
    group_songs_selected = NULL;
    group_songs_selected_count = 0;
}

bool group_songs_reset_selection(void) {
    group_songs_clear_selection();
    if (group_songs_edit_mode && group_songs_count > 0) {
        group_songs_selected = calloc((size_t) group_songs_count, sizeof(*group_songs_selected));
        return group_songs_selected != NULL;
    }
    return true;
}

void group_songs_update_edit_header(void) {
    if (!group_songs_edit_btn) return;
    lv_label_set_text(group_songs_edit_btn, TR("Edit"));
    if (group_songs_edit_mode) lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
    if (group_songs_delete_btn) {
        if (group_songs_edit_mode) lv_obj_clear_flag(group_songs_delete_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(group_songs_delete_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(group_songs_delete_btn,
            group_songs_selected_count > 0 ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_obj_set_style_image_opa(group_songs_delete_btn,
            group_songs_selected_count > 0 ? LV_OPA_COVER : LV_OPA_40, 0);
    }
    if (group_songs_select_all_checkbox) {
        if (group_songs_count > 0 && group_songs_selected_count == group_songs_count)
            lv_obj_add_state(group_songs_select_all_checkbox, LV_STATE_CHECKED);
        else lv_obj_remove_state(group_songs_select_all_checkbox, LV_STATE_CHECKED);
        if (group_songs_count == 0) lv_obj_add_state(group_songs_select_all_checkbox, LV_STATE_DISABLED);
        else lv_obj_remove_state(group_songs_select_all_checkbox, LV_STATE_DISABLED);
    }
    if (group_songs_select_all_label) {
        char text[64];
        if (group_songs_selected_count > 0)
            snprintf(text, sizeof(text), TR("Select all (%d)"), group_songs_selected_count);
        else snprintf(text, sizeof(text), "%s", TR("Select all"));
        lv_label_set_text(group_songs_select_all_label, text);
    }
    if (group_songs_select_all_row) {
        if (group_songs_count == 0) lv_obj_clear_flag(group_songs_select_all_row, LV_OBJ_FLAG_CLICKABLE);
        else lv_obj_add_flag(group_songs_select_all_row, LV_OBJ_FLAG_CLICKABLE);
    }
    if (group_songs_edit_m3u_path && group_songs_title_label) {
        lv_obj_t * action = group_songs_edit_mode ? group_songs_delete_btn : group_songs_edit_btn;
        if (action) reserve_title_width_before(group_songs_title_label, action);
    }
}

/* The accent-styled selection checkbox at the right edge of an edit-mode row. */
static void group_songs_style_check(lv_obj_t * checkbox) {
    lv_checkbox_set_text(checkbox, "");
    lv_obj_set_size(checkbox, BOARD_SCALE_PX(28), BOARD_SCALE_PX(28));
    lv_obj_align(checkbox, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(18), 0);
    lv_obj_set_style_border_color(checkbox, accent_lv_color(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(checkbox, accent_lv_color(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_image_src(checkbox, LV_SYMBOL_OK, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(checkbox,
        lv_color_brightness(accent_lv_color()) > 160 ? lv_color_hex(GUI_BREEZE_TEXT_ON_LIGHT) : lv_color_white(),
        LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_remove_flag(checkbox,
                       LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE |
                       LV_OBJ_FLAG_EVENT_BUBBLE);
}

lv_obj_t * add_group_songs_select_all_row(void) {
    lv_obj_t * row = add_pill_row_base(group_songs_list, TR("Select all"));
    group_songs_select_all_row = row;
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, group_songs_select_all_row_cb, LV_EVENT_CLICKED, NULL);

    group_songs_select_all_checkbox = lv_checkbox_create(row);
    group_songs_style_check(group_songs_select_all_checkbox);

    group_songs_select_all_label = lv_obj_get_child(row, 0);
    if (group_songs_select_all_label) {
        lv_obj_update_layout(row);
        int32_t text_width = lv_obj_get_width(row) - BOARD_SCALE_PX(100 + 18 + 28 + 8);
        if (text_width < BOARD_SCALE_PX(40)) text_width = BOARD_SCALE_PX(40);
        lv_obj_set_width(group_songs_select_all_label, text_width);
        lv_obj_align(group_songs_select_all_label, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(18 + 28 + 8), 0);
        lv_obj_set_style_text_align(group_songs_select_all_label, LV_TEXT_ALIGN_RIGHT, 0);
    }

    lv_obj_t * done_btn = lv_button_create(row);
    lv_obj_remove_style_all(done_btn);
    lv_obj_set_size(done_btn, BOARD_SCALE_PX(72), BOARD_SCALE_PX(48));
    lv_obj_align(done_btn, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(12), 0);
    lv_obj_add_style(done_btn, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(done_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(done_btn, 0, 0);
    lv_obj_remove_flag(done_btn, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(done_btn, group_songs_edit_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * done_label = lv_label_create(done_btn);
    lv_label_set_text(done_label, TR("Done"));
    lv_obj_add_style(done_label, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(done_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(done_label);
    lv_obj_remove_flag(done_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

/* One entry of the edit-mode page: its disc header when it starts a disc, then the row with its reorder handle and checkbox. */
void group_songs_add_edit_entry(int i) {
    group_songs_add_disc_header(i);
    /* Keep dragging on the left and selection on the right. The
     * cover and text share a left inset beyond the handle. */
    int32_t handle_width = group_songs_reorder_handle_width();
    int32_t text_reserve = BOARD_SCALE_PX(18 + 28 + 8);
    lv_obj_t * row = group_songs_new_row(i, group_songs_entries[i].title, text_reserve);
    group_song_row_text_offset(row, group_song_has_cover(i));
    int slot = i - group_songs_page_start;
    lv_obj_t * handle = lv_button_create(row);
    group_songs_reorder_handles[slot] = handle;
    lv_obj_set_size(handle, handle_width, ui_music_row_height());
    lv_obj_align(handle, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(handle, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(handle, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_style(handle, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_add_style(handle, &list_row_pressed_style, LV_STATE_USER_1);
    lv_obj_set_style_radius(handle, BOARD_SCALE_PX(8), 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(handle, 0, 0);
    lv_obj_set_style_shadow_width(handle, 0, 0);
    lv_obj_set_style_pad_all(handle, 0, 0);
    lv_obj_t * grip = lv_image_create(handle);
    lv_obj_set_size(grip, BOARD_SCALE_PX(32), BOARD_SCALE_PX(32));
    set_reorder_handle_icon(grip);
    lv_image_set_inner_align(grip, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_center(grip);
    lv_obj_remove_flag(grip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(handle, group_songs_reorder_handle_cb, LV_EVENT_ALL,
                        (void *) (intptr_t) slot);
    register_swipe_dead_zone(handle);
    lv_obj_t * checkbox = lv_checkbox_create(row);
    if (group_songs_selected && group_songs_selected[i]) lv_obj_add_state(checkbox, LV_STATE_CHECKED);
    group_songs_selection_checks[i - group_songs_page_start] = checkbox;
    group_songs_style_check(checkbox);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, group_song_selection_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
}

void group_songs_edit_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (group_songs_screen) search_close_if_active_for_screen(group_songs_screen);
    bool entering_edit_at_top = !group_songs_edit_mode && group_songs_list &&
        lv_obj_get_scroll_y(group_songs_list) == 0;
    if (entering_edit_at_top) group_songs_anchor_pending = false;
    else group_songs_capture_scroll_anchor(NULL, 0);
    if (group_songs_edit_mode) {
        group_songs_edit_mode = false;
        group_songs_clear_selection();
    } else {
        group_songs_edit_mode = true;
        if (!group_songs_reset_selection()) {
            group_songs_edit_mode = false;
            group_songs_anchor_pending = false;
            group_songs_update_edit_header();
            show_error_toast(TR("Cannot select playlist entries"));
            return;
        }
    }
    populate_group_songs_rows();
}

static void group_song_selection_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !group_songs_edit_mode ||
        group_songs_reorder_state.active) return;
    int pos = (int) (intptr_t) lv_event_get_user_data(e);
    if (!group_songs_edit_m3u_path || pos < 0 || pos >= group_songs_count || !group_songs_selected) return;
    if (group_songs_playlist_stale()) return;
    group_songs_selected[pos] = !group_songs_selected[pos];
    group_songs_selected_count += group_songs_selected[pos] ? 1 : -1;
    int slot = pos - group_songs_page_start;
    lv_obj_t * checkbox = slot >= 0 && slot < GROUP_SONGS_PAGE_SIZE
        ? group_songs_selection_checks[slot] : NULL;
    if (checkbox) {
        if (group_songs_selected[pos]) lv_obj_add_state(checkbox, LV_STATE_CHECKED);
        else lv_obj_remove_state(checkbox, LV_STATE_CHECKED);
    }
    group_songs_update_edit_header();
}

static void group_songs_select_all_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !group_songs_edit_mode ||
        group_songs_reorder_state.active || group_songs_count <= 0 || !group_songs_selected) return;
    if (group_songs_playlist_stale()) return;
    bool select_all = group_songs_selected_count != group_songs_count;
    for (int i = 0; i < group_songs_count; ++i) group_songs_selected[i] = select_all;
    group_songs_selected_count = select_all ? group_songs_count : 0;
    int visible_count = group_songs_visible_count();
    for (int slot = 0; slot < visible_count; ++slot) {
        lv_obj_t * checkbox = group_songs_selection_checks[slot];
        if (!checkbox) continue;
        if (select_all) lv_obj_add_state(checkbox, LV_STATE_CHECKED);
        else lv_obj_remove_state(checkbox, LV_STATE_CHECKED);
    }
    group_songs_update_edit_header();
}

static bool group_songs_playlist_check(void * context, char * const * paths, int count) {
    (void) context;
    if (!group_playlist_unchanged() || count != group_songs_count) return false;
    for (int i = 0; i < count; ++i) {
        if (strcmp(paths[i], group_songs_entries[i].path) != 0) return false;
    }
    return true;
}

/* Map a visible old playlist position to its new position after deleting a
 * set of ordered entry offsets. When the old top row is deleted, retain the
 * next surviving row, or the previous one when the deletion reaches EOF. */
static int group_songs_anchor_index_after_delete(int top_index, int song_count,
                                                 const int * deleted_indexes, int deleted_count) {
    if (top_index < 0 || top_index >= song_count) return -1;
    /* The delete UI builds unique ascending offsets, matching the backend's
     * ordering contract. One forward pass handles a playlist-wide selection
     * without quadratic scans. */
    int deleted_before_top = 0;
    while (deleted_before_top < deleted_count && deleted_indexes[deleted_before_top] < top_index)
        ++deleted_before_top;
    if (deleted_before_top == deleted_count || deleted_indexes[deleted_before_top] != top_index)
        return top_index - deleted_before_top;

    int next = top_index + 1;
    int deleted_cursor = deleted_before_top + 1;
    while (next < song_count && deleted_cursor < deleted_count && deleted_indexes[deleted_cursor] == next) {
        ++next;
        ++deleted_cursor;
    }
    if (next < song_count) return top_index - deleted_before_top;

    int previous = top_index - 1;
    int deleted_before_previous = deleted_before_top;
    while (previous >= 0 && deleted_before_previous > 0 &&
           deleted_indexes[deleted_before_previous - 1] == previous) {
        --previous;
        --deleted_before_previous;
    }
    return previous < 0 ? -1 : previous - deleted_before_previous;
}

void group_songs_capture_scroll_anchor(const int * deleted_indexes, int deleted_count) {
    group_songs_anchor_pending = false;
    if (!group_songs_list) return;
    lv_obj_update_layout(group_songs_list); /* the anchor is the topmost visible row: always built */
    lv_area_t list_area;
    lv_obj_get_coords(group_songs_list, &list_area);
    int best_index = -1;
    int32_t best_y = INT32_MAX;
    for (int slot = 0; slot < GROUP_SONGS_PAGE_SIZE; ++slot) {
        lv_obj_t * row = group_songs_visible_rows[slot];
        if (!row) continue;
        lv_area_t row_area;
        lv_obj_get_coords(row, &row_area);
        if (row_area.y2 < list_area.y1 || row_area.y1 > list_area.y2 || row_area.y1 >= best_y) continue;
        best_index = group_songs_page_start + slot;
        best_y = row_area.y1;
    }
    if (best_index < 0 || best_index >= group_songs_count) return;
    int anchor_index = group_songs_anchor_index_after_delete(
        best_index, group_songs_count, deleted_indexes, deleted_count);
    if (anchor_index < 0) return;
    group_songs_anchor_index = anchor_index;
    group_songs_anchor_offset = best_y - list_area.y1;
    group_songs_anchor_pending = true;
}

void group_songs_delete_selected_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || group_songs_selected_count <= 0 ||
        !group_songs_selected || !group_songs_edit_m3u_path) return;
    int * indexes = malloc((size_t) group_songs_selected_count * sizeof(*indexes));
    if (!indexes) { show_error_toast(TR("Cannot remove entries")); return; }
    int count = 0;
    for (int i = 0; i < group_songs_count; ++i)
        if (group_songs_selected[i]) indexes[count++] = i;
    group_songs_capture_scroll_anchor(indexes, count);
    playlist_edit_status_t status = playlist_files_delete_entries_checked(
        group_songs_edit_m3u_path, indexes, count, group_songs_playlist_check, NULL);
    free(indexes);
    if (status == PLAYLIST_EDIT_REFUSED) {
        group_songs_anchor_pending = false;
        reload_edited_playlist();
        show_info_toast(TR("Playlist changed. Select a song again."));
        return;
    }
    if (status != PLAYLIST_EDIT_OK) {
        group_songs_anchor_pending = false;
        show_error_toast(TR("Cannot remove entries"));
        return;
    }
    reload_edited_playlist();
    show_info_toast(TR("Removed from playlist"));
}

/* Reload an editable playlist after a checked edit. Re-reads the underlying
 * .m3u file, then
 * re-reads it back and redraws in place (no nav_push -- this stays on the
 * same group_songs_screen instance, just with the file's new contents)
 * rather than trusting the in-memory indices array to still match the file
 * after the rewrite. */
static void reload_edited_playlist(void) {
    char ** songs = NULL;
    int count = 0;
    if (!playlist_files_read(group_songs_edit_m3u_path, &songs, &count)) {
        group_songs_anchor_pending = false;
        show_error_toast(TR("Cannot read playlist")); return;
    }
    group_song_entry_t * entries = group_song_entries_take_paths(songs, count);
    if (count && !entries) { group_songs_anchor_pending = false; return; }
    group_songs_file_stat_valid = stat(group_songs_edit_m3u_path, &group_songs_file_stat) == 0;
    set_group_songs_entries_owned(entries, count);
    group_songs_drag_forget();
    if (!group_songs_reset_selection()) {
        group_songs_edit_mode = false;
        group_songs_clear_selection();
        show_error_toast(TR("Cannot select playlist entries"));
    }
    if (group_songs_anchor_pending) {
        if (count > 0 && group_songs_anchor_index >= count) group_songs_anchor_index = count - 1;
        if (count > 0 && group_songs_anchor_index >= 0)
            group_songs_page_start = (group_songs_anchor_index / GROUP_SONGS_PAGE_SIZE) * GROUP_SONGS_PAGE_SIZE;
    }
    populate_group_songs_rows();
}

void library_reload_edited_playlist_if_changed(lv_obj_t * screen) {
    if (screen == group_songs_screen && group_songs_edit_m3u_path && !group_playlist_unchanged()) {
        struct stat st;
        if (stat(group_songs_edit_m3u_path, &st) == 0) {
            int32_t scroll = lv_obj_get_scroll_y(group_songs_list);
            reload_edited_playlist();
            lv_obj_scroll_to_y(group_songs_list, scroll, LV_ANIM_OFF);
        }
    }
}

/* The page's rows are rebuilt: end a drag in progress and point the reorder state at the new list and rows. */
void group_songs_reorder_rearm(void) {
    if (group_songs_reorder_state.active)
        gui_list_reorder_finish(&group_songs_reorder_state, false);
    group_songs_unregister_reorder_handles();
    group_songs_reorder_state.list = group_songs_list;
    group_songs_reorder_state.rows = group_songs_visible_rows;
    group_songs_reorder_state.row_capacity = GROUP_SONGS_PAGE_SIZE;
    group_songs_reorder_state.prepare = group_songs_reorder_prepare;
    group_songs_reorder_state.commit = group_songs_reorder_commit;
    group_songs_reorder_state.restore = group_songs_reorder_restore;
}

