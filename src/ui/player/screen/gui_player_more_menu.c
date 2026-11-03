#include "gui_player_internal.h"
#include "gui_player.h"
#include "gui_track_info.h"
#include "../../application/gui.h"
#include "../../appearance/gui_theme.h"
#include "../../settings/gui_settings.h"
#include "../../shell/gui_notifications.h"
#include "../../text/i18n/i18n.h"
#include "../../widgets/builders/screen_builders.h"
#include "../queue/player_queue_model.h"
#include "../transport/player_volume_popup.h"
#include "../../../audio/engine/audio.h"
#include "../../../plugins/manager/plugin_manager.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static lv_obj_t * more_menu_popup = NULL;
static lv_obj_t * more_menu_popup_backdrop = NULL;

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
    show_error_toast(TR("Song deleted"));
}

/* Defined alongside build_confirm_popup() above -- see its own doc comment
 * for why this is a separate shared shape (an N-row menu, not a yes/no
 * confirmation) and why both are forward-declared here. */
/* menu_popup_row_t and build_menu_popup declared in gui.h */

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
    lv_obj_add_flag(more_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(more_menu_popup, LV_OBJ_FLAG_HIDDEN);
}

bool gui_player_boot_prompt_blocked(void) {
    return (volume_popup && !lv_obj_has_flag(volume_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (more_menu_popup && !lv_obj_has_flag(more_menu_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (more_menu_popup_backdrop && !lv_obj_has_flag(more_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN));
}

static void more_menu_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
}

static void more_menu_add_to_playlist_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    if (player_queue_model_current() < 0) return;
    open_add_to_playlist_for(gui_player_get_current_track_path());
}

static void more_menu_queue_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    open_queue_screen();
}

static void more_menu_information_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    gui_track_info_open();
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

void build_more_menu_popup(void);

void more_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (player_queue_model_current() < 0) return;

    /* Plugin availability can change after a reload. Rebuild the popup from
     * the current registry each time it opens. */
    build_more_menu_popup();
    lv_obj_remove_flag(more_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(more_menu_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(more_menu_popup_backdrop);
    lv_obj_move_foreground(more_menu_popup);
}

void build_more_menu_popup(void) {
    if (more_menu_popup) { lv_obj_delete(more_menu_popup); more_menu_popup = NULL; }
    if (more_menu_popup_backdrop) { lv_obj_delete(more_menu_popup_backdrop); more_menu_popup_backdrop = NULL; }

    menu_popup_row_t rows[6 + PLUGIN_MAX_PLAYER_LIST_ITEMS];
    int row_count = 0;
    rows[row_count++] = (menu_popup_row_t) { TR("Queue"), more_menu_queue_cb, false };
    rows[row_count++] = (menu_popup_row_t) { TR("Add to Playlist"), more_menu_add_to_playlist_cb, false };
    rows[row_count++] = (menu_popup_row_t) { TR("Information"), more_menu_information_cb, false };

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

    rows[row_count++] = (menu_popup_row_t) { TR("EQ"), more_menu_eq_cb, false };
    rows[row_count++] = (menu_popup_row_t) { TR("Delete"), more_menu_delete_cb, true };
    rows[row_count++] = (menu_popup_row_t) { TR("Cancel"), more_menu_popup_backdrop_cb, false, true };
    more_menu_popup = build_menu_popup(rows, row_count, more_menu_popup_backdrop_cb, &more_menu_popup_backdrop);
}

void player_more_menu_teardown(void) {
    gui_popup_teardown(&delete_song_popup);
    if (more_menu_popup) { lv_obj_delete(more_menu_popup); more_menu_popup = NULL; }
    if (more_menu_popup_backdrop) { lv_obj_delete(more_menu_popup_backdrop); more_menu_popup_backdrop = NULL; }
}

bool player_delete_song_popup_is_visible(void) {
    return gui_popup_is_visible(&delete_song_popup);
}

bool player_more_menu_is_visible(void) {
    return (more_menu_popup && !lv_obj_has_flag(more_menu_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (more_menu_popup_backdrop && !lv_obj_has_flag(more_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN));
}
