/* The Songs page shared by albums, artists, genres and playlists: rows, editing, probes, search and in-place refresh. */
#include "../../text/i18n/i18n.h"
#include "gui_library_internal.h"
#include "gui_album_header.h"
#include "../../widgets/builders/gui_play_bar.h"
#include "gui_artist_data.h"
#include "gui_artist_page.h"
#include "gui_page_durations.h"
#include "../library_selection_snapshot.h"
#include "gui_list_reorder.h"
#include "../../assets/assets.h"
#include "../../../hardware/indicators/backlight.h"
#include "../../../core/ui/display/ui_wake.h"
#include "../../../library/playlists/playlist_files.h"
#include "../../widgets/lists/scroll_blit.h"
#include "../../diagnostics/ui_frame_trace.h"
#include <limits.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static void refresh_group_song_thumbnails(void);
static void group_song_row_text_offset(lv_obj_t * row, bool with_thumbnail);
static bool group_song_has_cover(int index);
static void refresh_group_songs_search_now_playing(void);

bool group_songs_source_is_album = false;
bool group_songs_loading;
group_songs_refresh_source_t group_songs_refresh_source;
static char group_songs_refresh_album[128];
static char group_songs_refresh_album_artist[128];
static char group_songs_refresh_artist_filter[128];
char group_songs_refresh_artist_name[128];
metadata_db_group_kind_t group_songs_refresh_artist_kind;
char group_songs_refresh_m3u_path[PATH_MAX];
char group_songs_refresh_genre[METADATA_DB_TEXT_MAX] = {0};

/* Shared drill-down target for both Artists and Albums: one persistent
 * screen whose row list is rebuilt each time (same "one object, rebuilt on
 * demand" approach file_browser.c uses for its own directory listing)
 * rather than pre-building a screen per artist/album -- a real library can
 * have hundreds of those, most never opened in a given session. */
lv_obj_t * group_songs_edit_btn;
group_song_entry_t * group_songs_entries; /* owned -- see set_group_songs_entries() */
int group_songs_count;

/* Frees the previous group_songs_entries (if any) and replaces it with an
 * owned copy of entries[0..count) -- every show_group_songs_editable() call
 * and every "Removed from playlist" refresh goes through this single choke
 * point so the ownership rule (group_songs_entries is always either NULL or
 * a malloc'd copy this screen owns outright) never has to be re-derived at
 * each call site. Caller's own entries[] can be freed immediately after. */
static void set_group_songs_entries(const group_song_entry_t * entries, int count) {
    if (group_songs_screen) search_close_if_active_for_screen(group_songs_screen);
    free_group_song_entries(group_songs_entries, group_songs_count);
    group_songs_entries = NULL;
    group_songs_count = copy_group_song_entries(&group_songs_entries, entries, count) ? count : 0;
}

/* Sibling to set_group_songs_entries() that takes ownership of an already
 * allocated entries[] array rather than copying it, avoiding duplicate
 * allocations for large song lists. The caller transfers ownership and
 * must not free `entries` or its elements afterward. */
void set_group_songs_entries_owned(group_song_entry_t * entries, int count) {
    if (group_songs_screen) search_close_if_active_for_screen(group_songs_screen);
    free_group_song_entries(group_songs_entries, group_songs_count);
    group_songs_entries = entries;
    group_songs_count = count;
}

static int group_songs_page_start;
static bool * group_songs_selected;
static int group_songs_selected_count;
static lv_obj_t * group_songs_selection_checks[GROUP_SONGS_PAGE_SIZE];
static lv_obj_t * group_songs_reorder_handles[GROUP_SONGS_PAGE_SIZE];
static lv_obj_t * group_songs_delete_btn;
static lv_obj_t * group_songs_select_all_row;
static lv_obj_t * group_songs_select_all_checkbox;
static lv_obj_t * group_songs_select_all_label;
static lv_obj_t * group_songs_select_all_done_btn;
static int group_songs_populated_page_start = -1;
static bool group_songs_anchor_pending;
static bool group_songs_rebuild_on_load;
static bool group_songs_rows_rebuilding;
static int group_songs_anchor_index;
static int32_t group_songs_anchor_offset;

/* One bounded job at a time. The worker owns path copies and never sees
 * LVGL objects or group_songs_entries. Page generations invalidate late
 * results without waiting for the current decoder to finish. */
track_probe_job_t * group_probe_job;
uint64_t group_probe_generation = 1;
uint64_t group_probe_job_generation;
static uint64_t group_probe_requested_generation;
static int group_probe_page_start;
bool group_probe_storage_available = true;
static bool group_probe_retry_pending;
static uint32_t group_probe_retry_tick;

void cancel_group_song_probes(void) {
    ++group_probe_generation;
    group_probe_retry_pending = false;
    gui_page_durations_reset();
    if (group_probe_job) track_probe_job_cancel(group_probe_job);
}

/* Now-playing indicator bar -- recreated fresh every populate_group_songs_
 * rows() call (that function's own lv_obj_clean(group_songs_list) destroys
 * whatever was here before, same as every row), so this pointer is only
 * ever valid between one populate call and the next, never stale across
 * one -- see refresh_group_songs_now_playing_indicator()'s own comment. */
lv_obj_t * group_songs_now_playing_bar;
lv_obj_t * group_songs_visible_rows[GROUP_SONGS_PAGE_SIZE];
static gui_list_reorder_state_t group_songs_reorder_state = { .drag_id = -1 };
static lv_obj_t * group_songs_drag_original_children[GROUP_SONGS_PAGE_SIZE * 2 + 8];
static int group_songs_drag_original_child_count;
static int group_songs_drag_source_id = -1;
lv_obj_t * group_songs_thumbnail_images[GROUP_SONGS_PAGE_SIZE];
static bool group_songs_music_submenu;

/* Forward-declared here (defined after on_file_selected()) because
 * set_player_source_group_songs() needs group_songs_entries/count/title_label
 * already in scope, which the following definitions did not have. */
/* set_player_source_group_songs_direct defined in gui.c */

static void set_player_source_group_songs(int pos) {
    set_player_source_group_songs_direct(group_songs_entries, group_songs_count,
                                          group_songs_source_title ? group_songs_source_title : "", pos);
    current_settings.last_source_kind = group_songs_source_is_album ? 2 : 0;
    snprintf(current_settings.last_source_name, sizeof(current_settings.last_source_name), "%s",
             group_songs_source_is_album && group_songs_source_title ? group_songs_source_title : "");
}

/* Non-NULL only when the currently shown group is a user-created .m3u
 * playlist (set by show_m3u_playlist() via show_group_songs_editable()),
 * enabling the edit/remove-song UI. NULL for Artists/Albums/Favorites/
 * Most Played, which do not back onto a rewritable file. Borrowed pointer
 * from playlists_m3u_paths[]; valid for the lifetime of the screen.
 * group_songs_edit_mode is reset to false on every fresh entry. */
static const char * group_songs_edit_m3u_path = NULL;
static char group_songs_owned_m3u_path[PATH_MAX];
static void reload_edited_playlist(void);
static struct stat group_songs_file_stat;
static bool group_songs_file_stat_valid;
static bool group_playlist_unchanged(void) {
    struct stat st;
    return group_songs_edit_m3u_path && group_songs_file_stat_valid &&
        stat(group_songs_edit_m3u_path, &st) == 0 && st.st_ino == group_songs_file_stat.st_ino &&
        st.st_dev == group_songs_file_stat.st_dev && st.st_size == group_songs_file_stat.st_size &&
        st.st_mtim.tv_sec == group_songs_file_stat.st_mtim.tv_sec &&
        st.st_mtim.tv_nsec == group_songs_file_stat.st_mtim.tv_nsec;
}
static bool group_songs_edit_mode = false;

