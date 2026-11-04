#include "gui_player_internal.h"
#include "../../text/rendering/ui_label_text.h"
#include <limits.h>
#include "../../../library/metadata/parsing/metadata.h"
#include "../../../library/parsers/cue/cue_parser.h"
#include "../../../core/system/runtime/board_config.h"
#include "../../text/i18n/i18n.h"
#include "../../../hardware/usb/usb_mode_control.h"
#include "../../../library/browser/files/file_browser.h"
#include "../../../network/remote/control/remote_control.h"
#include "../../../network/hiby/hiby_sys_server.h"
#include "../../../network/bluetooth/media/bt_cover_art.h"
#include "../../../hardware/indicators/backlight.h"
#include "../../../hardware/indicators/led_control.h"
#include "../../../hardware/power/charge_limiter.h"
#include "../../../hardware/device/headphone_status.h"
#include "../../../hardware/power/battery.h"

/* ---- Playback state and advance machinery ---- */

#include "../../../audio/engine/audio_helpers.h"
#include "gui_player.h"
#include "../../application/gui.h"
#include "../../appearance/gui_theme.h"
#include "../../shell/gui_notifications.h"
#include "../../library/browse/gui_library.h"
#include "gui_queue.h"
#include "../../../library/playlists/playlist_files.h"
#include "../../../library/playlists/favorite_writer.h"
#include "../../../library/browser/paths/path_cache.h"
#include "../../../library/artwork/cache/cover_raster_cache.h"
#include "../../../library/artwork/prefetch/player_cover_prefetch.h"
#include "../../../core/ui/display/ui_wake.h"
#include "../../../library/workers/track_metadata_worker.h"
#include "../../../library/remote/remote_track.h"
#include "../../../library/playlists/playback_order.h"
#include "../../../library/playlists/queue_resume.h"
#include "../queue/player_queue_storage.h"
#include "../queue/player_queue_model.h"
#include "../queue/player_queue_resume.h"
#include "gui_lyrics.h"
#include "../../effects/frosted_glass.h"
#include "../layout/player_layout.h"
#include "../layout/player_layouts.h"
#include "../layout/player_layout_internal.h"
#include "../layout/player_layout_builtin.h"
#include "../transport/player_seekbar.h"
#include "../transport/player_seek_input.h"
#include "../artwork/player_cover_fade.h"
#include "../artwork/player_lyrics_backdrop.h"
#include "../artwork/player_backdrop.h"
#include "../artwork/player_cover.h"
#include "../lyrics/player_timeline_labels.h"
#include "../transport/player_volume_popup.h"
#include "../transport/player_transport_images.h"
#include "../artwork/player_artwork_pixels.h"
#include "../artwork/player_artwork_cache.h"
#include "../artwork/player_artwork_jobs.h"
#include "../lyrics/player_inline_lyrics.h"
#include "../../effects/transition_compositor.h"
#include "lvgl/src/core/lv_obj_draw_private.h"
#include "lvgl/src/draw/sw/lv_draw_sw_private.h"
#include "../../../library/artwork/coordinator/artwork_coordinator.h"
#include "../../../audio/visualization/waveform.h"
#include "gui_track_info.h"
#include "../../settings/gui_settings.h"
#include "../../network/subsonic/gui_subsonic.h"
#include "../../settings/lock/gui_lock_screen.h"
#include "../../../network/http/client/http_client.h"
#include "../../widgets/builders/screen_builders.h"

#include "../../../library/metadata/db/metadata_db.h"
#include "../../../library/artwork/decode/cover_decode.h"
#include "../../../core/system/logging/db_log.h"
#include "../../../network/airplay/airplay_control.h"
#include "../../../library/artwork/albumart/albumart.h"
#include "../../assets/assets.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "../../../core/persistence/storage_paths.h"
#include "../../../plugins/manager/plugin_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <math.h>
#include <sys/stat.h>

static lv_obj_t * player_screen = NULL;
lv_obj_t * player_dismiss_btn = NULL;
lv_obj_t * player_overlay_panel = NULL;
lv_obj_t * player_background_img = NULL;
lv_obj_t * cover_card = NULL;
lv_obj_t * cover_img = NULL;
lv_obj_t * artist_label = NULL;
lv_obj_t * album_label = NULL;
lv_obj_t * song_count_label = NULL;
lv_obj_t * song_title_label = NULL;
lv_obj_t * quality_pill = NULL;
lv_obj_t * format_badge_label = NULL;
static lv_obj_t * order_icon = NULL;
lv_obj_t * favorite_circle = NULL;
lv_obj_t * favorite_icon = NULL;
lv_obj_t * play_btn = NULL;
lv_obj_t * prev_btn = NULL;
lv_obj_t * next_btn = NULL;
lv_obj_t * progress_slider = NULL;
static bool player_waveform_seekbar;
lv_obj_t * volume_slider = NULL;

static lv_obj_t * pos_label = NULL;
static lv_obj_t * dur_label = NULL;

/* Player layout state, see the layout section above build_player_screen().
 * Declared here because the metadata and lyrics code read it. */
bool player_layout_builtin = true;
static lv_obj_t * player_layout_root;
static lv_anim_timeline_t * player_timeline_track_change;
static lv_anim_timeline_t * player_timeline_screen_enter;
/* Optional custom-layout roles. The thumbnail shares the decoded cover;
 * the play circle provides a larger hit target around the stock play icon. */
lv_obj_t * player_cover_thumbnail;
static lv_obj_t * player_play_circle;
/* Layouts without artist/album roles get hidden labels for the player
 * geometry code. They are not layout widgets: never laid out, scrolled,
 * styled or moved by the lyrics view. */
static bool player_artist_standin;
static bool player_album_standin;
static void player_now_playing_group_click_cb(lv_event_t * e);

/* Plays a layout timeline from its start and returns its length in ms. */
static uint32_t player_timeline_play(lv_anim_timeline_t * timeline,
                                     lv_anim_completed_cb_t completed_cb,
                                     bool * completion_attached,
                                     bool * runner_available) {
    if (completion_attached) *completion_attached = false;
    if (runner_available) *runner_available = false;
    if (!timeline) return 0;
    player_layouts_timeline_refresh_timing(timeline);
    /* A finished timeline would resume at its end and do nothing. */
    lv_anim_timeline_pause(timeline);
    lv_anim_timeline_set_progress(timeline, 0);
    uint32_t playtime = lv_anim_timeline_start(timeline);
    if (completed_cb) {
        /* The timeline runner is registered synchronously by start(). LVGL
         * removes it before calling completed_cb, so the callback can safely
         * finish UI state that pauses or updates the same timeline. */
        lv_anim_t * runner = lv_anim_get(timeline, NULL);
        if (runner) {
            if (runner_available) *runner_available = true;
            if (!runner->completed_cb) lv_anim_set_completed_cb(runner, completed_cb);
            if (completion_attached) *completion_attached = runner->completed_cb == completed_cb;
        }
    }
    return playtime;
}

static int32_t displayed_progress_percent = -1;
static int displayed_position_second = -1;
static int displayed_duration_second = -1;

void player_progress_display_reset(void) {
    player_seekbar_set_value(progress_slider, 0);
    displayed_progress_percent = 0;
    displayed_position_second = -1;
    displayed_duration_second = -1;
    if (pos_label) lv_label_set_text(pos_label, "0:00");
    if (dur_label) lv_label_set_text(dur_label, "--:--");
}

void player_progress_display_set_deferred(double position, double duration_seconds) {
    int32_t percent = (int32_t) ((position / duration_seconds) * 100.0);
    displayed_progress_percent = percent;
    player_seekbar_set_value(progress_slider, percent);

    displayed_position_second = (int) position;
    displayed_duration_second = (int) duration_seconds;
    char time_str[24];
    gui_format_time(position, time_str, sizeof(time_str));
    if (pos_label) lv_label_set_text(pos_label, time_str);
    gui_format_time(duration_seconds, time_str, sizeof(time_str));
    if (dur_label) lv_label_set_text(dur_label, time_str);
}

void player_track_change_timeline_play(void) {
    player_timeline_play(player_timeline_track_change, NULL, NULL, NULL);
}
/* Track display metadata belongs to playback, not to an XML widget tree.
 * Keep the resolved tag/fallback strings across UI reloads and omitted roles. */
static char * player_track_title_text;
static char * player_track_artist_text;
static char * player_track_album_text;
static char * player_reload_position_text;
static char * player_reload_duration_text;

static void player_replace_text(char ** stored, const char * text) {
    if (!text) {
        free(*stored);
        *stored = NULL;
        return;
    }
    size_t len = strlen(text) + 1;
    char * copy = malloc(len);
    if (!copy) return;
    memcpy(copy, text, len);
    free(*stored);
    *stored = copy;
}

void player_set_track_display_text(const char * title, const char * artist,
                                          const char * album) {
    player_replace_text(&player_track_title_text, title);
    player_replace_text(&player_track_artist_text, artist);
    player_replace_text(&player_track_album_text, album);
    if (song_title_label)
        lv_label_set_text(song_title_label, title ? title : TR("No track loaded"));
    if (artist_label) lv_label_set_text(artist_label, artist ? artist : "");
    if (album_label) lv_label_set_text(album_label, album ? album : "");
    gui_shell_update_quick_drawer_track(title, artist, album);
}

/* The queue just emptied: no track, so no pause/play state or labels to keep. */
void player_show_empty_queue(void) {
    set_play_button_state(false);
    player_set_track_display_text(NULL, NULL, NULL);
    player_reset_now_playing_extras();
}

static void player_capture_reload_label_text(void) {
    player_replace_text(&player_reload_position_text, pos_label ? lv_label_get_text(pos_label) : NULL);
    player_replace_text(&player_reload_duration_text, dur_label ? lv_label_get_text(dur_label) : NULL);
}

