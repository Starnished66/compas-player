#include "gui_queue.h"
#include "../../text/i18n/i18n.h"
#include "../../application/gui.h"
#include "gui_player.h"
#include "gui_player_internal.h" /* is_http_url */
#include "gui_track_info.h"
#include "../../appearance/gui_theme.h"
#include "../../shell/gui_notifications.h"
#include "../../library/browse/gui_library.h"
#include "../../library/browse/gui_list_reorder.h"
#include "../../text/rendering/gui_text_input.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../../library/metadata/parsing/metadata.h"
#include "../../../library/parsers/cue/cue_parser.h"
#include "../../../library/browser/paths/library_paths.h"
#include "../../../library/remote/remote_track.h"
#include "../../assets/assets.h"
#include "../../text/rendering/fallback_font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static lv_obj_t * queue_screen = NULL;
static lv_obj_t * queue_list = NULL;

static lv_obj_t * song_context_menu_popup = NULL;
static lv_obj_t * song_context_menu_popup_backdrop = NULL;
static char song_context_menu_target_path[600] = "";
static unsigned int song_context_menu_capabilities;
static unsigned int song_context_built_capabilities;
static bool song_context_built_extras;
static void (*song_context_queue_cb)(bool next);
static const menu_popup_row_t * song_context_menu_extra_rows;
static int song_context_menu_extra_row_count;
static void build_song_context_menu_popup(void);

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
static gui_popup_t queue_clear_popup;
static lv_obj_t * queue_actions, * queue_actions_backdrop;
static void queue_actions_open(lv_event_t * e);
static lv_obj_t * queue_current_row;
static lv_obj_t * queue_current_heading;

/* Slot = display index - queue_page. Only future songs are draggable. */
static lv_obj_t * queue_rows[QUEUE_PAGE_SIZE];
static lv_obj_t * queue_handles[QUEUE_PAGE_SIZE];
static gui_list_reorder_state_t queue_reorder_state = { .drag_id = -1 };
static int queue_first_draggable_slot = -1;
static int queue_drag_source_slot = -1;

static int32_t queue_reorder_handle_width(void) {
    int32_t width = BOARD_SCALE_PX(44);
    return width < 48 ? 48 : width;
}

static void queue_unregister_handles(void) {
    for (int i = 0; i < QUEUE_PAGE_SIZE; ++i) {
        if (queue_handles[i]) {
            unregister_swipe_dead_zone(queue_handles[i]);
            queue_handles[i] = NULL;
        }
        queue_rows[i] = NULL;
    }
}

static void queue_finish_drag(void) {
    if (queue_reorder_state.active) gui_list_reorder_finish(&queue_reorder_state, false);
}

static void queue_repopulate_keep_scroll(void) {
    int32_t scroll = lv_obj_get_scroll_y(queue_list);
    populate_queue_screen();
    lv_obj_scroll_to_y(queue_list, scroll, LV_ANIM_OFF);
}

static void queue_reorder_prepare(gui_list_reorder_state_t * state, int id) {
    state->count = 0;
    queue_first_draggable_slot = -1;
    queue_drag_source_slot = -1;
    if (!queue_editing || gui_player_queue_revision() != displayed_revision ||
        id < 0 || id >= QUEUE_PAGE_SIZE) {
        show_info_toast(TR("Queue changed. Try again."));
        return;
    }
    int n = 0, first = -1;
    for (int i = 0; i < QUEUE_PAGE_SIZE; ++i) {
        if (!queue_handles[i]) continue;
        if (first < 0) first = i;
        state->ids[n++] = i;
    }
    if (n < 2) return;
    queue_first_draggable_slot = first;
    queue_drag_source_slot = id;
    state->count = n;
}

static void queue_reorder_restore(gui_list_reorder_state_t * state) {
    (void) state;
    queue_drag_source_slot = -1;
    if (queue_list) queue_repopulate_keep_scroll();
}

