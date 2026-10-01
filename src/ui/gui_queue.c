#include "gui_queue.h"
#include "gui.h"
#include "gui_player.h"
#include "gui_theme.h"
#include "gui_notifications.h"
#include "gui_library.h"
#include "gui_text_input.h"
#include "screen_builders.h"
#include "metadata.h"
#include "assets.h"
#include "fallback_font.h"
#include "src/misc/lv_text_private.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static lv_obj_t * queue_screen = NULL;
static lv_obj_t * queue_list = NULL;

static lv_obj_t * song_context_menu_popup = NULL;
static lv_obj_t * song_context_menu_popup_backdrop = NULL;
static char song_context_menu_target_path[600] = "";

extern lv_style_t list_row_style;
extern lv_style_t list_row_pressed_style;
extern lv_style_t style_theme_text_muted;

extern void nav_push(lv_obj_t * screen);
extern void get_display_names(const char * path, char * out_title, size_t title_sz, char * out_folder, size_t folder_sz);
extern void row_label_enable_marquee(lv_obj_t * label);

#define QUEUE_PAGE_SIZE 100
static int queue_page;
static uint64_t displayed_revision;
static bool queue_editing;
static lv_obj_t * queue_actions, * queue_actions_backdrop;
static void queue_actions_open(lv_event_t * e);
static int displayed_current = -1;

static int32_t queue_state_column_width(void) {
    static const char * states[] = { "Played", "Playing", "Queued", "Next" };
    const lv_font_t * font = gui_theme_font(GUI_FONT_ROLE_SUBTEXT);
    lv_text_attributes_t attributes;
    lv_text_attributes_init(&attributes);
    int32_t width = BOARD_SCALE_PX(72);
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        int32_t text_width = lv_text_get_width(states[i], (uint32_t) strlen(states[i]), font, &attributes);
        if (text_width > width) width = text_width;
    }
    int32_t maximum = LIST_ROW_WIDTH / 3;
    if (width > maximum) width = maximum;
    return width + BOARD_SCALE_PX(8);
}