static void player_restore_reload_label_text(void) {
    if (song_title_label)
        lv_label_set_text(song_title_label, player_track_title_text ? player_track_title_text : TR("No track loaded"));
    if (artist_label) lv_label_set_text(artist_label, player_track_artist_text ? player_track_artist_text : "");
    if (album_label) lv_label_set_text(album_label, player_track_album_text ? player_track_album_text : "");
    if (player_reload_position_text && pos_label)
        lv_label_set_text(pos_label, player_reload_position_text);
    if (player_reload_duration_text && dur_label)
        lv_label_set_text(dur_label, player_reload_duration_text);
    player_replace_text(&player_reload_position_text, NULL);
    player_replace_text(&player_reload_duration_text, NULL);
}


extern bool favorite_is_set;

extern void prev_btn_event_cb(lv_event_t * e);
extern void play_btn_event_cb(lv_event_t * e);
extern void next_btn_event_cb(lv_event_t * e);

void arm_next_track_for_audio(int index);

/* Shared by the player screen's own order_icon tap and the Remote Control
 * web UI's mode-cycle button (remote_control_consume_mode_cycle()) -- same
 * single 5-state cycle either way, so both surfaces stay in sync and
 * neither reimplements the mode-advance/persist/re-arm logic. */
void cycle_play_mode(void) {
    play_mode_t mode = (play_mode_t) current_settings.play_mode;
    mode = (play_mode_t) ((mode + 1) % 5);
    gui_player_set_play_mode(mode);
    switch (mode) {
        case PLAY_MODE_REPEAT_ALL: show_info_toast(TR("Repeat all")); break;
        case PLAY_MODE_REPEAT_ONE: show_info_toast(TR("Repeat one")); break;
        case PLAY_MODE_SHUFFLE: show_info_toast(TR("Shuffle + repeat")); break;
        case PLAY_MODE_SHUFFLE_ONCE: show_info_toast(TR("Shuffle once")); break;
        default: show_info_toast(TR("Sequential")); break;
    }
}

void gui_player_set_play_mode(int mode_value) {
    play_mode_t mode = (play_mode_t) mode_value;
    if (mode < PLAY_MODE_SEQUENTIAL || mode > PLAY_MODE_SHUFFLE_ONCE) return;
    current_settings.play_mode = (int) mode;
    settings_save_async(&current_settings);

    if (order_icon) lv_image_set_src(order_icon, asset_path(play_mode_icon_asset(mode)));

    /* What comes after the current track changes with the mode (e.g.
     * entering Shuffle picks a random next instead of index+1) -- re-arm the
     * gapless/crossfade target immediately rather than waiting for whatever
     * was armed under the old mode to turn out wrong. */
    if (player_queue_model_current() >= 0) arm_next_track_for_audio(player_queue_model_current());
}

static void order_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    cycle_play_mode();
}

/* Song context menu moved to gui_queue.c. */

typedef struct {
    lv_obj_t * img;
    const char * normal_path;
    const char * pressed_path;
} transport_btn_ctx_t;

static void transport_btn_press_event_cb(lv_event_t * e) {
    transport_btn_ctx_t * ctx = (transport_btn_ctx_t *) lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_image_set_src(ctx->img, asset_path(ctx->pressed_path));
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_image_set_src(ctx->img, asset_path(ctx->normal_path));
    }
}

static void transport_btn_ctx_delete_cb(lv_event_t * e) {
    free(lv_event_get_user_data(e));
}

/* Public hardware seek entry point retained for gui.c and button mappings. */
void gui_player_hw_seek_steps(int direction, int step_count, bool is_first) {
    player_seek_input_hw_steps(direction, step_count, is_first);
}

#ifdef UI_GESTURE_TRACE
static void debug_transport_btn_all_cb(lv_event_t * e) {
    printf("[TRANSPORT_TRACE] obj=%p code=%d clickable=%d state=0x%x\n",
           (void *) lv_event_get_target(e), (int) lv_event_get_code(e),
           lv_obj_has_flag(lv_event_get_target(e), LV_OBJ_FLAG_CLICKABLE),
           (unsigned) lv_obj_get_state(lv_event_get_target(e)));
}
#endif

/* Extends the vertical reach of transport buttons (mode/play/prev/next/more)
 * up to a single shared top line (roughly level with song_count_label).
 * A separate invisible, absolutely-positioned sibling marked
 * LV_OBJ_FLAG_IGNORE_LAYOUT provides vertical hit area without altering
 * horizontal padding between neighboring buttons.
 *
 * `top_y`/`bottom_y_exclusive` are absolute screen coordinates resolved
 * after lv_obj_update_layout(). The resulting object covers the hit region
 * seamlessly. */
static lv_obj_t * player_hit_rect(lv_obj_t * scr, int32_t x, int32_t y, int32_t w, int32_t h,
                                  lv_event_cb_t cb, lv_color_t debug_color) {
    lv_obj_t * ext = lv_obj_create(scr);
    lv_obj_remove_style_all(ext);
    lv_obj_set_pos(ext, x, y);
    lv_obj_set_size(ext, w, h);
    lv_obj_add_flag(ext, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(ext, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ext, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ext, cb, LV_EVENT_CLICKED, NULL);
#ifdef UI_HITBOX_DEBUG
    lv_obj_set_style_border_width(ext, 3, 0);
    lv_obj_set_style_border_color(ext, debug_color, 0);
    lv_obj_set_style_border_opa(ext, LV_OPA_COVER, 0);
#else
    (void) debug_color;
#endif
    return ext;
}

static lv_obj_t * add_transport_hit_target(lv_obj_t * scr, int32_t center_x, int32_t width, int32_t top_y,
                                     int32_t bottom_y_exclusive, lv_event_cb_t cb, lv_color_t debug_color) {
    return player_hit_rect(scr, center_x - width / 2, top_y, width, bottom_y_exclusive - top_y, cb, debug_color);
}

/* Transport icons are visual-only elements inside controls_row.
 * Forwarding the hit target's PRESSED/RELEASED/PRESS_LOST onto the
 * underlying icon's state triggers icon_press_style dimming on real touch. */
static void forward_press_state_to_icon_cb(lv_event_t * e) {
    lv_obj_t * icon = (lv_obj_t *) lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) lv_obj_add_state(icon, LV_STATE_PRESSED);
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) lv_obj_clear_state(icon, LV_STATE_PRESSED);
}

static void player_forward_press(lv_obj_t * hit, lv_obj_t * icon) {
    lv_obj_add_event_cb(hit, forward_press_state_to_icon_cb, LV_EVENT_PRESSED, icon);
    lv_obj_add_event_cb(hit, forward_press_state_to_icon_cb, LV_EVENT_RELEASED, icon);
    lv_obj_add_event_cb(hit, forward_press_state_to_icon_cb, LV_EVENT_PRESS_LOST, icon);
}

static void library_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (player_inline_lyrics_area_is_open()) {
        player_inline_lyrics_set_open(false, true);
        return;
    }
    nav_pop();
}

static void player_label_enable_marquee(lv_obj_t * label) {
    static lv_anim_t player_marquee_anim;
    static bool initialized = false;
    if (!initialized) {
        lv_anim_init(&player_marquee_anim);
        lv_anim_set_delay(&player_marquee_anim, 2000);
        lv_anim_set_repeat_delay(&player_marquee_anim, 2000);
        initialized = true;
    }

    /* Local style properties outrank row_label_enable_marquee()'s shared
     * style. Install the player-only pause before that helper enables and
     * refreshes circular scrolling, while retaining its speed duration. */
    lv_obj_set_style_anim(label, &player_marquee_anim, LV_PART_MAIN);
    row_label_enable_marquee(label);
}

/* Fixed three-line metadata block above the cover: title, artist, album.
 * Each row is one line of its own font, stacked with a uniform gap. The
 * fonts are fixed per board, so this resolves to one layout per board. */
static void player_layout_metadata_stack(void) {
    if (!song_title_label || !album_label || !artist_label) return;

#if defined(BOARD_TEMPOTEC_V1)
    {
    const lv_font_t * title_font = lv_obj_get_style_text_font(song_title_label, 0);
    const lv_font_t * artist_font = lv_obj_get_style_text_font(artist_label, 0);
    int32_t title_h = title_font ? lv_font_get_line_height(title_font) : 16;
    int32_t artist_h = artist_font ? lv_font_get_line_height(artist_font) : 14;
    lv_obj_set_y(song_title_label, 136);
    lv_obj_set_height(song_title_label, title_h);
    lv_obj_set_y(artist_label, 136 + title_h + 4);
    lv_obj_set_height(artist_label, artist_h);
    lv_obj_add_flag(album_label, LV_OBJ_FLAG_HIDDEN);
    player_transition_mark_dirty();
    return;
    }
#endif

    const lv_font_t * title_font = lv_obj_get_style_text_font(song_title_label, 0);
    const lv_font_t * artist_font = lv_obj_get_style_text_font(artist_label, 0);
    const lv_font_t * album_font = lv_obj_get_style_text_font(album_label, 0);
    int32_t title_h = title_font ? lv_font_get_line_height(title_font) : player_y(32);
    int32_t artist_h = artist_font ? lv_font_get_line_height(artist_font) : player_y(26);
    int32_t album_h = album_font ? lv_font_get_line_height(album_font) : player_y(26);

    const int32_t title_y = player_y(36);
    /* Close the gaps rather than let the block reach the cover. */
    int32_t gap = player_y(6);
    int32_t room = player_y(152) - title_y - title_h - artist_h - album_h;
    if (2 * gap > room) gap = room > 0 ? room / 2 : 0;
    const int32_t artist_y = title_y + title_h + gap;
    const int32_t album_y = artist_y + artist_h + gap;

    lv_obj_set_y(song_title_label, title_y);
    lv_obj_set_height(song_title_label, title_h);
    lv_obj_set_y(artist_label, artist_y);
    lv_obj_set_height(artist_label, artist_h);
    lv_obj_set_y(album_label, album_y);
    lv_obj_set_height(album_label, album_h);

    player_transition_mark_dirty();
}

static void progress_slider_event_cb(lv_event_t * e) {
    player_seek_input_slider_event(e, &displayed_progress_percent);
}