static void queue_reorder_commit(gui_list_reorder_state_t * state) {
    int source = queue_drag_source_slot, destination = -1;
    queue_drag_source_slot = -1;
    for (int i = 0; source >= 0 && i < state->drag_count; ++i) {
        if (state->ids[i] == source) { destination = i; break; }
    }
    if (source >= 0 && destination >= 0 && queue_first_draggable_slot >= 0) {
        int from = queue_page + source;
        int to = queue_page + queue_first_draggable_slot + destination;
        if (from != to && !gui_player_queue_edit(displayed_revision, from, to))
            show_error_toast(TR("Cannot move this entry"));
    }
    queue_repopulate_keep_scroll();
}

static void queue_reorder_handle_cb(lv_event_t * e) {
    gui_list_reorder_event(&queue_reorder_state, (int) (intptr_t) lv_event_get_user_data(e), e);
}

static void queue_screen_unload_cb(lv_event_t * e) {
    (void) e;
    queue_finish_drag();
    gui_popup_hide(&queue_clear_popup);
}

bool gui_queue_boot_prompt_blocked(void) {
    return gui_popup_is_visible(&queue_clear_popup) ||
           (queue_actions && !lv_obj_has_flag(queue_actions, LV_OBJ_FLAG_HIDDEN)) ||
           (queue_actions_backdrop && !lv_obj_has_flag(queue_actions_backdrop, LV_OBJ_FLAG_HIDDEN)) ||
           (song_context_menu_popup && !lv_obj_has_flag(song_context_menu_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (song_context_menu_popup_backdrop &&
            !lv_obj_has_flag(song_context_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN));
}

bool gui_queue_song_context_menu_visible(void) {
    return (song_context_menu_popup && !lv_obj_has_flag(song_context_menu_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (song_context_menu_popup_backdrop && !lv_obj_has_flag(song_context_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN));
}

static void queue_row_click_cb(lv_event_t * e) {
    if (!gui_player_queue_select(displayed_revision, (int) (intptr_t) lv_event_get_user_data(e)))
        show_error_toast(TR("Queue changed. Try again."));
}

/* The row owns a copy of its path for the song menu, freed with the row. */
static void queue_row_long_press_cb(lv_event_t * e) {
    const char * path = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_DELETE) lv_free((void *) path);
    else if (path && path[0]) open_song_context_menu(path);
}

static void queue_edit_cb(lv_event_t * e) {
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    queue_finish_drag();
    if (!gui_player_queue_edit(displayed_revision, index, -1))
        show_error_toast(TR("Cannot move this entry"));
    populate_queue_screen();
}

static void queue_page_cb(lv_event_t * e) {
    queue_finish_drag();
    queue_page += (int) (intptr_t) lv_event_get_user_data(e);
    populate_queue_screen();
    lv_obj_scroll_to_y(queue_list, 0, LV_ANIM_OFF);
}

static lv_obj_t * queue_label(const char * text) {
    lv_obj_t * row = build_music_list_row(queue_list, text, NULL, 0);
    return row;
}

static void queue_cue_row(const char *path, cue_sheet_t *cache, char *cache_src, char *title, char *subtitle) {
    unsigned ordinal = 0;
    char source[PATH_MAX];
    bool resolved = cue_reference_parse(path, source, sizeof(source), &ordinal);
    if (resolved && strcmp(source, cache_src) != 0) {
        cue_sheet_free(cache);
        cache_src[0] = '\0';
        resolved = cue_sheet_load(source, cache);
        if (resolved) snprintf(cache_src, PATH_MAX, "%s", source);
    }
    if (resolved && ordinal > 0 && ordinal <= (unsigned) cache->track_count) {
        const cue_track_t * track = &cache->tracks[ordinal - 1];
        if (track->title[0]) snprintf(title, 128, "%s", track->title);
        else snprintf(title, 128, TR("Track %d"), track->number);
        const char * performer = track->performer[0] ? track->performer : cache->performer;
        if (performer[0] || cache->title[0]) {
            char display_artist[121], display_album[121];
            utf8_truncate_safe(display_artist, performer[0] ? performer : TR("Unknown artist"), sizeof(display_artist));
            utf8_truncate_safe(display_album, cache->title[0] ? cache->title : TR("Unknown album"), sizeof(display_album));
            snprintf(subtitle, 256, "%s · %s", display_artist, display_album);
        } else subtitle[0] = '\0';
    } else {
        char folder[128];
        get_display_names(path, title, 128, folder, sizeof(folder));
        subtitle[0] = '\0';
    }
}

void populate_queue_screen(void) {
    if (!queue_list) return;
    int * order = NULL, count, current;
    uint64_t revision;
    if (!gui_player_queue_snapshot(&order, &count, &current, &revision)) return;
    queue_unregister_handles();
    queue_reorder_state.list = queue_list;
    queue_reorder_state.rows = queue_rows;
    queue_reorder_state.row_capacity = QUEUE_PAGE_SIZE;
    queue_reorder_state.prepare = queue_reorder_prepare;
    queue_reorder_state.commit = queue_reorder_commit;
    queue_reorder_state.restore = queue_reorder_restore;
    lv_obj_clean(queue_list);
    displayed_revision = revision;
    queue_current_row = NULL;
    queue_current_heading = NULL;
    /* No in-list "Start playlist" row here -- the header's own "Options"
     * button (build_queue_screen()) already opens this exact same
     * queue_actions popup (Start sequentially/Shuffle/Edit/Clear/Save),
     * so this was a plain duplicate entry point, not the only way in. */
    if (!count) {
        build_list_message(queue_list, TR("Queue is empty"), TR("Play an album or playlist to see its songs here."));
        free(order); return;
    }
    if (queue_page < 0) queue_page = 0;
    if (queue_page >= count) queue_page = ((count - 1) / QUEUE_PAGE_SIZE) * QUEUE_PAGE_SIZE;
    if (queue_page > 0) {
        lv_obj_t * row = queue_label(TR("Previous page"));
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, queue_page_cb, LV_EVENT_CLICKED, (void *) (intptr_t) -QUEUE_PAGE_SIZE);
    }
    cue_sheet_t cue_cache = {0};
    char cue_cache_source[PATH_MAX] = "";
    int end = queue_page + QUEUE_PAGE_SIZE;
    if (end > count) end = count;
    int pending = gui_player_get_queued_count();
    if (pending < 0) pending = 0;
    if (pending > count - current - 1) pending = count - current - 1;
    int previous_section = -1;
    for (int i = queue_page; i < end; i++) {
        int section = i < current ? 0 : i == current ? 1 :
                      i - current <= pending ? 2 : 3;
        if (section != previous_section) {
            const char * heading;
            char queued_heading[160];
            if (section == 0) heading = TR("Played");
            else if (section == 1) heading = TR("Now Playing");
            else if (section == 2) {
                snprintf(queued_heading, sizeof(queued_heading), "%s · %d", TR("In queue"), pending);
                heading = queued_heading;
            } else heading = TR("Continue Playing");
            lv_obj_t * header = add_wrapped_section_header(queue_list, heading);
            lv_obj_set_width(header, LIST_ROW_WIDTH);
            lv_obj_set_style_pad_left(header, GUI_TEXT_INSET, 0);
            lv_obj_set_style_pad_right(header, GUI_TEXT_INSET, 0);
            if (section == 1) queue_current_heading = header;
            previous_section = section;
        }
        const char * path = gui_player_get_track_path_at(order[i]);
        char title[128], subtitle[256], numbered_title[160];
        song_row_t song;
        char stream_title[128], stream_artist[128], stream_album[128];
        static remote_track_meta_t remote;
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
                                   stream_artist[0] ? stream_artist : TR("Unknown artist"),
                                   sizeof(display_artist));
                utf8_truncate_safe(display_album,
                                   stream_album[0] ? stream_album : TR("Unknown album"),
                                   sizeof(display_album));
                snprintf(subtitle, sizeof(subtitle), "%s · %s",
                         display_artist, display_album);
            } else subtitle[0] = '\0';
        } else if (remote_track_path_is_remote(path) &&
                   remote_track_meta_copy_for_path(path, &remote)) {
            /* Streaming plugins pass the catalog identity with the queue;
             * the synthetic remote:// key would only show the track id. */
            if (remote.title[0]) snprintf(title, sizeof(title), "%s", remote.title);
            else {
                char folder[128];
                get_display_names(path, title, sizeof(title), folder, sizeof(folder));
            }
            if (remote.artist[0] || remote.album[0]) {
                char display_artist[121], display_album[121];
                utf8_truncate_safe(display_artist, remote.artist[0] ? remote.artist : TR("Unknown artist"),
                                   sizeof(display_artist));
                utf8_truncate_safe(display_album, remote.album[0] ? remote.album : TR("Unknown album"),
                                   sizeof(display_album));
                snprintf(subtitle, sizeof(subtitle), "%s · %s", display_artist, display_album);
            } else subtitle[0] = '\0';
        } else if (cue_reference_is(path)) {
            queue_cue_row(path, &cue_cache, cue_cache_source, title, subtitle);
        } else if (metadata_db_get_song_by_path(path, &song)) {
            gui_library_format_song_identity(&song, title, sizeof(title),
                                              subtitle, sizeof(subtitle));
        } else {
            char folder[128];
            get_display_names(path, title, sizeof(title), folder, sizeof(folder));
            subtitle[0] = '\0';
        }
        snprintf(numbered_title, sizeof(numbered_title), "%d. %s", i + 1, title);
        if (queue_editing && i > current) {
            /* Keep the destructive target close to 40 px on the compact
             * V1; retain a little extra hit area without stealing the drag
             * handle's gesture region. */
#if defined(BOARD_TEMPOTEC_V1)
            const int32_t action_slot = BOARD_SCALE_PX(60);
#else
            const int32_t action_slot = BOARD_SCALE_PX(44);
#endif
            const int32_t action_reserve = BOARD_SCALE_PX(34) + action_slot + BOARD_SCALE_PX(8);
            lv_obj_t * row = build_music_list_row(queue_list, numbered_title, subtitle, action_reserve);
            int slot = i - queue_page;
            queue_rows[slot] = row;
            /* Drag handle on the left; the labels shift right by its width.
             * Configured style values are used since new rows have no
             * computed coordinates yet. */
            const int32_t handle_width = queue_reorder_handle_width();
            for (int c = 0; c < 2; c++) {
                lv_obj_t * label = lv_obj_get_child(row, c);
                int32_t width = lv_obj_get_style_width(label, 0) - handle_width;
                if (width < 40) width = 40;
                lv_obj_set_x(label, lv_obj_get_style_x(label, 0) + handle_width);
                lv_obj_set_width(label, width);
                /* Keep the edit-row text static beside the drag handle. */
                lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            }
            lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_USER_1);
            lv_obj_t * handle = lv_button_create(row);
            queue_handles[slot] = handle;
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
            lv_obj_add_event_cb(handle, queue_reorder_handle_cb, LV_EVENT_ALL, (void *) (intptr_t) slot);
            register_swipe_dead_zone(handle);

            lv_obj_t * button = lv_obj_create(row);
            lv_obj_set_size(button, action_slot, action_slot);
            lv_obj_set_ext_click_area(button, BOARD_SCALE_PX(6));
            lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(button, 0, 0);
            lv_obj_set_style_pad_all(button, 0, 0);
            lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_style(button, &list_row_pressed_style, LV_STATE_PRESSED);

            lv_obj_t * symbol = lv_label_create(button);
            lv_label_set_text(symbol, LV_SYMBOL_TRASH);
            lv_obj_add_style(symbol, &style_theme_text_primary, 0);
            lv_obj_set_style_text_font(symbol, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
            lv_obj_center(symbol);
            lv_obj_remove_flag(symbol, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_align(button, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(34), 0);
            lv_obj_add_event_cb(button, queue_edit_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        } else {
            lv_obj_t * row = build_music_list_row(queue_list, numbered_title, subtitle, 0);
            if (i == current) {
                queue_current_row = row;
                lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
                lv_obj_set_style_border_width(row, 4, 0);
                lv_obj_set_style_border_color(row, accent_lv_color(), 0);
            }
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            char * row_path = lv_strdup(path ? path : "");
            lv_obj_add_event_cb(row, queue_row_click_cb, LV_EVENT_SHORT_CLICKED, (void *) (intptr_t) i);
            lv_obj_add_event_cb(row, queue_row_long_press_cb, LV_EVENT_LONG_PRESSED, row_path);
            lv_obj_add_event_cb(row, queue_row_long_press_cb, LV_EVENT_DELETE, row_path);
        }
    }
    if (end < count) {
        lv_obj_t * row = queue_label(TR("Next page"));
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, queue_page_cb, LV_EVENT_CLICKED, (void *) (intptr_t) QUEUE_PAGE_SIZE);
    }
    cue_sheet_free(&cue_cache);
    free(order);
}

