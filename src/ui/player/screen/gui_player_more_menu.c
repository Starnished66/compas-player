#include "gui_player_internal.h"
#include "gui_player.h"
#include "gui_queue.h"
#include "../../application/gui.h"
#include "../../appearance/gui_theme.h"
#include "../../settings/gui_settings.h"
#include "../../shell/gui_notifications.h"
#include "../../text/i18n/i18n.h"
#include "../../widgets/builders/screen_builders.h"
#include "../queue/player_queue_model.h"
#include "../transport/player_volume_popup.h"
#include "../../../audio/engine/audio.h"
#include "../../../library/remote/remote_track.h"
#include "../../../plugins/manager/plugin_manager.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


typedef struct {
    uint32_t registry_generation;
    int index;
} player_plugin_menu_callback_t;

static player_plugin_menu_callback_t player_plugin_menu_callbacks[PLUGIN_MAX_PLAYER_LIST_ITEMS];

static gui_popup_t delete_song_popup;
static lv_obj_t * delete_song_popup_title = NULL;

/* ---- Delete confirmation popup ---- */

static void hide_delete_song_popup(void) {
    gui_popup_hide(&delete_song_popup);
}

static void delete_song_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_delete_song_popup();
}

static void delete_song_confirm_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_delete_song_popup();
    int current = player_queue_model_current();
    if (current < 0 || current >= player_queue_model_count()) return;
    char *to_delete = strdup(player_queue_model_path_at(current));
    int next_index = -1;
    if (!player_queue_model_delete_current(&next_index)) { free(to_delete); return; }
    if (next_index < 0) {
        audio_stop();
        plugin_manager_notify_stopped();
        player_queue_model_finish_empty_delete();
        clear_player_source();
        player_show_empty_queue();
        nav_pop();
    } else {
        play_track_at(next_index);
    }
    unlink(to_delete);
    free(to_delete);
    show_info_toast(TR("Song deleted"));
}

/* Defined alongside build_confirm_popup() above -- see its own doc comment
 * for why this is a separate shared shape (an N-row menu, not a yes/no
 * confirmation) and why both are forward-declared here. */
void build_delete_song_popup(void) {
    /* LV_LABEL_LONG_DOT, not WRAP -- this title's text is set later
     * (delete_song_confirm_prompt() below) to "Delete <filename>?..." with
     * an arbitrary-length real filename spliced in, so it needs to
     * truncate rather than potentially wrap across several lines. */
    delete_song_popup.popup = build_confirm_popup("", LV_LABEL_LONG_DOT, &delete_song_popup_title, NULL, TR("Delete"),
                                             lv_color_hex(GUI_BREEZE_NEGATIVE), delete_song_confirm_cb, NULL, TR("Cancel"),
                                             accent_lv_color(), delete_song_cancel_cb, NULL,
                                             delete_song_cancel_cb, &delete_song_popup.backdrop);
}

/* ---- The "more" 3-row menu itself ---- */

void hide_more_menu_popup(void) {
    hide_song_context_menu_popup();
}

bool gui_player_boot_prompt_blocked(void) {
    return (volume_popup && !lv_obj_has_flag(volume_popup, LV_OBJ_FLAG_HIDDEN)) ||
           gui_queue_song_context_menu_visible();
}

static void more_menu_queue_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    open_queue_screen();
}

static void more_menu_plugin_item_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    player_plugin_menu_callback_t * callback = lv_event_get_user_data(e);
    if (!callback || callback->registry_generation != plugin_manager_get_list_registry_generation()) return;
    plugin_manager_player_list_item_clicked(callback->index);
}

static void more_menu_eq_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    nav_push(gui_settings_get_eq_screen());
}

static void more_menu_delete_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    if (!gui_player_has_active_track()) return;

    lv_label_set_text_fmt(delete_song_popup_title, TR("Delete %s?\nThis cannot be undone."), basename_of(gui_player_get_current_track_path()));
    gui_popup_show(&delete_song_popup);
}

/* Appends one row per plugin player-list item; returns the new row count. */
static int more_menu_add_plugin_rows(menu_popup_row_t * rows, int row_count) {
    int plugin_count = plugin_manager_get_player_list_item_count();
    if (plugin_count > PLUGIN_MAX_PLAYER_LIST_ITEMS) plugin_count = PLUGIN_MAX_PLAYER_LIST_ITEMS;
    uint32_t generation = plugin_manager_get_list_registry_generation();
    for (int i = 0; i < plugin_count; i++) {
        player_plugin_menu_callbacks[i].registry_generation = generation;
        player_plugin_menu_callbacks[i].index = i;
        rows[row_count++] = (menu_popup_row_t) {
            plugin_manager_get_player_list_item_label(i), more_menu_plugin_item_cb, false,
            false, &player_plugin_menu_callbacks[i]
        };
    }
    return row_count;
}

void more_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (player_queue_model_current() < 0) return;

    menu_popup_row_t rows[PLUGIN_MAX_PLAYER_LIST_ITEMS + 3];
    int row_count = 0;
    rows[row_count++] = (menu_popup_row_t) { TR("Queue"), more_menu_queue_cb, false };
    row_count = more_menu_add_plugin_rows(rows, row_count);
    rows[row_count++] = (menu_popup_row_t) { TR("Equalizer"), more_menu_eq_cb, false };
    const char * path = gui_player_get_current_track_path();
    if (path && !is_http_url(path) && !remote_track_path_is_remote(path)) /* only files can be deleted */
        rows[row_count++] = (menu_popup_row_t) { TR("Delete"), more_menu_delete_cb, true };
    hide_more_menu_popup();
    /* The shared song menu keeps rating and library navigation consistent
     * with the menus opened from library rows. */
    open_song_context_menu_with_rows(path,
        SONG_MENU_PLAYLIST | SONG_MENU_RATING | SONG_MENU_NAVIGATION |
        SONG_MENU_INFORMATION, rows, row_count);
}

void player_more_menu_teardown(void) {
    gui_popup_teardown(&delete_song_popup);
    hide_song_context_menu_popup();
}

bool player_delete_song_popup_is_visible(void) {
    return gui_popup_is_visible(&delete_song_popup);
}

bool player_more_menu_is_visible(void) {
    return gui_queue_song_context_menu_visible();
}