static int32_t group_songs_reorder_handle_width(void) {
    int32_t width = BOARD_SCALE_PX(44);
    return width < 48 ? 48 : width;
}

static void group_songs_delete_selected_cb(lv_event_t * e);
static void group_song_selection_row_cb(lv_event_t * e);
static void group_songs_select_all_row_cb(lv_event_t * e);
static void group_songs_edit_btn_cb(lv_event_t * e);
static void group_songs_capture_scroll_anchor(const int * deleted_indexes, int deleted_count);
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
    group_songs_drag_original_child_count = 0;
    group_songs_drag_source_id = -1;
}

static void group_songs_unregister_reorder_handles(void) {
    for (int i = 0; i < GROUP_SONGS_PAGE_SIZE; ++i) {
        if (group_songs_reorder_handles[i]) {
            unregister_swipe_dead_zone(group_songs_reorder_handles[i]);
            group_songs_reorder_handles[i] = NULL;
        }
    }
}

static void group_songs_reorder_prepare(gui_list_reorder_state_t * state, int id) {
    if (!state) return;
    state->count = 0;
    group_songs_drag_source_id = -1;
    group_songs_drag_original_child_count = 0;
    if (!group_songs_edit_mode || !group_songs_edit_m3u_path ||
        !group_playlist_unchanged() || id < 0 || id >= GROUP_SONGS_PAGE_SIZE) {
        show_info_toast(TR("Playlist changed. Try again."));
        return;
    }
    int visible_count = group_songs_count - group_songs_page_start;
    if (visible_count > GROUP_SONGS_PAGE_SIZE) visible_count = GROUP_SONGS_PAGE_SIZE;
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
            group_songs_drag_original_child_count = 0;
            group_songs_drag_source_id = -1;
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

static void group_songs_clear_selection(void) {
    free(group_songs_selected);
    group_songs_selected = NULL;
    group_songs_selected_count = 0;
}

static bool group_songs_reset_selection(void) {
    group_songs_clear_selection();
    if (group_songs_edit_mode && group_songs_count > 0) {
        group_songs_selected = calloc((size_t) group_songs_count, sizeof(*group_songs_selected));
        return group_songs_selected != NULL;
    }
    return true;
}

static void group_songs_update_edit_header(void) {
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

static lv_obj_t * add_group_songs_select_all_row(void) {
    lv_obj_t * row = add_pill_row_base(group_songs_list, TR("Select all"));
    group_songs_select_all_row = row;
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, group_songs_select_all_row_cb, LV_EVENT_CLICKED, NULL);

    group_songs_select_all_checkbox = lv_checkbox_create(row);
    lv_checkbox_set_text(group_songs_select_all_checkbox, "");
    lv_obj_set_size(group_songs_select_all_checkbox, BOARD_SCALE_PX(28), BOARD_SCALE_PX(28));
    lv_obj_align(group_songs_select_all_checkbox, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(18), 0);
    lv_obj_set_style_border_color(group_songs_select_all_checkbox, accent_lv_color(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(group_songs_select_all_checkbox, accent_lv_color(),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_image_src(group_songs_select_all_checkbox, LV_SYMBOL_OK,
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(group_songs_select_all_checkbox,
        lv_color_brightness(accent_lv_color()) > 160 ? lv_color_hex(GUI_BREEZE_TEXT_ON_LIGHT) : lv_color_white(),
        LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_remove_flag(group_songs_select_all_checkbox,
                       LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE |
                       LV_OBJ_FLAG_EVENT_BUBBLE);

    group_songs_select_all_label = lv_obj_get_child(row, 0);
    if (group_songs_select_all_label) {
        lv_obj_update_layout(row);
        int32_t text_width = lv_obj_get_width(row) - BOARD_SCALE_PX(100 + 18 + 28 + 8);
        if (text_width < BOARD_SCALE_PX(40)) text_width = BOARD_SCALE_PX(40);
        lv_obj_set_width(group_songs_select_all_label, text_width);
        lv_obj_align(group_songs_select_all_label, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(18 + 28 + 8), 0);
        lv_obj_set_style_text_align(group_songs_select_all_label, LV_TEXT_ALIGN_RIGHT, 0);
    }

    group_songs_select_all_done_btn = lv_button_create(row);
    lv_obj_remove_style_all(group_songs_select_all_done_btn);
    lv_obj_set_size(group_songs_select_all_done_btn, BOARD_SCALE_PX(72), BOARD_SCALE_PX(48));
    lv_obj_align(group_songs_select_all_done_btn, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(12), 0);
    lv_obj_add_style(group_songs_select_all_done_btn, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(group_songs_select_all_done_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(group_songs_select_all_done_btn, 0, 0);
    lv_obj_remove_flag(group_songs_select_all_done_btn,
                       LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(group_songs_select_all_done_btn, group_songs_edit_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * done_label = lv_label_create(group_songs_select_all_done_btn);
    lv_label_set_text(done_label, TR("Done"));
    lv_obj_add_style(done_label, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(done_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(done_label);
    lv_obj_remove_flag(done_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

/* Suppresses the follow-up LV_EVENT_CLICKED event when a long press has
 * already triggered the context menu on a group song row. */
static bool group_song_row_long_press_fired = false;

static void group_play_at(int pos) {
    if (library_rescan_active) return;
    if (pos < 0 || pos >= group_songs_count) return;
    gui_player_seed_selection_song(NULL, NULL);
    if (group_songs_edit_m3u_path && !group_playlist_unchanged()) {
        reload_edited_playlist(); show_info_toast(TR("Playlist changed. Select a song again.")); return;
    }
    /* Allocate and copy playlist paths using calloc to handle potential
     * allocation failures cleanly. */
    char ** playlist_copy = calloc((size_t) group_songs_count, sizeof(char *));
    if (!playlist_copy) return;
    bool ok = true;
    for (int i = 0; i < group_songs_count; i++) {
        playlist_copy[i] = strdup(group_songs_entries[i].path);
        if (!playlist_copy[i]) {
            ok = false;
            break;
        }
    }
    if (!ok) {
        for (int i = 0; i < group_songs_count; i++) free(playlist_copy[i]);
        free(playlist_copy);
        return;
    }
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_GROUP, -1,
                           group_songs_entries[pos].song_id,
                           group_songs_entries[pos].path);
    set_player_source_group_songs(pos);
    on_file_selected(playlist_copy, group_songs_count, pos);
}

static void group_song_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (group_song_row_long_press_fired) { group_song_row_long_press_fired = false; return; }
    group_play_at((int) (intptr_t) lv_event_get_user_data(e));
}

int playlist_start_source;

/* Starting a collection sequentially should keep an active repeat mode. */
play_mode_t collection_play_mode(play_mode_t requested_mode) {
    if (requested_mode == PLAY_MODE_SHUFFLE || requested_mode == PLAY_MODE_SHUFFLE_ONCE)
        return requested_mode;
    if (requested_mode == PLAY_MODE_SEQUENTIAL &&
        (current_settings.play_mode == PLAY_MODE_REPEAT_ALL ||
         current_settings.play_mode == PLAY_MODE_REPEAT_ONE)) {
        return (play_mode_t) current_settings.play_mode;
    }
    return requested_mode;
}

void playlist_start_selected(lv_event_t * e, bool shuffle) {
    (void) e;
    if (library_rescan_active) return;
    int count = playlist_start_source == PLAY_ALL_SOURCE_GROUP ? group_songs_count
                                                               : (int) metadata_db_get_song_count();
    if (count <= 0) { show_info_toast(TR("Playlist is empty")); return; }
    int index = 0;
    if (shuffle) {
        unsigned int value = (unsigned int) time(NULL) ^ lv_tick_get();
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) { ssize_t n = read(fd, &value, sizeof(value)); (void) n; close(fd); }
        index = (int) (value % (unsigned int) count);
    }
    gui_player_set_play_mode(collection_play_mode(shuffle ? PLAY_MODE_SHUFFLE : PLAY_MODE_SEQUENTIAL));
    if (playlist_start_source == PLAY_ALL_SOURCE_RECENTLY_ADDED) {
        set_player_source_recently_added(index);
        on_file_selected_lazy_recently_added(index);
    } else if (playlist_start_source == PLAY_ALL_SOURCE_ALL_SONGS) {
        set_player_source_all_songs(index);
        on_file_selected_lazy_all_songs(index);
    } else group_play_at(index);
}
static void playlist_start_sequential(lv_event_t * e) { playlist_start_selected(e, false); }
static void playlist_start_shuffle(lv_event_t * e) { playlist_start_selected(e, true); }
/* The Play and Shuffle buttons of the album, artist-songs and playlist pages and of the song lists: start
 * the whole collection in order, or from a random song in shuffle mode. */
static void group_play_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    playlist_start_source = PLAY_ALL_SOURCE_GROUP;
    playlist_start_sequential(e);
}
static void group_shuffle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    playlist_start_source = PLAY_ALL_SOURCE_GROUP;
    playlist_start_shuffle(e);
}

/* True while the group-songs screen shows an album (not a playlist, genre,
 * artist's songs or favourites): the only screen that gets the album header
 * and numbered rows. */
static bool group_songs_album_layout(void) {
    return group_songs_source_is_album && group_songs_music_submenu && !group_songs_edit_m3u_path;
}

static void group_song_row_long_press_cb(lv_event_t * e) {
    if (library_rescan_active) return;
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    group_song_row_long_press_fired = true;
    int pos = (int) (intptr_t) lv_event_get_user_data(e);
    open_song_context_menu(group_songs_entries[pos].path);
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

static int group_probe_batch_index[GROUP_SONGS_PAGE_SIZE]; /* probe job index -> page index */
static uint64_t group_probe_lookup_generation;

/* Shows "4:32 · FLAC" on page row `index` (a probe result or a cache hit). */
static void apply_group_probe_row(int index, unsigned int seconds, audio_codec_t codec) {
    int pos = group_probe_page_start + index;
    if (index < 0 || index >= GROUP_SONGS_PAGE_SIZE || pos >= group_songs_count) return;
    char detail[64];
    snprintf(detail, sizeof(detail), "%u:%02u · %s", seconds / 60, seconds % 60, library_codec_name(codec));
    const char * old = group_songs_entries[pos].title;
    const char * newline = strchr(old, '\n');
    size_t title_len = newline ? (size_t)(newline - old) : strlen(old);
    char * title = malloc(title_len + strlen(detail) + 2);
    if (!title) return;
    memcpy(title, old, title_len);
    title[title_len] = '\n';
    strcpy(title + title_len + 1, detail);
    free(group_songs_entries[pos].title);
    group_songs_entries[pos].title = title;
    lv_obj_t * row = group_songs_visible_rows[index];
    if (row && group_songs_album_layout()) {
        gui_album_header_row_set_duration(row, seconds);
    } else if (row) {
        lv_obj_t * secondary = lv_obj_get_child(row, 1);
        lv_label_set_text(secondary, detail);
        lv_obj_remove_flag(secondary, LV_OBJ_FLAG_HIDDEN);
        layout_music_submenu_row_text(row);
    }
}

static void apply_cached_group_row(int index, const duration_info_t * info, void * user) {
    (void) user;
    int pos = group_probe_page_start + index;
    if (group_songs_album_layout()) gui_album_header_note_cached(pos, info);
    apply_group_probe_row(index, (unsigned int)(info->duration_ms / 1000.0 + 0.5), (audio_codec_t) info->codec);
}

void gui_library_poll_track_probes(void) {
    bool artist_visible = artist_albums_screen && lv_screen_active() == artist_albums_screen &&
                          group_probe_storage_available && backlight_screen_is_on();
    gui_artist_data_poll(artist_visible);
    if (artist_albums_list && gui_artist_page_poll(artist_albums_list, artist_visible) &&
        !compact_list_rederive_cached(artist_albums_list, artist_album_rederive_row, NULL))
        compact_list_refetch(artist_albums_list);
    bool visible = group_songs_screen && lv_screen_active() == group_songs_screen &&
                   group_songs_music_submenu && group_probe_storage_available && backlight_screen_is_on();
    if (!visible && group_probe_job && group_probe_job_generation == group_probe_generation)
        cancel_group_song_probes();
    /* The album header's whole-album probe waits for the page's own probe. */
    if (gui_album_header_poll(visible && group_songs_album_layout(),
                              visible && group_songs_album_layout() && !group_probe_job &&
                              group_probe_requested_generation == group_probe_generation))
        refresh_group_songs_now_playing_indicator(); /* wrapped chips moved the rows */

    if (group_probe_job) {
        bool current = visible && group_probe_job_generation == group_probe_generation;
        /* Take the completion snapshot before draining results, so a final
         * result published just after next() returns false is not lost. */
        bool done = track_probe_job_done(group_probe_job);
        bool drained = false;
        if (current) {
            /* Limit label work per tick; never rebuild the list or reset
             * its scroll position when a probe completes. */
            for (int budget = 0; budget < 4; ++budget) {
                size_t index;
                audio_current_format_info_t info;
                if (!track_probe_job_next(group_probe_job, &index, &info)) {
                    drained = true;
                    break;
                }
                if (budget == 3) ui_wake_notify(); /* budget spent with results left: come back next pass */
                if (index >= GROUP_SONGS_PAGE_SIZE) continue;
                int page_index = group_probe_batch_index[index];
                int pos = group_probe_page_start + page_index;
                if (pos >= group_songs_count) continue;
                if (!info.valid || !(info.duration_seconds > 0.0 &&
                    info.duration_seconds <= UINT_MAX - 1.0)) {
                    /* Remembered only when the format itself was rejected. */
                    if (!info.valid && track_probe_job_confirmed_failure(group_probe_job, index)) {
                        gui_page_durations_note_failed(page_index);
                        if (group_songs_album_layout()) {
                            duration_info_t failed = { .bit_depth = DURATION_PROBE_REVISION,
                                                       .flags = DURATION_CACHE_FLAG_FAILED };
                            gui_album_header_note_cached(pos, &failed);
                        }
                    }
                    continue;
                }
                gui_page_durations_note_probe(page_index, &info);
                if (group_songs_album_layout()) gui_album_header_note_probe(pos, &info);
                apply_group_probe_row(page_index, (unsigned int)(info.duration_seconds + 0.5), info.codec);
            }
        }
        if (done && (!current || drained)) {
            track_probe_job_destroy(group_probe_job);
            group_probe_job = NULL;
        }
    }

    if (!visible || group_probe_job || group_probe_requested_generation == group_probe_generation)
        return;
    if (group_probe_retry_pending && lv_tick_elaps(group_probe_retry_tick) < 1000) return;
    int count = group_songs_count - group_songs_page_start;
    if (count > GROUP_SONGS_PAGE_SIZE) count = GROUP_SONGS_PAGE_SIZE;
    if (count <= 0) return;
    const char * paths[GROUP_SONGS_PAGE_SIZE];
    /* 1. Ask the duration cache about the whole page (on a worker). */
    if (group_probe_lookup_generation != group_probe_generation) {
        for (int i = 0; i < count; ++i) paths[i] = group_songs_entries[group_songs_page_start + i].path;
        if (!gui_page_durations_begin(paths, count)) {
            group_probe_retry_pending = true;
            group_probe_retry_tick = lv_tick_get();
            return;
        }
        group_probe_retry_pending = false;
        group_probe_lookup_generation = group_probe_generation;
        group_probe_page_start = group_songs_page_start;
    }
    if (!gui_page_durations_ready()) return;
    /* 2. Cached rows are filled now; only the misses are probed. */
    int miss[GROUP_SONGS_PAGE_SIZE];
    int n = gui_page_durations_plan(miss, GROUP_SONGS_PAGE_SIZE, apply_cached_group_row, NULL);
    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            paths[i] = group_songs_entries[group_probe_page_start + miss[i]].path;
            group_probe_batch_index[i] = miss[i];
        }
        group_probe_job = track_probe_job_start(paths, (size_t)n);
        if (!group_probe_job) {
            group_probe_retry_pending = true;
            group_probe_retry_tick = lv_tick_get();
            return;
        }
    }
    group_probe_retry_pending = false;
    group_probe_requested_generation = group_probe_generation;
    group_probe_job_generation = group_probe_generation;
}

/* Positions/shows or hides group_songs_now_playing_bar against the CURRENT
 * group_songs_entries/count -- callable standalone (no row rebuild, no
 * scroll reset) whenever now_playing_path changes while this screen
 * is open, and also called once at the end of populate_group_songs_rows()
 * itself so a freshly opened group (or an edit-mode toggle, which also goes
 * through a full repopulate) starts with the right state. The visible-row
 * table lets the marker follow LVGL's final flex layout, including runtime
 * font/touch sizing and any action or paging rows before the song. */
static void group_songs_place_now_playing_bar(bool laid_out);
void refresh_group_songs_now_playing_indicator(void) {
    group_songs_place_now_playing_bar(false);
}

/* `laid_out`: the list's layout is already current (a build chunk just ended with it). */
static void group_songs_place_now_playing_bar(bool laid_out) {
    /* Search results can include songs outside the underlying list's page. */
    refresh_group_songs_search_now_playing();
    if (!group_songs_now_playing_bar) return;

    int match = -1;
    if (now_playing_path[0]) {
        for (int i = 0; i < group_songs_count; i++) {
            if (strcmp(group_songs_entries[i].path, now_playing_path) == 0) { match = i; break; }
        }
    }

    int page_end = group_songs_page_start + GROUP_SONGS_PAGE_SIZE;
    if (page_end > group_songs_count) page_end = group_songs_count;
    if (match < group_songs_page_start || match >= page_end) {
        lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_t * row = group_songs_visible_rows[match - group_songs_page_start];
    if (!row) {
        lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    /* Runtime font/touch sizing can make the rendered row taller than its
     * compile-time minimum. Follow the real laid-out object rather than
     * reconstructing its bounds from fixed height and gap constants. */
    if (!laid_out) lv_obj_update_layout(group_songs_list);
    lv_obj_remove_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(group_songs_now_playing_bar, 0, lv_obj_get_y(row));
    lv_obj_set_height(group_songs_now_playing_bar, lv_obj_get_height(row));
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

static int group_songs_initial_rows(void) {
    int32_t stride = ui_music_row_height();
    if (stride > BOARD_SCALE_PX(76)) stride = BOARD_SCALE_PX(76); /* album rows are the shortest */
    stride += GUI_ROW_GAP;
    int32_t screen_h = lv_display_get_vertical_resolution(lv_display_get_default());
    return (int) ((screen_h * 3 / 2) / stride) + 2;
}

/* One entry of the plain (not editing) page: its disc header when it starts a disc, then the row. */
static void group_songs_add_plain_entry(int i, bool album_layout, bool was_bare) {
    if (group_songs_entries[i].show_disc_header) {
        char header[32];
        snprintf(header, sizeof(header), TR("Disc %d"), group_songs_entries[i].disc_number);
        add_section_header(group_songs_list, header);
    }
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
    lv_obj_t * row = build_music_list_row(group_songs_list,
        album_layout ? gui_album_header_row_title(title, album_title, sizeof(album_title)) : title,
        NULL, BOARD_SCALE_PX(70));
    free(bare);
    if (group_songs_music_submenu) lv_obj_set_width(row, lv_pct(100));
    group_songs_visible_rows[i - group_songs_page_start] = row;
    if (group_songs_music_submenu) layout_music_submenu_row_text(row);
    /* Room for the cover from the first frame: the cover (or its
     * placeholder) is added later, and moving the text then made
     * every row jump sideways. */
    if (group_song_has_cover(i)) group_song_row_text_offset(row, true);

    lv_obj_t * quality = lv_image_create(row);
    lv_image_set_src(quality, asset_path(song_quality_asset_for_path(group_songs_entries[i].path)));
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_size(quality, BOARD_SCALE_PX(34), BOARD_SCALE_PX(34));
    lv_image_set_inner_align(quality, LV_IMAGE_ALIGN_CONTAIN);
#endif
    /* Child alignment is relative to the label's padded content
     * box. Cancel the 70px text reserve so the badge is physically
     * 14px from the card edge (same rule as compact-list rows). */
    lv_obj_align(quality, LV_ALIGN_RIGHT_MID, BOARD_SCALE_PX(-14), 0);
    lv_obj_remove_flag(quality, LV_OBJ_FLAG_CLICKABLE);
    if (album_layout) gui_album_header_decorate_row(row, i);
    if (detail && detail[0]) { /* as apply_group_probe_row() does */
        lv_obj_t * secondary = lv_obj_get_child(row, 1);
        lv_label_set_text(secondary, detail);
        lv_obj_remove_flag(secondary, LV_OBJ_FLAG_HIDDEN);
        layout_music_submenu_row_text(row);
    }

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, group_song_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    lv_obj_add_event_cb(row, group_song_row_long_press_cb, LV_EVENT_LONG_PRESSED, (void *) (intptr_t) i);
}

static lv_obj_t * group_songs_add_next_row(int page_end) {
    if (page_end >= group_songs_count) return NULL;
    char page_text[96];
    snprintf(page_text, sizeof(page_text), TR("Next  •  %d–%d of %d"),
             group_songs_page_start + 1, page_end, group_songs_count);
    return add_group_songs_page_row(page_text, group_songs_next_page_cb);
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
static void group_songs_build_reset(void) {
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
        bool bare = (gsb.bare_mask[slot >> 3] >> (slot & 7)) & 1;
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
static void group_songs_scroll_cb(lv_event_t * e) {
    (void) e;
    if (!gsb.active || !gsb.spacer || !group_songs_list || group_songs_rows_rebuilding) return;
    if (gsb.timer && lv_timer_get_paused(gsb.timer)) lv_timer_resume(gsb.timer); /* a missed SCREEN_LOADED */
    int32_t stride = group_songs_stride_estimate();
    group_songs_build_to_reach(lv_obj_get_scroll_y(group_songs_list) + lv_obj_get_height(group_songs_list) + stride);
}

/* A touch after the screen was off: back to the normal period at once, not after up to 500 ms. */
static void group_songs_wake_cb(lv_event_t * e) {
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
        if ((gsb.bare_mask[slot >> 3] >> (slot & 7)) & 1) gsb.rows_bare_left++; else gsb.rows_detail_left++;
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
        int kind = (gsb.bare_mask[slot >> 3] >> (slot & 7)) & 1;
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
        lv_obj_t * probe = add_section_header(group_songs_list, TR("Disc 1"));
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
    if (group_songs_reorder_state.active)
        gui_list_reorder_finish(&group_songs_reorder_state, false);
    group_songs_unregister_reorder_handles();
    group_songs_reorder_state.list = group_songs_list;
    group_songs_reorder_state.rows = group_songs_visible_rows;
    group_songs_reorder_state.row_capacity = GROUP_SONGS_PAGE_SIZE;
    group_songs_reorder_state.prepare = group_songs_reorder_prepare;
    group_songs_reorder_state.commit = group_songs_reorder_commit;
    group_songs_reorder_state.restore = group_songs_reorder_restore;
    library_thumbnails_group_rows_rebuilt(group_songs_list);
    cancel_group_song_probes();
    memset(group_songs_reorder_handles, 0, sizeof(group_songs_reorder_handles));
    memset(group_songs_selection_checks, 0, sizeof(group_songs_selection_checks));
    group_songs_select_all_row = NULL;
    group_songs_select_all_checkbox = NULL;
    group_songs_select_all_label = NULL;
    group_songs_select_all_done_btn = NULL;
    /* Cleaning a scrolled list can synchronously emit SCROLL_END. Detach
     * borrowed row pointers before deletion, and suppress decoration until
     * the replacement rows are complete. */
    memset(group_songs_visible_rows, 0, sizeof(group_songs_visible_rows));
    memset(group_songs_thumbnail_images, 0, sizeof(group_songs_thumbnail_images));
    gui_album_header_forget();
    lv_obj_clean(group_songs_list);

    bool editable = group_songs_edit_m3u_path != NULL;
    bool editing = editable && group_songs_edit_mode;
    if (editable) {
        if (editing) lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
        group_songs_update_edit_header();
    } else {
        lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
        if (group_songs_delete_btn) lv_obj_add_flag(group_songs_delete_btn, LV_OBJ_FLAG_HIDDEN);
    }

    update_group_songs_search_button(editing || group_songs_loading);
    bool album_layout = group_songs_album_layout();
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

    if (group_songs_count <= GROUP_SONGS_PAGE_SIZE) group_songs_page_start = 0;
    if (group_songs_page_start >= group_songs_count && group_songs_count > 0) {
        group_songs_page_start = ((group_songs_count - 1) / GROUP_SONGS_PAGE_SIZE) * GROUP_SONGS_PAGE_SIZE;
    }
    if (group_songs_page_start != group_songs_populated_page_start) restore_scroll = false;
    int page_end = group_songs_page_start + GROUP_SONGS_PAGE_SIZE;
    if (page_end > group_songs_count) page_end = group_songs_count;

    if (group_songs_page_start > 0) {
        char page_text[96];
        snprintf(page_text, sizeof(page_text), TR("Previous  •  %d–%d of %d"),
                 group_songs_page_start + 1, page_end, group_songs_count);
        add_group_songs_page_row(page_text, group_songs_prev_page_cb);
    }

    /* A long page is built in pieces (see group_songs_build_begin()); editing builds everything. */
    bool incremental = !editing && page_end - group_songs_page_start >= GROUP_ROWS_INCREMENTAL_MIN;
    int build_end = incremental ? group_songs_page_start + group_songs_initial_rows() : page_end;
    if (build_end > page_end) build_end = page_end;
    for (int i = group_songs_page_start; i < build_end; i++) {
        if (!editing) {
            group_songs_add_plain_entry(i, album_layout, false);
            continue;
        }
        if (group_songs_entries[i].show_disc_header) {
            char header[32];
            snprintf(header, sizeof(header), TR("Disc %d"), group_songs_entries[i].disc_number);
            add_section_header(group_songs_list, header);
        }
        {
            /* Keep dragging on the left and selection on the right. The
             * cover and text share a left inset beyond the handle. */
            int32_t handle_width = group_songs_reorder_handle_width();
            int32_t text_reserve = BOARD_SCALE_PX(18 + 28 + 8);
            lv_obj_t * row = build_music_list_row(group_songs_list, group_songs_entries[i].title, NULL,
                                                  text_reserve);
            if (group_songs_music_submenu) lv_obj_set_width(row, lv_pct(100));
            group_songs_visible_rows[i - group_songs_page_start] = row;
            if (group_songs_music_submenu) layout_music_submenu_row_text(row);
            group_song_row_text_offset(row, group_song_has_cover(i)); /* see below */
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
            lv_checkbox_set_text(checkbox, "");
            if (group_songs_selected && group_songs_selected[i]) lv_obj_add_state(checkbox, LV_STATE_CHECKED);
            group_songs_selection_checks[i - group_songs_page_start] = checkbox;
            lv_obj_set_style_border_color(checkbox, accent_lv_color(), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(checkbox, accent_lv_color(), LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_style_bg_image_src(checkbox, LV_SYMBOL_OK, LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_style_text_color(checkbox,
                lv_color_brightness(accent_lv_color()) > 160 ? lv_color_hex(GUI_BREEZE_TEXT_ON_LIGHT) : lv_color_white(),
                LV_PART_INDICATOR | LV_STATE_CHECKED);
            lv_obj_set_size(checkbox, BOARD_SCALE_PX(28), BOARD_SCALE_PX(28));
            lv_obj_align(checkbox, LV_ALIGN_RIGHT_MID, BOARD_SCALE_PX(-18), 0);
            lv_obj_remove_flag(checkbox, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                                        LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, group_song_selection_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        }
    }

    if (incremental) group_songs_build_begin(build_end, page_end, album_layout);
    group_songs_tail_create(page_end);
    /* The scroll position below needs the rows it points into, not the rest: the spacer keeps
     * the content height exact and the scroll event builds what the new position shows. */
    if (gsb.active && restore_scroll && restore_scroll_y > 0)
        group_songs_build_to_reach(restore_scroll_y + lv_obj_get_height(group_songs_list));
    if (gsb.active && group_songs_anchor_pending) {
        int anchor = group_songs_anchor_index < group_songs_count ? group_songs_anchor_index : group_songs_count - 1;
        group_songs_build_through(anchor);
    }
    refresh_group_songs_now_playing_indicator();
    group_songs_rows_rebuilding = false;
    refresh_group_song_thumbnails();
    lv_obj_update_layout(group_songs_list);
    if (restore_scroll) lv_obj_scroll_to_y(group_songs_list, restore_scroll_y, LV_ANIM_OFF);
    if (group_songs_anchor_pending) {
        lv_area_t list_area;
        lv_obj_get_coords(group_songs_list, &list_area);
        int anchor_index = group_songs_anchor_index;
        if (anchor_index >= group_songs_count) anchor_index = group_songs_count - 1;
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
    group_songs_populated_page_start = group_songs_page_start;
    /* The page's duration lookup starts from gui_library_poll_track_probes(): ask the loop for that
     * pass now instead of leaving it to whichever worker notifies next or the 500 ms housekeeping.
     * Not on album pages: their rows built after the lookup lands take another (1 px different)
     * text layout than the ones it fills in, so the lookup keeps its old timing there. */
    if (group_songs_screen && lv_screen_active() == group_songs_screen && !group_songs_album_layout()) ui_wake_notify();
    UI_TRACE_SPAN_END(UI_TRACE_SPAN_GROUP_ROWS, perf_rows);
}

/* Every screen's back button is a fixed 64x64 at the screen's own left
 * edge (build_group_songs_screen() below, build_subsonic_list_screen()) --
 * this is where a title label should start, not centered over top of it. */
#define TITLE_LABEL_LEFT_INSET 76 /* 64px back button + 12px breathing room */
/* Default right margin for a title with no right-side icon of its own on
 * that particular screen -- most build_subsonic_list_screen() callers
 * (Playlists, Saved Servers, New Connection, Queue, DLNA, Remote Control,
 * the plugin list pool, ...) never get anywhere near this wide anyway
 * (always a short fixed string), so this is mostly headroom for the ones
 * that do have a long dynamic title but no button beside it (the local
 * library's Artist -> Albums drill-down, artist_albums_screen). */
#define TITLE_LABEL_DEFAULT_RIGHT_MARGIN 20

static void group_songs_edit_btn_cb(lv_event_t * e) {
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

static void group_songs_screen_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        gui_album_header_forget();
        gui_album_header_release_cover();
        library_thumbnails_forget_list(group_songs_list);
        gui_list_reorder_finish(&group_songs_reorder_state, false);
        group_songs_unregister_reorder_handles();
        group_songs_reorder_state.rows = NULL;
        group_songs_reorder_state.list = NULL;
        group_songs_clear_selection();
        group_songs_edit_mode = false;
        group_songs_edit_btn = NULL;
        group_songs_delete_btn = NULL;
        group_songs_select_all_row = NULL;
        group_songs_select_all_checkbox = NULL;
        group_songs_select_all_label = NULL;
        group_songs_select_all_done_btn = NULL;
        group_songs_title_label = NULL;
        free(group_songs_source_title);
        group_songs_source_title = NULL;
        group_songs_list = NULL;
        group_songs_build_reset();
        group_songs_now_playing_bar = NULL;
        group_songs_populated_page_start = -1;
        group_songs_anchor_pending = false;
        group_songs_rebuild_on_load = false;
        memset(group_songs_visible_rows, 0, sizeof(group_songs_visible_rows));
        memset(group_songs_thumbnail_images, 0, sizeof(group_songs_thumbnail_images));
        memset(group_songs_selection_checks, 0, sizeof(group_songs_selection_checks));
    } else if (code == LV_EVENT_SCREEN_UNLOADED) {
        if (gsb.timer) lv_timer_pause(gsb.timer); /* resumed by SCREEN_LOADED */
        gui_list_reorder_finish(&group_songs_reorder_state, false);
        group_songs_unregister_reorder_handles();
        group_songs_reorder_state.rows = NULL;
        group_songs_reorder_state.list = NULL;
        if (group_songs_edit_mode) {
            group_songs_capture_scroll_anchor(NULL, 0);
            group_songs_clear_selection();
            memset(group_songs_selection_checks, 0, sizeof(group_songs_selection_checks));
            group_songs_edit_mode = false;
            group_songs_rebuild_on_load = true;
        }
    } else if (code == LV_EVENT_SCREEN_LOADED) {
        if (group_songs_rebuild_on_load) {
            group_songs_rebuild_on_load = false;
            populate_group_songs_rows();
        }
        if (gsb.active && gsb.timer) lv_timer_resume(gsb.timer); /* paused while another screen was shown */
        if (!group_songs_album_layout()) ui_wake_notify(); /* the page's duration lookup starts now (see populate_group_songs_rows) */
    }
}

static void group_song_selection_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !group_songs_edit_mode ||
        group_songs_reorder_state.active) return;
    int pos = (int) (intptr_t) lv_event_get_user_data(e);
    if (!group_songs_edit_m3u_path || pos < 0 || pos >= group_songs_count || !group_songs_selected) return;
    if (!group_playlist_unchanged()) {
        reload_edited_playlist();
        show_info_toast(TR("Playlist changed. Select a song again."));
        return;
    }
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
    if (!group_playlist_unchanged()) {
        reload_edited_playlist();
        show_info_toast(TR("Playlist changed. Select a song again."));
        return;
    }
    bool select_all = group_songs_selected_count != group_songs_count;
    for (int i = 0; i < group_songs_count; ++i) group_songs_selected[i] = select_all;
    group_songs_selected_count = select_all ? group_songs_count : 0;
    int visible_count = group_songs_count - group_songs_page_start;
    if (visible_count > GROUP_SONGS_PAGE_SIZE) visible_count = GROUP_SONGS_PAGE_SIZE;
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

static void group_songs_capture_scroll_anchor(const int * deleted_indexes, int deleted_count) {
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

static void group_songs_delete_selected_cb(lv_event_t * e) {
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

/* entries[0..count) is copied into group_songs_entries (see set_group_songs_
 * entries()) -- the caller's own array/buffer can be freed immediately after
 * this call returns, unlike the old group_t-based API where the group_t's
 * .indices had to stay valid for as long as this screen kept showing it. */
static void show_group_songs_editable_impl(const char * name, const group_song_entry_t * entries, int count,
                                           const char * editable_m3u_path, bool music_submenu, bool take_ownership) {
    char * source_title = strdup(name ? name : "");
    if (!source_title) {
        if (take_ownership) free_group_song_entries((group_song_entry_t *) entries, count);
        show_error_toast(TR("Out of memory"));
        return;
    }
    album_load_bump_generation();
    group_songs_source_is_album = group_songs_refresh_source == GROUP_SONGS_REFRESH_ALBUM;
    group_songs_music_submenu = music_submenu;
    if (editable_m3u_path) snprintf(group_songs_owned_m3u_path, sizeof(group_songs_owned_m3u_path), "%s", editable_m3u_path);
    group_songs_edit_m3u_path = editable_m3u_path ? group_songs_owned_m3u_path : NULL;
    group_songs_file_stat_valid = editable_m3u_path && stat(editable_m3u_path, &group_songs_file_stat) == 0;
    group_songs_edit_mode = false;
    group_songs_anchor_pending = false;
    group_songs_rebuild_on_load = false;
    if (take_ownership) set_group_songs_entries_owned((group_song_entry_t *) entries, count);
    else set_group_songs_entries(entries, count);
    group_songs_clear_selection();
    group_songs_page_start = 0;
    group_songs_populated_page_start = -1;

    free(group_songs_source_title);
    group_songs_source_title = source_title;
    lv_label_set_text(group_songs_title_label, source_title);
    populate_group_songs_rows();

    nav_push(group_songs_screen);
}

void show_group_songs_editable(const char * name, const group_song_entry_t * entries, int count,
                                       const char * editable_m3u_path, bool music_submenu) {
    show_group_songs_editable_impl(name, entries, count, editable_m3u_path, music_submenu, false);
}

void show_group_songs(const char * name, const group_song_entry_t * entries, int count) {
    /* The title arrives already translated (show_favorites()/show_most_played()). */
    group_songs_refresh_source = name && strcmp(name, TR("Favorites")) == 0 ? GROUP_SONGS_REFRESH_FAVORITES
                                : name && strcmp(name, TR("Most Played")) == 0 ? GROUP_SONGS_REFRESH_MOST_PLAYED
                                : GROUP_SONGS_REFRESH_NONE;
    show_group_songs_editable(name, entries, count, NULL, false);
}

void show_music_group_songs_impl(const char * name, const char * album_artist, const char * artist_filter,
                                        const group_song_entry_t * entries, int count, bool take_ownership) {
    group_songs_refresh_source = GROUP_SONGS_REFRESH_ALBUM;
    snprintf(group_songs_refresh_album, sizeof(group_songs_refresh_album), "%s", name ? name : "");
    snprintf(group_songs_refresh_album_artist, sizeof(group_songs_refresh_album_artist), "%s",
             album_artist ? album_artist : "");
    snprintf(group_songs_refresh_artist_filter, sizeof(group_songs_refresh_artist_filter), "%s",
             artist_filter ? artist_filter : "");
    show_group_songs_editable_impl(name, entries, count, NULL, true, take_ownership);
}

void show_music_group_songs(const char * name, const char * album_artist, const char * artist_filter,
                                   const group_song_entry_t * entries, int count) {
    show_music_group_songs_impl(name, album_artist, artist_filter, entries, count, false);
}

/* Ownership-transferring sibling of show_group_songs() -- see set_group_
 * songs_entries_owned()'s own comment for why this exists. Never editable
 * (no m3u path makes sense for a flattened, non-playlist view), same as
 * plain show_group_songs()'s own default. Caller must not free `entries`
 * after this call -- ownership has moved to group_songs_entries. */
void show_group_songs_take_ownership(const char * name, group_song_entry_t * entries, int count,
                                            bool push_screen) {
    char * source_title = strdup(name ? name : "");
    if (!source_title) {
        free_group_song_entries(entries, count);
        show_error_toast(TR("Out of memory"));
        return;
    }
    album_load_bump_generation();
    group_songs_source_is_album = false;
    group_songs_music_submenu = true;
    group_songs_edit_m3u_path = NULL;
    group_songs_edit_mode = false;
    group_songs_anchor_pending = false;
    group_songs_rebuild_on_load = false;
    set_group_songs_entries_owned(entries, count);
    group_songs_clear_selection();
    group_songs_page_start = 0;
    group_songs_populated_page_start = -1;

    free(group_songs_source_title);
    group_songs_source_title = source_title;
    lv_label_set_text(group_songs_title_label, source_title);
    populate_group_songs_rows();

    if (push_screen) nav_push(group_songs_screen);
}

lv_obj_t * build_group_songs_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    group_songs_title_label = build_screen_header(scr, "", generic_back_cb, NULL, NULL);
    /* Keep dynamic album/group names on one line. The title remains bounded
     * by the existing Edit/search action reservations below. */
    row_label_enable_marquee(group_songs_title_label);
    /* Hidden by default -- populate_group_songs_rows() (via
     * show_group_songs_editable()) is what actually shows this, and only
     * for a group backed by an editable .m3u playlist. */
    group_songs_edit_btn = lv_label_create(scr);
    lv_label_set_text(group_songs_edit_btn, TR("Edit"));
    lv_obj_add_style(group_songs_edit_btn, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(group_songs_edit_btn, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    align_screen_header_action(group_songs_edit_btn, BOARD_SCALE_PX(100));
    lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(group_songs_edit_btn, group_songs_edit_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scr, group_songs_screen_event_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_obj_add_event_cb(scr, group_songs_screen_event_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(scr, group_songs_screen_event_cb, LV_EVENT_DELETE, NULL);

    group_songs_delete_btn = build_top_right_icon_button(scr, asset_path("playlist_actions/trash.png"),
                                                          group_songs_delete_selected_cb);
    align_screen_header_action(group_songs_delete_btn, BOARD_SCALE_PX(20));
    lv_obj_t * delete_image = lv_obj_get_child(group_songs_delete_btn, 0);
    if (delete_image) {
        lv_obj_set_size(delete_image, BOARD_SCALE_PX(44), BOARD_SCALE_PX(44));
        lv_image_set_inner_align(delete_image, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_add_style(delete_image, gui_theme_accent_style(), 0);
    }
    lv_obj_add_flag(group_songs_delete_btn, LV_OBJ_FLAG_HIDDEN);

    group_songs_list = lv_obj_create(scr);
    lv_obj_set_size(group_songs_list, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT - HOME_INDICATOR_CONTENT_INSET);
    lv_obj_align(group_songs_list, LV_ALIGN_BOTTOM_MID, 0, -HOME_INDICATOR_CONTENT_INSET);
    lv_obj_set_style_bg_opa(group_songs_list, 0, 0);
    lv_obj_set_style_border_width(group_songs_list, 0, 0);
    /* Clear padding so rows align flush with container edges. */
    lv_obj_set_style_pad_all(group_songs_list, 0, 0);
    lv_obj_set_scroll_dir(group_songs_list, LV_DIR_VER); /* see build_icon_grid_screen's comment */
    lv_obj_set_scrollbar_mode(group_songs_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(group_songs_list, BOARD_SCALE_PX(3), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(group_songs_list, BOARD_SCALE_PX(2), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(group_songs_list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_add_style(group_songs_list, gui_theme_accent_style(), LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(group_songs_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(group_songs_list, GUI_ROW_GAP, 0);
    lv_obj_set_style_pad_top(group_songs_list, GUI_ROW_GAP, 0);
    lv_obj_set_style_pad_bottom(group_songs_list, BOARD_SCALE_PX(8), 0);
    lv_obj_add_event_cb(group_songs_list, group_songs_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(group_songs_list, group_songs_wake_cb, LV_EVENT_PRESSED, NULL);
    ui_scroll_blit_enable(group_songs_list); /* rows keep their pixels while the page scrolls */

    library_thumbnails_bind_list(scr, group_songs_list, LIBRARY_THUMB_LIST_GROUP_SONGS_CUSTOM);

    finalize_screen_navigation(scr);
    return scr;
}

/* Whether a group row gets a cover (or its placeholder) at all. */
static bool group_song_has_cover(int index) {
    /* Album rows show the track number where the thumbnail was. */
    if (group_songs_album_layout()) return false;
    return index >= 0 && index < group_songs_count && group_songs_entries[index].song_id > 0 &&
           group_songs_entries[index].artwork_key != 0;
}

static void group_song_row_text_offset(lv_obj_t * row, bool with_thumbnail) {
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

static void refresh_group_song_thumbnails(void) {
    if (!group_songs_list || group_songs_rows_rebuilding) return;
    lv_obj_update_layout(group_songs_list);
    int page_end = group_songs_page_start + GROUP_SONGS_PAGE_SIZE;
    if (page_end > group_songs_count) page_end = group_songs_count;
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

library_thumbnail_context_t library_thumbnail_genre_context(void) {
    int view = genre_browse_view();
    if (view == GENRE_VIEW_ALBUMS)
        return (library_thumbnail_context_t) {
            .key_kind = THUMBNAIL_KEY_ALBUM,
            .artist_group_kind = METADATA_DB_GROUP_ARTIST
        };
    return (library_thumbnail_context_t) {
        .key_kind = THUMBNAIL_KEY_ARTIST,
        .artist_group_kind = view == GENRE_VIEW_ALBUM_ARTISTS
            ? METADATA_DB_GROUP_ALBUM_ARTIST : METADATA_DB_GROUP_ARTIST
    };
}

bool library_thumbnail_policy(library_thumbnail_policy_t policy) {
    if (policy == LIBRARY_THUMB_POLICY_LAZY) return library_rescan_active;
    return library_rescan_active || gui_library_sd_format_is_active();
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

void refresh_group_thumbnail_binding(void) {
    refresh_group_song_thumbnails();
}

void library_thumbnail_show_busy(library_thumbnail_cover_scope_t scope) {
    const char *title = scope == LIBRARY_THUMB_COVER_ALL
        ? TR("Refreshing\nall covers...") : TR("Reloading\ncover...");
    library_rescan_token = gui_busy_show(title, TR("Preparing cover refresh..."));
    gui_busy_set_progress(library_rescan_token, 0);
}

/* Called after list deletion: unregistering is pointer identity only, so it never
 * touches a freed LVGL object. */
void forget_deleted_library_lists(void) {
    lv_obj_t *dead[] = { all_songs_list, artists_list, albums_list, album_artist_list,
                         recently_added_list, genres_list };
    for (size_t i = 0; i < sizeof(dead) / sizeof(dead[0]); ++i)
        if (dead[i]) library_thumbnails_forget_list(dead[i]);
    all_songs_list = artists_list = albums_list = album_artist_list = recently_added_list = NULL;
    all_songs_bar = (songs_bar_t) { 0 };
    recent_bar = (songs_bar_t) { 0 };
}

const char * group_songs_search_name_of(int index) {
    if (index >= 0 && index < group_songs_count && group_songs_entries[index].title)
        return group_songs_entries[index].title;
    return "";
}

void group_songs_search_row_click_cb(int display_index) {
    if (library_rescan_active) return;
    int real_index = search_remap_index(SEARCH_BINDING_GROUP_SONGS, display_index);
    group_play_at(real_index);
}

void group_songs_search_row_long_press_cb(int display_index) {
    if (library_rescan_active) return;
    int real_index = search_remap_index(SEARCH_BINDING_GROUP_SONGS, display_index);
    if (real_index >= 0 && real_index < group_songs_count && group_songs_entries[real_index].path)
        open_song_context_menu(group_songs_entries[real_index].path);
}

void update_group_songs_search_button(bool editing) {
    /* Album views have no Edit action; give their title the space up to
     * Search. Editable playlists reserve whichever action remains visible:
     * Edit in normal mode, trash while editing. */
    lv_obj_t * title_action = group_songs_edit_m3u_path
        ? (group_songs_edit_mode ? group_songs_delete_btn : group_songs_edit_btn)
        : NULL;
    library_search_update_header(SEARCH_BINDING_GROUP_SONGS, group_songs_title_label,
                                 title_action, editing);
}

static void refresh_group_songs_search_now_playing(void) {
    if (!group_songs_search_list) return;
    int target = -1;
    if (library_search_is_active(SEARCH_BINDING_GROUP_SONGS) && now_playing_path[0]) {
        for (int i = 0; i < group_songs_count; i++) {
            if (strcmp(group_songs_entries[i].path, now_playing_path) == 0) {
                target = library_search_find_filtered_row(SEARCH_BINDING_GROUP_SONGS, i);
                break;
            }
        }
    }
    compact_list_set_now_playing(group_songs_search_list, target);
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
    group_song_entry_t * entries = count ? build_group_song_entries_from_paths(songs, count) : NULL;
    for (int i = 0; i < count; i++) free(songs[i]);
    free(songs);
    if (count && !entries) { group_songs_anchor_pending = false; return; }
    group_songs_file_stat_valid = stat(group_songs_edit_m3u_path, &group_songs_file_stat) == 0;
    set_group_songs_entries(entries, count);
    group_songs_drag_original_child_count = 0;
    group_songs_drag_source_id = -1;
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
    free_group_song_entries(entries, count);
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

static void refresh_group_songs_paths_in_place(void) {
    group_song_entry_t * entries = calloc((size_t)(group_songs_count > 0 ? group_songs_count : 1),
                                          sizeof(*entries));
    if (!entries) {
        set_group_songs_entries_owned(NULL, 0);
        populate_group_songs_rows();
        return;
    }
    int count = 0;
    for (int i = 0; i < group_songs_count; i++) {
        song_row_t song;
        if (!metadata_db_get_song_by_path(group_songs_entries[i].path, &song)) continue;
        char title[384];
        if (group_songs_music_submenu) format_music_submenu_identity(&song, title, sizeof(title));
        else format_song_identity(&song, title, sizeof(title));
        entries[count].path = strdup(song.path);
        entries[count].title = strdup(title);
        entries[count].song_id = song.id;
        entries[count].artwork_key = song_album_thumbnail_key(&song);
        entries[count].disc_number = group_songs_entries[i].disc_number;
        entries[count].show_disc_header = group_songs_entries[i].show_disc_header;
        if (!entries[count].path || !entries[count].title) {
            free_group_song_entries(entries, count + 1);
            set_group_songs_entries_owned(NULL, 0);
            populate_group_songs_rows();
            return;
        }
        count++;
    }
    set_group_songs_entries_owned(entries, count);
    populate_group_songs_rows();
}

void refresh_group_songs_from_database(void) {
    group_songs_page_start = 0;
    switch (group_songs_refresh_source) {
        case GROUP_SONGS_REFRESH_FAVORITES: {
            char ** paths = NULL;
            int count = 0;
            metadata_db_load_favorite_songs(&paths, &count);
            group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
            set_group_songs_entries(entries, entries ? count : 0);
            free_group_song_entries(entries, count);
            for (int i = 0; i < count; i++) free(paths[i]);
            free(paths);
            populate_group_songs_rows();
            break;
        }
        case GROUP_SONGS_REFRESH_MOST_PLAYED: {
            char ** paths = NULL;
            int count = 0;
            metadata_db_load_top_played_songs(MOST_PLAYED_LIMIT, &paths, &count);
            group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
            set_group_songs_entries(entries, entries ? count : 0);
            free_group_song_entries(entries, count);
            for (int i = 0; i < count; i++) free(paths[i]);
            free(paths);
            populate_group_songs_rows();
            break;
        }
        case GROUP_SONGS_REFRESH_M3U: {
            char ** paths = NULL;
            int count = 0;
            bool loaded = group_songs_refresh_m3u_path[0] &&
                          playlist_files_read(group_songs_refresh_m3u_path, &paths, &count);
            group_song_entry_t * entries = loaded && count ? build_group_song_entries_from_paths(paths, count) : NULL;
            set_group_songs_entries(entries, entries ? count : 0);
            free_group_song_entries(entries, count);
            for (int i = 0; i < count; i++) free(paths[i]);
            free(paths);
            group_songs_file_stat_valid = loaded &&
                stat(group_songs_refresh_m3u_path, &group_songs_file_stat) == 0;
            populate_group_songs_rows();
            break;
        }
        case GROUP_SONGS_REFRESH_ALBUM: {
            int64_t count64 = 0;
            if (group_songs_refresh_artist_filter[0]) {
                count64 = metadata_db_count_songs_filtered(NULL, group_songs_refresh_artist_filter,
                    group_songs_refresh_album_artist, group_songs_refresh_album, NULL);
            } else {
                int64_t offset = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM,
                    group_songs_refresh_album, group_songs_refresh_album_artist);
                group_row_t group;
                if (offset >= 0 && metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM,
                        (int) offset, 1, &group) == 1) count64 = group.song_count;
            }
            int count = count64 > 0 && count64 <= INT_MAX ? (int) count64 : 0;
            int loaded_count = 0;
            group_song_entry_t * entries = load_album_entries_filtered(group_songs_refresh_album,
                group_songs_refresh_album_artist, count, group_songs_refresh_artist_filter, &loaded_count, NULL);
            set_group_songs_entries(entries, entries ? loaded_count : 0);
            free_group_song_entries(entries, loaded_count);
            populate_group_songs_rows();
            break;
        }
        case GROUP_SONGS_REFRESH_ARTIST:
            if (!artist_albums_show_all_songs_internal(false, group_songs_refresh_artist_name,
                                                       group_songs_refresh_artist_kind)) {
                set_group_songs_entries(NULL, 0);
                populate_group_songs_rows();
            }
            break;
        case GROUP_SONGS_REFRESH_GENRE:
            if (!show_genre_songs_internal(false, group_songs_refresh_genre)) {
                set_group_songs_entries(NULL, 0);
                populate_group_songs_rows();
            }
            break;
        case GROUP_SONGS_REFRESH_NONE:
        default:
            refresh_group_songs_paths_in_place();
            break;
    }
}