/* ---- Player layouts: built-in creator and role binder ----
 *
 * The Player screen is assembled in two steps so that its widget tree can come
 * from more than one place. A layout (the built-in creator in
 * player_layout_builtin.c, a component exported to C by LVGL's UI Editor,
 * or an LVGL XML file; see player_layouts.h) only creates widgets, geometry and
 * styling and names the ones the app cares about. player_bind_widgets() then finds
 * those widgets by name and attaches everything that is behavior: events, touch
 * targets, the exported globals the rest of the app reads. The role names are the
 * public contract documented in docs/reference/PLAYER_LAYOUTS.md. */

static lv_obj_t * player_create_hidden_label(lv_obj_t * parent) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, "");
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    return label;
}

static bool player_obj_is_ancestor_of(const lv_obj_t * ancestor, const lv_obj_t * obj) {
    for (const lv_obj_t * p = obj ? lv_obj_get_parent(obj) : NULL; p; p = lv_obj_get_parent(p)) {
        if (p == ancestor) return true;
    }
    return false;
}

/* Looks a role up and keeps it only if it is one of the expected widget
 * types; a wrong-typed optional widget is treated as absent. */
static lv_obj_t * player_find_role(lv_obj_t * scr, const char * role, const lv_obj_class_t * expected_class) {
    lv_obj_t * obj = lv_obj_find_by_name(scr, role);
    if (obj && expected_class && !lv_obj_check_type(obj, expected_class)) {
        fprintf(stderr, "Warning: player layout: '%s' is not a %s, ignoring it\n", role,
                expected_class == &lv_label_class ? "label" :
                expected_class == &lv_image_class ? "image" : "slider");
        return NULL;
    }
    return obj;
}

/* Hit area of a transport icon. The built-in layout keeps its authored
 * numbers; any other layout derives the area from where the icon ended up,
 * at least 44x44 like the built-in one. */
static void player_hit_area(lv_obj_t * icon, int32_t builtin_center_x, int32_t builtin_size, int32_t builtin_center_y,
                            int32_t * center_x, int32_t * width, int32_t * top, int32_t * bottom_exclusive) {
    int32_t w, h, cy;
    if (player_layout_builtin) {
        w = h = builtin_size;
        *center_x = builtin_center_x;
        cy = builtin_center_y;
    } else {
        lv_area_t area;
        lv_obj_get_coords(icon, &area);
        w = lv_area_get_width(&area);
        h = lv_area_get_height(&area);
        *center_x = area.x1 + w / 2;
        cy = area.y1 + h / 2;
    }
    if (w < 44) w = 44;
    if (h < 44) h = 44;
    *width = w;
    *top = cy - h / 2;
    *bottom_exclusive = cy + (h + 1) / 2;
}

static lv_obj_t * player_add_hit_target(lv_obj_t * scr, lv_obj_t * icon, int32_t builtin_center_x,
                                        int32_t builtin_size, int32_t builtin_center_y, lv_event_cb_t cb,
                                        lv_color_t debug_color) {
    int32_t center_x, width, top, bottom;
    player_hit_area(icon, builtin_center_x, builtin_size, builtin_center_y, &center_x, &width, &top, &bottom);
    lv_obj_t * hit = add_transport_hit_target(scr, center_x, width, top, bottom, cb, debug_color);
    player_inline_lyrics_add_hit_target(hit);
    return hit;
}

static transport_btn_ctx_t * player_skip_ctx_new(lv_obj_t * icon, const char * normal, const char * pressed) {
    transport_btn_ctx_t * ctx = malloc(sizeof(*ctx));
    if (ctx) *ctx = (transport_btn_ctx_t){ icon, normal, pressed };
    return ctx;
}

/* Prev/next hit target; ctx owns the icon swap and is freed with the target. */
static lv_obj_t * player_add_skip_hit_target(lv_obj_t * scr, lv_obj_t * icon, transport_btn_ctx_t * ctx,
                                             int32_t center_x, int32_t center_y, lv_event_cb_t cb,
                                             lv_color_t debug_color, int direction) {
    lv_obj_t * hit = player_add_hit_target(scr, icon, center_x, player_s(56), center_y, cb, debug_color);
    lv_obj_add_event_cb(hit, transport_btn_press_event_cb, LV_EVENT_PRESSED, ctx);
    lv_obj_add_event_cb(hit, transport_btn_press_event_cb, LV_EVENT_RELEASED, ctx);
    lv_obj_add_event_cb(hit, transport_btn_press_event_cb, LV_EVENT_PRESS_LOST, ctx);
    lv_obj_add_event_cb(hit, transport_btn_ctx_delete_cb, LV_EVENT_DELETE, ctx);
    player_seek_input_bind_transport_target(hit, direction);
    return hit;
}

static void player_add_lyrics_control(lv_obj_t * obj) {
    player_inline_lyrics_add_control(obj);
}

static void player_screen_loaded_cb(lv_event_t * e) {
    (void) e;
    player_timeline_play(player_timeline_screen_enter, NULL, NULL, NULL);
}

/* A layout's own glyphs and captions, outside the fixed roles, follow the
 * backdrop too when named for it: images "glyph_*" take the transport tint
 * and labels "caption_*" the muted text color. */
static void player_style_named_extras(lv_obj_t * obj) {
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t * child = lv_obj_get_child(obj, (int32_t) i);
        const char * name = lv_obj_get_name(child);
        if (name && strcmp(name, "quality_icon") == 0 && lv_obj_check_type(child, &lv_image_class)) {
            lv_obj_add_style(child, gui_theme_accent_style(), 0);
            lv_obj_set_style_bg_opa(child, LV_OPA_TRANSP, 0);
            lv_obj_set_style_image_recolor_opa(child, LV_OPA_COVER, 0);
        }
        else if (name && strcmp(name, "accent_background") == 0 && lv_obj_check_type(child, &lv_obj_class))
            lv_obj_add_style(child, gui_theme_accent_style(), 0);
        else if (name && strncmp(name, "glyph_", 6) == 0 && lv_obj_check_type(child, &lv_image_class))
            lv_obj_add_style(child, gui_player_backdrop_icon_style(), 0);
        else if (name && strncmp(name, "caption_", 8) == 0 && lv_obj_check_type(child, &lv_label_class))
            lv_obj_add_style(child, gui_player_backdrop_text_style(true), 0);
        player_style_named_extras(child);
    }
}

bool gui_player_lyrics_animation_in_progress(void) {
    return player_inline_lyrics_is_animating();
}

static lv_obj_t * player_inline_current_screen(void) { return player_screen; }

static bool player_inline_setting_lyrics_enabled(void) { return current_settings.lyrics_enabled; }
static void player_inline_pause_artwork_cache_timer(void) {
    player_cover_cache_timer_pause();
}

static player_inline_lyrics_view_t player_inline_view_for_bind(
    lv_obj_t * screen, lv_obj_t * cover_fade, lv_obj_t * lyrics_area,
    lv_anim_timeline_t * lyrics_open, lv_anim_timeline_t * lyrics_close,
    bool backdrop_contains_cover) {
    return (player_inline_lyrics_view_t) {
        .screen = screen,
        .cover_card_obj = cover_card,
        .cover_image = cover_img,
        .cover_thumbnail = player_cover_thumbnail,
        .cover_fade = cover_fade,
        .overlay = player_overlay_panel,
        .title = song_title_label,
        .artist = artist_label,
        .album = album_label,
        .favorite_circle_obj = favorite_circle,
        .lyrics_area = lyrics_area,
        .lyrics_open = lyrics_open,
        .lyrics_close = lyrics_close,
        .builtin_layout = player_layout_builtin,
        .artist_is_standin = player_artist_standin,
        .album_is_standin = player_album_standin,
        .backdrop_contains_cover = backdrop_contains_cover,
        .cover_size_changed_cb = player_cover_size_changed_cb,
        .current_screen = player_inline_current_screen,
        .query_cover = player_inline_query_cover,
        .lyrics_enabled = player_inline_setting_lyrics_enabled,
        .display_refresh_allowed = gui_display_refresh_allowed,
        .fit_cover_img_to_card = fit_cover_img_to_card,
        .pause_artwork_cache_timer = player_inline_pause_artwork_cache_timer,
        .mark_transition_dirty = player_transition_mark_dirty,
        .timeline_play = player_timeline_play,
    };
}

typedef struct {
    lv_obj_t * cover_card;
    lv_obj_t * cover_img;
    lv_obj_t * title;
    lv_obj_t * play;
    lv_obj_t * progress;
    lv_obj_t * overlay;
    lv_obj_t * background;
    lv_obj_t * artist;
    lv_obj_t * album;
    lv_obj_t * fav_circle;
    lv_obj_t * fav_icon;
    lv_obj_t * pill;
    lv_obj_t * badge;
    lv_obj_t * pos;
    lv_obj_t * dur;
    lv_obj_t * count;
    lv_obj_t * order;
    lv_obj_t * prev;
    lv_obj_t * next;
    lv_obj_t * more;
    lv_obj_t * dismiss;
    lv_obj_t * volume;
    lv_obj_t * lyrics_area;
    lv_obj_t * lyrics_backdrop;
    lv_obj_t * lyrics_dim;
    lv_obj_t * lyrics_active;
    lv_obj_t * lyrics_toggle;
    lv_obj_t * cover_thumbnail;
    lv_obj_t * play_circle;
    lv_obj_t * transport_color;
    lv_obj_t * cover_fade;
    bool layout_has_overlay;
    transport_btn_ctx_t * prev_ctx, * next_ctx;
} player_roles_t;

/* Finds and validates a layout's roles. Roles and their contract:
 * docs/reference/PLAYER_LAYOUTS.md. Returns false, with no global assigned and nothing
 * attached, when a required role is missing or of the wrong type, so the
 * caller can discard the tree and fall back to the built-in layout. */