static void queue_actions_hide(lv_event_t * e) {
    (void) e;
    if (queue_actions_backdrop) gui_popup_backdrop_hide(queue_actions_backdrop);
}
static void queue_toggle_edit(lv_event_t * e) {
    queue_actions_hide(e); queue_finish_drag();
    queue_editing = !queue_editing; populate_queue_screen();
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
        show_error_toast(TR("Cannot start queue"));
        return;
    }
    clear_player_source();
    gui_player_set_play_mode(shuffle ? PLAY_MODE_SHUFFLE : PLAY_MODE_SEQUENTIAL);
    play_track_at(selected);
}
static void queue_start_sequential(lv_event_t * e) { queue_start(e, false); }
static void queue_start_shuffle(lv_event_t * e) { queue_start(e, true); }
static void queue_clear_confirm_cb(lv_event_t * e) {
    (void) e;
    gui_popup_hide(&queue_clear_popup);
    queue_finish_drag();
    gui_player_queue_clear_all();
    populate_queue_screen();
}
static void queue_clear_cb(lv_event_t * e) {
    queue_actions_hide(e);
    gui_popup_confirm(&queue_clear_popup, TR("Clear the queue?"), TR("The playing song keeps playing."),
                      TR("Clear queue"), queue_clear_confirm_cb);
}
static void queue_save_done(const char * name, void * data) {
    (void) data;
    if (gui_player_queue_save_as(name)) show_info_toast(TR("Saving playlist…"));
    else show_error_toast(TR("Cannot save playlist"));
}
static void queue_save_cb(lv_event_t * e) {
    queue_actions_hide(e);
    show_text_entry(TR("Save queue as playlist"), "", false, false, queue_save_done, NULL);
}
/* Fixed callbacks keep their rows; refresh labels and reopen at the top. */
static void refresh_menu_labels(lv_obj_t * popup, const menu_popup_row_t * rows, int count) {
    for (int i = 0; i < count; ++i) {
        lv_obj_t * row = build_menu_popup_row(popup, (uint32_t)i);
        lv_obj_t * label = lv_obj_get_child(row, 0);
        if (strcmp(lv_label_get_text(label), rows[i].label) != 0)
            lv_label_set_text(label, rows[i].label);
    }
    lv_obj_t * first = build_menu_popup_row(popup, 0);
    if (first) lv_obj_scroll_to_y(lv_obj_get_parent(first), 0, LV_ANIM_OFF);
}

