#include "gui_playlists.h"

#include "../../application/gui.h"
#include "../browse/gui_folder_actions.h"
#include "../../shell/gui_navigation.h"
#include "../../shell/gui_notifications.h"
#include "../../network/gui_network.h"
#include "gui_playlist_target.h"
#include "../../appearance/gui_theme.h"
#include "../../text/rendering/gui_text_input.h"
#include "../../../hardware/indicators/backlight.h"
#include "../../text/i18n/i18n.h"
#include "../../../library/browser/paths/library_paths.h"
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../library/playlists/playlist_files.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../storage/sd_card_identity.h"
#include "../../../hardware/usb/usb_mode_control.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"

typedef enum {
    PLAYLIST_NAME_TARGET_NONE,
    PLAYLIST_NAME_TARGET_STANDALONE,
    PLAYLIST_NAME_TARGET_SONG,
    PLAYLIST_NAME_TARGET_FOLDER,
} playlist_name_target_mode_t;

static lv_obj_t *playlists_screen;
static lv_obj_t *playlists_list;
static lv_obj_t *playlists_refresh_icon;
static bool playlists_manual_refresh;
static lv_obj_t *add_to_playlist_screen;
static lv_obj_t *add_to_playlist_list;
static playlist_name_target_mode_t playlist_name_target_mode;
static uint64_t playlist_name_target_generation;
static char **playlists_m3u_paths;
static int playlists_m3u_count;
static bool playlist_row_long_press_fired;
static gui_popup_t playlist_delete_popup;
static lv_obj_t *playlist_context_menu_popup;
static lv_obj_t *playlist_context_menu_backdrop;
static char playlist_action_path[PATH_MAX];
static gui_playlists_open_row_cb_t open_row_cb;
static gui_playlists_reload_edited_cb_t reload_edited_cb;
static gui_playlists_ensure_dir_cb_t ensure_dir_cb;
static void populate_add_to_playlist_screen(void);
static void populate_playlists_screen(void);
static void new_playlist_row_cb(lv_event_t *event);
static void existing_playlist_row_cb(lv_event_t *event);
static void playlist_row_click_cb(lv_event_t *event);
static void build_playlist_context_menu_popup(void);

static lv_obj_t *build_add_to_playlist_screen(void) {
    lv_obj_t *title_label;
    return build_subsonic_list_screen(TR("Add to Playlist"), &title_label, &add_to_playlist_list);
}

static void playlist_picker_path_free_cb(lv_event_t * e) {
    free(lv_event_get_user_data(e));
}