static bool player_find_roles(lv_obj_t * scr, player_roles_t * r) {
    /* Everything that can fail comes first. */
    r->cover_card = player_find_role(scr, "cover_card", NULL);
    r->cover_img = player_find_role(scr, "cover_img", &lv_image_class);
    r->title = player_find_role(scr, "title", &lv_label_class);
    r->play = player_find_role(scr, "play_btn", &lv_image_class);
    r->progress = player_find_role(scr, "progress_slider", NULL);
    if (!player_seekbar_is_supported(r->progress)) r->progress = NULL;
    const char * missing = !r->cover_card ? "cover_card" : !r->cover_img ? "cover_img" : !r->title ? "title" :
                           !r->play ? "play_btn" : !r->progress ? "progress_slider" : NULL;
    if (missing) {
        fprintf(stderr, "Warning: player layout: required widget '%s' is missing or has the wrong type\n", missing);
        return false;
    }
    if (!player_seekbar_configure(r->progress)) return false;
    r->overlay = player_find_role(scr, "overlay_panel", NULL);
    r->layout_has_overlay = r->overlay != NULL;
    r->background = player_find_role(scr, "background_img", &lv_image_class);
    r->artist = player_find_role(scr, "artist", &lv_label_class);
    r->album = player_find_role(scr, "album", &lv_label_class);
    r->fav_circle = player_find_role(scr, "favorite_circle", NULL);
    r->fav_icon = player_find_role(scr, "favorite_icon", &lv_image_class);
    r->pill = player_find_role(scr, "quality_pill", NULL);
    r->badge = player_find_role(scr, "format_badge", &lv_label_class);
    r->pos = player_find_role(scr, "pos_label", &lv_label_class);
    r->dur = player_find_role(scr, "dur_label", &lv_label_class);
    r->count = player_find_role(scr, "song_count", &lv_label_class);
    r->order = player_find_role(scr, "order_btn", &lv_image_class);
    r->prev = player_find_role(scr, "prev_btn", &lv_image_class);
    r->next = player_find_role(scr, "next_btn", &lv_image_class);
    r->more = player_find_role(scr, "more_btn", &lv_image_class);
    r->dismiss = player_find_role(scr, "dismiss_btn", NULL);
    r->volume = player_find_role(scr, "volume_slider", &lv_slider_class);
    r->lyrics_area = player_find_role(scr, "lyrics_area", NULL);
    r->lyrics_backdrop = player_find_role(scr, "lyrics_backdrop", NULL);
    r->lyrics_dim = player_find_role(scr, "lyrics_dim", NULL);
    if (!r->lyrics_dim) r->lyrics_dim = player_find_role(scr, "panorama_lyrics_dim", NULL);
    r->lyrics_active = player_find_role(scr, "lyrics_active", &lv_label_class);
    r->lyrics_toggle = player_find_role(scr, "lyrics_toggle", NULL);
    r->cover_thumbnail = player_find_role(scr, "cover_thumbnail", &lv_image_class);
    r->play_circle = player_find_role(scr, "play_circle", NULL);
    r->transport_color = player_find_role(scr, "transport_color", &lv_label_class);
    r->cover_fade = player_find_role(scr, "cover_fade", &lv_image_class);

    r->prev_ctx = r->prev ?
        player_skip_ctx_new(r->prev, "playing_plane/btn_prev.png", "playing_plane/btn_prev_s.png") : NULL;
    r->next_ctx = r->next ?
        player_skip_ctx_new(r->next, "playing_plane/btn_next.png", "playing_plane/btn_next_s.png") : NULL;
    if ((r->prev && !r->prev_ctx) || (r->next && !r->next_ctx)) {
        free(r->prev_ctx);
        free(r->next_ctx);
        return false;
    }

    return true;
}

static void player_publish_roles(lv_obj_t * scr, player_roles_t * r) {
    /* Hidden-but-required internals: frosted-background code and the volume
     * mirror assume they exist whatever the layout draws. */
    if (!r->overlay) {
        r->overlay = lv_obj_create(scr);
        lv_obj_set_name_static(r->overlay, "overlay_panel");
        player_init_overlay_panel(r->overlay);
        lv_obj_move_to_index(r->overlay, 0);
    }
    if (!r->background) r->background = player_create_hidden_background_img(r->overlay);
    if (!r->volume) {
        r->volume = lv_slider_create(scr);
        lv_obj_set_name_static(r->volume, "volume_slider");
        lv_obj_add_flag(r->volume, LV_OBJ_FLAG_HIDDEN);
    } else if (!player_layout_builtin) {
        /* A visible volume_slider is an indicator only; the volume popup and
         * the quick drawer own volume changes. */
        lv_obj_remove_flag(r->volume, LV_OBJ_FLAG_CLICKABLE);
    }

    player_overlay_panel = r->overlay;
    player_background_img = r->background;
    cover_card = r->cover_card;
    cover_img = r->cover_img;
    player_cover_thumbnail = r->cover_thumbnail;
    player_play_circle = r->play_circle;
    /* Only a descendant was baked into the card snapshot. Older XMLs may
     * keep their fade beside the card; leave that live overlay visible. */
    song_title_label = r->title;
    artist_label = r->artist;
    album_label = r->album;
    player_artist_standin = !r->artist;
    player_album_standin = !r->album;
    if (!artist_label) artist_label = player_create_hidden_label(scr);
    if (!album_label) album_label = player_create_hidden_label(scr);
    /* Bind on the actual artist/album role widgets, including XML layouts.
     * player_bind_widgets() runs once for each newly built player tree; role
     * rebinding after a UI reload therefore gets fresh callbacks without
     * accumulating duplicate event handlers on reused labels. */
    if (r->artist && strcmp(player_layouts_get_interactions()->artist_action, "none") != 0) {
        lv_obj_add_flag(r->artist, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r->artist, player_now_playing_group_click_cb,
                            LV_EVENT_CLICKED, (void *) (intptr_t) 0);
    }
    if (r->album && strcmp(player_layouts_get_interactions()->album_action, "none") != 0) {
        lv_obj_add_flag(r->album, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r->album, player_now_playing_group_click_cb,
                            LV_EVENT_CLICKED, (void *) (intptr_t) 1);
    }
    favorite_circle = r->fav_circle;
    favorite_icon = r->fav_icon;
    quality_pill = r->pill;
    format_badge_label = r->badge;
    pos_label = r->pos;
    dur_label = r->dur;
    song_count_label = r->count;
    order_icon = r->order;
    prev_btn = r->prev;
    play_btn = r->play;
    next_btn = r->next;
    progress_slider = r->progress;
    player_dismiss_btn = r->dismiss;
    volume_slider = r->volume;
}

static void player_bind_lyrics_look(lv_obj_t * scr, const player_roles_t * r) {
    /* Lyrics placement and colors, read once. The area's own text color and
     * alignment style the normal lines, `lyrics_active` the current one. The
     * area must not take taps meant for what lies under it; the lyrics list
     * handles its own. */
    gui_lyrics_look_t lyrics_look = { 0 };
    lyrics_look.active_marker = player_find_role(scr, "lyrics_marker", NULL) != NULL;
    if (r->lyrics_area) {
        lv_style_value_t value;
        lv_obj_remove_flag(r->lyrics_area, LV_OBJ_FLAG_CLICKABLE);
        if (lv_obj_get_local_style_prop(r->lyrics_area, LV_STYLE_TEXT_COLOR, &value, LV_PART_MAIN) == LV_STYLE_RES_FOUND) {
            lyrics_look.has_normal = true;
            lyrics_look.normal = value.color;
        }
        if (lv_obj_get_local_style_prop(r->lyrics_area, LV_STYLE_TEXT_ALIGN, &value, LV_PART_MAIN) == LV_STYLE_RES_FOUND) {
            lyrics_look.has_align = true;
            lyrics_look.align = (lv_text_align_t) value.num;
        }
        lyrics_look.on_empty_tap = player_inline_lyrics_area_empty_tap_cb;
    }
    if (r->lyrics_active) {
        lyrics_look.has_active = true;
        lyrics_look.active = lv_obj_get_style_text_color(r->lyrics_active, LV_PART_MAIN);
    }
    gui_lyrics_set_look(&lyrics_look);
    if (r->lyrics_toggle) {
        lv_obj_add_flag(r->lyrics_toggle, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r->lyrics_toggle, player_inline_lyrics_cover_tap_cb, LV_EVENT_CLICKED, NULL);
    }
    if (player_cover_thumbnail) {
        lv_obj_add_event_cb(player_cover_thumbnail, player_inline_lyrics_cover_tap_cb, LV_EVENT_CLICKED, NULL);
        player_inline_lyrics_update_thumbnail_clickable(player_cover_thumbnail);
    }
}