bool gui_queue_boot_prompt_blocked(void) {
    return (queue_actions && !lv_obj_has_flag(queue_actions, LV_OBJ_FLAG_HIDDEN)) ||
           (queue_actions_backdrop && !lv_obj_has_flag(queue_actions_backdrop, LV_OBJ_FLAG_HIDDEN)) ||
           (song_context_menu_popup && !lv_obj_has_flag(song_context_menu_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (song_context_menu_popup_backdrop &&
            !lv_obj_has_flag(song_context_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN));
}

static void queue_row_click_cb(lv_event_t * e) {
    if (!gui_player_queue_select(displayed_revision, (int) (intptr_t) lv_event_get_user_data(e)))
        show_error_toast("Queue changed. Try again.");
}

static void queue_edit_cb(lv_event_t * e) {
    int value = (int) (intptr_t) lv_event_get_user_data(e);
    int index = value / 3, action = value % 3;
    int to = action == 2 ? -1 : index + (action == 0 ? -1 : 1);
    if (!gui_player_queue_edit(displayed_revision, index, to))
        show_error_toast("Cannot move this entry");
    populate_queue_screen();
}

static void queue_page_cb(lv_event_t * e) {
    queue_page += (int) (intptr_t) lv_event_get_user_data(e);
    populate_queue_screen();
    lv_obj_scroll_to_y(queue_list, 0, LV_ANIM_OFF);
}

static lv_obj_t * queue_label(const char * text) {
    lv_obj_t * row = build_music_list_row(queue_list, text, NULL, 0);
    return row;
}

void populate_queue_screen(void) {
    if (!queue_list) return;
    int * order = NULL, count, current;
    uint64_t revision;
    if (!gui_player_queue_snapshot(&order, &count, &current, &revision)) return;
    lv_obj_clean(queue_list);
    displayed_revision = revision;
    displayed_current = current;
    /* No in-list "Start playlist" row here -- the header's own "Options"
     * button (build_queue_screen()) already opens this exact same
     * queue_actions popup (Start sequentially/Shuffle/Edit/Clear/Save),
     * so this was a plain duplicate entry point, not the only way in. */
    if (!count) {
        build_list_message(queue_list, "Queue is empty", "Play an album or playlist to see its songs here.");
        free(order); return;
    }
    if (queue_page < 0) queue_page = 0;
    if (queue_page >= count) queue_page = ((count - 1) / QUEUE_PAGE_SIZE) * QUEUE_PAGE_SIZE;
    if (queue_page > 0) {
        lv_obj_t * row = queue_label("Previous page");
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, queue_page_cb, LV_EVENT_CLICKED, (void *) (intptr_t) -QUEUE_PAGE_SIZE);
    }
    int end = queue_page + QUEUE_PAGE_SIZE;
    if (end > count) end = count;
    for (int i = queue_page; i < end; i++) {
        const char * path = gui_player_get_track_path_at(order[i]);
        char title[128], subtitle[256], numbered_title[160];
        song_row_t song;
        char stream_title[128], stream_artist[128], stream_album[128];
        if (gui_player_get_subsonic_track_identity(order[i], path,
                                                   stream_title, sizeof(stream_title),
                                                   stream_artist, sizeof(stream_artist),
                                                   stream_album, sizeof(stream_album))) {
            char folder[128];
            char display_artist[121], display_album[121];
            /* Keep the established URL-derived fallback for incomplete API
             * records while using Subsonic's catalog identity when present. */
            get_display_names(path, title, sizeof(title), folder, sizeof(folder));
            if (stream_title[0]) snprintf(title, sizeof(title), "%s", stream_title);
            if (stream_artist[0] || stream_album[0]) {
                utf8_truncate_safe(display_artist,
                                   stream_artist[0] ? stream_artist : "Unknown artist",
                                   sizeof(display_artist));
                utf8_truncate_safe(display_album,
                                   stream_album[0] ? stream_album : "Unknown album",
                                   sizeof(display_album));
                snprintf(subtitle, sizeof(subtitle), "%s · %s",
                         display_artist, display_album);
            } else subtitle[0] = '\0';
        } else if (metadata_db_get_song_by_path(path, &song)) {
            gui_library_format_song_identity(&song, title, sizeof(title),
                                              subtitle, sizeof(subtitle));
        } else {
            char folder[128];
            get_display_names(path, title, sizeof(title), folder, sizeof(folder));
            subtitle[0] = '\0';
        }
        const char * state = i < current ? "Played" : i == current ? "Playing" :
            i <= current + gui_player_get_queued_count() ? "Queued" : "Next";
        snprintf(numbered_title, sizeof(numbered_title), "%d. %s", i + 1, title);
        if (queue_editing && i > current) {
            const int32_t action_slot = BOARD_SCALE_PX(44);
            const int32_t action_gap = BOARD_SCALE_PX(8);
            const int32_t action_reserve = 3 * action_slot + 2 * action_gap + BOARD_SCALE_PX(12);
            lv_obj_t * row = build_music_list_row(queue_list, numbered_title, subtitle, action_reserve);
            for (int a = 0; a < 3; a++) {
                lv_obj_t * button = lv_obj_create(row);
                lv_obj_set_size(button, action_slot, action_slot);
                lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(button, 0, 0);
                lv_obj_set_style_pad_all(button, 0, 0);
                lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
                lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_style(button, &list_row_pressed_style, LV_STATE_PRESSED);

                lv_obj_t * symbol = lv_label_create(button);
                lv_label_set_text(symbol, a == 0 ? LV_SYMBOL_UP : a == 1 ? LV_SYMBOL_DOWN : LV_SYMBOL_TRASH);
                lv_obj_add_style(symbol, &style_theme_text_primary, 0);
                lv_obj_set_style_text_font(symbol, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
                lv_obj_center(symbol);
                lv_obj_remove_flag(symbol, LV_OBJ_FLAG_CLICKABLE);

                int32_t right_offset = BOARD_SCALE_PX(34 + (2 - a) * 52);
                lv_obj_align(button, LV_ALIGN_RIGHT_MID, -right_offset, 0);
                lv_obj_add_event_cb(button, queue_edit_cb, LV_EVENT_CLICKED, (void *) (intptr_t) (i * 3 + a));
            }
        } else {
            /* Same title/metadata geometry as Favorites/Most Played, with a
             * dedicated trailing column reserved before either label is
             * laid out. The queue state shares the metadata baseline but
             * cannot overlap or be crossed by either marquee. */
            const int32_t state_column_width = queue_state_column_width();
            const int32_t state_column_reserve = state_column_width + GUI_TEXT_INSET + BOARD_SCALE_PX(8);
            lv_obj_t * row = build_music_list_row(queue_list, numbered_title, subtitle, state_column_reserve);
            lv_obj_t * state_label = lv_label_create(row);
            lv_label_set_text(state_label, state);
            lv_obj_add_style(state_label, &style_theme_text_muted, 0);
            lv_obj_set_style_text_font(state_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
            lv_obj_set_width(state_label, state_column_width);
            lv_obj_set_pos(state_label, LIST_ROW_WIDTH - GUI_TEXT_INSET - state_column_width, BOARD_SCALE_PX(64));
            lv_obj_set_style_text_align(state_label, LV_TEXT_ALIGN_RIGHT, 0);
            row_label_apply_bounded_height(state_label, gui_theme_font(GUI_FONT_ROLE_BODY));
            lv_label_set_long_mode(state_label, LV_LABEL_LONG_DOT);
            if (i == current) {
                lv_obj_add_style(state_label, gui_theme_accent_style(), 0);
                lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
                lv_obj_set_style_border_width(row, 4, 0);
                lv_obj_set_style_border_color(row, accent_lv_color(), 0);
            }
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, queue_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        }
    }
    if (end < count) {
        lv_obj_t * row = queue_label("Next page");
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, queue_page_cb, LV_EVENT_CLICKED, (void *) (intptr_t) QUEUE_PAGE_SIZE);
    }
    free(order);
}

static void queue_actions_hide(lv_event_t * e) {
    (void) e;
    lv_obj_add_flag(queue_actions, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(queue_actions_backdrop, LV_OBJ_FLAG_HIDDEN);
}
static void queue_toggle_edit(lv_event_t * e) {
    queue_actions_hide(e); queue_editing = !queue_editing; populate_queue_screen();
}
static void queue_start(lv_event_t * e, bool shuffle) {
    queue_actions_hide(e);
    int * order = NULL, count, current;
    uint64_t revision;
    if (!gui_player_queue_snapshot(&order, &count, &current, &revision)) return;
    free(order);
    if (!count) return;
    unsigned int seed = (unsigned int) time(NULL) ^ lv_tick_get();
    int selected = shuffle ? (int) (rand_r(&seed) % (unsigned int) count) : 0;
    /* Snapshot while the current mode still defines display order, then
     * switch mode. Ranks stay integers; paths that are already resolved
     * keep their existing pointers. */
    if (!gui_player_queue_restart_displayed(selected)) {
        show_error_toast("Cannot start queue");
        return;
    }
    clear_player_source();
    gui_player_set_play_mode(shuffle ? PLAY_MODE_SHUFFLE : PLAY_MODE_SEQUENTIAL);
    play_track_at(selected);
}
static void queue_start_sequential(lv_event_t * e) { queue_start(e, false); }
static void queue_start_shuffle(lv_event_t * e) { queue_start(e, true); }
static void queue_clear_cb(lv_event_t * e) {
    queue_actions_hide(e); gui_player_queue_clear_all(); populate_queue_screen();
}
static void queue_save_done(const char * name, void * data) {
    (void) data;
    if (gui_player_queue_save_as(name)) show_info_toast("Saving playlist…");
    else show_error_toast("Cannot save playlist");
}
static void queue_save_cb(lv_event_t * e) {
    queue_actions_hide(e);
    show_text_entry("Save Queue as Playlist", "", false, false, queue_save_done, NULL);
}
static void queue_actions_open(lv_event_t * e) {
    (void) e;
    lv_obj_remove_flag(queue_actions_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(queue_actions, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(queue_actions_backdrop);
    lv_obj_move_foreground(queue_actions);
}

static lv_obj_t * build_queue_screen(void) {
    lv_obj_t * title;
    lv_obj_t * screen = build_subsonic_list_screen("Queue", &title, &queue_list);
    /* Real stock-firmware icon (sub_back/set.png, 51x51), not a text label --
     * present on every real R1 as-is (THEME_ROOT points straight at the
     * stock firmware's own resource pack on target builds, see assets.c's
     * own comment), and copied into assets/theme2/ here too for host-build
     * parity. build_top_right_icon_button() guarantees this lands at
     * exactly the same visual level as the screen's own back arrow. */
    build_top_right_icon_button(screen, asset_path("sub_back/set.png"), queue_actions_open);
    static const menu_popup_row_t rows[] = {
        { "Start sequentially", queue_start_sequential, false },
        { "Shuffle from a random song", queue_start_shuffle, false },
        { "Edit / Done", queue_toggle_edit, false },
        { "Clear Queue", queue_clear_cb, false },
        { "Save as Playlist", queue_save_cb, false },
        { "Cancel", queue_actions_hide, false, true },
    };
    queue_actions = build_menu_popup(rows, sizeof(rows) / sizeof(rows[0]), queue_actions_hide, &queue_actions_backdrop);
    return screen;
}

void open_queue_screen(void) {
    int * order = NULL, count, current;
    uint64_t revision;
    if (gui_player_queue_snapshot(&order, &count, &current, &revision)) {
        queue_page = current >= 0 ? (current / QUEUE_PAGE_SIZE) * QUEUE_PAGE_SIZE : 0;
        free(order);
    }
    queue_editing = false;
    populate_queue_screen();
    nav_push(queue_screen);
    int row = displayed_current - queue_page + 1 + (queue_page > 0 ? 1 : 0);
    lv_obj_update_layout(queue_list);
    if (row >= 0 && row < (int) lv_obj_get_child_count(queue_list))
        lv_obj_scroll_to_view(lv_obj_get_child(queue_list, row), LV_ANIM_OFF);
}

void gui_queue_poll(void) {
    bool save_done = false, save_ok = false;
    if (gui_player_queue_save_as_poll(&save_done, &save_ok) && save_done) {
        show_info_toast(save_ok ? "Playlist saved" : "Cannot save: invalid or streaming entries");
    }
    static uint64_t saved_revision;
    static uint32_t last_checkpoint;
    uint32_t now = lv_tick_get();
    uint64_t revision = gui_player_queue_revision();
    if (queue_screen && lv_screen_active() == queue_screen && displayed_revision != revision) {
        int32_t scroll = lv_obj_get_scroll_y(queue_list);
        populate_queue_screen();
        lv_obj_scroll_to_y(queue_list, scroll, LV_ANIM_OFF);
    }
    gui_player_queue_poll_urgent();
    if (now - last_checkpoint >= 30000 || (revision != saved_revision && now - last_checkpoint >= 1000)) {
        gui_player_queue_checkpoint();
        last_checkpoint = now; saved_revision = revision;
    }
}

void hide_song_context_menu_popup(void) {
    if (song_context_menu_popup_backdrop) lv_obj_add_flag(song_context_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    if (song_context_menu_popup) lv_obj_add_flag(song_context_menu_popup, LV_OBJ_FLAG_HIDDEN);
}

static void song_context_menu_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
}

static void song_context_menu_add_to_queue_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
    if (song_context_menu_target_path[0] != '\0') gui_player_queue_add(song_context_menu_target_path);
}

static void song_context_menu_play_next_cb(lv_event_t * e) {
    hide_song_context_menu_popup();
    if (song_context_menu_target_path[0]) gui_player_queue_play_next(song_context_menu_target_path);
}

static void song_context_menu_add_to_playlist_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
    if (song_context_menu_target_path[0] != '\0') open_add_to_playlist_for(song_context_menu_target_path);
}

static void song_context_menu_refresh_metadata_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
    if (song_context_menu_target_path[0] == '\0') return;
    const char * paths[] = { song_context_menu_target_path };
    start_library_metadata_refresh_paths(paths, 1);
}

static void song_context_menu_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
}

void open_song_context_menu(const char * path) {
    snprintf(song_context_menu_target_path, sizeof(song_context_menu_target_path), "%s", path ? path : "");
    if (song_context_menu_popup_backdrop) lv_obj_remove_flag(song_context_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    if (song_context_menu_popup) lv_obj_remove_flag(song_context_menu_popup, LV_OBJ_FLAG_HIDDEN);
    if (song_context_menu_popup_backdrop) lv_obj_move_foreground(song_context_menu_popup_backdrop);
    if (song_context_menu_popup) lv_obj_move_foreground(song_context_menu_popup);
}

static void build_song_context_menu_popup(void) {
    static const menu_popup_row_t rows[] = {
        { "Play Next", song_context_menu_play_next_cb, false },
        { "Add to Queue", song_context_menu_add_to_queue_cb, false },
        { "Add to Playlist", song_context_menu_add_to_playlist_cb, false },
        { "Refresh metadata", song_context_menu_refresh_metadata_cb, false },
        { "Cancel", song_context_menu_cancel_cb, false, true },
    };
    song_context_menu_popup = build_menu_popup(rows, (int) (sizeof(rows) / sizeof(rows[0])),
                                                song_context_menu_popup_backdrop_cb,
                                                &song_context_menu_popup_backdrop);
}

void gui_queue_init(void) {
    queue_screen = build_queue_screen();
    build_song_context_menu_popup();
}

/* For gui_reload.c's in-process UI reload -- deletes every screen/popup this
 * module owns so gui_queue_init() can rebuild them from a clean slate
 * without leaking the old objects. song_context_menu_popup/backdrop are
 * built via build_menu_popup() directly on lv_layer_top() (same shape as
 * build_confirm_popup(), see its own comment), not as children of
 * queue_screen, so they need their own explicit deletion. */
void gui_queue_teardown(void) {
    if (queue_actions) { lv_obj_delete(queue_actions); queue_actions = NULL; }
    if (queue_actions_backdrop) { lv_obj_delete(queue_actions_backdrop); queue_actions_backdrop = NULL; }
    if (song_context_menu_popup) { lv_obj_delete(song_context_menu_popup); song_context_menu_popup = NULL; }
    if (song_context_menu_popup_backdrop) { lv_obj_delete(song_context_menu_popup_backdrop); song_context_menu_popup_backdrop = NULL; }
    if (queue_screen) { lv_obj_delete(queue_screen); queue_screen = NULL; }
    queue_list = NULL;
}