static void queue_actions_open(lv_event_t * e) {
    (void) e;
    const menu_popup_row_t rows[] = {
        { TR("Start sequentially"), queue_start_sequential, false },
        { TR("Shuffle from a random song"), queue_start_shuffle, false },
        { queue_editing ? TR("Done") : TR("Edit"), queue_toggle_edit, false },
        { TR("Clear queue"), queue_clear_cb, false },
        { TR("Save as playlist"), queue_save_cb, false },
        { TR("Cancel"), queue_actions_hide, false, true },
    };
    if (!queue_actions)
        queue_actions = build_menu_popup(rows, sizeof(rows) / sizeof(rows[0]), queue_actions_hide, &queue_actions_backdrop);
    else
        refresh_menu_labels(queue_actions, rows, sizeof(rows) / sizeof(rows[0]));
    gui_popup_backdrop_show(queue_actions_backdrop);
    lv_obj_remove_flag(queue_actions, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t * build_queue_screen(void) {
    lv_obj_t * title;
    lv_obj_t * screen = build_subsonic_list_screen(TR("Queue"), &title, &queue_list);
    build_top_right_icon_button(screen, asset_path("settings/playback.png"), queue_actions_open);
    lv_obj_add_event_cb(screen, queue_screen_unload_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
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
    lv_obj_update_layout(queue_list);
    if (queue_current_row) {
        lv_obj_scroll_to_view(queue_current_row, LV_ANIM_OFF);
        lv_obj_scroll_to_view(queue_current_heading, LV_ANIM_OFF);
    }
}

void gui_queue_poll(void) {
    bool save_done = false, save_ok = false;
    if (gui_player_queue_save_as_poll(&save_done, &save_ok) && save_done) {
        show_info_toast(save_ok ? TR("Playlist saved") : TR("Cannot save: invalid or streaming entries"));
    }
    static uint64_t saved_revision;
    static uint32_t last_checkpoint;
    uint32_t now = lv_tick_get();
    uint64_t revision = gui_player_queue_revision();
    if (queue_screen && lv_screen_active() == queue_screen && displayed_revision != revision &&
        !queue_reorder_state.active)
        queue_repopulate_keep_scroll();
    gui_player_queue_poll_urgent();
    if (now - last_checkpoint >= 30000 || (revision != saved_revision && now - last_checkpoint >= 1000)) {
        gui_player_queue_checkpoint();
        last_checkpoint = now; saved_revision = revision;
    }
}

void hide_song_context_menu_popup(void) {
    if (song_context_menu_popup_backdrop) gui_popup_backdrop_hide(song_context_menu_popup_backdrop);
}

static void song_context_menu_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
}

static void song_context_menu_add_to_queue_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
    if (song_context_queue_cb) song_context_queue_cb(false);
    else if (song_context_menu_target_path[0]) gui_player_queue_add(song_context_menu_target_path);
}

static void song_context_menu_play_next_cb(lv_event_t * e) {
    hide_song_context_menu_popup();
    if (song_context_queue_cb) song_context_queue_cb(true);
    else if (song_context_menu_target_path[0]) gui_player_queue_play_next(song_context_menu_target_path);
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

static void song_context_menu_rating_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !song_context_menu_target_path[0]) return;
    hide_song_context_menu_popup();
    gui_player_open_rating_popup(song_context_menu_target_path);
}