static void player_style_custom_layout(lv_obj_t * scr, const player_roles_t * r) {
    if (player_layout_builtin) return;
    /* Custom layouts style themselves, but inherit the theme's text colors
     * and the accent so a light/dark or accent change still reaches them.
     * Local XML style properties outrank these shared styles. The built-in
     * creator applies the same styles itself, in its original order. */
    if (player_play_circle) lv_obj_add_style(player_play_circle, gui_theme_accent_style(), 0);
    if (r->transport_color) {
        lv_color_t color = lv_obj_get_style_text_color(r->transport_color, 0);
        lv_obj_t * icons[] = { r->order, r->play, r->prev, r->next, r->more, r->fav_icon,
                              player_find_role(scr, "dismiss_icon", &lv_image_class),
                              player_find_role(scr, "lyrics_icon", &lv_image_class),
                              r->lyrics_toggle && lv_obj_check_type(r->lyrics_toggle, &lv_image_class) ? r->lyrics_toggle : NULL };
        for (unsigned i = 0; i < sizeof(icons) / sizeof(icons[0]); ++i) {
            if (!icons[i]) continue;
            lv_obj_set_style_image_recolor(icons[i], color, 0);
            lv_obj_set_style_image_recolor_opa(icons[i], LV_OPA_COVER, 0);
        }
    }
    lv_obj_t * primary[] = { r->title, r->count };
    lv_obj_t * muted[] = { r->artist, r->album, r->badge, r->pos, r->dur };
    for (unsigned i = 0; i < sizeof(primary) / sizeof(primary[0]); ++i)
        if (primary[i]) lv_obj_add_style(primary[i], gui_player_backdrop_text_style(false), 0);
    for (unsigned i = 0; i < sizeof(muted) / sizeof(muted[0]); ++i)
        if (muted[i]) lv_obj_add_style(muted[i], gui_player_backdrop_text_style(true), 0);
    /* Local recolors (transport_color above) outrank this style. */
    lv_obj_t * glyphs[] = { r->order, r->prev, r->next, r->more };
    for (unsigned i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); ++i)
        if (glyphs[i]) lv_obj_add_style(glyphs[i], gui_player_backdrop_icon_style(), 0);
    backdrop_styles_init();
    lv_obj_add_style(r->progress, player_backdrop_progress_style(lv_obj_check_type(r->progress, &lv_arc_class)), LV_PART_MAIN);
    lv_obj_t * surfaces[] = { player_layout_root, r->cover_card, r->overlay, r->lyrics_backdrop, r->lyrics_dim, r->lyrics_area,
                              r->fav_circle, r->pill, r->count };
    for (unsigned i = 0; i < sizeof(surfaces) / sizeof(surfaces[0]); ++i)
        if (surfaces[i]) lv_obj_add_style(surfaces[i], player_backdrop_surface_style(), 0);
    player_style_named_extras(scr);
    lv_obj_add_style(r->progress, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(r->progress, gui_theme_accent_knob_style(), LV_PART_KNOB);
    if (r->fav_circle) lv_obj_add_style(r->fav_circle, gui_theme_accent_outline_style(), 0);
    lv_obj_t * pressable[] = { r->order, r->play, r->more, r->fav_icon };
    for (unsigned i = 0; i < sizeof(pressable) / sizeof(pressable[0]); ++i)
        if (pressable[i]) lv_obj_add_style(pressable[i], &icon_press_style, LV_STATE_PRESSED);

    /* The built-in creator sets these while it builds, because it sizes
     * its transport row from them. */
    gui_player_prepare_play_btn_images();
    lv_image_set_src(r->play, gui_player_play_btn_image_src(audio_is_playing()));
    if (r->order) lv_image_set_src(r->order, asset_path(play_mode_icon_asset((play_mode_t) current_settings.play_mode)));
    if (!lv_image_get_src(r->cover_img)) lv_image_set_src(r->cover_img, asset_path("playing_plane/default_cover_565.png"));
}