static void populate_add_to_playlist_screen(void) {
    lv_obj_clean(add_to_playlist_list);

    lv_obj_t * new_row = lv_obj_create(add_to_playlist_list);
    lv_obj_set_size(new_row, LIST_ROW_WIDTH, ui_list_row_height());
    lv_obj_add_style(new_row, &native_row_min_style, 0);
    lv_obj_set_style_radius(new_row, LIST_ROW_RADIUS, 0);
    lv_obj_add_style(new_row, &pill_row_bg_style, 0);
    lv_obj_remove_flag(new_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(new_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(new_row, new_playlist_row_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * new_label = lv_label_create(new_row);
    lv_label_set_text(new_label, TR("+ New Playlist"));
    lv_obj_add_style(new_label, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(new_label, &LIST_ROW_FONT, 0);
    const int32_t picker_label_w = ui_row_label_width(new_row, LIST_ROW_WIDTH, LIST_ROW_LABEL_INSET, LIST_ROW_LABEL_INSET);
    lv_obj_set_width(new_label, picker_label_w);
    lv_obj_set_style_text_align(new_label, LV_TEXT_ALIGN_LEFT, 0);
    ui_row_fit_label(new_row, new_label, &LIST_ROW_FONT, LIST_ROW_WIDTH, LIST_ROW_LABEL_INSET, picker_label_w);

    char ** paths;
    int count;
    /* Persistent cache read, not a live scan -- same reasoning as
     * populate_playlists_screen()'s own comment; this screen is reachable
     * from the player's "more" menu, so it was paying the same ~5s SD-card
     * walk on every open too. */
    metadata_db_load_all_playlists(&paths, &count);
    if (count == 0) return;

    for (int i = 0; i < count; i++) {
        lv_obj_t * row = lv_obj_create(add_to_playlist_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, ui_list_row_height());
        lv_obj_add_style(row, &native_row_min_style, 0);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_add_style(row, &pill_row_bg_style, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, existing_playlist_row_cb, LV_EVENT_CLICKED, paths[i]);
        lv_obj_add_event_cb(row, playlist_picker_path_free_cb, LV_EVENT_DELETE, paths[i]);

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, basename_of(paths[i]));
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
        lv_obj_set_width(label, picker_label_w);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        ui_row_fit_label(row, label, &LIST_ROW_FONT, LIST_ROW_WIDTH, LIST_ROW_LABEL_INSET, picker_label_w);
    }
    free(paths);
}


static void playlist_delete_cancel_cb(lv_event_t * e) {
    (void) e;
    gui_popup_hide(&playlist_delete_popup);
}

static void playlist_delete_confirm_cb(lv_event_t * e) {
    playlist_delete_cancel_cb(e);
    if (!playlist_files_delete(playlist_action_path)) {
        show_error_toast(TR("Cannot delete playlist")); return;
    }
    metadata_db_playlist_delete_one(playlist_action_path);
    populate_playlists_screen();
    show_info_toast(TR("Playlist deleted"));
}

static void playlist_rename_done_cb(const char * name, void * data) {
    (void) data;
    char dest[PATH_MAX];
    if (!playlist_files_rename(playlist_action_path, name, dest, sizeof(dest))) {
        show_error_toast(TR("Cannot rename: invalid name or file exists")); return;
    }
    metadata_db_playlist_delete_one(playlist_action_path);
    metadata_db_playlist_insert_one(dest);
    populate_playlists_screen();
    show_info_toast(TR("Playlist renamed"));
}

static void hide_playlist_context_menu_popup(void) {
    if (playlist_context_menu_popup) lv_obj_add_flag(playlist_context_menu_popup, LV_OBJ_FLAG_HIDDEN);
    if (playlist_context_menu_backdrop) lv_obj_add_flag(playlist_context_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void playlist_context_menu_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_playlist_context_menu_popup();
}

static void playlist_context_menu_rename_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_playlist_context_menu_popup();
    show_text_entry(TR("Rename Playlist"), basename_of(playlist_action_path), false, false, playlist_rename_done_cb, NULL);
}

static void playlist_context_menu_delete_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_playlist_context_menu_popup();
    if (!playlist_delete_popup.popup)
        playlist_delete_popup.popup = build_confirm_popup(TR("Delete playlist?"), LV_LABEL_LONG_WRAP, NULL,
            TR("The playlist file will be deleted. Music files are kept."), TR("Delete"),
            accent_lv_color(), playlist_delete_confirm_cb, NULL, TR("Cancel"), accent_lv_color(),
            playlist_delete_cancel_cb, NULL, playlist_delete_cancel_cb, &playlist_delete_popup.backdrop);
    gui_popup_show(&playlist_delete_popup);
}

static void playlist_context_menu_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_playlist_context_menu_popup();
}

static void build_playlist_context_menu_popup(void) {
    const menu_popup_row_t rows[] = {
        { TR("Rename Playlist"), playlist_context_menu_rename_cb, false, false, NULL },
        { TR("Delete Playlist"), playlist_context_menu_delete_cb, true, false, NULL },
        { TR("Cancel"), playlist_context_menu_cancel_cb, false, true, NULL },
    };
    playlist_context_menu_popup = build_menu_popup(rows, (int) (sizeof(rows) / sizeof(rows[0])),
                                                    playlist_context_menu_backdrop_cb,
                                                    &playlist_context_menu_backdrop);
}

static void playlist_row_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    playlist_row_long_press_fired = true;
    int i = (int) (intptr_t) lv_event_get_user_data(e);
    if (i < 0 || i >= playlists_m3u_count) return;
    snprintf(playlist_action_path, sizeof(playlist_action_path), "%s", playlists_m3u_paths[i]);
    lv_obj_remove_flag(playlist_context_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(playlist_context_menu_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(playlist_context_menu_backdrop);
    lv_obj_move_foreground(playlist_context_menu_popup);
}

static void populate_playlists_screen(void) {
    lv_obj_clean(playlists_list);

    for (int i = 0; i < playlists_m3u_count; i++) free(playlists_m3u_paths[i]);
    free(playlists_m3u_paths);
    playlists_m3u_paths = NULL;
    playlists_m3u_count = 0;
    /* Persistent cache of PLAYLISTS_DIR only -- see rescan_playlists(). */
    metadata_db_load_all_playlists(&playlists_m3u_paths, &playlists_m3u_count);

    lv_obj_t * create = build_wide_list_row(playlists_list, TR("+ New Playlist"), true);
    lv_obj_add_style(create, &style_theme_card_bg, 0);
    lv_obj_add_flag(create, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(create, new_playlist_row_cb, LV_EVENT_CLICKED, (void *) 1);
    build_list_section(playlists_list, TR("System playlists"));

    /* Favorites/Most Played/Queue/Recently Added are never deletable/
     * renamable (none is a real .m3u file -- see playlist_row_click_cb()'s
     * index==0..3 special cases), so they get no long-press handler. */
    lv_obj_t * favorites_row = build_wide_list_row(playlists_list, TR("Favorites"), true);
    lv_obj_add_flag(favorites_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(favorites_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 0);

    lv_obj_t * most_played_row = build_wide_list_row(playlists_list, TR("Most Played"), true);
    lv_obj_add_flag(most_played_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(most_played_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 1);

    lv_obj_t * queue_row = build_wide_list_row(playlists_list, TR("Queue"), true);
    lv_obj_add_flag(queue_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(queue_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 2);

    lv_obj_t * recently_added_row = build_wide_list_row(playlists_list, TR("Recently Added"), true);
    lv_obj_add_flag(recently_added_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(recently_added_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 3);

    build_list_section(playlists_list, TR("User playlists"));
    if (!playlists_m3u_count)
        build_list_message(playlists_list, TR("No user playlists"), TR("Create a playlist above or copy one to the SD card's Playlists folder."));
    for (int i = 0; i < playlists_m3u_count; i++) {
        const char * display = playlists_m3u_paths[i];
        if (strncmp(display, PLAYLISTS_DIR "/", strlen(PLAYLISTS_DIR) + 1) == 0) display += strlen(PLAYLISTS_DIR) + 1;
        lv_obj_t * row = build_wide_list_row(playlists_list, display, true);
        lv_obj_t * name_label = lv_obj_get_child(row, 0);
        const int32_t name_w = LIST_ROW_WIDTH_WIDE - LIST_ROW_LABEL_INSET - BOARD_SCALE_PX(60);
        lv_obj_set_width(name_label, name_w);
        /* The chevron is the row's other child: floating when the row grows. */
        if (ui_row_fit_label(row, name_label, &LIST_ROW_FONT, LIST_ROW_WIDTH_WIDE, LIST_ROW_LABEL_INSET, name_w)) {
            /* configure_scrolling_row_label() gave the label a fixed one-line
             * height, a top margin and the font-change re-bound flag; a tall
             * row needs none of them (the marquee style is inert in WRAP). */
            lv_obj_set_height(name_label, LV_SIZE_CONTENT);
            lv_obj_set_style_pad_top(name_label, 0, 0);
            lv_obj_remove_flag(name_label, LV_OBJ_FLAG_USER_3);
            lv_obj_add_flag(lv_obj_get_child(row, 1), LV_OBJ_FLAG_FLOATING);
        }
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) (4 + i));
        lv_obj_add_event_cb(row, playlist_row_long_press_cb, LV_EVENT_LONG_PRESSED, (void *) (intptr_t) i);
    }
}

static void playlists_refresh_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || playlists_manual_refresh) return;
#ifndef HOST_BUILD
    if (!sd_card_root_is_mounted()) {
        show_error_toast(TR("SD card not available"));
        return;
    }
#endif
    if (ensure_dir_cb) ensure_dir_cb();
    playlists_manual_refresh = true;
    set_header_refresh_action_busy(playlists_refresh_icon, true);
    playlist_files_refresh_async(PLAYLISTS_DIR);
}

static lv_obj_t * build_playlists_screen(void) {
    lv_obj_t * title_label;
    lv_obj_t * scr = build_subsonic_list_screen(TR("Playlists"), &title_label, &playlists_list);
    playlists_refresh_icon = build_header_refresh_action(scr, playlists_refresh_cb);
    set_header_refresh_action_busy(playlists_refresh_icon, playlists_manual_refresh);

    /* Explicit cross-axis centering scoped to this screen also keeps rows
     * correct if it is ever hosted in a parent narrower than the display. */
    lv_obj_set_flex_align(playlists_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    return scr;
}

static void new_playlist_name_done_cb(const char *text, void *user_data) {
    (void)user_data;
    if (text[0] == '\0') return;

    gui_playlist_target_snapshot_t target;
    gui_playlist_target_copy(&target);
    playlist_name_target_mode_t mode = playlist_name_target_mode;
    bool mode_target_current =
        (mode == PLAYLIST_NAME_TARGET_FOLDER && target.folder_path[0]) ||
        (mode == PLAYLIST_NAME_TARGET_SONG && target.song_path[0]) ||
        (mode == PLAYLIST_NAME_TARGET_STANDALONE &&
         !target.folder_path[0] && !target.song_path[0]);
    bool request_current = playlist_name_target_generation == target.generation && mode_target_current;
    if (!request_current) {
        if (mode == PLAYLIST_NAME_TARGET_FOLDER && !target.folder_path[0] && !target.song_path[0]) {
            int depth = gui_navigation_get_depth();
            for (int i = 0; i < depth - 1; i++) {
                if (gui_navigation_get_screen_at(i) == add_to_playlist_screen) {
                    nav_remove_stack_slot(i);
                    break;
                }
            }
        }
        show_error_toast(TR("Playlist selection expired"));
        return;
    }
    if (strlen(text) > 200) {
        show_error_toast(TR("Playlist name is too long"));
        return;
    }

    char created_path[512];
    if (mode == PLAYLIST_NAME_TARGET_FOLDER) {
        (void)gui_folder_actions_start_playlist_job(true, target.folder_path, NULL, text,
                                                    target.generation, add_to_playlist_screen);
        return;
    }
    const char *song_path = mode == PLAYLIST_NAME_TARGET_SONG ? target.song_path : "";
    bool ok = playlist_files_create(PLAYLISTS_DIR, text, song_path, created_path, sizeof(created_path));
    if (ok) metadata_db_playlist_insert_one(created_path);
    show_error_toast(ok ? TR("Playlist created") : TR("Failed to create playlist"));
    if (!ok) return;
    gui_playlist_target_clear_folder();
    playlist_name_target_mode = PLAYLIST_NAME_TARGET_NONE;
    if (mode == PLAYLIST_NAME_TARGET_STANDALONE) populate_playlists_screen();
    else nav_pop();
}

static void new_playlist_row_cb(lv_event_t *event) {
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    bool standalone = lv_event_get_user_data(event) != NULL;
    gui_playlist_target_snapshot_t target;
    if (standalone) {
        gui_playlist_target_begin_standalone();
        playlist_name_target_mode = PLAYLIST_NAME_TARGET_STANDALONE;
    } else {
        gui_playlist_target_copy(&target);
        if (target.kind == GUI_PLAYLIST_TARGET_FOLDER)
            playlist_name_target_mode = PLAYLIST_NAME_TARGET_FOLDER;
        else if (target.kind == GUI_PLAYLIST_TARGET_SONG)
            playlist_name_target_mode = PLAYLIST_NAME_TARGET_SONG;
        else {
            playlist_name_target_mode = PLAYLIST_NAME_TARGET_NONE;
            return;
        }
    }
    gui_playlist_target_copy(&target);
    playlist_name_target_generation = target.generation;
    show_text_entry(TR("Playlist Name"), "", false, false, new_playlist_name_done_cb, NULL);
}

static void existing_playlist_row_cb(lv_event_t *event) {
    const char *path = (const char *)lv_event_get_user_data(event);
    gui_playlist_target_snapshot_t target;
    gui_playlist_target_copy(&target);
    if (target.kind == GUI_PLAYLIST_TARGET_NONE) return;
    if (target.kind == GUI_PLAYLIST_TARGET_FOLDER) {
        (void)gui_folder_actions_start_playlist_job(false, target.folder_path, path, NULL,
                                                    target.generation, add_to_playlist_screen);
        return;
    }
    if (playlist_files_contains(path, target.song_path)) {
        show_error_toast(TR("Song already added"));
        nav_pop();
        return;
    }
    bool ok = playlist_files_append(path, target.song_path);
    show_error_toast(ok ? TR("Added to playlist") : TR("Failed to add to playlist"));
    nav_pop();
}

static void playlist_row_click_cb(lv_event_t *event) {
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    if (playlist_row_long_press_fired) { playlist_row_long_press_fired = false; return; }
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0) return;
    if (index < 4) {
        if (open_row_cb) open_row_cb((gui_playlist_row_kind_t)index, NULL, NULL, NULL, 0);
        return;
    }
    int i = index - 4;
    if (i < 0 || i >= playlists_m3u_count) return;
    char **songs = NULL;
    int count = 0;
    if (!playlist_files_read(playlists_m3u_paths[i], &songs, &count)) {
        show_error_toast(TR("Playlist unavailable or unreadable"));
        return;
    }
    if (open_row_cb)
        open_row_cb(GUI_PLAYLIST_ROW_SAVED_M3U, basename_of(playlists_m3u_paths[i]), playlists_m3u_paths[i], songs, count);
    for (int j = 0; j < count; j++) free(songs[j]);
    free(songs);
}

void gui_playlists_init(gui_playlists_open_row_cb_t open_row,
                        gui_playlists_reload_edited_cb_t reload_edited,
                        gui_playlists_ensure_dir_cb_t ensure_dir) {
    open_row_cb = open_row;
    reload_edited_cb = reload_edited;
    ensure_dir_cb = ensure_dir;
}

lv_obj_t *gui_playlists_build_saved_screen(void) {
    playlists_screen = build_playlists_screen();
    return playlists_screen;
}

lv_obj_t *gui_playlists_build_add_screen(void) {
    add_to_playlist_screen = build_add_to_playlist_screen();
    return add_to_playlist_screen;
}

void gui_playlists_build_context_menu(void) { build_playlist_context_menu_popup(); }

void gui_playlists_open_saved_screen(void) {
    populate_playlists_screen();
    nav_push(playlists_screen);
}

void gui_playlists_open_for_song(const char *path) {
    gui_playlist_target_begin_song(path);
    populate_add_to_playlist_screen();
    nav_push(add_to_playlist_screen);
}

void gui_playlists_open_for_folder(const char *path) {
    gui_playlist_target_begin_folder(path);
    populate_add_to_playlist_screen();
    nav_push(add_to_playlist_screen);
}

void gui_playlists_reset_name_target_mode(void) {
    playlist_name_target_mode = PLAYLIST_NAME_TARGET_NONE;
}

lv_obj_t *gui_playlists_get_saved_screen(void) { return playlists_screen; }

void gui_playlists_poll(void) {
    static lv_obj_t *last_screen;
    static uint32_t last_check;
    static bool startup = true, was_storage;
    lv_obj_t *screen = lv_screen_active();
    bool visible = screen && (screen == playlists_screen || screen == add_to_playlist_screen);
    bool entered = visible && screen != last_screen;
    last_screen = screen;
    bool refresh_ok = false;
    if (playlist_files_refresh_poll(&refresh_ok)) {
        if (visible) {
            lv_obj_t *list = screen == playlists_screen ? playlists_list : add_to_playlist_list;
            int32_t scroll = lv_obj_get_scroll_y(list);
            if (screen == playlists_screen) populate_playlists_screen();
            else populate_add_to_playlist_screen();
            lv_obj_scroll_to_y(list, scroll, LV_ANIM_OFF);
        }
        if (playlists_manual_refresh) {
            playlists_manual_refresh = false;
            set_header_refresh_action_busy(playlists_refresh_icon, false);
            if (refresh_ok) show_info_toast(TR("Playlists refreshed"));
            else show_error_toast(TR("Some playlists could not be read"));
        }
    }
    if (!backlight_screen_is_on()) return;
#ifndef HOST_BUILD
    if (!sd_card_root_is_mounted()) return;
#endif
    uint32_t now = lv_tick_get();
    if (!startup && !entered && now - last_check < 5000) return;
    last_check = now;
    usb_mode_t mode;
    bool storage = gui_network_usb_storage_session_active() &&
                   usb_mode_control_detect_current(&mode) && mode == USB_MODE_STORAGE;
    bool returned = was_storage && !storage;
    was_storage = storage;
    if (storage) return;
    if (reload_edited_cb) reload_edited_cb(screen);
    if (startup || visible || returned) playlist_files_refresh_async(PLAYLISTS_DIR);
    startup = false;
}

void gui_playlists_teardown_popups(void) {
    gui_popup_teardown(&playlist_delete_popup);
    if (playlist_context_menu_popup) { lv_obj_delete(playlist_context_menu_popup); playlist_context_menu_popup = NULL; }
    if (playlist_context_menu_backdrop) { lv_obj_delete(playlist_context_menu_backdrop); playlist_context_menu_backdrop = NULL; }
}

void gui_playlists_teardown_saved_screen(void) {
    if (playlists_screen) { lv_obj_delete(playlists_screen); playlists_screen = NULL; }
    playlists_refresh_icon = NULL;
    playlists_list = NULL;
}

void gui_playlists_teardown_add_screen(void) {
    if (add_to_playlist_screen) { lv_obj_delete(add_to_playlist_screen); add_to_playlist_screen = NULL; }
    add_to_playlist_list = NULL;
}

bool gui_playlists_menu_visible(void) {
    return (playlist_context_menu_popup &&
            !lv_obj_has_flag(playlist_context_menu_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (playlist_context_menu_backdrop &&
            !lv_obj_has_flag(playlist_context_menu_backdrop, LV_OBJ_FLAG_HIDDEN));
}