static void song_context_menu_album_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
    if (song_context_menu_target_path[0] &&
        !gui_library_open_now_playing_group(song_context_menu_target_path, true))
        show_info_toast(TR("Album unavailable"));
}

static void song_context_menu_artist_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
    if (song_context_menu_target_path[0] &&
        !gui_library_open_now_playing_group(song_context_menu_target_path, false))
        show_info_toast(TR("Artist unavailable"));
}

static void song_context_menu_information_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
    const char * current = gui_player_has_active_track() ? gui_player_get_current_track_path() : NULL;
    if (current && strcmp(current, song_context_menu_target_path) == 0) {
        gui_track_info_open();
        return;
    }
    if (is_http_url(song_context_menu_target_path) || remote_track_path_is_remote(song_context_menu_target_path)) return;
    gui_track_info_context_t context = { .source = GUI_TRACK_SOURCE_LOCAL };
    snprintf(context.path, sizeof(context.path), "%s", song_context_menu_target_path);
    gui_track_info_open_for_context(&context);
}

static void song_context_menu_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_song_context_menu_popup();
}

void open_song_context_menu(const char * path) {
    open_song_context_menu_for(path, SONG_MENU_ALL);
}

void open_song_context_menu_for(const char * path, unsigned int capabilities) {
    open_song_context_menu_with_rows(path, capabilities, NULL, 0);
}