static void player_bind_progress(lv_obj_t * scr, const player_roles_t * r) {
    player_seekbar_set_value(r->progress, 0);
    lv_obj_set_ext_click_area(r->progress, player_y(20));
    lv_obj_add_event_cb(r->progress, progress_slider_event_cb, LV_EVENT_ALL, NULL);
    register_swipe_dead_zone(r->progress);
    if (lv_obj_check_type(r->progress, &lv_arc_class)) {
        lv_obj_add_style(r->progress, gui_theme_accent_style(), LV_PART_INDICATOR);
        lv_obj_add_style(r->progress, gui_theme_accent_knob_style(), LV_PART_KNOB);
        lv_obj_set_style_border_width(r->progress, 0, LV_PART_KNOB);
        lv_obj_set_style_arc_width(r->progress, lv_obj_get_style_arc_width(r->progress, LV_PART_MAIN), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(r->progress, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(r->progress, true, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(r->progress, player_s(5), LV_PART_KNOB);
    }
    lv_obj_t * seek_style = player_find_role(scr, "seek_style", &lv_label_class);
    player_waveform_seekbar = seek_style &&
        player_seekbar_attach(r->progress, lv_label_get_text(seek_style));
    if (player_waveform_seekbar) {
        /* The waveform itself is taller than a normal rail. Keep a 44px
         * target without extending over the last lines of the lyrics. */
        lv_obj_update_layout(r->progress);
        int32_t extra = (44 - lv_obj_get_height(r->progress) + 1) / 2;
        lv_obj_set_ext_click_area(r->progress, extra > 0 ? extra : 0);
    }
}

static void player_bind_transport_hits(lv_obj_t * scr, const player_roles_t * r) {
    lv_obj_update_layout(scr);
    player_inline_lyrics_reset_hit_targets();

    int32_t transport_center_y = 0;
    if (player_layout_builtin) {
#if defined(BOARD_TEMPOTEC_V1)
        lv_obj_t * transport_row = lv_obj_get_parent(r->play);
        transport_center_y = lv_obj_get_y(transport_row) + lv_obj_get_height(transport_row) / 2;
#else
        transport_center_y = player_y(645) + lv_obj_get_height(lv_obj_get_parent(r->play)) / 2;
#endif
    }

    lv_obj_t * order_hit = NULL, * prev_hit = NULL, * next_hit = NULL, * more_hit = NULL;
    if (r->order) {
        order_hit = player_add_hit_target(scr, r->order, player_x(57), player_s(56), transport_center_y,
                                          order_icon_event_cb, lv_palette_main(LV_PALETTE_RED));
        player_forward_press(order_hit, r->order);
    }
    if (r->prev) {
        prev_hit = player_add_skip_hit_target(scr, r->prev, r->prev_ctx, player_x(142), transport_center_y,
                                              prev_btn_event_cb, lv_palette_main(LV_PALETTE_GREEN), -1);
    }

    lv_obj_t * play_hit = player_add_hit_target(scr, player_play_circle ? player_play_circle : r->play,
                                                player_x(240), player_s(94), transport_center_y,
                                                play_btn_event_cb, lv_palette_main(LV_PALETTE_BLUE));
    player_forward_press(play_hit, r->play);

    if (r->next) {
        next_hit = player_add_skip_hit_target(scr, r->next, r->next_ctx, player_x(338), transport_center_y,
                                              next_btn_event_cb, lv_palette_main(LV_PALETTE_ORANGE), 1);
    }

    if (r->more) {
        more_hit = player_add_hit_target(scr, r->more, player_x(423), player_s(56), transport_center_y,
                                         more_icon_event_cb, lv_palette_main(LV_PALETTE_PURPLE));
        player_forward_press(more_hit, r->more);
    }

#ifdef UI_GESTURE_TRACE
    lv_obj_t * traced[] = { order_hit, play_hit, prev_hit, next_hit, more_hit };
    for (unsigned i = 0; i < sizeof(traced) / sizeof(traced[0]); ++i)
        if (traced[i]) lv_obj_add_event_cb(traced[i], debug_transport_btn_all_cb, LV_EVENT_ALL, NULL);
#else
    (void) order_hit; (void) prev_hit; (void) next_hit; (void) more_hit;
#endif

    if (r->fav_circle) {
        lv_area_t favorite_area;
        lv_obj_get_coords(r->fav_circle, &favorite_area);
        int32_t fav_w = lv_area_get_width(&favorite_area);
        int32_t fav_h = lv_area_get_height(&favorite_area);
        int32_t fav_hit_w = fav_w < 44 ? 44 : fav_w;
        int32_t fav_hit_h = fav_h < 44 ? 44 : fav_h;
        lv_obj_t * favorite_hit = player_hit_rect(scr,
                                                  favorite_area.x1 - (fav_hit_w - fav_w) / 2,
                                                  favorite_area.y1 - (fav_hit_h - fav_h) / 2,
                                                  fav_hit_w, fav_hit_h, favorite_icon_event_cb,
                                                  lv_palette_main(LV_PALETTE_PINK));
        if (r->fav_icon) {
            player_forward_press(favorite_hit, r->fav_icon);
        }
        player_inline_lyrics_add_hit_target(favorite_hit);
    }
}

static void player_bind_lyrics_pane(lv_obj_t * scr, const player_roles_t * r, player_inline_lyrics_view_t * view) {
    const bool builtin = player_layout_builtin;
    /* Controls the lyrics view hides, for layouts whose tree is not a flat
     * list of the screen's children. Never a container of the cover or the
     * metadata, which stay visible. */
    player_inline_lyrics_reset_controls();
    if (!builtin) {
        lv_obj_t * controls[] = { r->order, r->prev, r->play, r->next, r->more, r->progress, r->pos, r->dur, r->pill,
                                  r->fav_circle, r->dismiss, r->count };
        lv_obj_t * kept[] = { r->cover_card, r->title, r->artist, r->album };
        for (unsigned i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
            bool hide = controls[i] != NULL;
            for (unsigned k = 0; hide && k < sizeof(kept) / sizeof(kept[0]); ++k)
                if (kept[k] && (kept[k] == controls[i] || player_obj_is_ancestor_of(controls[i], kept[k]) ||
                                player_obj_is_ancestor_of(kept[k], controls[i]))) hide = false;
            if (hide) player_add_lyrics_control(controls[i]);
        }
    }

    /* Named timelines an XML layout may provide; resolved once, here. */
    if (!builtin && player_layout_root) {
        view->lyrics_open = player_layouts_find_timeline(player_layout_root, "lyrics_open");
        view->lyrics_close = player_layouts_find_timeline(player_layout_root, "lyrics_close");
        player_timeline_track_change = player_layouts_find_timeline(player_layout_root, "track_change");
        player_timeline_screen_enter = player_layouts_find_timeline(player_layout_root, "screen_enter");
        if (player_timeline_screen_enter)
            lv_obj_add_event_cb(scr, player_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    }

    lv_obj_t * backdrop = r->lyrics_backdrop ? r->lyrics_backdrop :
        (r->layout_has_overlay && !r->lyrics_dim && !r->lyrics_area ? r->overlay : NULL);
    if (backdrop && !r->lyrics_dim && !r->lyrics_area) {
        /* Static caches may contain backgrounds, never live metadata or
         * controls. A plugin can place these roles anywhere in its tree. */
        lv_obj_t * live[] = { r->cover_card, r->title, r->artist, r->album, r->order, r->prev,
            r->play, r->next, r->more, r->progress, r->pos, r->dur, r->pill, r->fav_circle,
            r->dismiss, r->count, r->volume };
        for (unsigned i = 0; i < sizeof(live) / sizeof(live[0]); ++i) {
            if (live[i] && (live[i] == backdrop || player_obj_is_ancestor_of(backdrop, live[i]))) {
                backdrop = NULL;
                break;
            }
        }
    }
    view->backdrop_contains_cover = backdrop && player_obj_is_ancestor_of(backdrop, r->cover_card);
    player_lyrics_backdrop_bind(backdrop, r->lyrics_dim, r->lyrics_area,
                                view->lyrics_open, view->lyrics_close);
    player_inline_lyrics_bind_pane(view);
}

/* Returns false with nothing assigned or attached when player_find_roles() rejects the tree. */
static bool player_bind_widgets(lv_obj_t * scr) {
    player_count_row_reset();
    player_roles_t r = { 0 };
    if (!player_find_roles(scr, &r)) return false;

    player_publish_roles(scr, &r);
    player_inline_lyrics_view_t inline_view = player_inline_view_for_bind(
        scr, r.cover_fade, r.lyrics_area, NULL, NULL, false);
    player_bind_lyrics_look(scr, &r);
    player_style_custom_layout(scr, &r);

    /* Queue counts must stay on one line even when an XML layout gives the
     * label a narrow fixed width. Keep content-width labels content-sized so
     * their existing layout behavior is preserved. */
    if (r.count) {
        const lv_font_t * count_font = lv_obj_get_style_text_font(r.count, LV_PART_MAIN);
        int32_t line_height = count_font ? lv_font_get_line_height(count_font) : 0;
        int32_t vertical_padding = lv_obj_get_style_pad_top(r.count, LV_PART_MAIN) +
                                   lv_obj_get_style_pad_bottom(r.count, LV_PART_MAIN);
        if (line_height > 0) lv_obj_set_height(r.count, line_height + vertical_padding);

        if (lv_obj_get_style_width(r.count, LV_PART_MAIN) != LV_SIZE_CONTENT)
            row_label_enable_marquee(r.count);
    }

    lv_obj_update_layout(scr);
    player_count_row_capture(r.count, r.pill, r.badge);
    player_count_row_refresh();

    lv_obj_add_flag(r.cover_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r.cover_img, player_inline_lyrics_cover_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(r.cover_card, player_cover_size_changed_cb, LV_EVENT_SIZE_CHANGED, NULL);
    lv_obj_update_layout(scr);
    if (r.cover_fade) {
        lv_color_t fade_color = lv_obj_get_style_bg_color(player_layout_root, LV_PART_MAIN);
        if (!player_cover_fade_attach(r.cover_fade, fade_color)) {
            /* If the mask cannot be allocated, preserve metadata contrast. */
            lv_obj_set_style_bg_color(r.cover_fade, fade_color, 0);
            lv_obj_set_style_bg_opa(r.cover_fade, LV_OPA_60, 0);
        }
    }

    player_inline_lyrics_bind_cover(&inline_view);

    player_label_enable_marquee(r.title);
    if (r.album) player_label_enable_marquee(r.album);
    if (r.artist) player_label_enable_marquee(r.artist);
    if (player_layout_builtin) player_layout_metadata_stack();

    if (r.dismiss) {
        lv_obj_add_flag(r.dismiss, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r.dismiss, library_btn_event_cb, LV_EVENT_CLICKED, NULL);
    }

    player_bind_progress(scr, &r);

    lv_slider_set_range(r.volume, 0, 100);
    lv_slider_set_value(r.volume, audio_clamp_volume_percent((int32_t) (audio_get_volume() * 100.0f + 0.5f)), LV_ANIM_OFF);

    player_bind_transport_hits(scr, &r);
    player_bind_lyrics_pane(scr, &r, &inline_view);

    finalize_screen_navigation(scr);
    lv_obj_add_event_cb(scr, player_inline_lyrics_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    /* Decode future covers at the size this layout shows them. */
    player_cover_raster_set_card(lv_obj_get_width(cover_card), lv_obj_get_height(cover_card));
    fit_cover_img_to_card();
    return true;
}

/* Clears every widget global and cached pointer derived from the tree. Used
 * by teardown and when a layout is discarded. */
static void player_reset_widget_globals(void) {
    lv_draw_sw_image_cache_set_transition_active(false);
    lv_draw_sw_image_cache_set_intermediate_source(NULL);
    player_inline_lyrics_reset();
    if (player_waveform_seekbar) waveform_cancel();
    player_waveform_seekbar = false;
    player_overlay_panel = NULL;
    player_background_img = NULL;
    cover_card = NULL;
    cover_img = NULL;
    artist_label = NULL;
    album_label = NULL;
    song_count_label = NULL;
    song_title_label = NULL;
    quality_pill = NULL;
    format_badge_label = NULL;
    player_count_row_reset();
    order_icon = NULL;
    favorite_circle = NULL;
    favorite_icon = NULL;
    play_btn = NULL;
    prev_btn = NULL;
    next_btn = NULL;
    progress_slider = NULL;
    pos_label = NULL;
    dur_label = NULL;
    player_dismiss_btn = NULL;
    volume_slider = NULL;
    player_layout_root = NULL;
    player_timeline_track_change = NULL;
    player_timeline_screen_enter = NULL;
    player_cover_thumbnail = NULL;
    player_play_circle = NULL;
    gui_lyrics_set_look(NULL);
    player_artist_standin = false;
    player_album_standin = false;
}

static lv_obj_t * build_player_screen(uint32_t screen_width, uint32_t screen_height,
                                      bool refresh_registry) {
    (void) screen_width;
    (void) screen_height;
#ifdef UI_PERF_TRACE
    player_inline_lyrics_register_perf_display_events();
#endif
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    if (refresh_registry) player_layouts_init(player_layout_create_builtin);
    const char * layout_id = player_layouts_effective_id();
    bool built = false;
    if (strcmp(layout_id, PLAYER_LAYOUT_ID_DEFAULT) != 0) {
        player_layout_kind_t kind;
        player_layout_builtin = false;
        player_layout_root = player_layouts_create(layout_id, scr, &kind);
        if (player_layout_root && player_bind_widgets(scr)) {
            built = true;
        } else {
            fprintf(stderr, "Warning: player layout '%s' unusable, using the built-in layout\n", layout_id);
            /* The tree is deleted before its XML component, whose styles it uses. */
            player_timeline_labels_teardown();
            player_lyrics_backdrop_teardown();
            player_inline_lyrics_invalidate_cover_source();
            lv_obj_clean(scr);
            player_reset_widget_globals();
            player_layouts_release();
        }
    }
    if (!built) {
        player_layout_builtin = true;
        player_layout_root = NULL;
        /* Use the registry for Default too, so interaction options and the
         * previous XML tree's action owner cannot survive a layout switch. */
        if (!player_layouts_create(PLAYER_LAYOUT_ID_DEFAULT, scr, NULL)) {
            lv_obj_delete(scr);
            return NULL;
        }
        if (!player_bind_widgets(scr)) {
            lv_obj_delete(scr);
            return NULL;
        }
    }
    return scr;
}

/* Path of whichever song is currently playing, or an empty string if
 * nothing is (or the current track isn't part of the local library, e.g.
 * an Airplay/DLNA source). Set once per real track-start in apply_track_
 * metadata_to_ui(), from that function's own local `path`. This is the
 * single source of truth every now-playing indicator (Artists/Albums/All
 * Songs/group-songs rows) reads from -- each resolves it to a display
 * position via its own DB query (metadata_db_get_group_offset()/
 * metadata_db_get_song_title_offset()/a direct path comparison), rather
 * than an in-memory array index -- see refresh_now_playing_indicators()
 * below. */
char now_playing_path[600] = "";

static void player_now_playing_group_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (lv_obj_get_screen(lv_event_get_target(e)) != player_screen) return;
    /* Metadata remains visible in the lyrics view, but its cover/metadata
     * area belongs to lyrics navigation while that view is open or moving. */
    if (player_inline_lyrics_is_open() || player_inline_lyrics_is_animating() ||
        gui_navigation_transition_in_progress()) return;
    bool album = (intptr_t) lv_event_get_user_data(e) != 0;
    const player_layout_interactions_t * interactions = player_layouts_get_interactions();
    const char * action = album ? interactions->album_action : interactions->artist_action;
    if (strcmp(action, "none") == 0) return;
    if (strcmp(action, "library") != 0) {
        (void) player_layouts_dispatch_action_from(lv_event_get_target(e), action);
        return;
    }
    if (player_queue_model_count() <= 0 || !gui_player_has_active_track() ||
        !now_playing_path[0]) return;
    /* Resolve against the current path at tap time. The library helper first
     * confirms that path is a local indexed song, so remote display metadata
     * cannot accidentally match a similarly named local group. */
    (void) gui_library_open_now_playing_group(now_playing_path, album);
}

/* Set while the shared Group Songs screen represents an album.  The screen
 * is reused for artists, favorites and playlists too, so its title alone
 * cannot identify the source type for persistence. */

/* Resume-but-paused is a true deferred start.  Starting audio and then
 * immediately pausing races the output open on a headphone-less boot; an
 * ALSA failure can consume the queue before the pause lands. */
bool deferred_resume_pending = false;
double deferred_resume_position = 0.0;

/* Queue play mode -- cycled via the order/loop/single/random icon on the
 * player screen (order_icon_event_cb). Persisted as current_settings.play_mode
 * (plain int, see settings.h). Sequential is the only mode where reaching the
 * end of the queue actually stops playback instead of continuing somewhere. */
/* play_mode_t defined in gui.h */

void free_playlist(void) {
    player_next_cover_prefetch_reset();
    player_metadata_invalidate_queue();
    player_queue_model_clear_begin();
    reset_decoder_failure_tracking();
    player_playback_generation_reset();
    player_queue_model_clear_finish();
}

/* Splits a full path into a display title (filename, no extension) and the
 * name of its containing folder. No tag/metadata parsing yet, so the
 * filename is the best "song title" available. */
void get_display_names(const char * path, char * title_out, size_t title_size,
                               char * folder_out, size_t folder_size) {
    if (path && cue_reference_is(path)) {
        cue_sheet_t sheet = {0};
        unsigned ordinal = 0;
        if (cue_reference_resolve(path, &sheet, &ordinal)) {
            const cue_track_t * track = &sheet.tracks[ordinal - 1];
            get_display_names(track->audio_path, title_out, title_size, folder_out, folder_size);
            if (title_out && title_size) {
                if (track->title[0]) utf8_truncate_safe(title_out, track->title, title_size);
                else snprintf(title_out, title_size, TR("Track %d"), track->number);
            }
            cue_sheet_free(&sheet);
            return;
        }
        cue_sheet_free(&sheet);
    }
    if (!path) {
        if (title_out && title_size > 0) title_out[0] = '\0';
        if (folder_out && folder_size > 0) folder_out[0] = '\0';
        return;
    }
    const char * slash = strrchr(path, '/');
    const char * filename = slash ? slash + 1 : path;

    const char * dot = strrchr(filename, '.');
    size_t len = dot ? (size_t) (dot - filename) : strlen(filename);
    if (title_out && title_size > 0) {
        char * tmp = (char *) malloc(len + 1);
        if (tmp) {
            memcpy(tmp, filename, len);
            tmp[len] = '\0';
            utf8_truncate_safe(title_out, tmp, title_size);
            utf8_sanitize(title_out);
            free(tmp);
        } else {
            utf8_truncate_safe(title_out, filename, title_size);
            utf8_sanitize(title_out);
        }
    }

    if (folder_out && folder_size > 0) folder_out[0] = '\0';
    if (slash && folder_out && folder_size > 0) {
        char dir_path[PATH_MAX];
        size_t dir_len = (size_t) (slash - path);
        if (dir_len >= sizeof(dir_path)) dir_len = sizeof(dir_path) - 1;
        memcpy(dir_path, path, dir_len);
        dir_path[dir_len] = '\0';

        const char * folder_slash = strrchr(dir_path, '/');
        const char * folder_name = folder_slash ? folder_slash + 1 : dir_path;
        utf8_truncate_safe(folder_out, folder_name, folder_size);
        utf8_sanitize(folder_out);
    }
}

void refresh_play_btn_icon(void)
{
    gui_player_prepare_play_btn_images();
    set_play_button_state(audio_is_playing());
}

void set_play_button_state(bool is_playing) {
    if (play_btn) lv_image_set_src(play_btn, gui_player_play_btn_image_src(is_playing));
    player_transition_mark_dirty(); /* play_btn lives on player_screen -- see the cache's own doc comment */
    gui_shell_update_quick_drawer_play_state(is_playing);
}

/* Frees the player's rebuildable image caches for an external session: the
 * lyrics-animation cover snapshot (rebuilt on the next lyrics transition),
 * the scaled-cover intermediates (rebuilt by the idle service once a lyrics
 * transition publishes a source again), the generated-cover raster cache
 * (refilled by the next cover decode) and the lyrics backdrop frames. The
 * visible cover and its reflection stay. */
static void player_release_memory(bool quiet_backdrop) {
    player_next_cover_prefetch_reset();
    cover_raster_cache_clear();
    if (player_inline_lyrics_is_animating()) return;
    player_inline_lyrics_release_cache_memory();
    if (quiet_backdrop) player_lyrics_backdrop_release_frames_quiet();
    else player_lyrics_backdrop_release_frames();
}

void gui_player_release_memory_for_external_session(void) {
    player_release_memory(false);
}

/* Screen-off release: the lyrics backdrop recapture is deferred to the wake. */
void gui_player_release_memory_for_screen_off(void) {
    player_release_memory(true);
}

void gui_player_screen_off_release_wake(void) {
    player_lyrics_backdrop_warm_after_wake();
}

static void gui_player_init_internal(uint32_t screen_width, uint32_t screen_height,
                                     bool reuse_playback_state) {
    if (!reuse_playback_state) {
        track_metadata_worker_start();
        /* Pay thread and stack setup during initialization, not inside the first
         * favorite tap where it would visibly block the LVGL event callback. */
        favorite_writer_start();
        player_volume_popup_init();
        build_delete_song_popup();
        build_more_menu_popup();
        gui_track_info_init();
    }
    player_screen = build_player_screen(screen_width, screen_height, !reuse_playback_state);
    player_restore_reload_label_text();
    if (!gui_player_has_active_track() && song_title_label)
        lv_label_set_text(song_title_label, TR("No track loaded"));
    /* A rebuilt label can need the same live values written again even if
     * playback has not crossed a second or progress percentage boundary. */
    displayed_progress_percent = -1;
    displayed_position_second = -1;
    displayed_duration_second = -1;
    if (song_count_label && gui_player_has_active_track()) {
        player_set_song_count(player_queue_model_current(), player_queue_model_count());
        lv_obj_remove_flag(song_count_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (!reuse_playback_state && gui_player_has_active_track()) {
        player_metadata_request_current_for_init();
    }
    /* A UI reload keeps the decoded cover; show it again rather than the
     * placeholder, since the next track of the same album skips decoding. */
    player_cover_show_retained();
    /* Apply any player_layout_config already set (a plugin's top-level code
     * runs before this on boot/reload, per gui_plugin_set_player_layout()'s
     * own comment) to the freshly-built overlay right away, rather than
     * waiting for the next track's cover decode to pick it up. No-ops
     * cleanly if no config was ever set or no cover is loaded yet. */
    refresh_format_badge();
    gui_player_refresh_frosted_background();
}

void gui_player_init(uint32_t screen_width, uint32_t screen_height) {
    gui_player_init_internal(screen_width, screen_height, false);
}

/* For gui_reload.c's in-process UI reload -- deletes every screen/popup this
 * module owns so gui_player_init() can rebuild them from a clean slate
 * without leaking the old objects. volume_popup/delete_song_popup/
 * more_menu_popup and their backdrops are built directly on lv_layer_top()
 * (see build_confirm_popup()'s own comment), not as children of
 * player_screen, so deleting player_screen alone would not reach them. */
static void gui_player_teardown_internal(bool preserve_shared_state) {
    if (!preserve_shared_state) {
        player_next_cover_prefetch_reset();
        player_reflection_cache_clear();
        player_artwork_jobs_partial_clear();
        player_artwork_jobs_advance_generation();
        player_metadata_invalidate_ui();
    }
    /* Also cancel a pending fallback completion after an already-settled
     * transition, before any timeline or widget can be replaced. */
    player_inline_lyrics_cancel_timer();
    player_cover_cache_timer_delete();
    lv_draw_sw_image_cache_set_transition_active(false);
    lv_draw_sw_image_cache_set_intermediate_source(NULL);
    player_inline_lyrics_set_open(false, false);
    player_timeline_labels_teardown();
    player_lyrics_backdrop_teardown();
    player_inline_lyrics_generated_release(false);
    if (!preserve_shared_state) {
        player_volume_popup_teardown();
        player_more_menu_teardown();
        gui_track_info_teardown();
    }
    /* Capture while the old labels are still alive. The audio state and
     * current track continue across a UI-only reload. */
    player_capture_reload_label_text();
    /* Unbind retained cover pixels while their widgets are still alive. */
    player_inline_lyrics_invalidate_cover_source();
    /* The swipe registry outlives a layout-only rebuild. Remove its borrowed
     * seekbar pointer before deleting the widget, or the next touch walks
     * freed memory while resolving its screen. */
    if (progress_slider) unregister_swipe_dead_zone(progress_slider);
    if (player_screen) { lv_obj_delete(player_screen); player_screen = NULL; }
    /* player_overlay_panel/cover_img are descendants of player_screen, so
     * lv_obj_delete() above already destroyed them -- null the pointers too
     * so gui_player_refresh_frosted_background() (reachable from a plugin's
     * own top-level code via plugin.set_player_layout(), which can run
     * before gui_player_init() rebuilds this screen on reload) sees its
     * existing '!player_overlay_panel' guard correctly no-op instead of
     * writing LVGL styles to freed memory. */
    player_reset_widget_globals();
    /* An XML layout's styles live in its registered component, which must
     * outlive the widgets using them -- now gone. */
    player_layouts_release();
}

void gui_player_teardown(void) {
    gui_player_teardown_internal(false);
}

static bool player_layout_rebuild_ready(bool was_active) {
    if (player_artwork_jobs_active() || player_artwork_jobs_pending() ||
        player_artwork_jobs_reload_active() ||
        gui_player_queue_write_busy() || player_queue_storage_save_active() ||
        gui_navigation_transition_in_progress() || gui_player_lyrics_animation_in_progress() ||
        player_delete_song_popup_is_visible() ||
        (volume_popup && !lv_obj_has_flag(volume_popup, LV_OBJ_FLAG_HIDDEN)) ||
        player_more_menu_is_visible())
        return false;

    /* Player input owns these flags. Only inspect active inputs when Player
     * itself is visible, so a drag on a covered library row does not defer its
     * layout refresh. This also covers an in-progress lyrics-pane drag. */
    if (was_active) {
        if (player_seek_input_progress_busy()) return false;
        for (lv_indev_t * input = lv_indev_get_next(NULL); input; input = lv_indev_get_next(input))
            if (lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED) return false;
    }
    return true;
}

/* A layout rebuild is a UI-only operation. Keep the current player frame on
 * screen while replacing its widget tree, and leave other screens (including
 * Track Information) alone when Player is covered. */
bool gui_player_rebuild_layout(uint32_t screen_width, uint32_t screen_height) {
    lv_obj_t * old_screen = player_screen;
    if (!old_screen || screen_width == 0 || screen_height == 0) return false;
    lv_display_t * display = lv_obj_get_display(old_screen);
    bool was_active = display && lv_display_get_screen_active(display) == old_screen;
    if (!player_layout_rebuild_ready(was_active)) return false;

    bool lyrics_were_open = player_inline_lyrics_is_open();
    bool was_deferred_resume = deferred_resume_pending;
    int deferred_progress_percent = progress_slider ? player_seekbar_get_value(progress_slider) : 0;
    char deferred_position[32] = "";
    char deferred_duration[32] = "";
    if (was_deferred_resume) {
        snprintf(deferred_position, sizeof(deferred_position), "%s",
                 pos_label ? lv_label_get_text(pos_label) : "");
        snprintf(deferred_duration, sizeof(deferred_duration), "%s",
                 dur_label ? lv_label_get_text(dur_label) : "");
    }

    lv_draw_buf_t * frozen_frame = NULL;
    lv_obj_t * frozen_screen = NULL;
    if (was_active) {
        /* A failed snapshot must leave the functioning old player in place. */
        frozen_frame = lv_snapshot_take(old_screen, LV_COLOR_FORMAT_RGB565);
        if (!frozen_frame) return false;
        frozen_screen = lv_obj_create(NULL);
        if (!frozen_screen) {
            lv_draw_buf_destroy(frozen_frame);
            return false;
        }
        lv_obj_remove_style_all(frozen_screen);
        lv_obj_set_style_bg_opa(frozen_screen, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(frozen_screen, lv_color_black(), 0);
        lv_obj_set_style_border_width(frozen_screen, 0, 0);
        lv_obj_set_style_pad_all(frozen_screen, 0, 0);
        lv_obj_remove_flag(frozen_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(frozen_screen, screen_width, screen_height);
        lv_obj_t * image = lv_image_create(frozen_screen);
        if (!image) {
            lv_obj_delete(frozen_screen);
            lv_draw_buf_destroy(frozen_frame);
            return false;
        }
        lv_image_set_src(image, frozen_frame);
        lv_obj_set_size(image, screen_width, screen_height);
        lv_image_set_inner_align(image, LV_IMAGE_ALIGN_STRETCH);
        lv_obj_align(image, LV_ALIGN_CENTER, 0, 0);
        lv_screen_load(frozen_screen);
        /* Let the normal LVGL refresh present the completed replacement.
         * Never re-enter display refresh from this reload timer callback. */
    }

    gui_player_teardown_internal(true);
    gui_player_init_internal(screen_width, screen_height, true);
    lv_obj_t * new_screen = player_screen;
    LV_ASSERT_NULL(new_screen);
    if (!new_screen) abort(); /* gui_player_init() promises a built screen. */

    /* Restore state before navigation loads the replacement screen, so its
     * first transition snapshot sees the same playback state as the old UI. */
    set_play_button_state(audio_is_playing());
    show_favorite_state(favorite_is_set);
    if (was_deferred_resume) {
        if (progress_slider) player_seekbar_set_value(progress_slider, deferred_progress_percent);
        displayed_progress_percent = deferred_progress_percent;
        if (pos_label) lv_label_set_text(pos_label, deferred_position);
        if (dur_label) lv_label_set_text(dur_label, deferred_duration);
    }
    gui_player_update_progress();
    if (lyrics_were_open) player_inline_lyrics_set_open(true, false);

    gui_navigation_replace_player_screen(old_screen, new_screen, was_active);
    player_cover_resync_raster();
    sync_player_topbar_visibility(lv_screen_active());
    if (frozen_screen) {
        lv_obj_delete(frozen_screen);
        lv_image_cache_drop(frozen_frame);
        lv_draw_buf_destroy(frozen_frame);
    }
    return true;
}

void gui_player_refresh_static_assets(void) {
    player_transport_images_reset();
    refresh_play_btn_icon();
    player_volume_popup_refresh_assets();
}

void gui_format_time(double seconds, char * buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    if (seconds <= 0.0) {
        snprintf(buf, buf_size, "0:00");
        return;
    }
    uint64_t total = (uint64_t) (seconds + 0.5);
    uint64_t hours = total / 3600;
    unsigned int minutes = (unsigned int) ((total % 3600) / 60);
    unsigned int secs = (unsigned int) (total % 60);
    if (hours > 0)
        snprintf(buf, buf_size, "%llu:%02u:%02u", (unsigned long long) hours, minutes, secs);
    else
        snprintf(buf, buf_size, "%u:%02u", minutes, secs);
}

/* Queue reconciliation must run even with the display off or while seeking.
 * Only drawing the published result belongs in the visual refresh path. */
void gui_player_poll_waveform(void) {
    if (!player_waveform_seekbar || !progress_slider) {
        waveform_cancel();
        return;
    }
    const char * path = gui_player_get_current_track_path();
    waveform_request(path);
    /* Use the same prediction as natural playback, including explicit
     * queued tracks, shuffle bags and repeat. This also works with
     * gapless disabled, where audio.c does not have a staged decoder. */
    int next_index = audio_is_playing() && gui_player_has_active_track() ?
                     compute_auto_advance_index(player_queue_model_current()) : -1;
    waveform_prefetch(next_index >= 0 && next_index < player_queue_model_count() ?
                      playlist_path_at(next_index) : NULL);
}

void gui_player_update_progress(void) {
    if (player_waveform_seekbar && progress_slider) {
        const char * path = gui_player_get_current_track_path();
        waveform_data_t data;
        player_seekbar_update(progress_slider, waveform_copy(path, &data) ? &data : NULL);
    }
    if (deferred_resume_pending) return; /* audio_get_position_seconds()/
        audio_get_duration_seconds() are both meaningless here -- no decoder is open
        yet. Preserve prepare_deferred_resume()'s seeded slider/label values until the
        user presses Play (which clears deferred_resume_pending and opens the real
        decoder, at which point this function's normal live-value logic takes over
        correctly on its own). */
    double position = audio_get_position_seconds();
    double duration = audio_get_duration_seconds();

    if (player_seek_input_waiting_for_position(position)) return;

    if (duration > 0 && progress_slider) {
        int32_t percent = (int32_t) ((position / duration) * 100.0);
        if (percent != displayed_progress_percent) {
            displayed_progress_percent = percent;
            player_seekbar_set_value(progress_slider, percent);
        }
    }

    int position_second = (int) position;
    int duration_second = (int) duration;
    if (pos_label && position_second != displayed_position_second) {
        char pos_str[24];
        displayed_position_second = position_second;
        gui_format_time(position, pos_str, sizeof(pos_str));
        lv_label_set_text(pos_label, pos_str);
    }
    if (dur_label && duration_second != displayed_duration_second) {
        char dur_str[24];
        displayed_duration_second = duration_second;
        if (duration > 0) gui_format_time(duration, dur_str, sizeof(dur_str));
        else snprintf(dur_str, sizeof(dur_str), "--:--");
        lv_label_set_text(dur_label, dur_str);
    }
}

bool gui_player_has_background_work(void) {
    return player_artwork_jobs_active() || gui_player_queue_write_busy() || player_queue_storage_save_active();
}

void gui_player_cancel_background_work(void) {
    player_next_cover_prefetch_reset();
    player_queue_storage_save_cancel_and_join();
    if (player_artwork_jobs_active()) player_artwork_jobs_cancel_worker();
    if (player_artwork_jobs_reload_active())
        gui_player_cancel_cover_reload();
}

lv_obj_t * gui_player_get_screen(void) {
    return player_screen;
}

bool gui_player_is_seeking(void) {
    return user_seeking;
}

int32_t gui_player_get_volume_percent(void) {
    return audio_clamp_volume_percent(volume_slider ? lv_slider_get_value(volume_slider) : (int32_t)(audio_get_volume() * 100.0f + 0.5f));
}

void gui_player_set_volume_percent(int32_t percent) {
    /* External/hardware writes are newer than any coalesced drag sample. */
    player_volume_popup_cancel_pending();
    percent = audio_clamp_volume_percent(percent);
    if (volume_slider) lv_slider_set_value(volume_slider, percent, LV_ANIM_OFF);
}

void gui_player_remember_volume_percent(int32_t percent) {
    percent = audio_clamp_volume_percent(percent);
    if (current_settings.car_mode_enabled) {
        current_settings.car_mode_volume_percent = percent;
        gui_settings_sync_car_mode();
    } else {
        current_settings.volume = (float) percent / 100.0f;
    }
}

void gui_player_set_car_mode_enabled(bool enabled) {
    if (current_settings.car_mode_enabled != enabled) {
        if (enabled) current_settings.volume = audio_get_volume();
        current_settings.car_mode_enabled = enabled;
        float volume = enabled ? (float) current_settings.car_mode_volume_percent / 100.0f
                               : current_settings.volume;
        int32_t percent = audio_clamp_volume_percent((int32_t) (volume * 100.0f + 0.5f));
        volume = (float) percent / 100.0f;
        gui_player_set_volume_percent(percent);
        audio_set_volume(volume);
        refresh_volume_topbar(percent);
        settings_save_async(&current_settings);
    }
    gui_settings_sync_car_mode();
    gui_shell_refresh_quick_drawer_expansion_toggles();
}

bool gui_player_volume_is_being_adjusted(void) {
    return player_volume_popup_is_dragged() ||
           (volume_slider && lv_slider_is_dragged(volume_slider));
}

const char * gui_player_get_now_playing_title(void) {
    return player_track_title_text ? player_track_title_text : "";
}

const char * gui_player_get_now_playing_folder(void) {
    return player_track_artist_text ? player_track_artist_text : "";
}

const char * gui_player_get_now_playing_album(void) {
    return player_track_album_text ? player_track_album_text : "";
}

void gui_player_sync_topbar_visibility(lv_obj_t * screen) {
    if (player_dismiss_btn) {
        if (player_inline_lyrics_is_open() || player_inline_lyrics_is_animating() ||
            (current_settings.hide_player_topbar && screen == player_screen))
            lv_obj_add_flag(player_dismiss_btn, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(player_dismiss_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

void gui_player_refresh_font_geometry(void) {
    /* Only the built-in layout is positioned by code; any other layout owns
     * the geometry of its labels. */
    if (player_layout_builtin) player_layout_metadata_stack();
}

lv_obj_t * gui_player_get_dismiss_btn(void) {
    return player_dismiss_btn;
}