void open_song_context_menu_stream(const char * path, void (*queue_cb)(bool next)) {
    open_song_context_menu_for(path, SONG_MENU_PLAYBACK | SONG_MENU_NAVIGATION);
    song_context_queue_cb = queue_cb;
}

static unsigned int song_menu_supported_capabilities(const char * path) {
    if (!path || is_http_url(path) || remote_track_path_is_remote(path)) {
        const char * current = gui_player_has_active_track() ? gui_player_get_current_track_path() : NULL;
        unsigned navigation = gui_subsonic_song_for_path(path) ? SONG_MENU_NAVIGATION : 0;
        return navigation | (current && strcmp(current, path ? path : "") == 0
            ? SONG_MENU_PLAYBACK | SONG_MENU_INFORMATION : SONG_MENU_PLAYBACK);
    }
    char cue_source[600];
    unsigned cue_ordinal;
    if (cue_reference_parse(path, cue_source, sizeof(cue_source), &cue_ordinal))
        return SONG_MENU_ALL & ~SONG_MENU_NAVIGATION;
    /* A local path outside the mounted music root cannot be a tagcache row.
     * Keep this a lexical check so opening a menu never waits on the DB lock. */
    size_t path_len = strlen(path);
    size_t root_len = strlen(MUSIC_ROOT_DIR);
    if (path_len < root_len) return SONG_MENU_ALL & ~SONG_MENU_NAVIGATION;
    if (strncmp(path, MUSIC_ROOT_DIR, root_len) != 0 ||
        (path[root_len] != '\0' && path[root_len] != '/' && path[root_len] != '#'))
        return SONG_MENU_ALL & ~SONG_MENU_NAVIGATION;
    return SONG_MENU_ALL;
}

void open_song_context_menu_with_rows(const char * path, unsigned int capabilities,
                                      const menu_popup_row_t * extra_rows, int extra_row_count) {
    snprintf(song_context_menu_target_path, sizeof(song_context_menu_target_path), "%s", path ? path : "");
    song_context_menu_capabilities = capabilities & song_menu_supported_capabilities(path);
    song_context_queue_cb = NULL;
    song_context_menu_extra_rows = extra_rows;
    song_context_menu_extra_row_count = extra_row_count > 0 ? extra_row_count : 0;
    build_song_context_menu_popup();
    song_context_menu_extra_rows = NULL;
    song_context_menu_extra_row_count = 0;
    if (song_context_menu_popup_backdrop) gui_popup_backdrop_show(song_context_menu_popup_backdrop);
    if (song_context_menu_popup) lv_obj_remove_flag(song_context_menu_popup, LV_OBJ_FLAG_HIDDEN);
}

static void build_song_context_menu_popup(void) {
    menu_popup_row_t rows[24];
    int count = 0;
    if (song_context_menu_capabilities & SONG_MENU_PLAYBACK) {
        rows[count++] = (menu_popup_row_t) { TR("Play next"), song_context_menu_play_next_cb, false };
        rows[count++] = (menu_popup_row_t) { TR("Add to queue"), song_context_menu_add_to_queue_cb, false };
    }
    if (song_context_menu_capabilities & SONG_MENU_PLAYLIST)
        rows[count++] = (menu_popup_row_t) { TR("Add to playlist"), song_context_menu_add_to_playlist_cb, false };
    if (song_context_menu_capabilities & SONG_MENU_RATING)
        rows[count++] = (menu_popup_row_t) { TR("Rate song"), song_context_menu_rating_cb, false };
    if (song_context_menu_capabilities & SONG_MENU_NAVIGATION) {
        rows[count++] = (menu_popup_row_t) { TR("Go to album"), song_context_menu_album_cb, false };
        rows[count++] = (menu_popup_row_t) { TR("Go to artist"), song_context_menu_artist_cb, false };
    }
    if (song_context_menu_capabilities & SONG_MENU_INFORMATION)
        rows[count++] = (menu_popup_row_t) { TR("Information"), song_context_menu_information_cb, false };
    if (song_context_menu_capabilities & SONG_MENU_REFRESH)
        rows[count++] = (menu_popup_row_t) { TR("Refresh metadata"), song_context_menu_refresh_metadata_cb, false };
    int extras = song_context_menu_extra_row_count;
    if (extras > (int) (sizeof(rows) / sizeof(rows[0])) - count - 1)
        extras = (int) (sizeof(rows) / sizeof(rows[0])) - count - 1;
    for (int i = 0; i < extras; i++) rows[count++] = song_context_menu_extra_rows[i];
    rows[count++] = (menu_popup_row_t) { TR("Cancel"), song_context_menu_cancel_cb, false, true };
    /* Reuse only fixed menus; extra rows can change callback and row count. */
    if (song_context_menu_popup && !extras && !song_context_built_extras &&
        song_context_built_capabilities == song_context_menu_capabilities) {
        refresh_menu_labels(song_context_menu_popup, rows, count);
        return;
    }
    if (song_context_menu_popup_backdrop) gui_popup_backdrop_hide(song_context_menu_popup_backdrop);
    if (song_context_menu_popup) lv_obj_delete(song_context_menu_popup);
    if (song_context_menu_popup_backdrop) lv_obj_delete(song_context_menu_popup_backdrop);
    song_context_built_capabilities = song_context_menu_capabilities;
    song_context_built_extras = extras != 0;
    song_context_menu_popup = build_menu_popup(rows, count,
                                                song_context_menu_popup_backdrop_cb,
                                                &song_context_menu_popup_backdrop);
}

void gui_queue_init(void) {
    queue_screen = build_queue_screen();
    song_context_menu_capabilities = SONG_MENU_ALL;
    build_song_context_menu_popup();
}

/* For gui_reload.c's in-process UI reload -- deletes every screen/popup this
 * module owns so gui_queue_init() can rebuild them from a clean slate
 * without leaking the old objects. song_context_menu_popup/backdrop are
 * built via build_menu_popup() directly on lv_layer_top() (same shape as
 * build_confirm_popup(), see its own comment), not as children of
 * queue_screen, so they need their own explicit deletion. */
void gui_queue_teardown(void) {
    queue_finish_drag();
    queue_reorder_state.rows = NULL;
    queue_reorder_state.list = NULL;
    queue_unregister_handles();
    gui_popup_teardown(&queue_clear_popup);
    if (queue_actions_backdrop) gui_popup_backdrop_hide(queue_actions_backdrop);
    if (queue_actions) { lv_obj_delete(queue_actions); queue_actions = NULL; }
    if (queue_actions_backdrop) { lv_obj_delete(queue_actions_backdrop); queue_actions_backdrop = NULL; }
    if (song_context_menu_popup_backdrop) gui_popup_backdrop_hide(song_context_menu_popup_backdrop);
    if (song_context_menu_popup) { lv_obj_delete(song_context_menu_popup); song_context_menu_popup = NULL; }
    if (song_context_menu_popup_backdrop) { lv_obj_delete(song_context_menu_popup_backdrop); song_context_menu_popup_backdrop = NULL; }
    if (queue_screen) { lv_obj_delete(queue_screen); queue_screen = NULL; }
    song_context_built_capabilities = 0;
    song_context_built_extras = false;
    queue_list = NULL;
    queue_current_row = NULL;
    queue_current_heading = NULL;
}
