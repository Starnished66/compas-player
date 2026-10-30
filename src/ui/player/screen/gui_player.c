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
/* Set by on_file_selected_stay(): the next explicit start plays without
 * opening the player screen (a plugin starting audio from its own screen). */
static bool stay_on_screen_once;
static char transport_skip_directory[PATH_MAX];
static int transport_skip_seconds;
static bool transport_skip_manual(int direction);
static char now_playing_genre[128];
static int now_playing_track_number;
static int consecutive_decoder_failure_skips = 0;
#define MAX_FAILED_PHYSICAL_PATHS 5
static char failed_physical_paths[MAX_FAILED_PHYSICAL_PATHS][PATH_MAX];
static int failed_physical_paths_count = 0;
extern bool plugin_now_playing_loaded;
static uint64_t current_playback_generation = 0;
static uint64_t metadata_ui_token, metadata_next_token, metadata_next_queue_revision;
static uint64_t artwork_dispatched_metadata_token;
static uint64_t metadata_scalar_applied_token;
static char artwork_dispatched_artist[128], artwork_dispatched_album[128], artwork_dispatched_album_artist[128];
#ifdef UI_PERF_TRACE
static uint64_t metadata_request_perf_us;
#endif
static uint64_t metadata_ui_audio_generation, metadata_next_audio_generation;
static uint64_t favorite_ui_revision, metadata_favorite_revision;
static uint64_t favorite_request_token, favorite_request_revision;
static char favorite_request_path[PATH_MAX];
static bool favorite_track_known, favorite_toggle_pending;
static bool metadata_ui_notify, metadata_notify_ready;
static track_metadata_t metadata_notify_payload;
static bool selection_metadata_seed_valid;
static char selection_metadata_seed_path[PATH_MAX];
static track_metadata_t selection_metadata_seed;
static char metadata_ui_path[PATH_MAX], metadata_next_current_path[PATH_MAX];
static int metadata_next_gain_mode;
static bool metadata_next_gapless_enabled;
static char armed_next_path[PATH_MAX];
static uint64_t armed_next_generation;
static int armed_next_gain_mode;
static bool next_metadata_cached;
static char next_metadata_cached_path[PATH_MAX];
static track_metadata_t next_metadata_cache;
static uint64_t next_cover_prefetch_token;

static int open_sd_queue_dir(void);
static int open_queue_dir_from_snapshot(const struct tagcache_snapshot *snapshot);
static int open_queue_dir_from_file_index(const file_browser_index_t *index);
static bool is_sd_card_path(const char *path);

#include "../../../audio/engine/audio_helpers.h"
#include "../../input/hw_volume_coalesce.h"
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
#ifdef HOST_BUILD
#define MUSIC_ROOT_DIR "./music"
#else
#define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif
#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"
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
#include "../../text/rendering/fallback_font.h"
#include "../../../network/http/client/http_client.h"
#include "../../widgets/builders/screen_builders.h"
static void notify_plugin_track_started(const track_metadata_t * meta, const char * path, double duration);
static bool is_http_url(const char *path);

#include "../../../library/metadata/db/metadata_db.h"
#include "../../../library/artwork/decode/cover_decode.h"
#include "../../../core/system/logging/db_log.h"
static track_metadata_request_t metadata_ui_request;
#include "../../../network/airplay/airplay_control.h"
#include "../../../library/artwork/albumart/albumart.h"
#include "../../../audio/engine/audio.h"
#include "../../../core/persistence/settings.h"
#include "../../assets/assets.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "../../../hardware/device/device_config.h"
#include "../../../core/persistence/storage_paths.h"
#include "../../../plugins/manager/plugin_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include "../../../library/metadata/parsing/metadata.h"
#include <ctype.h>
#include <math.h>
#include <sys/stat.h>

#define VOLUME_POPUP_TIMEOUT_MS 3000

static lv_obj_t * player_screen = NULL;
lv_obj_t * player_dismiss_btn = NULL;
lv_obj_t * player_overlay_panel = NULL;
lv_obj_t * player_background_img = NULL;
lv_obj_t * cover_card = NULL;
lv_obj_t * cover_img = NULL;
lv_obj_t * artist_label = NULL;
lv_obj_t * album_label = NULL;
lv_obj_t * song_quality_label = NULL;
lv_obj_t * song_bitrate_label = NULL;
lv_obj_t * song_track_label = NULL;
lv_obj_t * song_count_label = NULL;
lv_obj_t * song_title_label = NULL;
lv_obj_t * quality_pill = NULL;
lv_obj_t * format_badge_label = NULL;
typedef struct {
    bool valid;
    int32_t count_x, count_width;
    int32_t pill_x, pill_width, gap;
} player_count_row_geometry_t;
static player_count_row_geometry_t player_count_row_geometry;
lv_obj_t * play_mode_img = NULL;
static lv_obj_t * order_icon = NULL;
lv_obj_t * favorite_circle = NULL;
lv_obj_t * favorite_icon = NULL;
lv_obj_t * play_btn = NULL;
lv_obj_t * prev_btn = NULL;
lv_obj_t * next_btn = NULL;
lv_obj_t * progress_slider = NULL;
static bool player_waveform_seekbar;
lv_obj_t * progress_label = NULL;
lv_obj_t * duration_label = NULL;
lv_obj_t * volume_slider = NULL;

static lv_obj_t * pos_label = NULL;
static lv_obj_t * dur_label = NULL;

static void player_count_row_refresh(void);

/* Keep the original count and badge geometry as the baseline. Each update is
 * recomputed from that snapshot so changing queue or format text cannot cause
 * cumulative position or width drift. */
static void player_set_song_count(int index, int total) {
    if (!song_count_label) return;
    char text[48];
    snprintf(text, sizeof(text), "%d/%d", index + 1, total);
    if (ui_label_text_matches(song_count_label, text)) return;
    lv_label_set_text(song_count_label, text);
    player_count_row_refresh();
}

static void player_set_format_badge_text(const char * text) {
    if (!format_badge_label) return;
    if (ui_label_text_matches(format_badge_label, text)) return;
    lv_label_set_text(format_badge_label, text);
    player_count_row_refresh();
}

static int32_t player_label_text_width(lv_obj_t * label) {
    if (!label) return 0;
    const char * text = lv_label_get_text(label);
    const lv_font_t * font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    if (!text || !font || !text[0]) return 0;
    lv_point_t size;
    lv_text_get_size(&size, text, font,
                     lv_obj_get_style_text_letter_space(label, LV_PART_MAIN),
                     lv_obj_get_style_text_line_space(label, LV_PART_MAIN),
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x > 0 ? size.x : 0;
}

static void player_count_row_refresh(void) {
    player_count_row_geometry_t * g = &player_count_row_geometry;
    if (!g->valid || !song_count_label || !quality_pill || !format_badge_label) return;

    const int32_t count_text_width = player_label_text_width(song_count_label);
    const int32_t count_content_width = count_text_width + (count_text_width > 0 ? 1 : 0) +
        lv_obj_get_style_pad_left(song_count_label, LV_PART_MAIN) +
        lv_obj_get_style_pad_right(song_count_label, LV_PART_MAIN) +
        2 * lv_obj_get_style_border_width(song_count_label, LV_PART_MAIN);
    int32_t badge_text_width = player_label_text_width(format_badge_label);
    if (badge_text_width == 0) {
        /* An empty badge keeps the full authored slot available for a later
         * format update, avoiding a needless jump when metadata arrives. */
        badge_text_width = g->pill_width;
    } else {
        badge_text_width += lv_obj_get_style_pad_left(format_badge_label, LV_PART_MAIN) +
                            lv_obj_get_style_pad_right(format_badge_label, LV_PART_MAIN) +
                            2 * lv_obj_get_style_border_width(format_badge_label, LV_PART_MAIN) +
                            lv_obj_get_style_pad_left(quality_pill, LV_PART_MAIN) +
                            lv_obj_get_style_pad_right(quality_pill, LV_PART_MAIN) +
                            2 * lv_obj_get_style_border_width(quality_pill, LV_PART_MAIN);
    }

    int32_t count_width = count_content_width > g->count_width ? count_content_width : g->count_width;
    int32_t growth_limit = g->pill_width > badge_text_width ? g->pill_width - badge_text_width : 0;
    if (count_width > g->count_width + growth_limit)
        count_width = g->count_width + growth_limit;

    int32_t pill_x = g->count_x + count_width + g->gap;
    int32_t pill_width = g->pill_x + g->pill_width - pill_x;
    lv_obj_set_width(song_count_label, count_width);
    lv_obj_set_x(quality_pill, pill_x);
    lv_obj_set_width(quality_pill, pill_width);
}

static void player_count_row_capture(lv_obj_t * count, lv_obj_t * pill, lv_obj_t * badge) {
    player_count_row_geometry = (player_count_row_geometry_t){0};
    if (!count || !pill || !badge || lv_obj_get_parent(count) != lv_obj_get_parent(pill) ||
        lv_obj_get_parent(badge) != pill || lv_obj_get_child_count(pill) != 1) return;
    lv_obj_t * parent = lv_obj_get_parent(count);
    if (!parent || lv_obj_get_style_layout(parent, LV_PART_MAIN) != LV_LAYOUT_NONE) return;
    lv_align_t count_align = lv_obj_get_style_align(count, LV_PART_MAIN);
    lv_align_t pill_align = lv_obj_get_style_align(pill, LV_PART_MAIN);
    lv_align_t badge_align = lv_obj_get_style_align(badge, LV_PART_MAIN);
    if ((count_align != LV_ALIGN_DEFAULT && count_align != LV_ALIGN_TOP_LEFT) ||
        (pill_align != LV_ALIGN_DEFAULT && pill_align != LV_ALIGN_TOP_LEFT) ||
        (badge_align != LV_ALIGN_DEFAULT && badge_align != LV_ALIGN_TOP_LEFT) ||
        lv_obj_get_style_x(count, LV_PART_MAIN) != lv_obj_get_x(count) ||
        lv_obj_get_style_y(count, LV_PART_MAIN) != lv_obj_get_y(count) ||
        lv_obj_get_style_x(pill, LV_PART_MAIN) != lv_obj_get_x(pill) ||
        lv_obj_get_style_y(pill, LV_PART_MAIN) != lv_obj_get_y(pill) ||
        !LV_COORD_IS_PX(lv_obj_get_style_x(count, LV_PART_MAIN)) ||
        !LV_COORD_IS_PX(lv_obj_get_style_y(count, LV_PART_MAIN)) ||
        !LV_COORD_IS_PX(lv_obj_get_style_x(pill, LV_PART_MAIN)) ||
        !LV_COORD_IS_PX(lv_obj_get_style_y(pill, LV_PART_MAIN)) ||
        lv_obj_get_style_x(badge, LV_PART_MAIN) != 0 ||
        !LV_COORD_IS_PCT(lv_obj_get_style_width(badge, LV_PART_MAIN)) ||
        LV_COORD_GET_PCT(lv_obj_get_style_width(badge, LV_PART_MAIN)) != 100) return;
    int32_t count_authored_width = lv_obj_get_style_width(count, LV_PART_MAIN);
    int32_t pill_authored_width = lv_obj_get_style_width(pill, LV_PART_MAIN);
    if (!LV_COORD_IS_PX(count_authored_width) || count_authored_width <= 0 ||
        !LV_COORD_IS_PX(pill_authored_width) || pill_authored_width <= 0) return;
    int32_t count_x = lv_obj_get_x(count), pill_x = lv_obj_get_x(pill);
    int32_t count_width = lv_obj_get_width(count), pill_width = lv_obj_get_width(pill);
    int32_t gap = pill_x - (count_x + count_width);
    if (count_width <= 0 || pill_width <= 0 || gap < 0 ||
        lv_obj_get_y(count) + lv_obj_get_height(count) <= lv_obj_get_y(pill) ||
        lv_obj_get_y(pill) + lv_obj_get_height(pill) <= lv_obj_get_y(count)) return;

    player_count_row_geometry = (player_count_row_geometry_t){
        .valid = true,
        .count_x = count_x,
        .count_width = count_width,
        .pill_x = pill_x,
        .pill_width = pill_width,
        .gap = gap,
    };
}

/* Player layout state, see the layout section above build_player_screen().
 * Declared here because the metadata and lyrics code read it. */
static bool player_layout_builtin = true;
static lv_obj_t * player_layout_root;
static lv_anim_timeline_t * player_timeline_track_change;
static lv_anim_timeline_t * player_timeline_screen_enter;
/* Optional custom-layout roles. The thumbnail shares the decoded cover;
 * the play circle provides a larger hit target around the stock play icon. */
static lv_obj_t * player_cover_thumbnail;
static lv_obj_t * player_play_circle;
static lv_timer_t * player_artwork_cache_timer;
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

static void player_set_track_display_text(const char * title, const char * artist,
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

lv_obj_t * more_menu_popup = NULL;
lv_obj_t * more_menu_popup_backdrop = NULL;

typedef struct {
    uint32_t registry_generation;
    int index;
} player_plugin_menu_callback_t;

static player_plugin_menu_callback_t player_plugin_menu_callbacks[PLUGIN_MAX_PLAYER_LIST_ITEMS];

static gui_popup_t delete_song_popup;
static lv_obj_t * delete_song_popup_title = NULL;

static lv_image_dsc_t current_cover_dsc;

static uint8_t * current_cover_bytes = NULL;
static int current_cover_for_index = -1;
static cover_file_id_t current_cover_file;
static audio_current_format_info_t player_cached_format;
static unsigned int cover_decode_partial_applied_generation;

static uint8_t * current_reflection_bytes = NULL;
static lv_image_dsc_t current_reflection_dsc;
static uint64_t current_cover_generation;
static struct {
    bool valid;
    const uint8_t * cover_bytes;
    uint64_t cover_generation;
    int blur_radius, blur_passes, darken_num, darken_den;
    uint32_t tint_rgb;
    int tint_weight;
} current_reflection_key;



extern lv_obj_t * volume_topbar_btn;
extern lv_obj_t * volume_topbar_label;
extern lv_obj_t * status_bar;
extern lv_obj_t * gui_settings_get_eq_screen();
extern player_settings_t current_settings;
extern bool favorite_is_set;

extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void generic_back_cb(lv_event_t * e);
extern void quick_drawer_mark_snapshot_dirty(void);
extern void clear_player_source(void);
extern void prev_btn_event_cb(lv_event_t * e);
extern void play_btn_event_cb(lv_event_t * e);
extern void next_btn_event_cb(lv_event_t * e);
extern void play_mode_tap_event_cb(lv_event_t * e);
extern void play_mode_long_press_cb(lv_event_t * e);
extern void slider_event_cb(lv_event_t * e);
extern const char * play_mode_icon_asset(play_mode_t mode);

/* Resolves effective frosted-glass layout parameters from player_layout_config,
 * applying defaults and defensive clamping. Reads the global layout config once
 * into a local snapshot to avoid data races with UI thread updates. */
static player_frost_params_t resolve_player_frost_params(void) {
    player_layout_config_t cfg = player_layout_config;
    player_frost_params_t params = {
        .flat = false,
        .has_bg_color = false,
        .bg_color = 0,
        .blur_radius = REFLECTION_BLUR_RADIUS,
        .blur_passes = REFLECTION_BLUR_PASSES,
        .darken_num = REFLECTION_DARKEN_NUM,
        .darken_den = REFLECTION_DARKEN_DEN,
    };

    if (cfg.configured) {
        if (cfg.flat) {
            params.flat = true;
            params.has_bg_color = cfg.has_bg_color;
            params.bg_color = cfg.bg_color;
            return params;
        }
        if (cfg.has_blur_radius) {
            params.blur_radius = cfg.blur_radius;
            if (params.blur_radius < 0) params.blur_radius = 0;
            if (params.blur_radius > 64) params.blur_radius = 64;
        }
        if (cfg.has_blur_passes) {
            params.blur_passes = cfg.blur_passes;
            if (params.blur_passes < 0) params.blur_passes = 0;
            if (params.blur_passes > 16) params.blur_passes = 16;
        }
        if (cfg.has_darken) {
            params.darken_num = cfg.darken_num;
            params.darken_den = cfg.darken_den;
            if (params.darken_num < 0) params.darken_num = 0;
            if (params.darken_num > 64) params.darken_num = 64;
            if (params.darken_den < 1) params.darken_den = 1;
            if (params.darken_den > 64) params.darken_den = 64;
        }
    }

    /* Like the stock player, the frost follows the theme's lightness: a dark
     * theme keeps the darkened artwork, a light theme washes it out toward
     * its own screen color instead. */
    lv_style_value_t screen;
    if (lv_style_get_prop(&style_theme_screen_bg, LV_STYLE_BG_COLOR, &screen) == LV_STYLE_RES_FOUND &&
        !gui_theme_prefers_light_text(screen.color)) {
        params.darken_num = 1;
        params.darken_den = 1;
        params.tint_rgb = lv_color_to_u32(screen.color) & 0xFFFFFF;
        params.tint_weight = REFLECTION_LIGHT_TINT_WEIGHT;
    }
    return params;
}

static bool reflection_matches(const player_frost_params_t * params) {
    return current_reflection_bytes && current_reflection_key.valid &&
           current_reflection_key.cover_bytes == current_cover_bytes &&
           current_reflection_key.cover_generation == current_cover_generation &&
           current_reflection_key.blur_radius == params->blur_radius &&
           current_reflection_key.blur_passes == params->blur_passes &&
           current_reflection_key.darken_num == params->darken_num &&
           current_reflection_key.darken_den == params->darken_den &&
           current_reflection_key.tint_rgb == params->tint_rgb &&
           current_reflection_key.tint_weight == params->tint_weight;
}

/* Clear all LVGL references before releasing reflection storage. The quick
 * drawer can hold its own image source for this same descriptor. */
static void drop_current_reflection(void) {
    bool had_state = current_reflection_bytes || current_reflection_dsc.data ||
                     current_reflection_key.valid;
    if (player_background_img) {
        lv_obj_add_flag(player_background_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(player_background_img, NULL);
    }
    lv_image_cache_drop(&current_reflection_dsc);
    uint8_t * old = current_reflection_bytes;
    current_reflection_bytes = NULL;
    memset(&current_reflection_dsc, 0, sizeof(current_reflection_dsc));
    memset(&current_reflection_key, 0, sizeof(current_reflection_key));
    if (had_state) gui_shell_refresh_quick_drawer_cover();
    free(old);
}

static void set_current_reflection_key(const player_frost_params_t * params) {
    current_reflection_key.valid = current_reflection_bytes != NULL;
    current_reflection_key.cover_bytes = current_cover_bytes;
    current_reflection_key.cover_generation = current_cover_generation;
    current_reflection_key.blur_radius = params->blur_radius;
    current_reflection_key.blur_passes = params->blur_passes;
    current_reflection_key.darken_num = params->darken_num;
    current_reflection_key.darken_den = params->darken_den;
    current_reflection_key.tint_rgb = params->tint_rgb;
    current_reflection_key.tint_weight = params->tint_weight;
}

static void attach_current_reflection(void) {
    if (!current_reflection_bytes || !player_background_img) return;
    memset(&current_reflection_dsc, 0, sizeof(current_reflection_dsc));
    current_reflection_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    current_reflection_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    current_reflection_dsc.header.w = REFLECTION_WIDTH;
    current_reflection_dsc.header.h = BOARD_SCREEN_HEIGHT;
    current_reflection_dsc.header.stride = REFLECTION_WIDTH * 2;
    current_reflection_dsc.data = current_reflection_bytes;
    current_reflection_dsc.data_size = (uint32_t) REFLECTION_WIDTH * BOARD_SCREEN_HEIGHT * 2;
    lv_image_set_src(player_background_img, &current_reflection_dsc);
    lv_obj_set_pos(player_background_img, 0, 0);
    lv_obj_set_size(player_background_img, REFLECTION_WIDTH, BOARD_SCREEN_HEIGHT);
    lv_image_set_pivot(player_background_img, 0, 0);
    lv_image_set_scale_x(player_background_img, LV_SCALE_NONE);
    lv_image_set_scale_y(player_background_img, LV_SCALE_NONE);
    lv_obj_remove_flag(player_background_img, LV_OBJ_FLAG_HIDDEN);
    gui_player_update_backdrop_contrast();
}

/* Text on the Now Playing backdrop follows what is behind it, not the theme:
 * the backdrop is cover artwork (blurred and darkened by default) or a
 * layout's flat color, either of which a light theme's text can vanish on. */
static lv_style_t backdrop_text_primary_style;
static lv_style_t backdrop_text_muted_style;
static lv_style_t backdrop_icon_style;
/* Custom layouts' panels and chips take the theme's window color, and their
 * progress groove the backdrop text at low opacity, as Breeze draws them.
 * Both carry only colors, so a layout's own opacity still decides whether a
 * panel shows at all. */
static lv_style_t backdrop_surface_style;
static lv_style_t backdrop_track_style;
static lv_style_t backdrop_arc_style;
static bool backdrop_styles_ready;

static void backdrop_styles_init(void) {
    if (backdrop_styles_ready) return;
    lv_style_init(&backdrop_text_primary_style);
    lv_style_init(&backdrop_text_muted_style);
    lv_style_init(&backdrop_icon_style);
    lv_style_init(&backdrop_surface_style);
    lv_style_init(&backdrop_track_style);
    lv_style_set_bg_color(&backdrop_surface_style, lv_color_hex(GUI_COLOR_SCREEN));
    lv_style_set_bg_color(&backdrop_track_style, lv_color_hex(GUI_COLOR_PRIMARY));
    lv_style_set_bg_opa(&backdrop_track_style, LV_OPA_40);
    lv_style_init(&backdrop_arc_style);
    lv_style_set_arc_color(&backdrop_arc_style, lv_color_hex(GUI_COLOR_PRIMARY));
    lv_style_set_arc_opa(&backdrop_arc_style, LV_OPA_40);
    lv_style_set_text_color(&backdrop_text_primary_style, lv_color_hex(GUI_COLOR_PRIMARY));
    lv_style_set_text_color(&backdrop_text_muted_style, lv_color_hex(GUI_COLOR_SECONDARY));
    lv_style_set_image_recolor_opa(&backdrop_icon_style, LV_OPA_TRANSP);
    backdrop_styles_ready = true;
}

lv_style_t * gui_player_backdrop_text_style(bool muted) {
    backdrop_styles_init();
    return muted ? &backdrop_text_muted_style : &backdrop_text_primary_style;
}

lv_style_t * gui_player_backdrop_icon_style(void) {
    backdrop_styles_init();
    return &backdrop_icon_style;
}

/* Mean color of what is visible behind the player's text. */
static lv_color_t player_backdrop_color(void) {
    if (current_reflection_bytes && player_background_img &&
        !lv_obj_has_flag(player_background_img, LV_OBJ_FLAG_HIDDEN)) {
        const uint16_t * px = (const uint16_t *) current_reflection_bytes;
        uint32_t r = 0, g = 0, b = 0, n = 0;
        for (int32_t y = 0; y < BOARD_SCREEN_HEIGHT; y += 8) {
            for (int32_t x = 0; x < REFLECTION_WIDTH; x += 8) {
                uint16_t p = px[(size_t) y * REFLECTION_WIDTH + x];
                r += (p >> 11) << 3;
                g += ((p >> 5) & 63) << 2;
                b += (p & 31) << 3;
                n++;
            }
        }
        if (n) return lv_color_make((uint8_t) (r / n), (uint8_t) (g / n), (uint8_t) (b / n));
    }
    if (player_overlay_panel && lv_obj_get_style_bg_opa(player_overlay_panel, 0) > LV_OPA_50)
        return lv_obj_get_style_bg_color(player_overlay_panel, 0);
    lv_style_value_t screen;
    if (lv_style_get_prop(&style_theme_screen_bg, LV_STYLE_BG_COLOR, &screen) == LV_STYLE_RES_FOUND)
        return screen.color;
    return lv_color_hex(GUI_COLOR_SCREEN);
}

void gui_player_update_backdrop_contrast(void) {
    backdrop_styles_init();
    lv_style_value_t primary, muted;
    if (lv_style_get_prop(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, &primary) != LV_STYLE_RES_FOUND ||
        lv_style_get_prop(&style_theme_text_muted, LV_STYLE_TEXT_COLOR, &muted) != LV_STYLE_RES_FOUND)
        return;
    lv_style_value_t screen;
    lv_color_t surface = lv_style_get_prop(&style_theme_screen_bg, LV_STYLE_BG_COLOR, &screen) ==
                         LV_STYLE_RES_FOUND ? screen.color : lv_color_hex(GUI_COLOR_SCREEN);
    lv_style_value_t current_surface;
    if (lv_style_get_prop(&backdrop_surface_style, LV_STYLE_BG_COLOR, &current_surface) != LV_STYLE_RES_FOUND ||
        !lv_color_eq(current_surface.color, surface)) {
        lv_style_set_bg_color(&backdrop_surface_style, surface);
        lv_obj_report_style_change(&backdrop_surface_style);
    }
    lv_color_t bg = player_backdrop_color();
    lv_color_t text = primary.color, secondary = muted.color;
    /* The theme's own colors stay wherever they read cleanly (the player's
     * title and metadata are large text, so WCAG's 3:1); otherwise the
     * matching Breeze pair. A dark theme's text on its darkened frost always
     * passes, so it keeps today's look. */
    bool neutral = gui_theme_contrast_ratio(text, bg) < GUI_CONTRAST_MUTED;
    if (neutral) {
        bool light = gui_theme_prefers_light_text(bg);
        text = lv_color_hex(light ? GUI_BREEZE_TEXT_ON_DARK : GUI_BREEZE_TEXT_ON_LIGHT);
        secondary = lv_color_hex(light ? GUI_BREEZE_MUTED_ON_DARK : GUI_BREEZE_MUTED_ON_LIGHT);
    }
    /* Transport glyphs come in a white set (theme2) and a dark one (theme1),
     * whichever the theme picked. Tint them with the text when the text is
     * dark, or when the theme's text was replaced: either way the stock
     * glyph color is not known to read on this backdrop. */
    bool tint_icons = neutral || gui_theme_color_is_dark(text);
    lv_style_value_t current;
    bool same = lv_style_get_prop(&backdrop_text_primary_style, LV_STYLE_TEXT_COLOR, &current) == LV_STYLE_RES_FOUND &&
                lv_color_eq(current.color, text) &&
                lv_style_get_prop(&backdrop_text_muted_style, LV_STYLE_TEXT_COLOR, &current) == LV_STYLE_RES_FOUND &&
                lv_color_eq(current.color, secondary) &&
                lv_style_get_prop(&backdrop_icon_style, LV_STYLE_IMAGE_RECOLOR_OPA, &current) == LV_STYLE_RES_FOUND &&
                (current.num == LV_OPA_COVER) == tint_icons;
    if (same) return;
    lv_style_set_bg_color(&backdrop_track_style, text);
    lv_style_set_arc_color(&backdrop_arc_style, text);
    lv_obj_report_style_change(&backdrop_track_style);
    lv_obj_report_style_change(&backdrop_arc_style);
    lv_style_set_text_color(&backdrop_text_primary_style, text);
    lv_style_set_text_color(&backdrop_text_muted_style, secondary);
    lv_style_set_image_recolor(&backdrop_icon_style, text);
    lv_style_set_image_recolor_opa(&backdrop_icon_style, tint_icons ? LV_OPA_COVER : LV_OPA_TRANSP);
    lv_obj_report_style_change(&backdrop_text_primary_style);
    lv_obj_report_style_change(&backdrop_text_muted_style);
    lv_obj_report_style_change(&backdrop_icon_style);
    bool glyph_on_light = gui_theme_contrast_ratio(lv_color_white(), bg) < GUI_CONTRAST_MUTED;
    if (player_transport_images_set_glyph_on_light(glyph_on_light)) {
        refresh_play_btn_icon();
    }
    gui_shell_refresh_quick_drawer_cover();
    /* The status bar over the player follows the same choice. */
    if (player_screen && lv_screen_active() == player_screen)
        gui_shell_set_status_bar_screen_context(player_screen);
    player_transition_mark_dirty();
}


static void apply_player_flat_background(bool has_bg_color, uint32_t bg_color) {
    drop_current_reflection();

    if (!player_overlay_panel) return;

    if (has_bg_color) {
        lv_obj_set_style_bg_color(player_overlay_panel, lv_color_hex(bg_color), 0);
    } else {
        lv_obj_remove_local_style_prop(player_overlay_panel, LV_STYLE_BG_COLOR, 0);
    }
    lv_obj_set_style_bg_opa(player_overlay_panel, LV_OPA_COVER, 0);
    gui_player_update_backdrop_contrast();
    player_transition_mark_dirty();
}

void gui_player_refresh_frosted_background(void) {
    player_lyrics_backdrop_invalidate();
    if (!player_overlay_panel) return;

    player_frost_params_t params = resolve_player_frost_params();
    if (params.flat) {
        /* Flat mode is a plain color fill -- doesn't need decoded cover
         * pixels, so it must apply even before any track has played or
         * after a failed decode, unlike the blur branch below. */
        apply_player_flat_background(params.has_bg_color, params.bg_color);
        return;
    }
    if (!current_cover_bytes) {
        /* Nothing to blur yet -- next decode picks up the current config.
         * Still clear any flat-mode BG_COLOR left over from a previous
         * live switch away from flat, so a stale tint doesn't linger
         * behind the frosted image once a cover does decode. */
        drop_current_reflection();
        lv_obj_remove_local_style_prop(player_overlay_panel, LV_STYLE_BG_COLOR, 0);
        gui_player_update_backdrop_contrast();
        return;
    }

    if (reflection_matches(&params)) {
#ifdef UI_PERF_TRACE
        printf("PERF frost_cache reused=1 generation=%llu\n",
               (unsigned long long) current_cover_generation);
#endif
        lv_obj_remove_local_style_prop(player_overlay_panel, LV_STYLE_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(player_overlay_panel, LV_OPA_TRANSP, 0);
        attach_current_reflection();
        player_transition_mark_dirty();
        return;
    }

#ifdef UI_PERF_TRACE
    printf("PERF frost_cache reused=0 generation=%llu blur=%d passes=%d darken=%d/%d\n",
           (unsigned long long) current_cover_generation, params.blur_radius,
           params.blur_passes, params.darken_num, params.darken_den);
#endif
    drop_current_reflection();
    current_reflection_bytes = player_reflection_cache_get(&current_cover_file, &params);
    if (!current_reflection_bytes)
        current_reflection_bytes = player_compute_reflection_bytes_cancelable(current_cover_bytes,
                                                                               params.blur_radius,
                                                                               params.blur_passes,
                                                                               params.darken_num,
                                                                               params.darken_den,
                                                                               params.tint_rgb,
                                                                               params.tint_weight,
                                                                               NULL, NULL);
    set_current_reflection_key(&params);

    /* TRANSP, not COVER -- player_background_img (the frosted image, about
     * to be shown) fully covers this panel; keeping the panel's own paint
     * opaque underneath it forces LVGL to composite two full-screen opaque
     * layers every frame for no visible benefit. The lyrics screen's own
     * analogous wrapper (dark_overlay in gui_lyrics.c) is deliberately kept
     * transparent for exactly this reason -- its own comment explains
     * darkness is already baked into its backdrop image. */
    lv_obj_remove_local_style_prop(player_overlay_panel, LV_STYLE_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(player_overlay_panel, LV_OPA_TRANSP, 0);

    attach_current_reflection();
    gui_shell_refresh_quick_drawer_cover();
    player_transition_mark_dirty();
}

static cover_decode_request_t last_local_cover_request;
static bool last_local_cover_request_valid;

static void cover_decode_request_snapshot_frost(cover_decode_request_t * req) {
    player_frost_params_t p = resolve_player_frost_params();
    req->flat = p.flat;
    req->has_bg_color = p.has_bg_color;
    req->bg_color = p.bg_color;
    req->blur_radius = p.blur_radius;
    req->blur_passes = p.blur_passes;
    req->darken_num = p.darken_num;
    req->darken_den = p.darken_den;
    req->tint_rgb = p.tint_rgb;
    req->tint_weight = p.tint_weight;
}
void gui_player_invalidate_cover_negatives(void) {
    player_artwork_jobs_invalidate_negatives();
}

static void launch_cover_decode_req(cover_decode_request_t r) {
    player_cover_prefetch_cancel();
    if (r.generation == 0) {
#ifdef UI_PERF_TRACE
        r.request_perf_us = ui_perf_now_us();
#endif
        r.bt_art_token = bt_cover_art_begin_track(playlist_path_at(r.for_index));
        r.generation = player_artwork_jobs_advance_generation();
    }
    cover_file_id_t shown = current_cover_bytes ? current_cover_file : (cover_file_id_t) { .valid = false };
    player_artwork_jobs_partial_clear();
    cover_decode_partial_applied_generation = 0;
    if (r.local_track_path[0]) {
        last_local_cover_request = r;
        last_local_cover_request_valid = true;
    } else if (r.stream_url[0]) {
        last_local_cover_request_valid = false;
    }
    player_artwork_jobs_submit(r, &shown);
}

static void launch_cover_decode_from_track(int for_index, const char * track_path, const char * artist,
                                            const char * album, const char * album_artist) {
    cover_decode_request_t r = { .for_index = for_index, .generation = 0,
                                  .picture_data = NULL, .picture_size = 0,
                                  .stream_url = "", .stream_verify_tls = false };
    snprintf(r.local_track_path, sizeof(r.local_track_path), "%s", track_path ? track_path : "");
    snprintf(r.artist, sizeof(r.artist), "%s", artist ? artist : "");
    snprintf(r.album, sizeof(r.album), "%s", album ? album : "");
    snprintf(r.album_artist, sizeof(r.album_artist), "%s", album_artist ? album_artist : "");
    cover_decode_request_snapshot_frost(&r);
    launch_cover_decode_req(r);
}

/* Subsonic streaming's own art source -- see cover_decode_request_t's own
 * comment. url is subsonic_build_cover_art_url()'s output, fetched on the
 * same background thread that would otherwise be decoding already-local
 * bytes, so a slow/flaky connection can't block the UI here either, same
 * reasoning as launch_cover_decode_from_track() itself. */
static void launch_cover_decode_from_url(int for_index, const char * url, bool verify_tls) {
    cover_decode_request_t r = { .for_index = for_index, .generation = 0,
                                  .picture_data = NULL, .picture_size = 0,
                                  .stream_verify_tls = verify_tls };
    snprintf(r.stream_url, sizeof(r.stream_url), "%s", url);
    cover_decode_request_snapshot_frost(&r);
    launch_cover_decode_req(r);
}


static void fit_cover_img_to_card(void) {
    if (!cover_img || !cover_card) return;
    const void * source = lv_image_get_src(cover_img);
    const void * thumbnail_target = player_inline_lyrics_source_for_scaling(source);
    const void * thumbnail_source = player_cover_thumbnail ? lv_image_get_src(player_cover_thumbnail) : NULL;
    bool same_source = thumbnail_target == thumbnail_source;
    if (!same_source && thumbnail_target && thumbnail_source &&
        lv_image_src_get_type(thumbnail_target) == LV_IMAGE_SRC_FILE &&
        lv_image_src_get_type(thumbnail_source) == LV_IMAGE_SRC_FILE)
        same_source = strcmp(thumbnail_target, thumbnail_source) == 0;
    if (player_cover_thumbnail && !same_source) {
        lv_image_set_src(player_cover_thumbnail, thumbnail_target);
        lv_image_set_inner_align(player_cover_thumbnail, LV_IMAGE_ALIGN_CONTAIN);
    }
    /* lv_image_set_src already decoded the source dimensions. Reuse them
     * while the card animates instead of reopening a file on every frame. */
    int32_t source_w = lv_image_get_src_width(cover_img);
    int32_t source_h = lv_image_get_src_height(cover_img);
    if (source_w <= 0 || source_h <= 0) return;
    int32_t card_w = lv_obj_get_width(cover_card);
    int32_t card_h = lv_obj_get_height(cover_card);
    if (card_w <= 0 || card_h <= 0) {
        card_w = player_s(350);
        card_h = player_s(350);
    }
    int32_t scale_w = (card_w * LV_SCALE_NONE + source_w - 1) / source_w;
    int32_t scale_h = (card_h * LV_SCALE_NONE + source_h - 1) / source_h;
    int32_t scale = scale_w > scale_h ? scale_w : scale_h;
#if defined(BOARD_TEMPOTEC_V1)
    if (player_layout_builtin) {
        scale_w = card_w * LV_SCALE_NONE / source_w;
        scale_h = card_h * LV_SCALE_NONE / source_h;
        scale = scale_w < scale_h ? scale_w : scale_h;
        if (scale < 1) scale = 1;
    }
#endif
    lv_image_set_scale(cover_img, scale);
    lv_obj_align(cover_img, LV_ALIGN_CENTER, 0, 0);
}

static void player_cover_size_changed_cb(lv_event_t * e) {
    (void) e;
    fit_cover_img_to_card();
}

static bool player_artwork_cache_idle(void) {
    /* Source pixels are immutable. Unrelated scrolling labels may continue;
     * foreground gestures and artwork/layout changes defer construction. */
    bool idle = player_screen && lv_display_get_screen_active(NULL) == player_screen &&
        backlight_screen_is_on() && !player_artwork_jobs_active() &&
        !gui_navigation_transition_in_progress() && !gui_player_lyrics_animation_in_progress() &&
        !gui_shell_boot_prompt_blocked() && !gui_player_boot_prompt_blocked() &&
        gui_popup_visible_count() == 0 && !gui_notifications_toast_visible() &&
        (!cover_img || !lv_anim_get(cover_img, NULL)) &&
        (!cover_card || !lv_anim_get(cover_card, NULL)) &&
        lv_display_get_inactive_time(NULL) >= 350;
    for (lv_indev_t * input = lv_indev_get_next(NULL); idle && input; input = lv_indev_get_next(input))
        if (lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED) idle = false;
    return idle;
}

static void player_artwork_cache_service(lv_timer_t * timer) {
    bool idle = player_artwork_cache_idle();
    lv_draw_sw_image_cache_stats_t stats;
    lv_draw_sw_image_cache_get_intermediate_stats(&stats);
    /* Without allocations there is nothing to reclaim. Read memory only
     * when pressure can release storage or eligible idle work can start. */
    if (!stats.entries && !stats.pending && !cover_raster_cache_bytes() &&
        (!idle || !stats.qualified_candidates)) {
        if (timer) lv_timer_pause(timer);
        return;
    }
    size_t available = system_get_mem_available_bytes();
    if (available < player_raster_cache_memory_floor()) {
        player_cover_prefetch_cancel();
        cover_raster_cache_clear();
        player_reflection_cache_clear();
    }
    /* Nearly full swap is memory pressure too: MemAvailable alone stays high
     * while the process is being pushed out to swap. */
    lv_draw_sw_image_cache_service(system_swap_under_pressure() ? 0 : available, idle);
    lv_draw_sw_image_cache_get_intermediate_stats(&stats);
    if (timer && (!idle || !stats.pending)) lv_timer_pause(timer);
    if (!timer && idle && stats.pending) {
        if (!player_artwork_cache_timer)
            player_artwork_cache_timer = lv_timer_create(player_artwork_cache_service, 20, NULL);
        else lv_timer_resume(player_artwork_cache_timer);
    }
}

/* NEXT tags are validated by the metadata poll. Start speculative work
 * only after current artwork has settled; it must never race the initial
 * foreground decode for the artwork coordinator or SD bandwidth. */
static void maybe_prefetch_next_cover(void) {
    if (!player_screen || !next_metadata_cached || !metadata_ui_token ||
        artwork_dispatched_metadata_token != metadata_ui_token ||
        player_artwork_jobs_active() || player_artwork_jobs_pending() ||
        player_artwork_jobs_reload_active() ||
        next_cover_prefetch_token == metadata_next_token ||
        player_queue_model_current() < 0 || player_queue_model_current() >= player_queue_model_count() ||
        strcmp(metadata_next_current_path, playlist_path_at(player_queue_model_current())) != 0 ||
        strcmp(next_metadata_cached_path, playlist_path_at(player_queue_model_current())) == 0 ||
        is_http_url(next_metadata_cached_path) || remote_track_path_is_remote(next_metadata_cached_path)) return;
    next_cover_prefetch_token = metadata_next_token; /* At most one attempt per queue prediction. */
    player_cover_prefetch_submit(next_metadata_cached_path, next_metadata_cache.artist,
                                next_metadata_cache.album, next_metadata_cache.album_artist,
                                COVER_ART_WIDTH, COVER_ART_HEIGHT);
}

static void apply_decoded_cover_pixels(uint16_t * pixels, int for_index,
                                       const cover_file_id_t * file,
                                       bool accent_valid, uint32_t accent) {
    if (!pixels) return;
    player_inline_lyrics_invalidate_cover_source();
    lv_image_cache_drop(&current_cover_dsc);
    free(current_cover_bytes);
    current_cover_bytes = (uint8_t *) pixels;
    current_cover_generation++;
    current_cover_for_index = for_index;
    current_cover_file = *file;
    memset(&current_cover_dsc, 0, sizeof(current_cover_dsc));
    current_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    current_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    current_cover_dsc.header.flags |= LV_IMAGE_FLAGS_USER3;
    current_cover_dsc.header.w = COVER_ART_WIDTH;
    current_cover_dsc.header.h = COVER_ART_HEIGHT;
    current_cover_dsc.header.stride = COVER_ART_WIDTH * 2;
    current_cover_dsc.data = current_cover_bytes;
    current_cover_dsc.data_size = (uint32_t) COVER_ART_WIDTH * COVER_ART_HEIGHT * 2;
    if (cover_img) lv_image_set_src(cover_img, &current_cover_dsc);
    fit_cover_img_to_card();
    player_lyrics_backdrop_invalidate();
    gui_shell_refresh_quick_drawer_cover();
    gui_lock_screen_refresh(true);

    /* The old reflection belongs to the old cover. Keep the background
     * empty until this request's effect pass completes; flat styling is
     * resolved from live layout state on the UI thread. */
    player_frost_params_t live = resolve_player_frost_params();
    if (live.flat) apply_player_flat_background(live.has_bg_color, live.bg_color);
    else {
        drop_current_reflection();
        if (player_overlay_panel) {
            lv_obj_remove_local_style_prop(player_overlay_panel, LV_STYLE_BG_COLOR, 0);
            lv_obj_set_style_bg_opa(player_overlay_panel, LV_OPA_TRANSP, 0);
        }
    }
    gui_theme_set_cover_accent(accent_valid, accent);
    gui_lyrics_on_cover_changed(for_index);
    player_transition_mark_dirty();
}

static void poll_cover_decode_partial(void) {
    cover_decode_partial_t partial = player_artwork_jobs_take_partial();
    if (!partial.valid) return;
    bool current = !player_artwork_jobs_reload_active() &&
                   partial.generation == player_artwork_jobs_generation() &&
                   partial.for_index == player_queue_model_current();
    if (current) {
        apply_decoded_cover_pixels(partial.pixels, partial.for_index, &partial.file,
                                   partial.accent_valid, partial.accent);
        cover_decode_partial_applied_generation = partial.generation;
#ifdef UI_PERF_TRACE
        printf("PERF player_cover_partial_request_to_apply_ms elapsed=%u index=%d\n",
               (unsigned)((ui_perf_now_us() - partial.request_perf_us) / 1000), partial.for_index);
#endif
    } else {
        free(partial.pixels);
    }
}

/* Called from the player UI timer. Applies the finished decode to
 * cover_img/player_overlay_panel -- unless player_queue_model_current() has already moved
 * on to a different track by the time this lands (another launch_cover_
 * decode() call superseded it via cover_decode_pending, in which case that
 * one is now either running or about to be), in which case the result is
 * just discarded rather than briefly flashing a stale track's art. */
void poll_cover_decode(void) {
    /* Slides display owned snapshots. Applying artwork now cannot update
     * those frames, and a reload must retain its just-finished result until
     * finish_cover_reload() queues the forced reread. */
    if (gui_navigation_transition_in_progress() || gui_player_lyrics_animation_in_progress() ||
        player_artwork_jobs_reload_active()) return;
    poll_cover_decode_partial();
    if (!player_artwork_jobs_active()) {
        maybe_prefetch_next_cover();
        return;
    }
    if (!player_artwork_jobs_done()) return;
    player_artwork_result_t cover_result;
    if (!player_artwork_jobs_take_result(&cover_result)) return;

    bool result_generation_current = cover_result.generation == player_artwork_jobs_generation();

#ifdef UI_PERF_TRACE
    if (result_generation_current && cover_result.for_index == player_queue_model_current()) {
        printf("PERF player_cover_request_to_apply_ms elapsed=%u index=%d\n",
               (unsigned)((ui_perf_now_us() - cover_result.request_perf_us) / 1000), player_queue_model_current());
    }
#endif

    if (result_generation_current && cover_result.format.valid &&
        strcmp(cover_result.format.path, playlist_path_at(player_queue_model_current())) == 0) {
        player_cached_format = cover_result.format;
        refresh_format_badge();
    }

    if (!result_generation_current || cover_result.for_index != player_queue_model_current()) {
        free(cover_result.pixels);
        free(cover_result.reflection);
    } else if (cover_result.ok && cover_result.same && current_cover_bytes &&
               cover_file_id_same(&cover_result.file, &current_cover_file)) {
        /* Same cached file as the cover on screen (next track of the same
         * album): the pixels, reflection, lock screen, drawer and accent
         * already match. Only the owning queue slot moves on. */
        free(cover_result.pixels);
        free(cover_result.reflection);
        current_cover_for_index = cover_result.for_index;
        gui_player_refresh_frosted_background();
        gui_lyrics_on_cover_changed(player_queue_model_current());
    } else if (cover_result.same) {
        /* The shown cover changed after this worker started, so the result
         * has no pixels to apply; decode this track's cover properly. */
        if (last_local_cover_request_valid && !player_artwork_jobs_pending()) {
            cover_decode_request_t retry = last_local_cover_request;
            retry.generation = 0;
            retry.picture_data = NULL;
            player_artwork_jobs_park_retry(retry);
        }
    } else if (!cover_result.ok) {
        free(cover_result.reflection);
        player_inline_lyrics_invalidate_cover_source();
        /* Retarget every live image object before releasing the backing
         * buffers. The lock screen may keep current_cover_dsc as its source,
         * so clear the descriptor before freeing its pixels. */
        if (cover_img) lv_image_set_src(cover_img, asset_path("playing_plane/default_cover_565.png"));
        lv_image_cache_drop(&current_cover_dsc);
        uint8_t * old_cover_bytes = current_cover_bytes;
        current_cover_bytes = NULL;
        current_cover_generation++;
        current_cover_for_index = -1;
        current_cover_file.valid = false;
        /* current_cover_dsc.data still points at the old pixels until it is
         * cleared below -- gui_player_get_current_cover_dsc() hands this same static
         * struct's address out to other callers (the lock screen), who keep
         * referencing &current_cover_dsc for as long as they're showing. */
        current_cover_dsc.data = NULL;
        current_cover_dsc.data_size = 0;
        fit_cover_img_to_card();
        player_lyrics_backdrop_invalidate();
        /* No in-memory raw bitmap to reflect for the static placeholder
         * cover. Apply a configured flat color if set (it needs no cover
         * pixels), otherwise reset the panel back to its plain background
         * rather than leaving a stale reflection from whatever track played
         * before this one. */
        player_frost_params_t failure_params = resolve_player_frost_params();
        if (failure_params.flat) {
            apply_player_flat_background(failure_params.has_bg_color, failure_params.bg_color);
        } else {
            drop_current_reflection();
            /* Clear any flat-mode BG_COLOR left over from a previous live
             * switch away from flat -- same reasoning as gui_player_
             * refresh_frosted_background()'s own no-cover-yet branch. */
            if (player_overlay_panel)
                lv_obj_remove_local_style_prop(player_overlay_panel, LV_STYLE_BG_COLOR, 0);
            gui_player_update_backdrop_contrast();
        }
        free(old_cover_bytes);
        gui_lock_screen_refresh(true);
        gui_theme_set_cover_accent(false, 0);
        player_transition_mark_dirty(); /* cover_img just changed to the placeholder -- see the cache's own doc comment */
    } else {
        bool pixels_already_applied = cover_decode_partial_applied_generation == cover_result.generation;
        if (pixels_already_applied) {
            /* The UI owns the partial display copy; this original remained
             * worker-owned for reflection generation. */
            free(cover_result.pixels);
        } else {
            apply_decoded_cover_pixels(cover_result.pixels, cover_result.for_index,
                                       &cover_result.file, cover_result.accent_valid,
                                       cover_result.accent);
            cover_result.pixels = NULL;
        }

        /* The reflection this decode computed was built from frost params
         * snapshotted when the decode was LAUNCHED. If a plugin changed
         * player_layout_config while this decode was in flight (a rare
         * race, e.g. plugin.set_player_layout()/refresh_theme() firing
         * mid-decode), that stale snapshot must not silently overwrite the
         * live config the plugin just applied -- compare against the
         * current live params and only trust the worker's precomputed
         * result when they still match. */
        player_frost_params_t live_params = resolve_player_frost_params();
        bool snapshot_is_stale = (live_params.flat != cover_result.flat) ||
            (live_params.flat
                ? (live_params.has_bg_color != cover_result.has_bg_color ||
                   (live_params.has_bg_color && live_params.bg_color != cover_result.bg_color))
                : (live_params.blur_radius != cover_result.blur_radius ||
                   live_params.blur_passes != cover_result.blur_passes ||
                   live_params.darken_num != cover_result.darken_num ||
                   live_params.darken_den != cover_result.darken_den ||
                   live_params.tint_rgb != cover_result.tint_rgb ||
                   live_params.tint_weight != cover_result.tint_weight));

        if (snapshot_is_stale) {
            /* Discard the worker's now-outdated result and recompute
             * synchronously against the live config and the cover art
             * just installed above -- a one-off UI-thread recompute here
             * is the same acceptable tradeoff gui_player_refresh_frosted_
             * background()'s own live-refresh callers already make. */
            free(cover_result.reflection);
            gui_player_refresh_frosted_background();
        } else if (cover_result.flat) {
            free(cover_result.reflection);
            apply_player_flat_background(cover_result.has_bg_color, cover_result.bg_color);
        } else {
            drop_current_reflection();
            current_reflection_bytes = cover_result.reflection;
            set_current_reflection_key(&live_params);

            /* TRANSP, not COVER -- see gui_player_refresh_frosted_background()'s
             * own comment on this exact line for why. */
            if (player_overlay_panel) {
                lv_obj_remove_local_style_prop(player_overlay_panel, LV_STYLE_BG_COLOR, 0);
                lv_obj_set_style_bg_opa(player_overlay_panel, LV_OPA_TRANSP, 0);
            }

            attach_current_reflection();
        }

        /* The drawer shows this same frosted copy, so it can only be
         * refreshed once the new one has replaced the previous track's. */
        gui_shell_refresh_quick_drawer_cover();

        /* Refresh lyrics screen backdrop now that current_cover_bytes has
         * been updated with new cover art. */
        if (!pixels_already_applied) {
            gui_lyrics_on_cover_changed(player_queue_model_current());
            gui_theme_set_cover_accent(cover_result.accent_valid, cover_result.accent);
        }
        player_transition_mark_dirty(); /* cover_img/player_overlay_panel's reflection just changed -- see the cache's own doc comment */
    }

    if (player_artwork_jobs_pending()) {
        cover_decode_request_t pending;
        if (player_artwork_jobs_take_pending(&pending))
            launch_cover_decode_req(pending); /* original submit wrapper, after result application */
    }
    maybe_prefetch_next_cover();
}

/* Keep optional cache pressure checks and idle candidate cleanup on the
 * slower system timer; decoding completion polling remains cheap. */
void gui_player_service_artwork_cache(void) {
    player_artwork_cache_service(NULL);
}

void gui_player_begin_cover_reload(void) {
    player_cover_prefetch_cancel();
    player_artwork_jobs_begin_reload();
    player_reflection_cache_clear();
    player_artwork_jobs_partial_clear();
    next_cover_prefetch_token = 0;
    /* New decodes are held (launch_cover_decode_req() parks them in
     * cover_decode_pending) until gui_player_finish_cover_reload(). One
     * already running, local or streamed, finishes normally: the reload
     * worker waits for gui_player_cover_decode_idle() before deleting any
     * file, so it cannot write an old cover back afterwards, and the UI
     * thread never blocks on an isolated extraction here. */
}

bool gui_player_cover_decode_idle(void) {
    return !!player_artwork_jobs_running_idle();
}

void gui_player_finish_cover_reload(void) {
    player_artwork_jobs_end_reload();
    /* Re-read the playing track's cover whatever album was reloaded: the
     * player's tag-derived key can differ from the library's, and one cached
     * read is cheap. The result replaces (and frees) the old cover; a pending
     * request for a newer track wins instead. */
    cover_decode_request_t req;
    if (player_artwork_jobs_take_pending(&req)) {
        /* Ownership of any pending picture_data transfers to this UI request. */
    } else if (last_local_cover_request_valid && last_local_cover_request.for_index == player_queue_model_current() &&
               strcmp(last_local_cover_request.local_track_path, playlist_path_at(player_queue_model_current())) == 0) {
        req = last_local_cover_request;
        req.picture_data = NULL;
        req.picture_size = 0;
        cover_decode_request_snapshot_frost(&req);
    } else {
        return;
    }
    if (req.local_track_path[0]) {
        /* Held or current local track alike: the cache it would hit may
         * predate the reload, so read the track's own art first and fall
         * back to the rebuilt album cover. Use the library's current tags: a
         * metadata refresh may have renamed the album since playback
         * started, and the reload rebuilt the cache under the new identity. */
        req.force_source = true;
        song_row_t row;
        if (metadata_db_get_song_by_path(req.local_track_path, &row)) {
            snprintf(req.artist, sizeof(req.artist), "%s", row.tags.artist);
            snprintf(req.album, sizeof(req.album), "%s", row.tags.album);
            snprintf(req.album_artist, sizeof(req.album_artist), "%s", row.tags.album_artist);
        }
    }
    req.generation = 0;
    launch_cover_decode_req(req);
}

void gui_player_cancel_cover_reload(void) {
    player_cover_prefetch_cancel();
    next_cover_prefetch_token = 0;
    player_artwork_jobs_end_reload();
    player_artwork_jobs_advance_generation();
    player_artwork_jobs_clear_pending();
}


/* ---- Lyrics: async .lrc load ------------------------------------------
 * Deliberately independent from the cover-decode pipeline above, despite
 * both running off the UI thread and both being triggered from the same
 * track-change call site (apply_track_metadata_to_ui() below): this job
 * only reads a small text sidecar (lyrics_load_sidecar(), bounded at
 * LYRICS_MAX_FILE_BYTES) rather than decoding/blurring image data, and
 * runs eagerly on every track change (not lazily like the backdrop below)
 * so lyrics are already parsed by the time the user taps the album art.
 * Same "background thread writes then sets a volatile done flag last, poll
 * function on the UI thread only reads after seeing it" contract as
 * cover_decode_done_flag above, and the same "at most one job in flight,
 * a track change arriving mid-load replaces the pending request rather
 * than queuing" shape as launch_cover_decode_req()'s cover_decode_pending. ---- */







/* ---- Lyrics: fullscreen blurred backdrop ------------------------------
 * Generated lazily -- only when the lyrics screen actually opens (see
 * open_lyrics_screen()) -- rather than eagerly per track change like cover
 * art/reflection above: most tracks' lyrics view is never opened, and this
 * needs nothing the async load above produces, just the already-decoded
 * current_cover_bytes. A private copy is taken up front (cover_copy below)
 * rather than reading current_cover_bytes directly from the background
 * thread, since poll_cover_decode() can free and replace that pointer out
 * from under a still-running backdrop job on a rapid track change. ---- */

/* Final brightness is baked in before RGB565 dithering (9/20 net brightness)
 * to avoid an alpha overlay blend re-quantizing the dithered image. */












/* The now-playing heart: player icon, Quick Drawer, and the cached state
 * Remote Control reports in /status. */
static void show_favorite_state(bool is_favorite) {
    favorite_is_set = is_favorite;
    if (favorite_icon) {
        lv_image_set_src(favorite_icon, asset_path(favorite_is_set ? "playing_plane/collect_in.png" : "playing_plane/collect_out.png"));
    }
    gui_shell_update_quick_drawer_favorite(favorite_is_set);
}

/* Resolve a tap made while the initial Favorite read was pending against
 * the stored value, rather than treating the placeholder as authoritative. */
static void apply_loaded_favorite(bool value) {
    bool toggle = favorite_toggle_pending;
    favorite_toggle_pending = false;
    favorite_track_known = true;
    show_favorite_state(toggle ? !value : value);
    if (toggle) {
        ++favorite_ui_revision;
        favorite_writer_submit(playlist_path_at(player_queue_model_current()), favorite_is_set);
    }
}

/* Remote Control changed some favorite: reload the now-playing heart from
 * the writer, which also sees this player's own unsaved taps. */
void gui_player_refresh_favorite(void) {
    if (player_queue_model_current() < 0 || player_queue_model_current() >= player_queue_model_count()) return;
    const char * current = playlist_path_at(player_queue_model_current());
    if (!current || !current[0]) return;
    ++favorite_ui_revision;
    favorite_request_revision = favorite_ui_revision;
    track_metadata_request_t request = { .token = ++favorite_request_token };
    snprintf(request.path, sizeof(request.path), "%s", current);
    snprintf(favorite_request_path, sizeof(favorite_request_path), "%s", current);
    track_metadata_submit(TRACK_METADATA_FAVORITE, &request);
}

void favorite_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (player_queue_model_current() < 0 || player_queue_model_current() >= player_queue_model_count()) return;

    const char * path = playlist_path_at(player_queue_model_current());
    if (!path) return;

    if (!favorite_track_known) {
        favorite_toggle_pending = !favorite_toggle_pending;
        show_favorite_state(!favorite_is_set);
        return;
    }
    ++favorite_ui_revision;
    show_favorite_state(!favorite_is_set);

    favorite_writer_submit(path, favorite_is_set);
}

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
    gui_shell_update_quick_drawer_play_mode((int) mode);

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

/* refresh_now_playing_indicators() is defined down with the compact-list
 * screens it updates (Artists/Albums/All Songs/group songs) -- forward-
 * declared here since apply_track_metadata_to_ui() below is the single
 * place a real "this track started playing" event is known to have
 * happened, for every playback source (local library, Group Songs, Files,
 * .m3u playlists). */

/* Shared by both an explicit track pick (play_track_at_from) and the audio
 * thread autonomously advancing into a queued next track on its own
 * (on_track_auto_advanced) -- title/folder/art/format-badge/progress-reset
 * are identical either way. Returns the metadata it read (out_meta) so
 * callers that also need ReplayGain (play_track_at_from, to hand it to
 * audio_play_file_at) don't have to read the file's tags a second time. */
/* Set by subsonic_song_row_click_cb() (gui.c, further down) right before a
 * streamed mp3/flac Subsonic queue starts playing -- metadata_read() can't
 * read tags from a network URL the way it does a local file, but the real
 * title/artist/album (and where to fetch cover art) for every song in the
 * queue are already known from the API responses that built it, so they're
 * stashed here, one entry per playlist[] slot (same indexing, built and
 * freed together), and matched by exact URL string in apply_track_metadata_
 * to_ui() below -- the string match (not just the index) is what makes it
 * safe to leave this array up indefinitely rather than needing to track
 * every other place a new, unrelated playlist might start: subsonic_build_
 * stream_url() bakes a fresh random salt into every URL it builds, so a
 * later, different playlist's paths can never coincidentally match one of
 * these even if this array outlives its own playlist (the array is only
 * ever replaced, on the next Subsonic streaming tap, not proactively freed
 * when some other playback source starts). Forward-declared here (rather
 * than moving apply_track_metadata_to_ui() itself) since the Subsonic click
 * handler that WRITES these lives much further down this file, alongside
 * the rest of the Subsonic screens. */

static audio_codec_t info_codec_from_hint(const char * hint) {
    if (!hint || !hint[0]) return AUDIO_CODEC_UNKNOWN;
    if (*hint == '.') hint++;
    if (strcasecmp(hint, "flac") == 0) return AUDIO_CODEC_FLAC;
    if (strcasecmp(hint, "mp3") == 0) return AUDIO_CODEC_MP3;
    if (strcasecmp(hint, "wav") == 0 || strcasecmp(hint, "rf64") == 0 ||
        strcasecmp(hint, "w64") == 0 || strcasecmp(hint, "aif") == 0 ||
        strcasecmp(hint, "aiff") == 0 || strcasecmp(hint, "aifc") == 0) return AUDIO_CODEC_PCM;
    if (strcasecmp(hint, "dsf") == 0 || strcasecmp(hint, "dff") == 0)
        return AUDIO_CODEC_DSD;
    if (strcasecmp(hint, "aac") == 0 || strcasecmp(hint, "aacp") == 0)
        return AUDIO_CODEC_AAC;
    if (strcasecmp(hint, "wv") == 0 || strcasecmp(hint, "wavpack") == 0) return AUDIO_CODEC_WAVPACK;
    if (strcasecmp(hint, "ape") == 0) return AUDIO_CODEC_APE;
    if (strcasecmp(hint, "wma") == 0 || strcasecmp(hint, "asf") == 0)
        return AUDIO_CODEC_WMA;
    if (strcasecmp(hint, "opus") == 0) return AUDIO_CODEC_OPUS;
    return AUDIO_CODEC_UNKNOWN; /* M4A/Ogg need the actual decoder to disambiguate. */
}

static void info_container_from_hint(const char * hint, char out[16]) {
    out[0] = '\0';
    if (!hint || !hint[0]) return;
    if (*hint == '.') hint++;
    if (strcasecmp(hint, "aif") == 0 || strcasecmp(hint, "aiff") == 0 ||
        strcasecmp(hint, "aifc") == 0)
        snprintf(out, 16, "AIFF");
    else if (strcasecmp(hint, "aac") == 0 || strcasecmp(hint, "aacp") == 0)
        snprintf(out, 16, "ADTS");
    else if (strcasecmp(hint, "wma") == 0 || strcasecmp(hint, "asf") == 0)
        snprintf(out, 16, "ASF");
    else if (strcasecmp(hint, "m4a") == 0 || strcasecmp(hint, "m4b") == 0 ||
             strcasecmp(hint, "mp4") == 0)
        snprintf(out, 16, "M4A");
    else if (strcasecmp(hint, "ogg") == 0 || strcasecmp(hint, "oga") == 0 ||
             strcasecmp(hint, "opus") == 0)
        snprintf(out, 16, "Ogg");
    else {
        size_t i = 0;
        while (hint[i] && i + 1 < 16) {
            out[i] = (char) toupper((unsigned char) hint[i]);
            i++;
        }
        out[i] = '\0';
    }
}

static bool is_http_url(const char * path) {
    if (!path) return false;
    return strncasecmp(path, "http://", 7) == 0 || strncasecmp(path, "https://", 8) == 0;
}

static const char * info_path_hint(const char * path) {
    if (!path) return NULL;
    if (is_http_url(path)) {
        /* Only this app's local-only #.<ext> hint is safe to inspect on a
         * stream. Never treat a URL path/query suffix as a container name:
         * besides being unreliable, a query can contain an auth token and
         * must not reach the Information UI even in truncated form. */
        const char * fragment = strrchr(path, '#');
        return fragment && fragment[1] == '.' ? fragment + 2 : NULL;
    }
    const char * base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char * dot = strrchr(base, '.');
    return dot ? dot + 1 : NULL;
}

/* The stream catalog is initially parallel to the queue created by
 * Subsonic. Queue edits can move playlist slots without moving that array,
 * so retain the fast indexed lookup and fall back to the stable stream URL.
 * Both callers and the live playlist must agree on the exact path before a
 * metadata record is trusted. */
static const subsonic_stream_song_meta_t * subsonic_meta_for_path(int index, const char * path) {
    if (!path || !is_http_url(path) || index < 0 || index >= player_queue_model_count() ||
        !subsonic_stream_meta || subsonic_stream_meta_count <= 0) return NULL;
    const char * live_path = playlist_path_at(index);
    if (!live_path || strcmp(path, live_path) != 0) return NULL;
    if (index < subsonic_stream_meta_count &&
        strcmp(path, subsonic_stream_meta[index].url) == 0)
        return &subsonic_stream_meta[index];
    for (int i = 0; i < subsonic_stream_meta_count; i++) {
        if (strcmp(path, subsonic_stream_meta[i].url) == 0)
            return &subsonic_stream_meta[i];
    }
    return NULL;
}

static bool seed_track_metadata(int index, track_metadata_t * out_meta) {
    /* Resolved once -- this is player_queue_model_current()'s first real touch on every
     * track-start (play_track_at_from()/on_track_auto_advanced() both call
     * this immediately after setting player_queue_model_current()), so it's also where a
     * lazy All-Songs slot actually gets strdup'd. Every other playlist[index]
     * use below reuses this same resolved pointer rather than re-deriving
     * it (playlist_path_at() is idempotent/cheap on a re-call either way,
     * but there's no reason to). */
    const char * path = playlist_path_at(index);


    const subsonic_stream_song_meta_t * subsonic_meta = subsonic_meta_for_path(index, path);
    bool is_subsonic_stream = subsonic_meta != NULL;
    remote_track_meta_t remote_meta;
    bool is_remote_track = remote_track_meta_copy_for_path(path, &remote_meta);

    if (is_remote_track) {
        /* No file to open for a remote track -- unlike the Subsonic branch
         * below, this also fixes the ReplayGain gap Subsonic has today
         * (that branch never calls metadata_read(), so has_replaygain stays
         * false): the plugin declares replaygain_db up front from its own
         * catalog metadata, fed into resolve_replaygain() the same as any
         * local track's tag would be. */
        memset(out_meta, 0, sizeof(*out_meta));
        if (remote_meta.title[0]) {
            snprintf(out_meta->title, sizeof(out_meta->title), "%s", remote_meta.title);
            out_meta->has_title = true;
        }
        if (remote_meta.artist[0]) {
            snprintf(out_meta->artist, sizeof(out_meta->artist), "%s", remote_meta.artist);
            out_meta->has_artist = true;
        }
        if (remote_meta.album[0]) {
            snprintf(out_meta->album, sizeof(out_meta->album), "%s", remote_meta.album);
            out_meta->has_album = true;
        }
        if (remote_meta.has_replaygain) {
            out_meta->has_replaygain = true;
            out_meta->replaygain_gain_db = remote_meta.replaygain_db;
        }
    } else if (is_subsonic_stream) {
        memset(out_meta, 0, sizeof(*out_meta));
        if (subsonic_meta->title[0]) {
            snprintf(out_meta->title, sizeof(out_meta->title), "%s", subsonic_meta->title);
            out_meta->has_title = true;
        }
        if (subsonic_meta->artist[0]) {
            snprintf(out_meta->artist, sizeof(out_meta->artist), "%s", subsonic_meta->artist);
            out_meta->has_artist = true;
        }
        if (subsonic_meta->album[0]) {
            snprintf(out_meta->album, sizeof(out_meta->album), "%s", subsonic_meta->album);
            out_meta->has_album = true;
        }
    } else {
        memset(out_meta, 0, sizeof(*out_meta));
        /* The tapped list row may have an exact owned snapshot even when the
         * opportunistic RAM display cache has evicted it. It is display-only;
         * audio still reads fresh tags for ReplayGain. */
        if (selection_metadata_seed_valid && path &&
            strcmp(path, selection_metadata_seed_path) == 0) {
            *out_meta = selection_metadata_seed;
        } else {
            /* Browsing already read these tags. Reuse an owned RAM snapshot:
             * no database lock or storage access is allowed on this UI path. */
            song_row_t row;
            if (!is_http_url(path) && metadata_db_try_get_cached_song_by_path(path, &row)) {
#define DISPLAY_TEXT(field) \
            snprintf(out_meta->field, sizeof(out_meta->field), "%s", row.tags.field); \
            out_meta->has_##field = out_meta->field[0] != '\0'
                DISPLAY_TEXT(title);
                DISPLAY_TEXT(artist);
                DISPLAY_TEXT(album);
                DISPLAY_TEXT(album_artist);
                DISPLAY_TEXT(genre);
#undef DISPLAY_TEXT
                out_meta->track_number = row.tags.track_number;
                out_meta->has_track_number = row.tags.track_number > 0;
                out_meta->disc_number = row.tags.disc_number;
                out_meta->has_disc_number = row.tags.disc_number > 0;
                out_meta->release_year = row.tags.release_year;
                out_meta->has_release_year = row.tags.release_year > 0;
                metadata_normalize_text_tags(out_meta);
            }
        }
    }
    /* Stream and remote catalog text reaches the rows too: same single-line,
     * non-empty rules as file tags. */
    if (is_remote_track || is_subsonic_stream) metadata_normalize_text_tags(out_meta);

    return !is_remote_track && !is_subsonic_stream && !is_http_url(path);
}

/* Called by list selection handlers while they still own the visible row.
 * The copied snapshot is keyed by the exact path and contains display tags
 * only; it performs no database or filesystem work and cannot supply audio
 * ReplayGain. */
void gui_player_seed_selection_song(const char * path, const track_metadata_t * metadata) {
    selection_metadata_seed_valid = false;
    selection_metadata_seed_path[0] = '\0';
    memset(&selection_metadata_seed, 0, sizeof(selection_metadata_seed));
    if (!path || !path[0] || !metadata || strlen(path) >= sizeof(selection_metadata_seed_path)) return;
    snprintf(selection_metadata_seed_path, sizeof(selection_metadata_seed_path), "%s", path);
#define COPY_SELECTION_TEXT(field) do { \
    if (metadata->has_##field) { \
        snprintf(selection_metadata_seed.field, sizeof(selection_metadata_seed.field), "%s", metadata->field); \
        selection_metadata_seed.has_##field = selection_metadata_seed.field[0] != '\0'; \
    } \
} while (0)
    COPY_SELECTION_TEXT(title);
    COPY_SELECTION_TEXT(artist);
    COPY_SELECTION_TEXT(album);
    COPY_SELECTION_TEXT(album_artist);
    COPY_SELECTION_TEXT(genre);
#undef COPY_SELECTION_TEXT
    if (metadata->has_track_number && metadata->track_number > 0) {
        selection_metadata_seed.track_number = metadata->track_number;
        selection_metadata_seed.has_track_number = true;
    }
    if (metadata->has_disc_number && metadata->disc_number > 0) {
        selection_metadata_seed.disc_number = metadata->disc_number;
        selection_metadata_seed.has_disc_number = true;
    }
    if (metadata->has_release_year && metadata->release_year > 0) {
        selection_metadata_seed.release_year = metadata->release_year;
        selection_metadata_seed.has_release_year = true;
    }
    metadata_normalize_text_tags(&selection_metadata_seed);
    selection_metadata_seed_valid = true;
}

static void clear_selection_metadata_seed(void) {
    selection_metadata_seed_valid = false;
    selection_metadata_seed_path[0] = '\0';
    memset(&selection_metadata_seed, 0, sizeof(selection_metadata_seed));
}

/* Artwork is independent of favorite/lyrics completion. Dispatch once the
 * current track has enough identity and scalar tags to find its art. */
static void dispatch_track_artwork(int index, const char * path, track_metadata_t * meta,
                                   uint64_t metadata_token) {
    if (artwork_dispatched_metadata_token == metadata_token &&
        strcmp(artwork_dispatched_artist, meta->artist) == 0 &&
        strcmp(artwork_dispatched_album, meta->album) == 0 &&
        strcmp(artwork_dispatched_album_artist, meta->album_artist) == 0) return;
    const subsonic_stream_song_meta_t * subsonic_meta = subsonic_meta_for_path(index, path);
    bool is_subsonic_stream = subsonic_meta != NULL;
    remote_track_meta_t remote_meta;
    bool is_remote_track = remote_track_meta_copy_for_path(path, &remote_meta);
    if (is_remote_track && remote_meta.artwork_url[0]) {
        launch_cover_decode_from_url(index, remote_meta.artwork_url, remote_meta.verify_tls);
    } else if (is_subsonic_stream && subsonic_meta->cover_url[0]) {
        launch_cover_decode_from_url(index, subsonic_meta->cover_url, subsonic_meta->verify_tls);
    } else if (is_remote_track) {
        /* Synthetic remote keys have no local file fallback. */
        cover_decode_request_t no_art = { .for_index = index };
        cover_decode_request_snapshot_frost(&no_art);
        launch_cover_decode_req(no_art);
    } else {
        free(meta->picture_data);
        meta->picture_data = NULL;
        launch_cover_decode_from_track(index, path, meta->artist, meta->album, meta->album_artist);
    }
    /* An allocation/thread-start failure leaves neither active nor pending
     * work. Let metadata completion retry instead of marking it dispatched. */
    if (player_artwork_jobs_active() || player_artwork_jobs_pending()) {
        artwork_dispatched_metadata_token = metadata_token;
        snprintf(artwork_dispatched_artist, sizeof(artwork_dispatched_artist), "%s", meta->artist);
        snprintf(artwork_dispatched_album, sizeof(artwork_dispatched_album), "%s", meta->album);
        snprintf(artwork_dispatched_album_artist, sizeof(artwork_dispatched_album_artist), "%s", meta->album_artist);
    }
}

static void apply_loaded_track_metadata(int index, track_metadata_t * out_meta, bool favorite, bool favorite_ready) {
    const char * path = playlist_path_at(index);
    char title[128], folder[128];
    get_display_names(path, title, sizeof(title), folder, sizeof(folder));
    const subsonic_stream_song_meta_t * subsonic_meta = subsonic_meta_for_path(index, path);
    bool is_subsonic_stream = subsonic_meta != NULL;
    remote_track_meta_t remote_meta;
    bool is_remote_track = remote_track_meta_copy_for_path(path, &remote_meta);
    gui_track_info_context_t info = {0};
    snprintf(info.path, sizeof(info.path), "%s", path);
    info.replaygain_mode = current_settings.replaygain_mode;
    info.has_replaygain_track = out_meta->has_replaygain;
    info.replaygain_track_db = out_meta->replaygain_gain_db;
    info.has_replaygain_album = out_meta->has_replaygain_album;
    info.replaygain_album_db = out_meta->replaygain_album_gain_db;
    info.has_track_number = out_meta->has_track_number;
    info.track_number = out_meta->track_number;
    info.has_disc_number = out_meta->has_disc_number;
    info.disc_number = out_meta->disc_number;

    const char * format_hint = info_path_hint(path);
    if (is_remote_track) {
        info.source = GUI_TRACK_SOURCE_PLUGIN;
        snprintf(info.provider, sizeof(info.provider), "%s", remote_meta.provider);
        snprintf(info.track_id, sizeof(info.track_id), "%s", remote_meta.track_id);
        format_hint = remote_meta.codec;
        info.declared_codec = info_codec_from_hint(remote_meta.codec);
        if (info.declared_codec == AUDIO_CODEC_UNKNOWN) format_hint = NULL;
        info.declared_sample_rate = remote_meta.sample_rate;
        info.declared_bit_depth = remote_meta.bit_depth;
        info.declared_channels = remote_meta.channels;
        info.declared_bitrate_kbps = remote_meta.bitrate_kbps;
        info.declared_duration_seconds = (double) remote_meta.duration_ms / 1000.0;
    } else if (is_subsonic_stream) {
        const subsonic_stream_song_meta_t * sm = subsonic_meta;
        info.source = GUI_TRACK_SOURCE_SUBSONIC;
        format_hint = sm->suffix;
        info.declared_codec = info_codec_from_hint(sm->suffix);
        info.declared_sample_rate = sm->sample_rate;
        info.declared_bit_depth = sm->bit_depth;
        info.declared_channels = sm->channels;
        info.declared_bitrate_kbps = sm->bitrate_kbps;
        info.declared_duration_seconds = sm->duration_seconds;
        info.has_track_number = sm->track > 0;
        info.track_number = sm->track;
        info.has_disc_number = sm->disc > 0;
        info.disc_number = sm->disc;
    } else if (is_http_url(path)) {
        info.source = GUI_TRACK_SOURCE_RADIO;
        info.declared_codec = info_codec_from_hint(format_hint);
        if (info.declared_codec == AUDIO_CODEC_UNKNOWN) format_hint = NULL;
    } else {
        info.source = GUI_TRACK_SOURCE_LOCAL;
        info.declared_codec = info_codec_from_hint(format_hint);
    }
    info_container_from_hint(format_hint, info.container);
    gui_track_info_set_current(&info);


    const char * title_text = out_meta->has_title ? out_meta->title : title;
    const char * folder_text = out_meta->has_artist ? out_meta->artist : folder;
    const char * album_text = out_meta->has_album ? out_meta->album : "";
    snprintf(now_playing_genre, sizeof(now_playing_genre), "%s", out_meta->has_genre ? out_meta->genre : "");
    now_playing_track_number = out_meta->has_track_number && out_meta->track_number > 0 ? out_meta->track_number : 0;
    if (is_subsonic_stream && subsonic_meta->track > 0)
        now_playing_track_number = subsonic_meta->track;

    player_set_track_display_text(title_text, folder_text, album_text);
    refresh_format_badge();
    dispatch_track_artwork(index, path, out_meta, metadata_ui_token);

    /* No local sidecar (or embedded tag) makes sense for a Subsonic stream
     * or remote-provider URL -- same reasoning cover art uses a server-
     * supplied cover URL instead of a local-file lookup for those. A
     * streamed track just never gets lyrics in this first release. */
    if (!is_subsonic_stream && !is_remote_track) {
        gui_lyrics_load_track(index, path);
    } else {
        gui_lyrics_load_track(index, NULL);
    }
    /* Results contain only scalar tags; keep this defensive ownership guard
     * if the metadata loader later supplies optional blobs. */
    free(out_meta->lyrics);
    out_meta->lyrics = NULL;


    if (favorite_ready && metadata_favorite_revision == favorite_ui_revision) apply_loaded_favorite(favorite);
    player_transition_mark_dirty();
}

/* Establish the new UI identity immediately; file/DB enrichment arrives
 * through an owned scalar snapshot and never resets live playback progress. */
static void prepare_track_metadata(int index, bool notify, bool probe, bool audio_tags,
                                   bool submit_request) {
    player_cover_prefetch_cancel();
    next_cover_prefetch_token = 0;
    const char * path = playlist_path_at(index);
    track_metadata_request_t request = { .token = ++metadata_ui_token, .probe = probe };
    snprintf(request.path, sizeof(request.path), "%s", path);
    request.read_file = seed_track_metadata(index, &request.metadata);
    if (request.read_file && notify) {
        if (next_metadata_cached && strcmp(path, next_metadata_cached_path) == 0) {
            request.metadata = next_metadata_cache;
            request.read_file = false;
        } else if (audio_tags) {
            request.read_audio = true;
            request.read_file = false;
            request.audio_generation = audio_get_playback_generation();
        }
    }
    /* Consume the prefetched NEXT snapshot only after this path-checked copy.
     * A later current request must not inherit a stale queue entry. */
    next_metadata_cached = false;
    snprintf(metadata_ui_path, sizeof(metadata_ui_path), "%s", path);
#ifdef UI_PERF_TRACE
    metadata_request_perf_us = ui_perf_now_us();
#endif
    metadata_ui_notify = notify;
    metadata_notify_ready = false;
    plugin_now_playing_loaded = false;
    metadata_ui_audio_generation = audio_get_playback_generation();
    metadata_favorite_revision = ++favorite_ui_revision;
    metadata_ui_request = request;
    if (submit_request) track_metadata_submit(TRACK_METADATA_CURRENT, &request);
    /* Cancel a cover result even if a new queue reuses the same slot. */
    player_artwork_jobs_advance_generation();
    player_artwork_jobs_clear_pending();
    gui_lyrics_load_track(index, NULL);
    bt_cover_art_begin_track(path);
    gui_track_info_context_t pending_info = {0};
    snprintf(pending_info.path, sizeof(pending_info.path), "%s", path);
    pending_info.replaygain_mode = current_settings.replaygain_mode;
    pending_info.source = remote_track_path_is_remote(path) ? GUI_TRACK_SOURCE_PLUGIN :
        subsonic_meta_for_path(index, path) ? GUI_TRACK_SOURCE_SUBSONIC :
        is_http_url(path) ? GUI_TRACK_SOURCE_RADIO : GUI_TRACK_SOURCE_LOCAL;
    pending_info.declared_codec = info_codec_from_hint(info_path_hint(path));
    gui_track_info_set_current(&pending_info);
    refresh_format_badge();
    snprintf(now_playing_path, sizeof(now_playing_path), "%s", path);
    refresh_now_playing_indicators();
    char title[128], folder[128];
    get_display_names(path, title, sizeof(title), folder, sizeof(folder));
    player_set_track_display_text(request.metadata.has_title ? request.metadata.title : title,
                                  request.metadata.has_artist ? request.metadata.artist : folder,
                                  request.metadata.has_album ? request.metadata.album : "");
    now_playing_genre[0] = '\0';
    now_playing_track_number = 0;
    favorite_track_known = false;
    favorite_toggle_pending = false;
    show_favorite_state(false);

    player_seekbar_set_value(progress_slider, 0);
    displayed_progress_percent = 0;
    displayed_position_second = -1;
    displayed_duration_second = -1;
    if (pos_label) lv_label_set_text(pos_label, "0:00");
    if (dur_label) lv_label_set_text(dur_label, "--:--");

    if (song_count_label) {
        player_set_song_count(index, player_queue_model_count());
        lv_obj_remove_flag(song_count_label, LV_OBJ_FLAG_HIDDEN);
    }
    /* Cached catalog tags can identify artwork immediately even while the
     * fresh audio tags/favorite are pending. Later changed album identity
     * cancels/replaces this request through the normal generation guard. */
    if ((!request.read_file && !request.read_audio) || request.metadata.has_title ||
        request.metadata.has_artist || request.metadata.has_album)
        dispatch_track_artwork(index, path, &request.metadata, request.token);
#ifdef UI_PERF_TRACE
    printf("PERF player_selection_seed token=%llu known_tags=%d audio_wait=%d artwork_started=%d\n",
           (unsigned long long)request.token, request.metadata.has_title || request.metadata.has_artist || request.metadata.has_album,
           request.read_audio, artwork_dispatched_metadata_token == request.token);
#endif
    player_transition_mark_dirty(); /* title/artist/format badge/progress reset above all just changed player_screen's own content -- see the cache's own doc comment */
    player_timeline_play(player_timeline_track_change, NULL, NULL, NULL);
}

static void request_track_metadata(int index, bool notify, bool probe, bool audio_tags) {
    prepare_track_metadata(index, notify, probe, audio_tags, true);
}

/* The manual selection path seeds the UI before audio startup can block.
 * Once audio has its new generation, submit the prepared request with
 * fresh audio-tag reading while retaining its token and display snapshot.
 * Preparing the display does not submit a worker request or parse tags. */
static void request_track_audio_metadata(int index) {
    if (index < 0 || index >= player_queue_model_count() || index != player_queue_model_current() ||
        !metadata_ui_notify || metadata_ui_request.token != metadata_ui_token) return;
    const char * path = playlist_path_at(index);
    if (!path || strcmp(path, metadata_ui_path) != 0 ||
        strcmp(path, metadata_ui_request.path) != 0) return;
    if (metadata_ui_request.read_file && metadata_ui_notify) {
        metadata_ui_request.read_file = false;
        metadata_ui_request.read_audio = true;
        metadata_ui_request.audio_generation = audio_get_playback_generation();
    }
    metadata_ui_audio_generation = audio_get_playback_generation();
    track_metadata_submit(TRACK_METADATA_CURRENT, &metadata_ui_request);
}

/* Resolves which of a track's own ReplayGain fields to actually hand to
 * audio.c, per Settings -> Playback -> ReplayGain's mode: Off (no gain at
 * all), Per Track (the default -- normalizes every track to the same
 * perceived loudness), or Per Album (preserves intentional relative
 * loudness differences between tracks on the same album). Falls back to
 * track gain when Per Album is selected but this particular file has no
 * album-level tag -- most taggers only write one or the other depending on
 * whether the rip was tagged as a whole album or track-at-a-time, and
 * silently playing unnormalized instead of falling back to whatever IS
 * available would be a worse outcome than just using track gain here. */
void resolve_replaygain(const track_metadata_t * meta, bool * out_has_gain, double * out_gain_db,
                                bool * out_has_peak, double * out_peak) {
    int mode = current_settings.replaygain_mode;
    if (mode == 2 && meta->has_replaygain_album) {
        *out_has_gain = true;
        *out_gain_db = meta->replaygain_album_gain_db;
        if (meta->has_replaygain_album_peak) {
            *out_has_peak = true;
            *out_peak = meta->replaygain_album_peak;
        } else {
            *out_has_peak = meta->has_replaygain_peak;
            *out_peak = meta->replaygain_peak;
        }
        return;
    }
    bool use_track = mode != 0;
    *out_has_gain = use_track && meta->has_replaygain;
    *out_gain_db = meta->replaygain_gain_db;
    *out_has_peak = use_track && meta->has_replaygain_peak;
    *out_peak = meta->replaygain_peak;
}

/* ---- Delete confirmation popup ---- */

static void hide_delete_song_popup(void) {
    gui_popup_hide(&delete_song_popup);
}

static void delete_song_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_delete_song_popup();
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
        set_play_button_state(false);
        player_set_track_display_text(NULL, NULL, NULL);
        now_playing_genre[0] = '\0';
        now_playing_track_number = 0;
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

static void build_delete_song_popup(void) {
    /* LV_LABEL_LONG_DOT, not WRAP -- this title's text is set later
     * (delete_song_confirm_prompt() below) to "Delete <filename>?..." with
     * an arbitrary-length real filename spliced in, so it needs to
     * truncate rather than potentially wrap across several lines. */
    delete_song_popup.popup = build_confirm_popup("", LV_LABEL_LONG_DOT, &delete_song_popup_title, NULL, TR("Delete"),
                                             lv_color_hex(GUI_BREEZE_NEGATIVE), delete_song_confirm_cb, NULL, TR("Cancel"),
                                             accent_lv_color(), delete_song_cancel_cb, NULL,
                                             delete_song_popup_backdrop_cb, &delete_song_popup.backdrop);
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
    open_add_to_playlist_for(playlist_path_at(player_queue_model_current()));
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
    if (player_queue_model_current() < 0 || player_queue_model_current() >= player_queue_model_count()) return;

    lv_label_set_text_fmt(delete_song_popup_title, TR("Delete %s?\nThis cannot be undone."), basename_of(playlist_path_at(player_queue_model_current())));
    gui_popup_show(&delete_song_popup);
}

static void build_more_menu_popup(void);

static void more_icon_event_cb(lv_event_t * e) {
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

static void build_more_menu_popup(void) {
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
static lv_obj_t * add_transport_hit_target(lv_obj_t * scr, int32_t center_x, int32_t width, int32_t top_y,
                                     int32_t bottom_y_exclusive, lv_event_cb_t cb, lv_color_t debug_color) {
    lv_obj_t * ext = lv_obj_create(scr);
    lv_obj_remove_style_all(ext);
    lv_obj_set_pos(ext, center_x - width / 2, top_y);
    lv_obj_set_size(ext, width, bottom_y_exclusive - top_y);
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

/* Transport icons are visual-only elements inside controls_row.
 * Forwarding the hit target's PRESSED/RELEASED/PRESS_LOST onto the
 * underlying icon's state triggers icon_press_style dimming on real touch. */
static void forward_press_state_to_icon_cb(lv_event_t * e) {
    lv_obj_t * icon = (lv_obj_t *) lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) lv_obj_add_state(icon, LV_STATE_PRESSED);
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) lv_obj_clear_state(icon, LV_STATE_PRESSED);
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
        /* 2s, matching player_secondary_label_enable_marquee()'s own wait
         * below -- was 3s, inconsistent with it for no functional reason. */
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

static void player_secondary_label_enable_marquee(lv_obj_t * label) {
    static lv_anim_t player_secondary_marquee_anim;
    static bool initialized = false;
    if (!initialized) {
        lv_anim_init(&player_secondary_marquee_anim);
        lv_anim_set_delay(&player_secondary_marquee_anim, 2000);
        lv_anim_set_repeat_delay(&player_secondary_marquee_anim, 2000);
        initialized = true;
    }

    lv_obj_set_style_anim(label, &player_secondary_marquee_anim, LV_PART_MAIN);
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

static void player_add_lyrics_control(lv_obj_t * obj) {
    player_inline_lyrics_add_control(obj);
}

static void player_screen_loaded_cb(lv_event_t * e) {
    (void) e;
    player_timeline_play(player_timeline_screen_enter, NULL, NULL, NULL);
}

/* Wires a screen built by a layout to the app. Roles and their contract:
 * docs/reference/PLAYER_LAYOUTS.md. Returns false, with no global assigned and nothing
 * attached, when a required role is missing or of the wrong type, so the
 * caller can discard the tree and fall back to the built-in layout. */
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

static bool player_inline_query_cover(player_inline_cover_info_t * info) {
    if (!info) return false;
    info->descriptor = &current_cover_dsc;
    info->has_pixels = current_cover_bytes != NULL;
    info->generation = current_cover_generation;
    return true;
}

static bool player_inline_setting_lyrics_enabled(void) { return current_settings.lyrics_enabled; }
static void player_inline_pause_artwork_cache_timer(void) {
    if (player_artwork_cache_timer) lv_timer_pause(player_artwork_cache_timer);
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

static bool player_bind_widgets(lv_obj_t * scr) {
    const bool builtin = player_layout_builtin;
    player_count_row_geometry = (player_count_row_geometry_t){0};

    /* Everything that can fail comes first. */
    lv_obj_t * r_cover_card = player_find_role(scr, "cover_card", NULL);
    lv_obj_t * r_cover_img = player_find_role(scr, "cover_img", &lv_image_class);
    lv_obj_t * r_title = player_find_role(scr, "title", &lv_label_class);
    lv_obj_t * r_play = player_find_role(scr, "play_btn", &lv_image_class);
    lv_obj_t * r_progress = player_find_role(scr, "progress_slider", NULL);
    if (!player_seekbar_is_supported(r_progress)) r_progress = NULL;
    const char * missing = !r_cover_card ? "cover_card" : !r_cover_img ? "cover_img" : !r_title ? "title" :
                           !r_play ? "play_btn" : !r_progress ? "progress_slider" : NULL;
    if (missing) {
        fprintf(stderr, "Warning: player layout: required widget '%s' is missing or has the wrong type\n", missing);
        return false;
    }
    if (!player_seekbar_configure(r_progress)) return false;
    lv_obj_t * r_overlay = player_find_role(scr, "overlay_panel", NULL);
    const bool layout_has_overlay = r_overlay != NULL;
    lv_obj_t * r_background = player_find_role(scr, "background_img", &lv_image_class);
    lv_obj_t * r_artist = player_find_role(scr, "artist", &lv_label_class);
    lv_obj_t * r_album = player_find_role(scr, "album", &lv_label_class);
    lv_obj_t * r_fav_circle = player_find_role(scr, "favorite_circle", NULL);
    lv_obj_t * r_fav_icon = player_find_role(scr, "favorite_icon", &lv_image_class);
    lv_obj_t * r_pill = player_find_role(scr, "quality_pill", NULL);
    lv_obj_t * r_badge = player_find_role(scr, "format_badge", &lv_label_class);
    lv_obj_t * r_pos = player_find_role(scr, "pos_label", &lv_label_class);
    lv_obj_t * r_dur = player_find_role(scr, "dur_label", &lv_label_class);
    lv_obj_t * r_count = player_find_role(scr, "song_count", &lv_label_class);
    lv_obj_t * r_order = player_find_role(scr, "order_btn", &lv_image_class);
    lv_obj_t * r_prev = player_find_role(scr, "prev_btn", &lv_image_class);
    lv_obj_t * r_next = player_find_role(scr, "next_btn", &lv_image_class);
    lv_obj_t * r_more = player_find_role(scr, "more_btn", &lv_image_class);
    lv_obj_t * r_dismiss = player_find_role(scr, "dismiss_btn", NULL);
    lv_obj_t * r_volume = player_find_role(scr, "volume_slider", &lv_slider_class);
    lv_obj_t * r_lyrics_area = player_find_role(scr, "lyrics_area", NULL);
    lv_obj_t * r_lyrics_backdrop = player_find_role(scr, "lyrics_backdrop", NULL);
    lv_obj_t * r_lyrics_dim = player_find_role(scr, "lyrics_dim", NULL);
    if (!r_lyrics_dim) r_lyrics_dim = player_find_role(scr, "panorama_lyrics_dim", NULL);
    lv_obj_t * r_lyrics_active = player_find_role(scr, "lyrics_active", &lv_label_class);
    lv_obj_t * r_lyrics_toggle = player_find_role(scr, "lyrics_toggle", NULL);
    player_cover_thumbnail = player_find_role(scr, "cover_thumbnail", &lv_image_class);
    player_play_circle = player_find_role(scr, "play_circle", NULL);
    lv_obj_t * r_transport_color = player_find_role(scr, "transport_color", &lv_label_class);
    lv_obj_t * r_cover_fade = player_find_role(scr, "cover_fade", &lv_image_class);

    transport_btn_ctx_t * prev_ctx = NULL;
    transport_btn_ctx_t * next_ctx = NULL;
    if (r_prev) {
        prev_ctx = malloc(sizeof(transport_btn_ctx_t));
        if (prev_ctx) *prev_ctx = (transport_btn_ctx_t){ r_prev, "playing_plane/btn_prev.png", "playing_plane/btn_prev_s.png" };
    }
    if (r_next) {
        next_ctx = malloc(sizeof(transport_btn_ctx_t));
        if (next_ctx) *next_ctx = (transport_btn_ctx_t){ r_next, "playing_plane/btn_next.png", "playing_plane/btn_next_s.png" };
    }
    if ((r_prev && !prev_ctx) || (r_next && !next_ctx)) {
        free(prev_ctx);
        free(next_ctx);
        return false;
    }

    /* Hidden-but-required internals: frosted-background code and the volume
     * mirror assume they exist whatever the layout draws. */
    if (!r_overlay) {
        r_overlay = lv_obj_create(scr);
        lv_obj_set_name_static(r_overlay, "overlay_panel");
        player_init_overlay_panel(r_overlay);
        lv_obj_move_to_index(r_overlay, 0);
    }
    if (!r_background) r_background = player_create_hidden_background_img(r_overlay);
    if (!r_volume) {
        r_volume = lv_slider_create(scr);
        lv_obj_set_name_static(r_volume, "volume_slider");
        lv_obj_add_flag(r_volume, LV_OBJ_FLAG_HIDDEN);
    } else if (!builtin) {
        /* A visible volume_slider is an indicator only; the volume popup and
         * the quick drawer own volume changes. */
        lv_obj_remove_flag(r_volume, LV_OBJ_FLAG_CLICKABLE);
    }

    player_overlay_panel = r_overlay;
    player_background_img = r_background;
    cover_card = r_cover_card;
    cover_img = r_cover_img;
    /* Only a descendant was baked into the card snapshot. Older XMLs may
     * keep their fade beside the card; leave that live overlay visible. */
    song_title_label = r_title;
    artist_label = r_artist;
    album_label = r_album;
    player_artist_standin = !r_artist;
    player_album_standin = !r_album;
    if (!artist_label) artist_label = player_create_hidden_label(scr);
    if (!album_label) album_label = player_create_hidden_label(scr);
    /* Bind on the actual artist/album role widgets, including XML layouts.
     * player_bind_widgets() runs once for each newly built player tree; role
     * rebinding after a UI reload therefore gets fresh callbacks without
     * accumulating duplicate event handlers on reused labels. */
    if (r_artist && strcmp(player_layouts_get_interactions()->artist_action, "none") != 0) {
        lv_obj_add_flag(r_artist, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r_artist, player_now_playing_group_click_cb,
                            LV_EVENT_CLICKED, (void *) (intptr_t) 0);
    }
    if (r_album && strcmp(player_layouts_get_interactions()->album_action, "none") != 0) {
        lv_obj_add_flag(r_album, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r_album, player_now_playing_group_click_cb,
                            LV_EVENT_CLICKED, (void *) (intptr_t) 1);
    }
    favorite_circle = r_fav_circle;
    favorite_icon = r_fav_icon;
    quality_pill = r_pill;
    format_badge_label = r_badge;
    pos_label = r_pos;
    dur_label = r_dur;
    song_count_label = r_count;
    order_icon = r_order;
    prev_btn = r_prev;
    play_btn = r_play;
    next_btn = r_next;
    progress_slider = r_progress;
    player_dismiss_btn = r_dismiss;
    volume_slider = r_volume;

    /* Lyrics placement and colors, read once. The area's own text color and
     * alignment style the normal lines, `lyrics_active` the current one. The
     * area must not take taps meant for what lies under it; the lyrics list
     * handles its own. */
    player_inline_lyrics_view_t inline_view = player_inline_view_for_bind(
        scr, r_cover_fade, r_lyrics_area, NULL, NULL, false);
    gui_lyrics_look_t lyrics_look = { 0 };
    lyrics_look.active_marker = player_find_role(scr, "lyrics_marker", NULL) != NULL;
    if (r_lyrics_area) {
        lv_style_value_t value;
        lv_obj_remove_flag(r_lyrics_area, LV_OBJ_FLAG_CLICKABLE);
        if (lv_obj_get_local_style_prop(r_lyrics_area, LV_STYLE_TEXT_COLOR, &value, LV_PART_MAIN) == LV_STYLE_RES_FOUND) {
            lyrics_look.has_normal = true;
            lyrics_look.normal = value.color;
        }
        if (lv_obj_get_local_style_prop(r_lyrics_area, LV_STYLE_TEXT_ALIGN, &value, LV_PART_MAIN) == LV_STYLE_RES_FOUND) {
            lyrics_look.has_align = true;
            lyrics_look.align = (lv_text_align_t) value.num;
        }
        lyrics_look.on_empty_tap = player_inline_lyrics_area_empty_tap_cb;
    }
    if (r_lyrics_active) {
        lyrics_look.has_active = true;
        lyrics_look.active = lv_obj_get_style_text_color(r_lyrics_active, LV_PART_MAIN);
    }
    gui_lyrics_set_look(&lyrics_look);
    if (r_lyrics_toggle) {
        lv_obj_add_flag(r_lyrics_toggle, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r_lyrics_toggle, player_inline_lyrics_cover_tap_cb, LV_EVENT_CLICKED, NULL);
    }
    if (player_cover_thumbnail) {
        lv_obj_add_event_cb(player_cover_thumbnail, player_inline_lyrics_cover_tap_cb, LV_EVENT_CLICKED, NULL);
        player_inline_lyrics_update_thumbnail_clickable(player_cover_thumbnail);
    }

    /* Custom layouts style themselves, but inherit the theme's text colors
     * and the accent so a light/dark or accent change still reaches them.
     * Local XML style properties outrank these shared styles. The built-in
     * creator applies the same styles itself, in its original order. */
    if (!builtin) {
        if (player_play_circle) lv_obj_add_style(player_play_circle, gui_theme_accent_style(), 0);
        if (r_transport_color) {
            lv_color_t color = lv_obj_get_style_text_color(r_transport_color, 0);
            lv_obj_t * icons[] = { r_order, r_play, r_prev, r_next, r_more, r_fav_icon,
                                  player_find_role(scr, "dismiss_icon", &lv_image_class),
                                  player_find_role(scr, "lyrics_icon", &lv_image_class),
                                  r_lyrics_toggle && lv_obj_check_type(r_lyrics_toggle, &lv_image_class) ? r_lyrics_toggle : NULL };
            for (unsigned i = 0; i < sizeof(icons) / sizeof(icons[0]); ++i) {
                if (!icons[i]) continue;
                lv_obj_set_style_image_recolor(icons[i], color, 0);
                lv_obj_set_style_image_recolor_opa(icons[i], LV_OPA_COVER, 0);
            }
        }
        lv_obj_t * primary[] = { r_title, r_count };
        lv_obj_t * muted[] = { r_artist, r_album, r_badge, r_pos, r_dur };
        for (unsigned i = 0; i < sizeof(primary) / sizeof(primary[0]); ++i)
            if (primary[i]) lv_obj_add_style(primary[i], gui_player_backdrop_text_style(false), 0);
        for (unsigned i = 0; i < sizeof(muted) / sizeof(muted[0]); ++i)
            if (muted[i]) lv_obj_add_style(muted[i], gui_player_backdrop_text_style(true), 0);
        /* Local recolors (transport_color above) outrank this style. */
        lv_obj_t * glyphs[] = { r_order, r_prev, r_next, r_more };
        for (unsigned i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); ++i)
            if (glyphs[i]) lv_obj_add_style(glyphs[i], gui_player_backdrop_icon_style(), 0);
        backdrop_styles_init();
        lv_obj_add_style(r_progress, lv_obj_check_type(r_progress, &lv_arc_class) ? &backdrop_arc_style
                                                                                  : &backdrop_track_style, LV_PART_MAIN);
        lv_obj_t * surfaces[] = { player_layout_root, r_cover_card, r_overlay, r_lyrics_backdrop, r_lyrics_dim, r_lyrics_area,
                                  r_fav_circle, r_pill, r_count };
        for (unsigned i = 0; i < sizeof(surfaces) / sizeof(surfaces[0]); ++i)
            if (surfaces[i]) lv_obj_add_style(surfaces[i], &backdrop_surface_style, 0);
        player_style_named_extras(scr);
        lv_obj_add_style(r_progress, gui_theme_accent_style(), LV_PART_INDICATOR);
        lv_obj_add_style(r_progress, gui_theme_accent_knob_style(), LV_PART_KNOB);
        if (r_fav_circle) lv_obj_add_style(r_fav_circle, gui_theme_accent_outline_style(), 0);
        lv_obj_t * pressable[] = { r_order, r_play, r_more, r_fav_icon };
        for (unsigned i = 0; i < sizeof(pressable) / sizeof(pressable[0]); ++i)
            if (pressable[i]) lv_obj_add_style(pressable[i], &icon_press_style, LV_STATE_PRESSED);

        /* The built-in creator sets these while it builds, because it sizes
         * its transport row from them. */
        gui_player_prepare_play_btn_images();
        lv_image_set_src(r_play, gui_player_play_btn_image_src(audio_is_playing()));
        if (r_order) lv_image_set_src(r_order, asset_path(play_mode_icon_asset((play_mode_t) current_settings.play_mode)));
        if (!lv_image_get_src(r_cover_img)) lv_image_set_src(r_cover_img, asset_path("playing_plane/default_cover_565.png"));
    }

    /* Queue counts must stay on one line even when an XML layout gives the
     * label a narrow fixed width. Keep content-width labels content-sized so
     * their existing layout behavior is preserved. */
    if (r_count) {
        const lv_font_t * count_font = lv_obj_get_style_text_font(r_count, LV_PART_MAIN);
        int32_t line_height = count_font ? lv_font_get_line_height(count_font) : 0;
        int32_t vertical_padding = lv_obj_get_style_pad_top(r_count, LV_PART_MAIN) +
                                   lv_obj_get_style_pad_bottom(r_count, LV_PART_MAIN);
        if (line_height > 0) lv_obj_set_height(r_count, line_height + vertical_padding);

        if (lv_obj_get_style_width(r_count, LV_PART_MAIN) != LV_SIZE_CONTENT)
            lv_label_set_long_mode(r_count, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }

    lv_obj_update_layout(scr);
    player_count_row_capture(r_count, r_pill, r_badge);
    player_count_row_refresh();

    lv_obj_add_flag(r_cover_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r_cover_img, player_inline_lyrics_cover_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(r_cover_card, player_cover_size_changed_cb, LV_EVENT_SIZE_CHANGED, NULL);
    if (r_cover_fade) {
        lv_obj_update_layout(scr);
        lv_color_t fade_color = lv_obj_get_style_bg_color(player_layout_root, LV_PART_MAIN);
        if (!player_cover_fade_attach(r_cover_fade, fade_color)) {
            /* If the mask cannot be allocated, preserve metadata contrast. */
            lv_obj_set_style_bg_color(r_cover_fade, fade_color, 0);
            lv_obj_set_style_bg_opa(r_cover_fade, LV_OPA_60, 0);
        }
    } else {
        lv_obj_update_layout(scr);
    }

    player_inline_lyrics_bind_cover(&inline_view);

    player_label_enable_marquee(r_title);
    if (r_album) player_secondary_label_enable_marquee(r_album);
    if (r_artist) player_secondary_label_enable_marquee(r_artist);
    if (builtin) player_layout_metadata_stack();

    if (r_dismiss) {
        lv_obj_add_flag(r_dismiss, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r_dismiss, library_btn_event_cb, LV_EVENT_CLICKED, NULL);
    }

    player_seekbar_set_value(r_progress, 0);
    lv_obj_set_ext_click_area(r_progress, player_y(20));
    lv_obj_add_event_cb(r_progress, progress_slider_event_cb, LV_EVENT_ALL, NULL);
    register_swipe_dead_zone(r_progress);
    if (lv_obj_check_type(r_progress, &lv_arc_class)) {
        lv_obj_add_style(r_progress, gui_theme_accent_style(), LV_PART_INDICATOR);
        lv_obj_add_style(r_progress, gui_theme_accent_knob_style(), LV_PART_KNOB);
        lv_obj_set_style_border_width(r_progress, 0, LV_PART_KNOB);
        lv_obj_set_style_arc_width(r_progress, lv_obj_get_style_arc_width(r_progress, LV_PART_MAIN), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(r_progress, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(r_progress, true, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(r_progress, player_s(5), LV_PART_KNOB);
    }
    lv_obj_t * r_seek_style = player_find_role(scr, "seek_style", &lv_label_class);
    player_waveform_seekbar = r_seek_style &&
        player_seekbar_attach(r_progress, lv_label_get_text(r_seek_style));
    if (player_waveform_seekbar) {
        /* The waveform itself is taller than a normal rail. Keep a 44px
         * target without extending over the last lines of the lyrics. */
        lv_obj_update_layout(r_progress);
        int32_t extra = (44 - lv_obj_get_height(r_progress) + 1) / 2;
        lv_obj_set_ext_click_area(r_progress, extra > 0 ? extra : 0);
    }

    lv_slider_set_range(r_volume, 0, 100);
    lv_slider_set_value(r_volume, audio_clamp_volume_percent((int32_t) (audio_get_volume() * 100.0f + 0.5f)), LV_ANIM_OFF);

    /* Part D: Hit targets */
    lv_obj_update_layout(scr);
    player_inline_lyrics_reset_hit_targets();

    int32_t transport_center_y = 0;
    if (builtin) {
#if defined(BOARD_TEMPOTEC_V1)
        lv_obj_t * transport_row = lv_obj_get_parent(r_play);
        transport_center_y = lv_obj_get_y(transport_row) + lv_obj_get_height(transport_row) / 2;
#else
        transport_center_y = player_y(645) + lv_obj_get_height(lv_obj_get_parent(r_play)) / 2;
#endif
    }

    lv_obj_t * order_hit = NULL, * prev_hit = NULL, * next_hit = NULL, * more_hit = NULL;
    if (r_order) {
        order_hit = player_add_hit_target(scr, r_order, player_x(57), player_s(56), transport_center_y,
                                          order_icon_event_cb, lv_palette_main(LV_PALETTE_RED));
        lv_obj_add_event_cb(order_hit, forward_press_state_to_icon_cb, LV_EVENT_PRESSED, r_order);
        lv_obj_add_event_cb(order_hit, forward_press_state_to_icon_cb, LV_EVENT_RELEASED, r_order);
        lv_obj_add_event_cb(order_hit, forward_press_state_to_icon_cb, LV_EVENT_PRESS_LOST, r_order);
    }
    if (r_prev) {
        prev_hit = player_add_hit_target(scr, r_prev, player_x(142), player_s(56), transport_center_y,
                                         prev_btn_event_cb, lv_palette_main(LV_PALETTE_GREEN));
        lv_obj_add_event_cb(prev_hit, transport_btn_press_event_cb, LV_EVENT_PRESSED, prev_ctx);
        lv_obj_add_event_cb(prev_hit, transport_btn_press_event_cb, LV_EVENT_RELEASED, prev_ctx);
        lv_obj_add_event_cb(prev_hit, transport_btn_press_event_cb, LV_EVENT_PRESS_LOST, prev_ctx);
        lv_obj_add_event_cb(prev_hit, transport_btn_ctx_delete_cb, LV_EVENT_DELETE, prev_ctx);
        player_seek_input_bind_transport_target(prev_hit, -1);
    }

    lv_obj_t * play_hit = player_add_hit_target(scr, player_play_circle ? player_play_circle : r_play,
                                                player_x(240), player_s(94), transport_center_y,
                                                play_btn_event_cb, lv_palette_main(LV_PALETTE_BLUE));
    lv_obj_add_event_cb(play_hit, forward_press_state_to_icon_cb, LV_EVENT_PRESSED, r_play);
    lv_obj_add_event_cb(play_hit, forward_press_state_to_icon_cb, LV_EVENT_RELEASED, r_play);
    lv_obj_add_event_cb(play_hit, forward_press_state_to_icon_cb, LV_EVENT_PRESS_LOST, r_play);

    if (r_next) {
        next_hit = player_add_hit_target(scr, r_next, player_x(338), player_s(56), transport_center_y,
                                         next_btn_event_cb, lv_palette_main(LV_PALETTE_ORANGE));
        lv_obj_add_event_cb(next_hit, transport_btn_press_event_cb, LV_EVENT_PRESSED, next_ctx);
        lv_obj_add_event_cb(next_hit, transport_btn_press_event_cb, LV_EVENT_RELEASED, next_ctx);
        lv_obj_add_event_cb(next_hit, transport_btn_press_event_cb, LV_EVENT_PRESS_LOST, next_ctx);
        lv_obj_add_event_cb(next_hit, transport_btn_ctx_delete_cb, LV_EVENT_DELETE, next_ctx);
        player_seek_input_bind_transport_target(next_hit, 1);
    }

    if (r_more) {
        more_hit = player_add_hit_target(scr, r_more, player_x(423), player_s(56), transport_center_y,
                                         more_icon_event_cb, lv_palette_main(LV_PALETTE_PURPLE));
        lv_obj_add_event_cb(more_hit, forward_press_state_to_icon_cb, LV_EVENT_PRESSED, r_more);
        lv_obj_add_event_cb(more_hit, forward_press_state_to_icon_cb, LV_EVENT_RELEASED, r_more);
        lv_obj_add_event_cb(more_hit, forward_press_state_to_icon_cb, LV_EVENT_PRESS_LOST, r_more);
    }

#ifdef UI_GESTURE_TRACE
    lv_obj_t * traced[] = { order_hit, play_hit, prev_hit, next_hit, more_hit };
    for (unsigned i = 0; i < sizeof(traced) / sizeof(traced[0]); ++i)
        if (traced[i]) lv_obj_add_event_cb(traced[i], debug_transport_btn_all_cb, LV_EVENT_ALL, NULL);
#else
    (void) order_hit; (void) prev_hit; (void) next_hit; (void) more_hit;
#endif

    if (r_fav_circle) {
        lv_area_t favorite_area;
        lv_obj_get_coords(r_fav_circle, &favorite_area);
        int32_t fav_w = lv_area_get_width(&favorite_area);
        int32_t fav_h = lv_area_get_height(&favorite_area);
        int32_t fav_hit_w = fav_w < 44 ? 44 : fav_w;
        int32_t fav_hit_h = fav_h < 44 ? 44 : fav_h;
        lv_obj_t * favorite_hit = lv_obj_create(scr);
        lv_obj_remove_style_all(favorite_hit);
        lv_obj_set_pos(favorite_hit,
                       favorite_area.x1 - (fav_hit_w - fav_w) / 2,
                       favorite_area.y1 - (fav_hit_h - fav_h) / 2);
        lv_obj_set_size(favorite_hit, fav_hit_w, fav_hit_h);
        lv_obj_add_flag(favorite_hit, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(favorite_hit, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(favorite_hit, favorite_icon_event_cb, LV_EVENT_CLICKED, NULL);
        if (r_fav_icon) {
            lv_obj_add_event_cb(favorite_hit, forward_press_state_to_icon_cb, LV_EVENT_PRESSED, r_fav_icon);
            lv_obj_add_event_cb(favorite_hit, forward_press_state_to_icon_cb, LV_EVENT_RELEASED, r_fav_icon);
            lv_obj_add_event_cb(favorite_hit, forward_press_state_to_icon_cb, LV_EVENT_PRESS_LOST, r_fav_icon);
        }
        player_inline_lyrics_add_hit_target(favorite_hit);
#ifdef UI_HITBOX_DEBUG
        lv_obj_set_style_border_width(favorite_hit, 3, 0);
        lv_obj_set_style_border_color(favorite_hit, lv_palette_main(LV_PALETTE_PINK), 0);
        lv_obj_set_style_border_opa(favorite_hit, LV_OPA_COVER, 0);
#endif
    }

    /* Controls the lyrics view hides, for layouts whose tree is not a flat
     * list of the screen's children. Never a container of the cover or the
     * metadata, which stay visible. */
    player_inline_lyrics_reset_controls();
    if (!builtin) {
        lv_obj_t * controls[] = { r_order, r_prev, r_play, r_next, r_more, r_progress, r_pos, r_dur, r_pill,
                                  r_fav_circle, r_dismiss, r_count };
        lv_obj_t * kept[] = { r_cover_card, r_title, r_artist, r_album };
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
        inline_view.lyrics_open = player_layouts_find_timeline(player_layout_root, "lyrics_open");
        inline_view.lyrics_close = player_layouts_find_timeline(player_layout_root, "lyrics_close");
        player_timeline_track_change = player_layouts_find_timeline(player_layout_root, "track_change");
        player_timeline_screen_enter = player_layouts_find_timeline(player_layout_root, "screen_enter");
        if (player_timeline_screen_enter)
            lv_obj_add_event_cb(scr, player_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    }

    lv_obj_t * backdrop = r_lyrics_backdrop ? r_lyrics_backdrop :
        (layout_has_overlay && !r_lyrics_dim && !r_lyrics_area ? r_overlay : NULL);
    if (backdrop && !r_lyrics_dim && !r_lyrics_area) {
        /* Static caches may contain backgrounds, never live metadata or
         * controls. A plugin can place these roles anywhere in its tree. */
        lv_obj_t * live[] = { r_cover_card, r_title, r_artist, r_album, r_order, r_prev,
            r_play, r_next, r_more, r_progress, r_pos, r_dur, r_pill, r_fav_circle,
            r_dismiss, r_count, r_volume };
        for (unsigned i = 0; i < sizeof(live) / sizeof(live[0]); ++i) {
            if (live[i] && (live[i] == backdrop || player_obj_is_ancestor_of(backdrop, live[i]))) {
                backdrop = NULL;
                break;
            }
        }
    }
    inline_view.backdrop_contains_cover = backdrop && player_obj_is_ancestor_of(backdrop, r_cover_card);
    player_lyrics_backdrop_bind(backdrop, r_lyrics_dim, r_lyrics_area,
                                inline_view.lyrics_open, inline_view.lyrics_close);
    player_inline_lyrics_bind_pane(&inline_view);

    finalize_screen_navigation(scr);
    lv_obj_add_event_cb(scr, player_inline_lyrics_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
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
    player_count_row_geometry = (player_count_row_geometry_t){0};
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



/* A stream's URL is not a format label. Only known catalog/local hint
 * values may supply a fallback while its decoder is still opening. */
static const char * stream_format_hint_name(const char * hint) {
    const char * name = audio_codec_name(info_codec_from_hint(hint));
    if (name[0] || !hint) return name;
    if (*hint == '.') hint++;
    if (strcasecmp(hint, "alac") == 0) return "ALAC";
    if (strcasecmp(hint, "vorbis") == 0) return "Vorbis";
    if (strcasecmp(hint, "m4a") == 0 || strcasecmp(hint, "m4b") == 0 ||
        strcasecmp(hint, "mp4") == 0) return "M4A";
    if (strcasecmp(hint, "ogg") == 0 || strcasecmp(hint, "oga") == 0) return "Ogg";
    return "";
}

void refresh_format_badge(void) {
    const char * path = player_queue_model_current() >= 0 ? playlist_path_at(player_queue_model_current()) : NULL;
    if (!path || !path[0]) {
        player_set_format_badge_text("");
        gui_shell_update_quick_drawer_format("");
        return;
    }

    remote_track_meta_t remote_meta;
    bool is_remote_track = remote_track_meta_copy_for_path(path, &remote_meta);
    unsigned int sample_rate = is_remote_track ? remote_meta.sample_rate : 0;
    unsigned int bit_depth = is_remote_track ? remote_meta.bit_depth : 0;
    audio_codec_t codec = AUDIO_CODEC_UNKNOWN;
    if (player_cached_format.valid && strcmp(player_cached_format.path, path) == 0) {
        sample_rate = player_cached_format.source_sample_rate;
        bit_depth = player_cached_format.source_bit_depth;
        codec = player_cached_format.codec;
    }
    audio_current_format_info_t fmt_info;
    if (audio_get_current_format_info(&fmt_info) && fmt_info.valid &&
        strcmp(fmt_info.path, path) == 0) {
        sample_rate = fmt_info.source_sample_rate;
        bit_depth = fmt_info.source_bit_depth;
        codec = fmt_info.codec;
    }

    char ext[16] = "";
    const char * hint = info_path_hint(path);
    if (cue_reference_is(path)) {
        /* The reference identifies a CUE entry, not its audio container. */
        snprintf(ext, sizeof(ext), "%s", audio_codec_name(codec));
    } else if (is_http_url(path) || is_remote_track) {
        const char * name = audio_codec_name(codec);
        if (!name[0] && is_remote_track) name = stream_format_hint_name(remote_meta.codec);
        if (!name[0]) name = stream_format_hint_name(hint);
        snprintf(ext, sizeof(ext), "%s", name);
    } else if (hint) {
        /* Local badges retain container extensions such as DSF and M4A. */
        size_t i = 0;
        for (; hint[i] && i < sizeof(ext) - 1; i++)
            ext[i] = (char) toupper((unsigned char) hint[i]);
        ext[i] = '\0';
    }

    char text[96];
    const char * separator = ext[0] ? " " : "";
    if (sample_rate > 0 && bit_depth > 0) {
        snprintf(text, sizeof(text), "%s%s%u-bit / %.1fkHz", ext, separator,
                 bit_depth, sample_rate / 1000.0);
    } else if (sample_rate > 0) {
        snprintf(text, sizeof(text), "%s%s%.1fkHz", ext, separator, sample_rate / 1000.0);
    } else if (bit_depth > 0) {
        snprintf(text, sizeof(text), "%s%s%u-bit", ext, separator, bit_depth);
    } else {
        snprintf(text, sizeof(text), "%s", ext);
    }
    /* Polling unchanged text must not restart its marquee every tick. */
    player_set_format_badge_text(text);
    gui_shell_update_quick_drawer_format(text);
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
    if (player_queue_model_count() <= 0 || player_queue_model_current() < 0 || player_queue_model_current() >= player_queue_model_count() ||
        !now_playing_path[0]) return;
    /* Resolve against the current path at tap time. The library helper first
     * confirms that path is a local indexed song, so remote display metadata
     * cannot accidentally match a similarly named local group. */
    (void) gui_library_open_now_playing_group(now_playing_path, album);
}

/* Where the current playlist came from. Deliberately NOT derived
 * from `playlist` itself: that's just a flat array of paths with no
 * memory of which screen/group built it. Each interactive play-launch
 * site (all_songs_row_click_cb, group_song_row_click_cb,
 * files_search_row_click_cb, on_file_browser_selected) calls the
 * matching set_player_source_*() helper right before on_file_selected();
 * the Subsonic-download and DLNA-cast play sites call
 * clear_player_source() instead, since a streamed/cast single track has
 * no on-device list to go back to. */
/* player_source_kind_t defined in gui.h */

player_source_kind_t player_source_kind = PLAYER_SOURCE_NONE;

int player_source_all_songs_index = -1; /* row index into all_songs_list -- the DB's own title-sorted order */
int player_source_recently_added_index = -1; /* row index into recently_added_list -- the DB's own first_seen-DESC order */

/* Own copy of the group's song entries at the moment playback started --
 * group_songs_entries/count/title_label themselves just describe
 * whichever group gui_library_get_group_songs_screen() CURRENTLY shows, which can change
 * (browsing to a different artist/album, or a library rescan) before the
 * playback continues. group_song_entry_t (gui.c further down) is
 * declared after this point in the file -- forward-declared here since
 * this struct only needs a pointer to it, not its layout. */
/* group_song_entry_t defined in gui_library.h */
char * player_source_group_title = NULL;
group_song_entry_t * player_source_group_entries = NULL;
int player_source_group_count = 0;
int player_source_group_pos = -1; /* row index within the group */

char player_source_file_browser_dir[PATH_MAX];
int player_source_file_browser_row = -1;
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


/* Queue ordering lives in player_queue_model; these semantic wrappers retain
 * the GUI-facing transition entry points and supply the current explicit mode. */
static bool play_mode_uses_shuffle_bag(play_mode_t mode) {
    return mode == PLAY_MODE_SHUFFLE || mode == PLAY_MODE_SHUFFLE_ONCE;
}

/* User playback intent and decoder failure history stay controller-owned. */
static uint64_t playback_intent_epoch = 1;
uint64_t gui_player_playback_intent_epoch(void) { return playback_intent_epoch; }

void reset_decoder_failure_tracking(void) {
    consecutive_decoder_failure_skips = 0;
    failed_physical_paths_count = 0;
}

static bool is_failed_physical_path(const char *path) {
    if (!path || !path[0]) return false;
    for (int i = 0; i < failed_physical_paths_count; i++)
        if (strcmp(failed_physical_paths[i], path) == 0) return true;
    return false;
}

static void record_failed_physical_path(const char *path) {
    if (!path || !path[0] || is_failed_physical_path(path) ||
        failed_physical_paths_count >= MAX_FAILED_PHYSICAL_PATHS) return;
    size_t len = strlen(path);
    if (len < sizeof(failed_physical_paths[failed_physical_paths_count])) {
        memcpy(failed_physical_paths[failed_physical_paths_count], path, len + 1);
        failed_physical_paths_count++;
    }
}

int compute_auto_advance_index(int index) {
    return player_queue_model_predict_auto_advance(current_settings.play_mode, index);
}

void commit_auto_advance(void) {
    player_queue_model_commit_auto_advance(current_settings.play_mode);
}

int compute_manual_step_index(int index, int direction) {
    return player_queue_model_manual_step(current_settings.play_mode, index, direction);
}

const char *play_mode_icon_asset(play_mode_t mode) {
    switch (mode) {
        case PLAY_MODE_REPEAT_ALL: return "playing_plane/loop.png";
        case PLAY_MODE_REPEAT_ONE: return "playing_plane/single.png";
        case PLAY_MODE_SHUFFLE: return "playing_plane/random.png";
        case PLAY_MODE_SHUFFLE_ONCE: return "playing_plane/random_s.png";
        default: return "playing_plane/order.png";
    }
}

void free_playlist(void) {
    player_cover_prefetch_cancel();
    next_cover_prefetch_token = 0;
    ++metadata_ui_token;
    ++metadata_next_token;
    next_metadata_cached = false;
    next_metadata_cached_path[0] = '\0';
    ++favorite_ui_revision;
    player_queue_model_clear_begin();
    reset_decoder_failure_tracking();
    current_playback_generation = 0;
    player_queue_model_clear_finish();
}

static void queue_pin_sd_dir_if_needed(void) {
    if (player_queue_model_has_sd_dirfd_pin()) return;
    int count = player_queue_model_count();
    for (int i = 0; i < count; i++) {
        const char *path = player_queue_model_path_if_resolved(i);
        if (path && is_sd_card_path(path)) {
            player_queue_model_adopt_sd_dirfd(open_sd_queue_dir());
            return;
        }
    }
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



/* Tint the outer ring of the outline play/pause assets while preserving
 * their white center symbols. Also support the older stock assets:
 * stock btn_play.png / btn_pause.png are a white disc with a baked-in
 * #009FF6 play/pause mark. LVGL image_recolor mixes every pixel toward the
 * recolor color, so applying gui_theme_accent_style() would tint the disc
 * as well (same COVER flattening apply_accent_color() documents for
 * on.png). Rewrite only pixels that aren't white / near-white, keeping the
 * disc and the anti-aliased circle edge, and mixing the glyph toward the
 * current accent. */
void refresh_play_btn_icon(void)
{
    gui_player_prepare_play_btn_images();
    bool is_playing = audio_is_playing();
    if (play_btn) lv_image_set_src(play_btn, gui_player_play_btn_image_src(is_playing));
    gui_shell_update_quick_drawer_play_state(is_playing);
    player_transition_mark_dirty(); /* play_btn lives on player_screen -- see the cache's own doc comment */
}

void set_play_button_state(bool is_playing) {
    if (play_btn) lv_image_set_src(play_btn, gui_player_play_btn_image_src(is_playing));
    player_transition_mark_dirty(); /* play_btn lives on player_screen -- see the cache's own doc comment */
    gui_shell_update_quick_drawer_play_state(is_playing);
}

void gui_player_stop_for_external_session(void) {
    if (gui_player_has_active_track()) {
        /* A deferred resume has the authoritative checkpoint when no audio
         * is currently loaded; do not replace it with audio's idle position. */
        if (!(deferred_resume_pending && audio_is_idle())) {
            double pos = audio_get_resume_position_seconds();
            deferred_resume_position = (pos > 0.0) ? pos : 0.0;
        }
        deferred_resume_pending = true;
    }
    set_play_button_state(false);
    plugin_manager_notify_stopped();
    audio_stop();
}

/* Frees the player's rebuildable image caches for an external session: the
 * lyrics-animation cover snapshot (rebuilt on the next lyrics transition),
 * the scaled-cover intermediates (rebuilt by the idle service once a lyrics
 * transition publishes a source again), the generated-cover raster cache
 * (refilled by the next cover decode) and the lyrics backdrop frames. The
 * visible cover and its reflection stay. */
static void player_release_memory(bool quiet_backdrop) {
    player_cover_prefetch_cancel();
    next_cover_prefetch_token = 0;
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

/* resolve_replaygain moved to gui_player.c */

/* Tells audio.c what comes after `index` (per the current play mode -- see
 * compute_auto_advance_index()) so its playback thread can gapless-handoff
 * or crossfade into it near `index`'s natural end without a GUI round-trip.
 * Must be re-called (from on_track_auto_advanced) every time the thread
 * advances on its own, or the chain of automatic transitions stops after
 * one hop. */
void arm_next_track_for_audio(int index) {
    player_cover_prefetch_cancel();
    next_cover_prefetch_token = 0;
    ++metadata_next_token;
    next_metadata_cached = false;
    next_metadata_cached_path[0] = '\0';
    int next_index = -1;
    uint64_t revision = 0;
    const char *next_path = player_queue_model_note_next_armed(
        current_settings.play_mode, index, &next_index, &revision);
    if (next_index < 0) {
        armed_next_path[0] = '\0';
        audio_set_next_track(NULL, false, 0.0, false, 0.0);
        return;
    }
    uint64_t generation = audio_get_playback_generation();
    if (!current_settings.gapless_enabled || armed_next_generation != generation ||
        armed_next_gain_mode != current_settings.replaygain_mode || strcmp(armed_next_path, next_path) != 0) {
        armed_next_path[0] = '\0';
        audio_set_next_track(NULL, false, 0.0, false, 0.0);
    }
    track_metadata_request_t request = { .token = ++metadata_next_token };
    snprintf(request.path, sizeof(request.path), "%s", next_path);
    request.read_file = seed_track_metadata(next_index, &request.metadata);
    metadata_next_queue_revision = revision;
    metadata_next_audio_generation = audio_get_playback_generation();
    snprintf(metadata_next_current_path, sizeof(metadata_next_current_path), "%s",
             player_queue_model_path_at(index));
    metadata_next_gain_mode = current_settings.replaygain_mode;
    metadata_next_gapless_enabled = current_settings.gapless_enabled;
    track_metadata_submit(TRACK_METADATA_NEXT, &request);
}

static void poll_track_started_notification(void) {
    if (!metadata_ui_notify || !metadata_notify_ready) return;
    if (audio_get_playback_generation() != metadata_ui_audio_generation) {
        metadata_ui_notify = false;
        metadata_notify_ready = false;
        return;
    }
    audio_playback_progress_t progress;
    if (!audio_get_playback_progress(metadata_ui_path, &progress) ||
        progress.generation != metadata_ui_audio_generation ||
        progress.state != AUDIO_PLAYBACK_PROGRESS_ACTIVE ||
        (!progress.is_playing && !progress.is_paused)) return;
#ifndef HOST_BUILD
    hiby_sys_server_report_metadata(metadata_notify_payload.title, metadata_notify_payload.artist,
        metadata_notify_payload.album, metadata_notify_payload.genre,
        (long) (progress.duration_seconds * 1000.0));
    hiby_sys_server_report_position((long) (progress.position_seconds * 1000.0));
#endif
    notify_plugin_track_started(&metadata_notify_payload, metadata_ui_path, progress.duration_seconds);
    metadata_ui_notify = false;
    metadata_notify_ready = false;
}

static bool arm_cached_next_track(const char * path, const track_metadata_t * tags,
                                  uint64_t generation, int gain_mode) {
    if (!path || !tags || generation != audio_get_playback_generation() ||
        !current_settings.gapless_enabled) return false;
    if (remote_track_path_is_remote(path) && !remote_track_stream_is_fresh(path)) return false;
    if (armed_next_generation == generation && armed_next_gain_mode == gain_mode &&
        strcmp(armed_next_path, path) == 0) return true;

    bool album = gain_mode == 2 && tags->has_replaygain_album;
    bool gain = gain_mode != 0 && (album || tags->has_replaygain);
    bool album_peak = album && tags->has_replaygain_album_peak;
    bool peak = gain_mode != 0 && (album_peak || tags->has_replaygain_peak);
    if (!audio_set_next_track_for_generation(path, gain,
        album ? tags->replaygain_album_gain_db : tags->replaygain_gain_db, peak,
        album_peak ? tags->replaygain_album_peak : tags->replaygain_peak, generation)) return false;
    snprintf(armed_next_path, sizeof(armed_next_path), "%s", path);
    armed_next_generation = generation;
    armed_next_gain_mode = gain_mode;
    return true;
}

void gui_player_poll_track_metadata(void) {
    poll_track_started_notification();
    track_metadata_result_t result;
    if (track_metadata_take(TRACK_METADATA_FAVORITE, &result) &&
        result.request.token == favorite_request_token && favorite_request_revision == favorite_ui_revision &&
        player_queue_model_current() >= 0 && player_queue_model_current() < player_queue_model_count() &&
        strcmp(result.request.path, playlist_path_at(player_queue_model_current())) == 0 &&
        strcmp(result.request.path, favorite_request_path) == 0)
        apply_loaded_favorite(result.favorite);
    if (track_metadata_take(TRACK_METADATA_NEXT, &result) &&
        result.request.token == metadata_next_token && player_queue_model_revision() == metadata_next_queue_revision &&
        audio_get_playback_generation() == metadata_next_audio_generation &&
        player_queue_model_current() >= 0 && player_queue_model_current() < player_queue_model_count() &&
        strcmp(playlist_path_at(player_queue_model_current()), metadata_next_current_path) == 0) {
        next_metadata_cached = true;
        snprintf(next_metadata_cached_path, sizeof(next_metadata_cached_path), "%s", result.request.path);
        next_metadata_cache = result.request.metadata;
        if (metadata_next_gapless_enabled && current_settings.gapless_enabled)
            arm_cached_next_track(result.request.path, &result.request.metadata,
                                  metadata_next_audio_generation, metadata_next_gain_mode);
    }
    /* Resolve a signed provider URL in the background shortly before the
     * current track ends. This runs from the existing UI poll, and never
     * waits for the plugin response. */
    if (player_queue_model_current() >= 0 && player_queue_model_current() < player_queue_model_count() &&
        audio_get_playback_generation() == metadata_next_audio_generation &&
        player_queue_model_revision() == metadata_next_queue_revision &&
        strcmp(playlist_path_at(player_queue_model_current()), metadata_next_current_path) == 0 &&
        next_metadata_cached) {
        int next_index = compute_auto_advance_index(player_queue_model_current());
        if (next_index < 0 || strcmp(next_metadata_cached_path, playlist_path_at(next_index)) != 0)
            next_metadata_cached = false;
    }
    if (player_queue_model_current() >= 0 && player_queue_model_current() < player_queue_model_count() &&
        audio_get_playback_generation() == metadata_next_audio_generation &&
        player_queue_model_revision() == metadata_next_queue_revision &&
        strcmp(playlist_path_at(player_queue_model_current()), metadata_next_current_path) == 0 &&
        next_metadata_cached) {
        audio_playback_progress_t progress;
        if (audio_get_playback_progress(metadata_ui_path, &progress) &&
            progress.generation == metadata_next_audio_generation &&
            progress.state == AUDIO_PLAYBACK_PROGRESS_ACTIVE && progress.is_playing &&
            progress.duration_seconds > 0.0 &&
            progress.duration_seconds - progress.position_seconds <= 30.0) {
            if (remote_track_path_is_remote(next_metadata_cached_path))
                remote_track_stream_prefetch(next_metadata_cached_path, metadata_next_audio_generation);
            if (metadata_next_gapless_enabled && current_settings.gapless_enabled)
                arm_cached_next_track(next_metadata_cached_path, &next_metadata_cache,
                                      metadata_next_audio_generation, metadata_next_gain_mode);
        }
    }
    if (!track_metadata_take(TRACK_METADATA_CURRENT, &result)) return;
    if (result.request.token != metadata_ui_token ||
        player_queue_model_current() < 0 || player_queue_model_current() >= player_queue_model_count() || !player_screen ||
        strcmp(result.request.path, playlist_path_at(player_queue_model_current())) != 0 ||
        strcmp(result.request.path, metadata_ui_path) != 0) return;
    if (result.display_hint) {
        /* A catalog snapshot can reach us before audio startup on a RAM
         * cache miss. Show it without finalizing lyrics, favorites, format
         * or plugin notifications; fresh file tags still own those. */
        char title[128], folder[128];
        get_display_names(result.request.path, title, sizeof(title), folder, sizeof(folder));
        track_metadata_t * hint = &result.request.metadata;
        player_set_track_display_text(hint->has_title ? hint->title : title,
                                      hint->has_artist ? hint->artist : folder,
                                      hint->has_album ? hint->album : "");
        dispatch_track_artwork(player_queue_model_current(), result.request.path, hint, result.request.token);
        player_transition_mark_dirty();
        return;
    }
#ifdef UI_PERF_TRACE
    printf("PERF player_metadata_request_to_result_ms elapsed=%u token=%llu\n",
           (unsigned)((ui_perf_now_us() - metadata_request_perf_us) / 1000),
           (unsigned long long) result.request.token);
#endif
    if (metadata_scalar_applied_token != result.request.token) {
        apply_loaded_track_metadata(player_queue_model_current(), &result.request.metadata, result.favorite,
                                     !result.enrichment_pending);
        metadata_scalar_applied_token = result.request.token;
    } else if (!result.enrichment_pending && metadata_favorite_revision == favorite_ui_revision) {
        apply_loaded_favorite(result.favorite);
    }
    if (metadata_ui_notify) {
        metadata_notify_payload = result.request.metadata;
        metadata_notify_ready = true;
        poll_track_started_notification();
    }
    if (deferred_resume_pending && result.has_probe && result.probe.duration_seconds > 0.0) {
        double position = isfinite(deferred_resume_position) ? deferred_resume_position : 0.0;
        if (position < 0.0) position = 0.0;
        if (position > result.probe.duration_seconds) position = result.probe.duration_seconds;

        int32_t percent = (int32_t) ((position / result.probe.duration_seconds) * 100.0);
        displayed_progress_percent = percent;
        player_seekbar_set_value(progress_slider, percent);

        displayed_position_second = (int) position;
        displayed_duration_second = (int) result.probe.duration_seconds;
        char time_str[24];
        gui_format_time(position, time_str, sizeof(time_str));
        if (pos_label) lv_label_set_text(pos_label, time_str);
        gui_format_time(result.probe.duration_seconds, time_str, sizeof(time_str));
        if (dur_label) lv_label_set_text(dur_label, time_str);
    }
}

/* Song long-press context menu's "Add to Queue" -- splices `path` into the
 * live playlist right after whatever was queued last (or right after the
 * currently-playing track, if the queue's currently empty), so repeated
 * Add to Queue taps play back in the order they were added. See
 * queued_pending_count's own comment for why this reuses playlist[]
 * directly instead of a separate list. No-op with a toast if nothing's
 * playing -- there's no "currently playing track" position to queue
 * after. */
void queue_add_song(const char * path) {
    if (path && path[0]) gui_player_queue_add_many(&path, 1);
}

static bool queue_budget_add(size_t * total, size_t bytes) {
    if (*total > SIZE_MAX - bytes) return false;
    *total += bytes;
    return true;
}

static bool queue_batch_fits_budget(const char * const *paths, int count,
                                    size_t staging_bytes, size_t memory_limit) {
    int queue_count = player_queue_model_count();
    int pending = player_queue_model_pending_count();
    if (!paths || count <= 0 || count > INT_MAX - queue_count ||
        count > INT_MAX - pending) return false;
    size_t total = staging_bytes;
    if ((size_t)count > SIZE_MAX / (3 * sizeof(char *) + sizeof(int)) ||
        !queue_budget_add(&total, (size_t)count * (3 * sizeof(char *) + sizeof(int)))) return false;
    for (int i = 0; i < count; i++) {
        if (!paths[i] || !queue_budget_add(&total, strlen(paths[i]) + 1)) return false;
    }
    return total <= memory_limit;
}

bool gui_player_queue_try_add_many_bounded(const char * const *paths, int count,
                                           size_t staging_bytes, size_t memory_limit) {
    if (!queue_batch_fits_budget(paths, count, staging_bytes, memory_limit)) return false;
    return gui_player_queue_try_add_many(paths, count);
}

bool gui_player_queue_try_add_many(const char * const *paths, int count) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    int old_count = player_queue_model_count();
    int old_pending = player_queue_model_pending_count();
    if (count <= 0 || count > INT_MAX - old_count || count > INT_MAX - old_pending) return false;

    if (!gui_player_has_active_track()) {
        char **copies = calloc((size_t)count, sizeof(*copies));
        if (!copies) return false;
        for (int i = 0; i < count; i++) {
            copies[i] = paths[i] ? strdup(paths[i]) : NULL;
            if (!copies[i]) {
                for (int j = 0; j < count; j++) free(copies[j]);
                free(copies);
                return false;
            }
        }
        free_playlist();
        clear_player_source();
        player_queue_model_install_take((player_queue_install_t){
            .paths = copies, .count = count, .current = 0, .pending_count = count - 1,
            .shuffle_pos = -1, .next_insert_index = count > 1 ? count : -1,
            .queue_sd_dirfd = -1
        });
        request_track_metadata(0, false, false, false);
        set_play_button_state(false);
        deferred_resume_pending = true; deferred_resume_position = 0.0;
        snprintf(current_settings.last_track, sizeof(current_settings.last_track), "%s",
                 player_queue_model_path_at(0));
        current_settings.last_position = 0;
        current_settings.last_source_kind = 0;
        current_settings.last_source_name[0] = '\0';
        settings_save_async(&current_settings);
        show_info_toast(TR("Queue ready. Press Play to start."));
        playback_intent_epoch++;
        return true;
    }
    int added = 0;
    if (!player_queue_model_try_add_many(paths, count, current_settings.play_mode, &added)) return false;
    int current = player_queue_model_current();
    player_set_song_count(current, player_queue_model_count());
    arm_next_track_for_audio(current);
    char msg[128];
    snprintf(msg, sizeof(msg), TR_N("Added %d song to queue", "Added %d songs to queue", added), added);
    show_info_toast(msg);
    playback_intent_epoch++;
    return true;
}

void gui_player_queue_add_many(const char * const * paths, int count) {
    (void) gui_player_queue_try_add_many(paths, count);
}

void queue_remove_song_at_offset(int offset) {
    int current = player_queue_model_current();
    if (offset < 0 || offset >= player_queue_model_pending_count() || current < 0) return;
    if (!player_queue_model_remove_pending(offset)) return;
    player_set_song_count(current, player_queue_model_count());
    arm_next_track_for_audio(current);
    show_info_toast(TR("Removed from queue"));
    playback_intent_epoch++;
}

void queue_clear_pending(void) {
    while (player_queue_model_pending_count() > 0) queue_remove_song_at_offset(player_queue_model_pending_count() - 1);
    show_info_toast(TR("Queue cleared"));
}

/* Bluetooth DAC mode and AirPlay receive mode both feed real-time audio
 * from another device straight into this device's own physical ALSA
 * hardware (hw:0,0) -- confirmed by checking what each one's consumer
 * process (aplay -D bluealsa; shairport -o ot) actually targets. Local
 * playback uses that same hardware directly via tinyalsa, so all three are
 * mutually exclusive: only one may be "using" the speaker/DAC output at a
 * time. bt_dac_toggle_cb()/airplay_toggle_cb() turn each other off when
 * either is enabled; this covers local playback's side of the same rule.
 *
 * bt_dac_mode_enabled is also gated on bt_is_powered_cached: it's a
 * persisted setting, so it can still read true from a previous session
 * after a reboot where Bluetooth was never turned back on (chip
 * re-init isn't automatic -- see local/notes/development/TESTING.md). With Bluetooth actually off,
 * bt_control_apply_output_settings() never started bluealsa/aplay, so
 * nothing is really using the DAC output -- blocking playback in that case
 * was a real dead end: the Bluetooth DAC toggle to turn it back off only
 * appears once Bluetooth is powered, which needs a chip re-init the app
 * itself has no way to trigger. */
/* Checks whether Bluetooth DAC or USB DAC mode is actively blocking
 * local playback. AirPlay discoverability does not block local playback;
 * active streams are auto-disconnected instead. */
static const char * external_dac_block_reason(void) {
    if (current_settings.bt_dac_mode_enabled && bt_is_powered_cached) return TR("Turn off Bluetooth DAC to play music on this device");
    if (current_settings.usb_mode == USB_MODE_DAC) return TR("Exit USB DAC mode to play music on this device");
    return NULL;
}

/* Cached now-playing metadata for plugin.get_now_playing() -- populated at
 * the same two call sites that fire the "track_started" plugin event
 * (notify_plugin_track_started() below), so there's no separate tracking
 * needed. plugin_now_playing_loaded distinguishes "nothing has ever played
 * this session" from "a track is playing with an empty title" -- the title
 * buffer alone can't tell those apart. */
char plugin_now_playing_title[128];
char plugin_now_playing_artist[128];
char plugin_now_playing_album[128];
double plugin_now_playing_duration;
bool plugin_now_playing_loaded = false;

/* Shared by play_track_at_from()/on_track_auto_advanced() below -- caches
 * meta into the plugin_now_playing_* globals above and fires the
 * "track_started" plugin event. Called after audio_play_file_at()/the
 * gapless handoff has already happened in both call sites, so
 * audio_get_duration_seconds() reflects the new track. */
static void notify_plugin_track_started(const track_metadata_t * meta, const char * path, double duration) {
    snprintf(plugin_now_playing_title, sizeof(plugin_now_playing_title), "%s", meta->title);
    snprintf(plugin_now_playing_artist, sizeof(plugin_now_playing_artist), "%s", meta->artist);
    snprintf(plugin_now_playing_album, sizeof(plugin_now_playing_album), "%s", meta->album);
    plugin_now_playing_duration = duration;
    plugin_now_playing_loaded = true;
    gui_lock_screen_refresh(false);

    remote_track_meta_t remote_meta;
    bool is_remote_track = remote_track_meta_copy_for_path(path, &remote_meta);
    plugin_manager_notify_track_started(meta->title, meta->artist, meta->album, plugin_now_playing_duration,
                                         is_remote_track ? remote_meta.provider : "", is_remote_track ? remote_meta.track_id : "");
}

static void play_track_at_from_internal(int index, double start_seconds, bool push_nav, bool skip_airplay_disconnect) {
    if (index < 0 || index >= player_queue_model_count()) {
        clear_selection_metadata_seed();
        return;
    }

    /* Symmetric with airplay_bridge.c's own side of this: an actual
     * incoming AirPlay stream starting stops local playback (airplay_
     * bridge_is_streaming() transitioning, see gui_network.c's gui_network_
     * poll_airplay_overlay()); starting local playback here while a stream
     * is already active should equally win and disconnect it, rather than
     * the two silently fighting over the shared output device or this call
     * failing/behaving oddly underneath a session it doesn't know about.
     * No-op when nothing is actively streaming. Must run before the block-
     * reason check below: that check no longer even considers AirPlay
     * (see external_dac_block_reason()'s own comment), but this call is
     * also what actually frees the output device before ensure_device()
     * further down tries to claim it.
     *
     * skip_airplay_disconnect exists solely for toggle_play_pause()'s own
     * resume-after-takeover path: it already called airplay_control_
     * disconnect_active_stream() itself to decide whether to resume at all,
     * and airplay_bridge_stop()/airplay_control_stop() are fire-and-forget
     * (see their own comments) -- is_streaming() can still read true for a
     * short window afterward. Calling disconnect a second time here could
     * see that stale true, and kill+respawn shairport a second time right
     * on top of the first respawn still settling. Every OTHER caller has
     * not already disconnected anything, so they must still run this call
     * normally. */
    if (!skip_airplay_disconnect) {
        airplay_control_disconnect_active_stream();
    }

    const char * block_reason = external_dac_block_reason();
    if (block_reason) {
        clear_selection_metadata_seed();
        show_error_toast(block_reason);
        return;
    }

    player_seek_input_progress_reset();
    deferred_resume_pending = false;
    deferred_resume_position = 0.0;
    player_queue_model_select_physical(index);

    const char *path = player_queue_model_path_at(index);
    track_metadata_t seed;
    bool local = seed_track_metadata(index, &seed);
    /* Present cached list metadata and begin artwork work before audio
     * startup. The fresh audio-tag request is promoted below after playback
     * has established the generation ReplayGain must belong to. */
    prepare_track_metadata(index, true, false, false, false);
    clear_selection_metadata_seed();
    track_metadata_count_play(path);
    if (local) audio_play_file_at_with_replaygain_mode(path, start_seconds, current_settings.replaygain_mode);
    else {
        bool has_gain, has_peak;
        double gain_db, peak;
        resolve_replaygain(&seed, &has_gain, &gain_db, &has_peak, &peak);
        audio_play_file_at(path, start_seconds, has_gain, gain_db, has_peak, peak);
    }
    if (push_nav) playback_intent_epoch++;
    current_playback_generation = audio_get_playback_generation();
    request_track_audio_metadata(index);
    arm_next_track_for_audio(index);

    set_play_button_state(true);
    /* Manual next/previous and a deferred startup resume can already be on
     * the player screen.  Do not stack a duplicate copy of the same screen
     * just because playback is being (re)started there. */
    if (push_nav && !stay_on_screen_once && lv_screen_active() != player_screen) nav_push(player_screen);
    stay_on_screen_once = false;

    /* A remote track's stream_url can expire or be single-use -- resuming
     * into it blind on next launch can't work the way resuming a local
     * file (or even a Subsonic stream, which at least re-derives its own
     * salted URL from a stable server+song id) can. Leave last_track
     * untouched rather than saving a synthetic key that build_saved_
     * resume_playlist() has no way to turn back into a playable URL --
     * same "don't build expiring-URL-aware resume in this pass" scope
     * call as everywhere else remote tracks touch existing machinery. */
    if (path[0] && !remote_track_path_is_remote(path)) {
        snprintf(current_settings.last_track, sizeof(current_settings.last_track), "%s", path);
        current_settings.last_position = start_seconds;
    }
    settings_save_async(&current_settings);
}

void play_track_at_from(int index, double start_seconds) {
    reset_decoder_failure_tracking();
    play_track_at_from_internal(index, start_seconds, true, false);
}

/* Called from update_timer_cb when audio_consume_track_advanced() reports
 * the playback thread moved on to the queued next track by itself (gapless
 * handoff or a completed crossfade) -- audio is already playing it, so
 * unlike play_track_at_from() this must NOT call audio_play_file_at()
 * (that would hard-restart audio that's already mid-track). */
void on_track_auto_advanced(int index) {
    if (index < 0 || index >= player_queue_model_count()) return;

    player_seek_input_progress_reset();
    player_queue_model_select_physical(index);

    current_playback_generation = audio_get_playback_generation();
    track_metadata_count_play(playlist_path_at(index));
    request_track_metadata(index, true, false, false);
    arm_next_track_for_audio(index);

    set_play_button_state(true);

    /* See play_track_at_from()'s own comment on why a remote track's
     * synthetic key is never saved as last_track. */
    const char *advanced_path = playlist_path_at(index);
    if (advanced_path[0] && !remote_track_path_is_remote(advanced_path)) {
        snprintf(current_settings.last_track, sizeof(current_settings.last_track), "%s", advanced_path);
        current_settings.last_position = 0.0;
    }
    settings_save_async(&current_settings);
}

void play_track_at(int index) {
    reset_decoder_failure_tracking();
    play_track_at_from_internal(index, 0.0, true, false);
}

void on_file_selected(char ** new_playlist, int count, int selected_index) {
    free_playlist();
    player_queue_model_install_take((player_queue_install_t){
        .paths = new_playlist, .count = count, .current = -1, .shuffle_pos = -1,
        .next_insert_index = -1, .queue_sd_dirfd = -1
    });
    queue_pin_sd_dir_if_needed();
    /* A brand new playback context (a fresh song tapped from any list)
     * invalidates whatever was queued against the OLD playlist[] -- those
     * array positions no longer mean anything once the array itself has
     * been replaced. Matches every other music app: starting something new
     * clears "Up Next". */
    play_track_at(selected_index);
}

/* on_file_selected() without opening the player screen. */
void on_file_selected_stay(char ** new_playlist, int count, int selected_index) {
    stay_on_screen_once = true;
    on_file_selected(new_playlist, count, selected_index);
    stay_on_screen_once = false;
}

/* Same as on_file_selected(), starting at a track-relative resume offset. */
void on_file_selected_at(char ** new_playlist, int count, int selected_index, double start_seconds) {
    free_playlist();
    player_queue_model_install_take((player_queue_install_t){
        .paths = new_playlist, .count = count, .current = -1, .shuffle_pos = -1,
        .next_insert_index = -1, .queue_sd_dirfd = -1
    });
    queue_pin_sd_dir_if_needed();
    play_track_at_from(selected_index, start_seconds);
}

/* set_player_source_group_songs() is defined further down, right after
 * group_songs_entries/count/title_label -- it needs those already in
 * scope. These three don't. */
void clear_player_source(void) {
    player_source_kind = PLAYER_SOURCE_NONE;
    player_source_all_songs_index = -1;
    player_source_recently_added_index = -1;
    free(player_source_group_title);
    player_source_group_title = NULL;
    free_group_song_entries(player_source_group_entries, player_source_group_count);
    player_source_group_entries = NULL;
    player_source_group_count = 0;
    player_source_group_pos = -1;
    player_source_file_browser_row = -1;
}

void set_player_source_all_songs(int display_index) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_ALL_SONGS;
    player_source_all_songs_index = display_index;
    current_settings.last_source_kind = 1;
    current_settings.last_source_name[0] = '\0';
}

void set_player_source_recently_added(int display_index) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_RECENTLY_ADDED;
    player_source_recently_added_index = display_index;
    /* No dedicated resume kind for this source -- falls back to "unknown",
     * same as Favorites/Most Played/user playlists (see group_song_row_
     * click_cb's own last_source_kind assignment); only All Songs and Album
     * get a real boot-resume slot (settings.h's own last_source_kind
     * comment). */
    current_settings.last_source_kind = 0;
    current_settings.last_source_name[0] = '\0';
}


void set_player_source_group_songs_direct(const group_song_entry_t * entries, int count, const char * title, int selected_index) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_GROUP_SONGS;
    copy_group_song_entries(&player_source_group_entries, entries, count);
    player_source_group_count = count;
    player_source_group_pos = selected_index;
    if (title) player_source_group_title = strdup(title);
    current_settings.last_source_kind = 2;
    snprintf(current_settings.last_source_name, sizeof(current_settings.last_source_name), "%s", title ? title : "");
}

void set_player_source_file_browser(const char * dir, int row) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_FILE_BROWSER;
    snprintf(player_source_file_browser_dir, sizeof(player_source_file_browser_dir), "%s", dir);
    player_source_file_browser_row = row;
    current_settings.last_source_kind = 0;
    current_settings.last_source_name[0] = '\0';
}

/* Wraps on_file_selected() as file_browser_create()'s on_select, rather than
 * passing on_file_selected directly, so the source snapshot above only
 * ever gets set for an actual folder-browse tap -- on_file_selected()
 * itself is shared by every play-launch path (All Songs, group songs,
 * Subsonic downloads, DLNA casts, ...) and has no way to tell which of
 * them is calling it. file_browser_get_last_selected_dir()/_row() are
 * only valid synchronously within file_browser.c's own select_cb() call,
 * which is exactly where this reads them. */
void on_file_browser_selected(char ** new_playlist, int count, int selected_index) {
    set_player_source_file_browser(file_browser_get_last_selected_dir(), file_browser_get_last_selected_row());
    on_file_selected(new_playlist, count, selected_index);
}

/* Installs a directory index as the queue and plays one playable ordinal.
 * Takes ownership of index. source_dir must stay valid through the call.
 * False when nothing changed (bad arguments or no memory). */
static bool play_file_browser_index(file_browser_index_t *index, unsigned playable_count,
                                    unsigned selected_playable, const char *source_dir, int source_row) {
    if (!index || !playable_count || selected_playable >= playable_count || playable_count > (unsigned) INT_MAX) {
        if (index) file_browser_index_close(index);
        return false;
    }
    char ** slots = calloc(playable_count, sizeof(*slots));
    int * order = malloc((size_t) playable_count * sizeof(*order));
    if (!slots || !order) {
        free(slots); free(order); file_browser_index_close(index); return false;
    }
    int pinned_queue_dirfd = open_queue_dir_from_file_index(index);
    for (unsigned i = 0; i < playable_count; i++) order[i] = (int) i;
    free_playlist();
    player_queue_model_install_take((player_queue_install_t){
        .paths = slots, .count = (int)playable_count, .current = -1,
        .lazy_ranks = order, .lazy_order_is_recency = false, .file_index = index,
        .shuffle_pos = -1, .next_insert_index = -1, .queue_sd_dirfd = pinned_queue_dirfd
    });
    set_player_source_file_browser(source_dir, source_row);
    play_track_at((int) selected_playable);
    return true;
}

void on_file_browser_index_selected(file_browser_index_t *index, unsigned playable_count,
                                    unsigned selected_playable) {
    (void) play_file_browser_index(index, playable_count, selected_playable,
                                   file_browser_get_last_selected_dir(), file_browser_get_last_selected_row());
}

/* Remote Control folder play: same queue a Files tap builds, the track's
 * directory index in browser order. */
bool gui_player_play_folder_track(const char * track_path) {
    file_browser_index_t * index = NULL;
    unsigned playable = 0, selected = 0;
    if (!file_browser_open_lazy_directory(track_path, &index, &playable, &selected)) return false;
    /* The index owns its directory string for as long as the queue holds it. */
    return play_file_browser_index(index, playable, selected, file_browser_index_directory(index), -1);
}

bool gui_player_play_folder_index(file_browser_index_t *index, unsigned playable_count,
                                  int play_mode) {
    if (play_mode != PLAY_MODE_SEQUENTIAL && play_mode != PLAY_MODE_SHUFFLE &&
        play_mode != PLAY_MODE_SHUFFLE_ONCE) {
        if (index) file_browser_index_close(index);
        return false;
    }
    unsigned initial = 0;
    if (play_mode_uses_shuffle_bag((play_mode_t) play_mode) && playable_count > 1)
        initial = (unsigned)player_queue_model_random_index((int)playable_count);
    gui_player_set_play_mode(play_mode);
    return play_file_browser_index(index, playable_count, initial,
                                   index ? file_browser_index_directory(index) : "", -1);
}

void toggle_play_pause(void) {
    reset_decoder_failure_tracking();
    if (player_queue_model_current() < 0) return; /* nothing loaded yet */
    /* Same reasoning as play_track_at_from_internal()'s own call -- local
     * playback (resuming here) should win over an active AirPlay stream,
     * not silently fight it or no-op underneath a session it doesn't know
     * about. No-op when nothing is actively streaming. */
    if (airplay_control_disconnect_active_stream()) {
        /* AirPlay's own audio_stop() already tore down have_current back
         * when the stream first took over the output device (see airplay_
         * bridge.c's run_session()) -- audio_toggle_pause() further below
         * checks !have_current and would be a guaranteed no-op here, doing
         * nothing while set_play_button_state(false) claims it did.
         * Resume directly from the pending-aware checkpoint rather than
         * falling through to a no-op pause toggle.
         *
         * Calls play_track_at_from_internal() directly (skip_airplay_
         * disconnect=true) rather than the public play_track_at_from() --
         * that would call airplay_control_disconnect_active_stream() a
         * SECOND time. airplay_control_stop()/airplay_bridge_stop() are
         * fire-and-forget (see their own comments), so is_streaming() can
         * still read true for a short window after the call just above
         * returned; a second disconnect seeing that stale true would kill
         * and respawn shairport again right on top of the first respawn
         * still settling. reset_decoder_failure_tracking() was already
         * called at the top of this function, so skip play_track_at_from()'s
         * own redundant call to it too. */
        play_track_at_from_internal(player_queue_model_current(), audio_get_resume_position_seconds(), true, true);
        return;
    }
    if (deferred_resume_pending) {
        double start_seconds = deferred_resume_position;
        deferred_resume_pending = false;
        deferred_resume_position = 0.0;
        play_track_at_from(player_queue_model_current(), start_seconds);
        return;
    }
    /* Same DAC-mode exclusion as play_track_at_from() -- only blocks
     * resuming (paused -> playing), pausing an already-playing track always
     * goes through (though bt_dac_toggle_cb()/airplay_toggle_cb() already
     * stop playback the moment either DAC mode turns on, so in practice
     * there's nothing left playing to pause by the time this could
     * matter). */
    if (!audio_is_playing()) {
        const char * block_reason = external_dac_block_reason();
        if (block_reason) {
            show_error_toast(block_reason);
            return;
        }
    }
    audio_toggle_pause();
    bool now_playing = audio_is_playing();
    set_play_button_state(now_playing);

    if (now_playing) {
        plugin_manager_notify_resumed();
    } else {
        /* Checkpoint the resume position on pause -- a natural point to
         * persist, and far less write-heavy than saving on every tick.
         * Also urgently (re)writes the queue-checkpoint file (the file
         * startup restore actually reads for SD/internal tracks, separate
         * from settings.txt's own last_position field), so a pause
         * immediately followed by any kind of reboot does not resume from
         * a stale pre-pause position. Keep this settings save synchronous:
         * the urgent checkpoint is part of the same reboot-sensitive pause
         * persistence boundary. */
        current_settings.last_position = audio_get_resume_position_seconds();
        settings_save(&current_settings);
        gui_player_queue_checkpoint_urgent();
        plugin_manager_notify_paused();
    }
}

void play_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    toggle_play_pause();
}

/* Standard CD-player/iPod convention: a tap partway into a track restarts
 * it; only a tap already near the start (first press rewinds, second press
 * goes to previous song) moves to the actual previous track -- no separate
 * double-tap timer needed, since "already near the start" is naturally true
 * right after the first tap rewound it. */
#define PREV_BUTTON_REWIND_THRESHOLD_SECONDS 3.0

void prev_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* Consume LVGL's CLICKED after a long press; PRESS_LOST cleanup is in the seek controller. */
    if (player_seek_input_consume_transport_click(-1)) return;
    reset_decoder_failure_tracking();
    if (player_queue_model_current() < 0) return;

    if (transport_skip_manual(-1)) return;
    if (audio_get_position_seconds() > PREV_BUTTON_REWIND_THRESHOLD_SECONDS) {
        audio_seek(0.0);
        return;
    }

    gui_player_step_manual(-1);
}

void next_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (player_seek_input_consume_transport_click(1)) return;
    reset_decoder_failure_tracking();
    if (player_queue_model_current() < 0) return;
    gui_player_step_manual(1);
}



/* The one place crossfade is applied, for both entry points (the Settings
 * switch below and the quick drawer's own tile) -- the gapless implication
 * here is easy to apply in one and forget in the other.
 *
 * Crossfade blends into an ARMED next track, so it cannot engage at all
 * while gapless is off, because then nothing gets armed (see
 * arm_next_track_for_audio()). Enabling crossfade therefore switches
 * gapless back on rather than leaving a control that reads On and silently
 * does nothing. */
void gui_player_set_crossfade_enabled(bool enabled) {
    current_settings.crossfade_enabled = enabled;
    if (enabled) current_settings.gapless_enabled = true;
    audio_set_crossfade_enabled(enabled);
    settings_save_async(&current_settings);
    /* Apply to the next transition, not the one after it. */
    if (gui_player_has_active_track()) arm_next_track_for_audio(player_queue_model_current());
    refresh_quick_drawer_crossfade_icon(); /* see its own comment -- keeps the drawer icon in sync */
    /* Both of gapless's own surfaces, since enabling crossfade may have just
     * switched it back on. Done here rather than in each caller so no entry
     * point can forget one of them. */
    gui_settings_sync_gapless_toggle();
    gui_shell_refresh_quick_drawer_expansion_toggles();
}

void crossfade_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    gui_player_set_crossfade_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

/* gui_player_set_crossfade_enabled()'s counterpart, and for the same reason:
 * two entry points (the Settings switch and the quick drawer tile) share
 * one rule, so it cannot be applied in one and missed in the other.
 *
 * Turning gapless off turns crossfade off with it -- crossfade has nothing
 * to blend into once no next track is armed. */
void gui_player_set_gapless_enabled(bool enabled) {
    current_settings.gapless_enabled = enabled;
    if (!enabled && current_settings.crossfade_enabled) {
        /* Sets crossfade false and saves; deliberately called before this
         * function's own save so the two flags persist together. */
        gui_player_set_crossfade_enabled(false);
        gui_settings_sync_crossfade_toggle();
    }
    settings_save_async(&current_settings);
    /* Apply to the next transition, not the one after it: an already-armed
     * next track would otherwise still hand off gaplessly once more. */
    if (gui_player_has_active_track()) arm_next_track_for_audio(player_queue_model_current());
    gui_settings_sync_gapless_toggle();
    gui_shell_refresh_quick_drawer_expansion_toggles();
}

void gui_player_set_replaygain_mode(int mode) {
    if (mode < 0 || mode > 2 || current_settings.replaygain_mode == mode) return;
    current_settings.replaygain_mode = mode;
    settings_save_async(&current_settings);
    /* The already prepared successor captured the old gain mode. Rebuild it
     * now so the new setting takes effect at the next transition. */
    if (player_queue_model_current() >= 0) arm_next_track_for_audio(player_queue_model_current());
}

void gapless_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    gui_player_set_gapless_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

void car_mode_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    gui_player_set_car_mode_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

void inline_remote_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.inline_remote_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    headphone_status_refresh_earpods_adc();
    settings_save_async(&current_settings);
}

void lyrics_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.lyrics_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
}

void swipe_up_home_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.swipe_up_home_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    gui_navigation_refresh_home_snapshot(); /* Home's frame exists only while the gesture is on */
    lv_obj_t * active = lv_screen_active();
    gui_shell_set_home_indicator_visible(current_settings.swipe_up_home_enabled &&
                                         active != gui_shell_get_home_screen() &&
                                         active != gui_lyrics_get_screen());
    settings_save_async(&current_settings);
}

void hide_player_topbar_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.hide_player_topbar = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
    sync_player_topbar_visibility(lv_screen_active());
    player_transition_mark_dirty(); /* topbar/back-button target-state visibility just changed -- see the cache's own doc comment */
}

void led_indicator_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.led_indicator_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    led_control_apply(current_settings.led_indicator_enabled);
    settings_save_async(&current_settings);
}

void charge_limiter_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.charge_limiter_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    charge_limiter_poll(current_settings.charge_limiter_enabled, true);
    settings_save_async(&current_settings);
}

void safe_charging_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.safe_charging_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    safe_charging_poll(current_settings.safe_charging_enabled, true);
    settings_save_async(&current_settings);
}

/* refresh_battery_topbar() (defined earlier in this file, topbar setup near
 * gui_init()'s own layout code) already re-syncs the wifi/bt icon positions
 * itself whenever battery_topbar_group's hidden flag actually changes --
 * called here so toggling this switch is reflected immediately, without
 * waiting for the next battery poll tick. */
void battery_percent_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.show_battery_percent = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
    refresh_battery_topbar();
}



/* Log-scale mapping so the slider gives fine control at low frequencies
 * (where the ear is more sensitive to small Hz changes) instead of wasting
 * most of the slider's travel on the top octave. */



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
        track_metadata_request_t request = { .token = ++metadata_ui_token, .probe = deferred_resume_pending };
        snprintf(request.path, sizeof(request.path), "%s", playlist_path_at(player_queue_model_current()));
        request.read_file = seed_track_metadata(player_queue_model_current(), &request.metadata);
        snprintf(metadata_ui_path, sizeof(metadata_ui_path), "%s", request.path);
        metadata_ui_audio_generation = audio_get_playback_generation();
        metadata_favorite_revision = favorite_ui_revision;
        track_metadata_submit(TRACK_METADATA_CURRENT, &request);
    }
    /* A UI reload keeps the decoded cover; show it again rather than the
     * placeholder, since the next track of the same album skips decoding. */
    if (current_cover_bytes && current_cover_dsc.data && cover_img) {
        lv_image_set_src(cover_img, &current_cover_dsc);
        fit_cover_img_to_card();
    }
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
        player_cover_prefetch_cancel();
        next_cover_prefetch_token = 0;
        player_reflection_cache_clear();
        player_artwork_jobs_partial_clear();
        player_artwork_jobs_advance_generation();
        ++metadata_ui_token;
        ++favorite_ui_revision;
    }
    /* Also cancel a pending fallback completion after an already-settled
     * transition, before any timeline or widget can be replaced. */
    player_inline_lyrics_cancel_timer();
    if (player_artwork_cache_timer) {
        lv_timer_delete(player_artwork_cache_timer);
        player_artwork_cache_timer = NULL;
    }
    lv_draw_sw_image_cache_set_transition_active(false);
    lv_draw_sw_image_cache_set_intermediate_source(NULL);
    player_inline_lyrics_set_open(false, false);
    player_timeline_labels_teardown();
    player_lyrics_backdrop_teardown();
    player_inline_lyrics_generated_release(false);
    if (!preserve_shared_state) {
        player_volume_popup_teardown();
        gui_popup_teardown(&delete_song_popup);
        if (more_menu_popup) { lv_obj_delete(more_menu_popup); more_menu_popup = NULL; }
        if (more_menu_popup_backdrop) { lv_obj_delete(more_menu_popup_backdrop); more_menu_popup_backdrop = NULL; }
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
        gui_popup_is_visible(&delete_song_popup) ||
        (volume_popup && !lv_obj_has_flag(volume_popup, LV_OBJ_FLAG_HIDDEN)) ||
        (more_menu_popup && !lv_obj_has_flag(more_menu_popup, LV_OBJ_FLAG_HIDDEN)) ||
        (more_menu_popup_backdrop && !lv_obj_has_flag(more_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN)))
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

void gui_player_poll_confirmed_playback(void) {
    if (consecutive_decoder_failure_skips > 0 && audio_is_playing()) {
        if (audio_get_position_seconds() >= 3.0) {
            reset_decoder_failure_tracking();
        }
    }
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
    int next_index = audio_is_playing() && player_queue_model_current() >= 0 &&
                     player_queue_model_current() < player_queue_model_count() ?
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
    player_cover_prefetch_cancel();
    next_cover_prefetch_token = 0;
    player_queue_storage_save_cancel_and_join();
    if (player_artwork_jobs_active()) player_artwork_jobs_cancel_worker();
    if (player_artwork_jobs_reload_active())
        gui_player_cancel_cover_reload();
}

const char *playlist_path_at(int index) {
    const char *path = player_queue_model_path_at(index);
    return path ? path : "";
}

void on_file_selected_lazy_all_songs(int selected_index) {
    int64_t count64 = metadata_db_get_song_count();
    int count = count64 > 0 ? (int) count64 : 0;
    if (count <= 0 || selected_index < 0 || selected_index >= count) return;

    char ** new_pl = calloc((size_t) count, sizeof(char *));
    int * new_order = malloc(sizeof(int) * (size_t) count);
    if (!new_pl || !new_order) {
        free(new_pl);
        free(new_order);
        return;
    }

    metadata_db_snapshot_t * snapshot = metadata_db_snapshot_open(false);
    if (!snapshot || metadata_db_snapshot_count(snapshot) != count) {
        if (snapshot) metadata_db_snapshot_close(snapshot);
        free(new_pl); free(new_order); return;
    }
    for (int i = 0; i < count; i++) new_order[i] = i;
    free_playlist();
    int queue_dirfd = open_queue_dir_from_snapshot(snapshot);
    player_queue_model_install_take((player_queue_install_t){
        .paths = new_pl, .count = count, .current = -1, .lazy_ranks = new_order,
        .lazy_order_is_recency = false, .snapshot = snapshot, .shuffle_pos = -1,
        .next_insert_index = -1, .queue_sd_dirfd = queue_dirfd
    });
    play_track_at(selected_index);
}

void on_file_selected_lazy_recently_added(int selected_index) {
    int64_t count64 = metadata_db_get_song_count();
    int count = count64 > 0 ? (int) count64 : 0;
    if (count <= 0 || selected_index < 0 || selected_index >= count) return;

    char ** new_pl = calloc((size_t) count, sizeof(char *));
    int * new_order = malloc(sizeof(int) * (size_t) count);
    if (!new_pl || !new_order) {
        free(new_pl);
        free(new_order);
        return;
    }

    metadata_db_snapshot_t * snapshot = metadata_db_snapshot_open(true);
    if (!snapshot || metadata_db_snapshot_count(snapshot) != count) {
        if (snapshot) metadata_db_snapshot_close(snapshot);
        free(new_pl); free(new_order); return;
    }
    for (int i = 0; i < count; i++) new_order[i] = i;
    free_playlist();
    int queue_dirfd = open_sd_queue_dir();
    player_queue_model_install_take((player_queue_install_t){
        .paths = new_pl, .count = count, .current = -1, .lazy_ranks = new_order,
        .lazy_order_is_recency = true, .snapshot = snapshot, .shuffle_pos = -1,
        .next_insert_index = -1, .queue_sd_dirfd = queue_dirfd
    });
    play_track_at(selected_index);
}

bool gui_player_play_lazy_collection(metadata_db_snapshot_t *snapshot,
                                     int *title_ranks, int count, int play_mode) {
    if (!snapshot || !title_ranks || count <= 0 || count > INT_MAX ||
        play_mode < PLAY_MODE_SEQUENTIAL || play_mode > PLAY_MODE_SHUFFLE_ONCE)
        return false;
    int snapshot_count = metadata_db_snapshot_count(snapshot);
    if (snapshot_count <= 0) return false;
    if ((size_t)snapshot_count > SIZE_MAX - 7u) return false;
    size_t seen_size = ((size_t)snapshot_count + 7u) / 8u;
    uint8_t *seen_ranks = calloc(seen_size, 1);
    if (!seen_ranks) return false;
    for (int i = 0; i < count; i++) {
        int rank = title_ranks[i];
        if (rank < 0 || rank >= snapshot_count) { free(seen_ranks); return false; }
        size_t byte = (size_t)rank >> 3;
        uint8_t mask = (uint8_t)(1u << ((unsigned)rank & 7u));
        if (seen_ranks[byte] & mask) { free(seen_ranks); return false; }
        seen_ranks[byte] |= mask;
    }
    free(seen_ranks);

    char **new_playlist = calloc((size_t)count, sizeof(*new_playlist));
    int *new_shuffle_order = NULL;
    if (!new_playlist) return false;
    if (play_mode == PLAY_MODE_SHUFFLE || play_mode == PLAY_MODE_SHUFFLE_ONCE) {
        int first = 0;
        if (!player_queue_model_make_shuffle_order(count, &new_shuffle_order, &first)) {
            free(new_playlist); return false;
        }
    }

    int start = new_shuffle_order ? new_shuffle_order[0] : 0;
    clear_player_source();
    current_settings.last_source_kind = 0;
    current_settings.last_source_name[0] = '\0';
    free_playlist();
    gui_player_set_play_mode(play_mode);
    int queue_dirfd = open_queue_dir_from_snapshot(snapshot);
    player_queue_model_install_take((player_queue_install_t){
        .paths = new_playlist, .count = count, .current = -1, .lazy_ranks = title_ranks,
        .lazy_order_is_recency = false, .snapshot = snapshot,
        .shuffle_order = new_shuffle_order, .shuffle_pos = new_shuffle_order ? 0 : -1,
        .next_insert_index = -1, .queue_sd_dirfd = queue_dirfd
    });
    play_track_at(start);
    return true;
}

bool gui_player_play_collection_paths(char **paths, int count, int play_mode) {
    if (!paths || count <= 0 || count > INT_MAX ||
        play_mode < PLAY_MODE_SEQUENTIAL || play_mode > PLAY_MODE_SHUFFLE_ONCE)
        return false;
    for (int i = 0; i < count; i++) if (!paths[i] || !paths[i][0]) return false;
    clear_player_source();
    current_settings.last_source_kind = 0;
    current_settings.last_source_name[0] = '\0';
    free_playlist();
    gui_player_set_play_mode(play_mode);
    player_queue_model_install_take((player_queue_install_t){
        .paths = paths, .count = count, .current = -1, .shuffle_pos = -1,
        .next_insert_index = -1, .queue_sd_dirfd = -1
    });
    queue_pin_sd_dir_if_needed();
    int start = 0;
    if (play_mode_uses_shuffle_bag((play_mode_t)play_mode) && count > 1)
        start = player_queue_model_random_index(count);
    play_track_at(start);
    return true;
}

static bool resume_playlist_needs_lazy_order = false;
static file_browser_index_t *resume_file_index = NULL;
#ifdef HOST_BUILD
#define QUEUE_RESUME_PATH "./.compas/queue.bin"
#define SD_QUEUE_RESUME_PATH "./music/.compas/queue.bin"
#define SD_QUEUE_RESUME_DIR "./music/.compas"
#else
#define QUEUE_RESUME_PATH "/usr/data/.compas/queue.bin"
#define SD_QUEUE_RESUME_PATH "/data/mnt/sd_0/.compas/queue.bin"
#define SD_QUEUE_RESUME_DIR "/data/mnt/sd_0/.compas"
#endif

/* A lazy All-Songs queue has no paths until playlist_path_at() resolves them.
 * Persisting one above this bound would materialize and strdup the whole
 * library, which is unbounded work for a periodic checkpoint on the target.
 * The settings last_track/last_source_kind resume path remains available. */
#define LAZY_QUEUE_CHECKPOINT_MAX_ENTRIES 32768

static atomic_bool sd_card_absent_immediate = false;

void gui_player_notify_sd_unmounted_immediate(void) {
    atomic_store(&sd_card_absent_immediate, true);
}

/* Call on every poll that currently observes the SD card mounted, regardless
 * of whether a full confirmed mount/unmount transition fires -- a transient
 * one-poll blip also calls gui_player_notify_sd_unmounted_immediate() above,
 * and this is what un-sticks the resulting flag for a card that was never
 * actually removed. */
void gui_player_notify_sd_mounted(void) {
    atomic_store(&sd_card_absent_immediate, false);
}

static bool is_sd_card_path(const char * path) {
    if (!path || !path[0]) return false;
    size_t root_len = strlen(MUSIC_ROOT_DIR);
    if (strncmp(path, MUSIC_ROOT_DIR, root_len) != 0) return false;
    return path[root_len] == '/' || path[root_len] == '\0';
}

static queue_resume_t restored_queue;
static bool have_restored_queue;

static atomic_bool checkpoint_urgent_pending;

static bool queue_checkpoint_is_large_lazy(void) {
    return player_queue_model_has_lazy_order() &&
           player_queue_model_count() > LAZY_QUEUE_CHECKPOINT_MAX_ENTRIES;
}

static int open_queue_dir(const char * path) {
    return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

static int open_sd_queue_dir(void) {
    /* Pin the root mount before creating/opening the child.  All later worker
     * operations use this descriptor, so a card replacement cannot redirect
     * rename/unlink to the new card. */
    int rootfd = open(MUSIC_ROOT_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rootfd < 0) return -1;
    (void) mkdirat(rootfd, COMPAS_STORAGE_DIR, 0755);
    int dirfd = openat(rootfd, COMPAS_STORAGE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    close(rootfd);
    return dirfd;
}

static int open_queue_dir_from_snapshot(const metadata_db_snapshot_t *snapshot) {
    int dbfd = metadata_db_snapshot_dup_directory_fd(snapshot);
    if (dbfd < 0) return -1;
    int rootfd = openat(dbfd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    close(dbfd);
    if (rootfd < 0) return -1;
    (void) mkdirat(rootfd, COMPAS_STORAGE_DIR, 0755);
    int dirfd = openat(rootfd, COMPAS_STORAGE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    close(rootfd);
    return dirfd;
}

static int open_queue_dir_from_file_index(const file_browser_index_t *index) {
    const char *dir = file_browser_index_directory(index);
    size_t root_len = strlen(MUSIC_ROOT_DIR);
    if (!dir || strncmp(dir, MUSIC_ROOT_DIR, root_len) != 0 ||
        (dir[root_len] != '/' && dir[root_len] != '\0')) return -1;
    int fd = file_browser_index_dup_directory_fd(index);
    if (fd < 0) return -1;
    const char *suffix = dir + root_len;
    unsigned levels = 0;
    for (const char *p = suffix; *p;) {
        while (*p == '/') p++;
        if (!*p) break;
        levels++;
        while (*p && *p != '/') p++;
    }
    for (unsigned i = 0; i < levels; i++) {
        int parent = openat(fd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent < 0) { close(fd); return -1; }
        close(fd); fd = parent;
    }
    (void) mkdirat(fd, COMPAS_STORAGE_DIR, 0755);
    int queuefd = openat(fd, COMPAS_STORAGE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    close(fd);
    return queuefd;
}

static int open_checkpoint_target_dir(const char * path) {
    char dir[PATH_MAX];
    const char * slash = strrchr(path, '/');
    if (!slash) return open_queue_dir(".");
    size_t n = (size_t) (slash - path);
    if (n == 0) { dir[0] = '/'; dir[1] = 0; }
    else if (n < sizeof(dir)) { memcpy(dir, path, n); dir[n] = 0; }
    else return -1;
    return open_queue_dir(dir);
}

/* Deduplication state belongs to the UI thread. The worker reports failure
 * separately; checkpoint revision lanes need no cross-thread atomics. */
static uint64_t checkpoint_revision_internal;
static double checkpoint_position_internal = -1;
static uint64_t checkpoint_revision_sd;
static double checkpoint_position_sd = -1;
/* Hash of the non-path file layout at the last full write (0 = none), so an
 * unchanged queue only rewrites the file header with the new position. */
static uint64_t checkpoint_layout_internal;
static uint64_t checkpoint_layout_sd;

bool gui_player_queue_write_busy(void) {
    return player_queue_storage_checkpoint_busy();
}

/* UI code may poll this and present one persistent status indicator; the flag
 * is deliberately level-triggered so a failed read-only card does not create
 * a toast on every periodic checkpoint attempt. */
bool gui_player_queue_checkpoint_failed(void) {
    return player_queue_storage_checkpoint_failed();
}

void gui_player_queue_checkpoint(void) {
    if (queue_checkpoint_is_large_lazy()) {
        /* Set this before observing the worker: if it is just finishing, the
         * worker will remove anything it wrote; the reap path below covers the
         * race where it has already published completion. */
        player_queue_storage_checkpoint_defer_unlink();
        atomic_store(&checkpoint_urgent_pending, false);
        if (!player_queue_storage_checkpoint_reap_if_done(NULL, NULL)) return;
        int internal_dirfd = open_checkpoint_target_dir(QUEUE_RESUME_PATH);
        if (internal_dirfd >= 0) { player_queue_storage_remove_checkpoint_at(internal_dirfd, "queue.bin"); close(internal_dirfd); }
        player_queue_model_remove_sd_checkpoint("queue.bin");
        checkpoint_revision_internal = 0;
        checkpoint_position_internal = -1;
        checkpoint_layout_internal = 0;
        checkpoint_layout_sd = 0;
        checkpoint_revision_sd = 0;
        checkpoint_position_sd = -1;
        player_queue_storage_checkpoint_clear_deferred_unlink();
        return;
    }

    bool completed, failed;
    if (!player_queue_storage_checkpoint_reap_if_done(&completed, &failed)) return;
    if (completed && failed) {
        checkpoint_revision_internal = 0;
        checkpoint_revision_sd = 0;
    }
    if (player_queue_storage_checkpoint_take_header_fallback()) {
        checkpoint_revision_internal = 0;
        checkpoint_revision_sd = 0;
    }
    player_queue_storage_checkpoint_clear_deferred_unlink();
    if (!gui_player_has_active_track()) return;
    double position = deferred_resume_pending ? deferred_resume_position : audio_get_resume_position_seconds();

    bool is_sd_persistable = true;
    int queue_count = player_queue_model_count();
    for (int i = 0; i < queue_count; i++) {
        const char *path = player_queue_model_path_at(i);
        if (!path || !path[0] || !is_sd_card_path(path) ||
            player_queue_model_is_stream_cache_path(path)) {
            is_sd_persistable = false;
            break;
        }
    }

    bool sd_mounted = sd_card_root_is_mounted() && !atomic_load(&sd_card_absent_immediate);

    const char * target_path = QUEUE_RESUME_PATH;
    const char * target_name = "queue.bin";
    int target_dirfd = open_checkpoint_target_dir(target_path);
    uint64_t * last_rev = &checkpoint_revision_internal;
    double * last_pos = &checkpoint_position_internal;
    uint64_t * last_layout = &checkpoint_layout_internal;

    if (is_sd_persistable && sd_mounted) {
        target_name = "queue.bin";
        target_path = SD_QUEUE_RESUME_PATH;
        if (target_dirfd >= 0) close(target_dirfd);
        /* A restored queue.bin fills playlist[] without going through
         * on_file_selected(), which is the only place that used to pin this
         * directory. Leaving it at -1 made every boot checkpoint fail before
         * any write, and the toast blamed a read-only card. */
        if (!player_queue_model_has_sd_dirfd_pin())
            player_queue_model_adopt_sd_dirfd(open_sd_queue_dir());
        target_dirfd = player_queue_model_dup_sd_dirfd();
        last_rev = &checkpoint_revision_sd;
        last_pos = &checkpoint_position_sd;
        last_layout = &checkpoint_layout_sd;
    }

    uint64_t revision = player_queue_model_revision();
    if (revision == *last_rev && fabs(position - *last_pos) < 1.0) { if (target_dirfd >= 0) close(target_dirfd); return; }
    if (target_dirfd < 0) { *last_rev = 0; player_queue_storage_checkpoint_mark_failed(); return; }

    queue_resume_header_t header;
    uint64_t layout = 0;
    bool have_header = player_queue_model_checkpoint_header(current_settings.play_mode, position, &header, &layout);
    if (have_header && revision == *last_rev && layout == *last_layout) {
        /* Same queue as the file on disk: no path copy, just the new position. */
        *last_pos = position;
        if (!player_queue_storage_checkpoint_start_header(header, target_dirfd, target_name)) *last_rev = 0;
        return;
    }

    queue_resume_t s = {0};
    if (!player_queue_model_checkpoint_snapshot(current_settings.play_mode, position, &s)) {
        close(target_dirfd); return;
    }
    *last_rev = revision;
    *last_pos = position;
    *last_layout = have_header ? layout : 0;

    if (!player_queue_storage_checkpoint_start(s, target_dirfd, target_name)) *last_rev = 0;
}

/* Like gui_player_queue_checkpoint(), but guarantees the request is not silently
 * dropped if a previous checkpoint's background worker is still writing: it marks
 * an urgent-pending flag instead, which gui_player_queue_poll_urgent() below
 * retries on the very next UI poll tick once the in-flight worker finishes,
 * rather than waiting for the next scheduled 30-second/1-second periodic tick. */
void gui_player_queue_checkpoint_urgent(void) {
    if (player_queue_storage_checkpoint_busy()) {
        atomic_store(&checkpoint_urgent_pending, true);
        return;
    }
    gui_player_queue_checkpoint();
}

/* Called every UI poll tick (gui_queue_poll()) to retry a checkpoint request that
 * gui_player_queue_checkpoint_urgent() deferred because a worker was already
 * in flight. No-op unless a retry is actually pending. */
void gui_player_queue_poll_urgent(void) {
    if (!atomic_load(&checkpoint_urgent_pending)) return;
    if (player_queue_storage_checkpoint_busy()) return; /* still busy */
    atomic_store(&checkpoint_urgent_pending, false);
    gui_player_queue_checkpoint();
}

void gui_player_queue_flush(void) {
    player_queue_storage_checkpoint_join();
    gui_player_queue_checkpoint();
    player_queue_storage_checkpoint_join();
}

void gui_player_handle_sd_unmount(void) {
    atomic_store(&sd_card_absent_immediate, true);
    checkpoint_revision_sd = 0;
    checkpoint_position_sd = -1;

    /* Deliberately do NOT join the queue-storage worker here, even if one is
     * still running: this runs on the UI thread (via poll_sd_card_hotplug()'s
     * 500ms timer), and the worker may be blocked inside a write()/fsync()
     * to the very card that's being pulled out from under it -- joining
     * unconditionally here would risk freezing the whole UI on a stalled/
     * hung SD write. This is safe to skip: the worker only ever touches its
     * own deep-copied queue_resume_t snapshot (built synchronously before
     * pthread_create()), never the live playlist[] this function is
     * about to free, so there's no actual data race to guard against by
     * waiting for it here. It will finish (or fail harmlessly, retried on
     * the next checkpoint) on its own; the service's nonblocking worker
     * status is rechecked by the next call to gui_player_queue_checkpoint(). */

    /* Lazy song queues belong to the SD database even when no path has been
     * resolved yet. Never resolve every entry of a 300K queue on the UI
     * thread just to decide whether it must be cleared. */
    bool has_sd_playlist = player_queue_model_has_lazy_order() ||
        player_queue_model_has_resolved_sd_path(MUSIC_ROOT_DIR);
    if (!has_sd_playlist) return;

    const char *cur_path = player_queue_model_path_at(player_queue_model_current());
    if (is_sd_card_path(cur_path)) {
        audio_stop();
        plugin_manager_notify_stopped();
    }
    free_playlist();
    clear_player_source();
    set_play_button_state(false);
    player_set_track_display_text(NULL, NULL, NULL);
    now_playing_genre[0] = '\0';
    now_playing_track_number = 0;
    if (lv_screen_active() == player_screen) nav_pop();
}

bool build_sd_card_resume_playlist(char *** out_playlist, int * out_count, int * out_index, double * out_position) {
    resume_playlist_needs_lazy_order = false;
    queue_resume_free(&restored_queue);
    have_restored_queue = false;

    if (!queue_resume_read(SD_QUEUE_RESUME_PATH, &restored_queue)) return false;

    if (restored_queue.count <= 0 || restored_queue.current < 0 || restored_queue.current >= restored_queue.count) {
        queue_resume_free(&restored_queue);
        return false;
    }

    bool all_sd = true;
    for (int i = 0; i < restored_queue.count; i++) {
        const char * path = restored_queue.paths[i];
        if (!path || !path[0] || !is_sd_card_path(path) ||
            player_queue_model_is_stream_cache_path(path)) {
            all_sd = false;
            break;
        }
    }
    if (!all_sd) {
        queue_resume_free(&restored_queue);
        return false;
    }

    *out_playlist = restored_queue.paths;
    *out_count = restored_queue.count;
    *out_index = restored_queue.current;
    if (out_position) *out_position = restored_queue.position;
    restored_queue.paths = NULL;
    have_restored_queue = true;
    return true;
}

int gui_player_boot_resume_mode(void) {
    if (!current_settings.car_mode_enabled) return current_settings.resume_mode;
    if (current_settings.car_mode_autoresume_enabled &&
        battery_get_external_power_state() == BATTERY_EXTERNAL_POWER_CONNECTED &&
        get_headphone_state() != HEADPHONE_STATE_NONE) return 1;
    /* Keep the saved position ready for Play without starting the output. */
    return 2;
}

bool gui_player_restore_sd_queue(bool is_boot) {
    atomic_store(&sd_card_absent_immediate, false);

    if (is_boot) {
        /* This can be called a second time shortly after boot -- once from
         * gui_init() directly, and again from poll_sd_card_hotplug()'s
         * boot-time-mount-race recheck (which covers the case where the
         * card wasn't mounted yet when gui_init() ran) -- both with
         * is_boot=true. Block on ANY already-loaded track, not just an
         * SD-based one: an SD-based track means the first call (or the
         * pre-existing car-mode/resume_mode fallback) already handled it,
         * so re-running here would free_playlist() and restart playback
         * moments after it had already started correctly -- but a NON-SD
         * track is just as much a reason to skip (e.g. the user started a
         * Subsonic stream while the card was out, reinserted it -- the
         * confirmed-reinsertion path already correctly preserved that
         * session, is_boot=true must not then clobber it on the very next
         * poll just because it doesn't recognize the path as "already SD").
         * Only actually restore here if nothing is loaded at all -- that's
         * the genuine late-mount case this second call exists for. */
        if (gui_player_has_active_track()) return false;
    } else {
        /* Preserve any loaded non-SD session, not just an actively-playing
         * one -- a paused (or deferred-pending) stream is just as much a
         * session the user cares about and doesn't want silently replaced
         * by a newly-inserted card's queue. */
        if (gui_player_has_active_track()) {
            const char * cur = playlist_path_at(player_queue_model_current());
            if (!is_sd_card_path(cur)) return false;
        }
    }

    char ** resume_playlist = NULL;
    int resume_count = 0, resume_index = 0;
    double position = 0.0;
    if (!build_sd_card_resume_playlist(&resume_playlist, &resume_count, &resume_index, &position)) return false;

    /* install_saved_resume_playlist() only swaps the queue bookkeeping
     * (free_playlist() + replace playlist[]/player_queue_model_count()) -- it never
     * touches the audio subsystem. The guards above block the common cases
     * where something is still active, but the is_boot=false path
     * deliberately still allows through an active track that's already
     * SD-based (a genuinely unusual leftover, since gui_player_handle_sd_unmount()
     * normally clears that on confirmed unmount) -- if that's somehow still
     * playing here, stop it first so playback and the newly-installed queue
     * never disagree about what's current. */
    if (gui_player_has_active_track() && audio_is_playing()) audio_stop();

    if (!install_saved_resume_playlist(resume_playlist, resume_count)) return false;

    int resume_mode = is_boot ? gui_player_boot_resume_mode() : current_settings.resume_mode;
    if (resume_mode == 1) {
        play_track_at_from(resume_index, position);
    } else if (resume_mode == 2) {
        prepare_deferred_resume(resume_index, position);
    } else {
        /* Resume disabled: load and display the queue/track, but don't
         * resume playback position -- deferred_resume_pending still needs
         * to be set (position 0, not the checkpoint's) so a later Play
         * actually starts this track from the top, matching the existing
         * "queue loaded, not playing" pattern used elsewhere in this file.
         * Without it, player_queue_model_current() is set but nothing is "current" in the
         * audio subsystem yet, so toggle_play_pause() would be a no-op. */
        player_queue_model_select_physical(resume_index);
        request_track_metadata(resume_index, false, false, false);
        set_play_button_state(false);
        deferred_resume_pending = true; deferred_resume_position = 0.0;
    }
    return true;
}

bool gui_player_queue_restart_displayed(int selected) {
    int physical = -1;
    return player_queue_model_restart_displayed(selected, current_settings.play_mode, &physical);
}

bool build_saved_resume_playlist(char *** out_playlist, int * out_count, int * out_index) {
    resume_playlist_needs_lazy_order = false;
    if (resume_file_index) {
        file_browser_index_close(resume_file_index);
        resume_file_index = NULL;
    }
    queue_resume_free(&restored_queue);
    have_restored_queue = false;
    if (queue_resume_read(QUEUE_RESUME_PATH, &restored_queue)) {
        bool local = true;
        for (int i = 0; i < restored_queue.count; i++) {
            const char * path = restored_queue.paths[i];
            if (remote_track_path_is_remote(path) || player_queue_model_is_stream_cache_path(path))
                local = false;
        }
        if (local && strcmp(restored_queue.paths[restored_queue.current], current_settings.last_track) == 0) {
            *out_playlist = restored_queue.paths;
            *out_count = restored_queue.count;
            *out_index = restored_queue.current;
            restored_queue.paths = NULL;
            current_settings.last_position = restored_queue.position;
            have_restored_queue = true;
            return true;
        }
        queue_resume_free(&restored_queue);
    }
    song_row_t current;
    bool indexed = metadata_db_get_song_by_path(current_settings.last_track, &current);

    if (indexed && current_settings.last_source_kind == 2 && current_settings.last_source_name[0] &&
        strcasecmp(current.tags.album, current_settings.last_source_name) == 0) {
        int64_t count64 = metadata_db_count_songs_filtered(NULL, NULL, current.tags.album_artist, current.tags.album, NULL);
        if (count64 > 0 && count64 <= INT_MAX) {
            int count = (int) count64;
            group_song_entry_t * entries = calloc((size_t) count, sizeof(*entries));
            char ** paths = calloc((size_t) count, sizeof(*paths));
            song_row_t rows[64];
            int loaded = 0, selected = -1;
            while (entries && paths && loaded < count) {
                int want = count - loaded;
                if (want > 64) want = 64;
                int got = metadata_db_get_songs_filtered_page(NULL, NULL, current.tags.album_artist,
                                                               current.tags.album, NULL, loaded, want, rows);
                if (got <= 0) break;
                for (int i = 0; i < got; i++) {
                    char title[128];
                    metadata_db_song_display_title(&rows[i], title, sizeof(title));
                    entries[loaded + i].path = strdup(rows[i].path);
                    entries[loaded + i].title = strdup(title);
                    paths[loaded + i] = strdup(rows[i].path);
                    if (strcmp(rows[i].path, current_settings.last_track) == 0) selected = loaded + i;
                    if (!entries[loaded + i].path || !entries[loaded + i].title || !paths[loaded + i]) {
                        got = 0;
                        break;
                    }
                }
                if (got <= 0) break;
                loaded += got;
                if (got < want) break;
            }
            if (loaded == count && selected >= 0) {
                set_player_source_group_songs_direct(entries, count, current.tags.album, selected);
                free_group_song_entries(entries, count);
                *out_playlist = paths;
                *out_count = count;
                *out_index = selected;
                return true;
            }
            if (entries) free_group_song_entries(entries, loaded);
            if (paths) {
                for (int i = 0; i < loaded; i++) free(paths[i]);
                free(paths);
            }
        }
    } else if (indexed && current_settings.last_source_kind == 1) {
        int64_t count64 = metadata_db_get_song_count();
        int64_t selected64 = metadata_db_get_song_title_offset(current_settings.last_track);
        if (count64 > 0 && count64 <= INT_MAX && selected64 >= 0 && selected64 < count64) {
            *out_playlist = calloc((size_t) count64, sizeof(char *));
            if (!*out_playlist) return false;
            *out_count = (int) count64;
            *out_index = (int) selected64;
            resume_playlist_needs_lazy_order = true;
            set_player_source_all_songs(*out_index);
            return true;
        }
    }

    /* A Files folder, including one too large to checkpoint, reopens its
     * directory index. Do not fall through to a full path allocation. */
    file_browser_index_t *folder = NULL;
    unsigned playable = 0, selected = 0;
    if (!current_settings.last_track[0] ||
        !file_browser_open_lazy_directory(current_settings.last_track, &folder, &playable, &selected) ||
        playable == 0 || playable > (unsigned) INT_MAX) {
        if (folder) file_browser_index_close(folder);
        return false;
    }
    char **slots = calloc(playable, sizeof(*slots));
    if (!slots) {
        file_browser_index_close(folder);
        return false;
    }
    *out_playlist = slots;
    *out_count = (int) playable;
    *out_index = (int) selected;
    resume_file_index = folder;
    return true;
}

bool install_saved_resume_playlist(char ** resume_playlist, int resume_count) {
    file_browser_index_t *file_index = resume_file_index;
    resume_file_index = NULL;
    int * new_order = NULL;
    if (resume_playlist_needs_lazy_order || file_index) {
        new_order = malloc(sizeof(int) * (size_t) resume_count);
        if (!new_order) {
            for (int i = 0; i < resume_count; i++) free(resume_playlist[i]);
            free(resume_playlist);
            if (file_index) file_browser_index_close(file_index);
            return false;
        }
        for (int i = 0; i < resume_count; i++) new_order[i] = i;
    }
    metadata_db_snapshot_t * resume_snapshot = NULL;
    if (resume_playlist_needs_lazy_order && !file_index) {
        resume_snapshot = metadata_db_snapshot_open(false);
        if (!resume_snapshot || metadata_db_snapshot_count(resume_snapshot) != resume_count) {
            if (resume_snapshot) metadata_db_snapshot_close(resume_snapshot);
            free(new_order);
            for (int i = 0; i < resume_count; i++) free(resume_playlist[i]);
            free(resume_playlist);
            return false;
        }
    }
    player_queue_install_t owned = {
        .paths = resume_playlist, .count = resume_count, .current = -1,
        .lazy_ranks = new_order, .lazy_order_is_recency = false,
        .snapshot = resume_snapshot, .file_index = file_index, .shuffle_pos = -1,
        .next_insert_index = -1, .queue_sd_dirfd = -1
    };
    if (have_restored_queue) {
        owned.pending_count = restored_queue.pending;
        owned.next_insert_index = owned.pending_count
            ? restored_queue.current + 1 + owned.pending_count : -1;
        owned.shuffle_order = restored_queue.order;
        owned.shuffle_continuation = restored_queue.continuation;
        owned.shuffle_pos = restored_queue.shuffle_pos;
        restored_queue.order = NULL;
        restored_queue.continuation = NULL;
        current_settings.play_mode = restored_queue.mode;
        queue_resume_free(&restored_queue);
        have_restored_queue = false;
    }
    free_playlist();
    owned.queue_sd_dirfd = file_index ? open_queue_dir_from_file_index(file_index)
        : (resume_snapshot ? open_queue_dir_from_snapshot(resume_snapshot) : -1);
    player_queue_model_install_take(owned);
    return true;
}

void prepare_deferred_resume(int index, double start_seconds) {
    player_queue_model_select_physical(index);
    set_play_button_state(false);
    deferred_resume_pending = true;
    deferred_resume_position = start_seconds;
    request_track_metadata(index, false, true, false);

    nav_push(player_screen);
}


int gui_player_get_playlist_count(void) { return player_queue_model_count(); }

int gui_player_get_playlist_index(void) { return player_queue_model_current(); }

bool gui_player_has_active_track(void) {
    int current = player_queue_model_current();
    return current >= 0 && current < player_queue_model_count();
}

const char * gui_player_get_current_track_path(void) {
    int current = player_queue_model_current();
    if (current < 0 || current >= player_queue_model_count()) return "";
    return player_queue_model_path_at(current);
}

const char * gui_player_get_track_path_at(int index) {
    return playlist_path_at(index);
}

bool gui_player_get_subsonic_track_identity(int track_index, const char * path,
                                            char * title, size_t title_size,
                                            char * artist, size_t artist_size,
                                            char * album, size_t album_size) {
    if (title && title_size) title[0] = '\0';
    if (artist && artist_size) artist[0] = '\0';
    if (album && album_size) album[0] = '\0';
    /* Snapshot rows use physical playlist slots; metadata can have moved
     * relative to those slots after a queue edit, so resolve by exact URL. */
    const subsonic_stream_song_meta_t * meta = subsonic_meta_for_path(track_index, path);
    if (!meta) return false;

    if (title && title_size) utf8_truncate_safe(title, meta->title, title_size);
    if (artist && artist_size) utf8_truncate_safe(artist, meta->artist, artist_size);
    if (album && album_size) utf8_truncate_safe(album, meta->album, album_size);
    return true;
}

int gui_player_get_queued_count(void) { return player_queue_model_pending_count(); }

const char * gui_player_get_queued_path_at(int offset) {
    int current = player_queue_model_current();
    if (offset < 0 || offset >= player_queue_model_pending_count() || current < 0) return "";
    int physical = current + 1 + offset;
    return physical < player_queue_model_count() ? player_queue_model_path_at(physical) : "";
}

void gui_player_queue_add(const char * path) {
    queue_add_song(path);
}

void gui_player_queue_remove_at(int offset) {
    queue_remove_song_at_offset(offset);
}

void gui_player_queue_clear(void) {
    queue_clear_pending();
}

uint64_t gui_player_queue_revision(void) { return player_queue_model_revision(); }

bool gui_player_queue_snapshot(int **order, int *count, int *current, uint64_t *revision) {
    if (!order || !count || !current || !revision) return false;
    player_queue_display_snapshot_t snapshot = {0};
    if (!player_queue_model_display_snapshot(current_settings.play_mode, &snapshot)) return false;
    *order = snapshot.display_order;
    *count = snapshot.count;
    *current = snapshot.current;
    *revision = snapshot.revision;
    return true;
}

/* Publish the displayed Up Next order only when the queue revision changes.
 * The remote listener owns its own copy, so it never reads playlist state
 * from its worker thread. This also seeds the endpoint the first time
 * Remote Control is enabled after boot/restoration. */
void gui_player_sync_remote_queue(void) {
    static uint64_t published_revision = 0;
    if (player_queue_model_revision() == published_revision) return;
    int * order = NULL, count = 0, current = -1;
    uint64_t revision = 0;
    if (!gui_player_queue_snapshot(&order, &count, &current, &revision)) return;
    if (revision == published_revision) { free(order); return; }
    int upcoming = current >= 0 && current < count ? count - current - 1 : 0;
    const char ** paths = upcoming ? malloc(sizeof(*paths) * (size_t) upcoming) : NULL;
    if (upcoming && !paths) { free(order); return; }
    for (int i = 0; i < upcoming; i++) paths[i] = playlist_path_at(order[current + 1 + i]);
    remote_control_sync_queue(paths, upcoming, revision);
    free(paths);
    free(order);
    published_revision = revision;
}

/* Maps a Remote Control Up Next offset (current track excluded) to its
 * displayed queue index, or -1 when the revision or offset is stale. */
static int remote_queue_displayed_index(int offset, uint64_t revision) {
    if (revision != player_queue_model_revision() || offset < 0) return -1;
    int * order = NULL, count = 0, current = -1;
    uint64_t actual = 0;
    if (!gui_player_queue_snapshot(&order, &count, &current, &actual)) return -1;
    free(order);
    if (actual != revision || current < 0 || offset >= count - current - 1) return -1;
    return current + 1 + offset;
}

bool gui_player_remote_queue_remove(int offset, uint64_t revision) {
    int from = remote_queue_displayed_index(offset, revision);
    return from >= 0 && gui_player_queue_edit(revision, from, -1);
}

bool gui_player_remote_queue_move(int from_offset, int to_offset, uint64_t revision) {
    int from = remote_queue_displayed_index(from_offset, revision);
    int to = remote_queue_displayed_index(to_offset, revision);
    return from >= 0 && to >= 0 && (from == to || gui_player_queue_edit(revision, from, to));
}

bool gui_player_remote_queue_play(int offset, uint64_t revision) {
    int index = remote_queue_displayed_index(offset, revision);
    return index >= 0 && gui_player_queue_select(revision, index);
}

bool gui_player_remote_queue_clear(uint64_t revision) {
    if (revision != player_queue_model_revision()) return false;
    gui_player_queue_clear_all();
    return revision != player_queue_model_revision();
}

bool gui_player_queue_edit(uint64_t revision, int from, int to) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    player_queue_edit_result_t result = player_queue_model_edit_displayed(
        revision, from, to, current_settings.play_mode);
    if (result != PLAYER_QUEUE_EDIT_OK) return false;
    arm_next_track_for_audio(player_queue_model_current());
    playback_intent_epoch++;
    return true;
}

bool gui_player_queue_select(uint64_t revision, int index) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    int physical = -1;
    if (!player_queue_model_select_displayed(revision, index, current_settings.play_mode, &physical))
        return false;
    play_track_at(physical);
    return true;
}

/* Clear surrounding queue entries while keeping the playing track active. */
void gui_player_queue_clear_all(void) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    if (!gui_player_has_active_track()) return;
    if (!player_queue_model_clear_to_current(current_settings.play_mode)) return;
    arm_next_track_for_audio(player_queue_model_current());
    playback_intent_epoch++;
}

void gui_player_queue_play_next(const char *path) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    if (!gui_player_has_active_track()) {
        queue_add_song(path);
        return;
    }
    if (!player_queue_model_play_next(path, current_settings.play_mode)) return;
    int current = player_queue_model_current();
    player_set_song_count(current, player_queue_model_count());
    arm_next_track_for_audio(current);
    char msg[128];
    snprintf(msg, sizeof(msg), TR_N("Added %d song to queue", "Added %d songs to queue", 1), 1);
    show_info_toast(msg);
    playback_intent_epoch++;
}

bool gui_player_queue_save_as(const char *name) {
    if (player_queue_storage_save_active()) return false;
    bool has_sd = player_queue_model_has_resolved_sd_path(MUSIC_ROOT_DIR);
    return player_queue_model_save_as(name, current_settings.play_mode, has_sd);
}

bool gui_player_queue_save_as_poll(bool *done, bool *ok) {
    player_queue_storage_save_completion_t completion;
    bool worker_done = false;
    if (!player_queue_storage_save_poll(&completion, &worker_done)) {
        if (done) *done = true;
        if (ok) *ok = false;
        return false;
    }
    if (!worker_done) { if (done) *done = false; return true; }
    struct stat job_dir, current_dir;
    bool same_destination = fstat(completion.output_dirfd, &job_dir) == 0 &&
                            stat(PLAYLISTS_DIR, &current_dir) == 0 &&
                            job_dir.st_dev == current_dir.st_dev && job_dir.st_ino == current_dir.st_ino;
    bool cache_bound = false;
    if (same_destination) {
        int parent = openat(completion.output_dirfd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent >= 0) { cache_bound = path_cache_bind_directory_fd(parent); close(parent); }
    }
    bool result = completion.worker_ok && same_destination && cache_bound && metadata_db_storage_current();
    if (result) { char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s", PLAYLISTS_DIR, completion.leaf); metadata_db_playlist_insert_one(path); }
    if (done) *done = true;
    if (ok) *ok = result;
    player_queue_storage_save_release_completed();
    return true;
}

void gui_player_play_at(int index) {
    play_track_at(index);
}

void gui_player_play_at_from(int index, double start_seconds) {
    play_track_at_from(index, start_seconds);
}

void gui_player_step_manual(int direction) {
    if (transport_skip_manual(direction)) return;
    reset_decoder_failure_tracking();
    if (player_queue_model_current() < 0) return;
    int next_idx = compute_manual_step_index(player_queue_model_current(), direction);
    if (next_idx >= 0) play_track_at(next_idx);
    else plugin_manager_notify_queue_exhausted(direction);
}

void gui_player_set_transport_skip(const char * directory, int seconds) {
    snprintf(transport_skip_directory, sizeof(transport_skip_directory), "%s", directory ? directory : "");
    size_t n = strlen(transport_skip_directory);
    while (n > 1 && transport_skip_directory[n - 1] == '/') transport_skip_directory[--n] = '\0';
    transport_skip_seconds = seconds;
}

static bool transport_skip_manual(int direction) {
    const char * path = gui_player_get_current_track_path();
    size_t n = strlen(transport_skip_directory);
    if (!transport_skip_seconds || !n || strncmp(path, transport_skip_directory, n) != 0 ||
        path[n] != '/' || (!audio_is_playing() && !audio_is_paused())) return false;
    /* Consume a press during decoder startup as well, so it cannot change files. */
    double duration = audio_get_duration_seconds();
    double position = audio_get_resume_position_seconds();
    if (!isfinite(duration) || duration <= 0 || !isfinite(position) || deferred_resume_pending) return true;
    double target = position + (direction < 0 ? -transport_skip_seconds : transport_skip_seconds);
    if (target < 0) target = 0;
    double max_target = duration > TRANSPORT_SEEK_EOF_GUARD_SECONDS ? duration - TRANSPORT_SEEK_EOF_GUARD_SECONDS : 0;
    if (target > max_target) target = max_target;
    audio_seek(target);
    return true;
}

lv_obj_t * gui_player_get_screen(void) {
    return player_screen;
}

lv_obj_t * gui_player_get_cover_img(void) {
    return cover_img;
}

const lv_image_dsc_t * gui_player_get_current_reflection_dsc(void) {
    return current_reflection_bytes ? &current_reflection_dsc : NULL;
}

bool gui_player_copy_cover_rgb565(int for_index, uint8_t * out, size_t out_size) {
    const size_t required = (size_t) COVER_ART_WIDTH * COVER_ART_HEIGHT * 2;
    if (!out || out_size < required || !current_cover_bytes || current_cover_for_index != for_index)
        return false;
    memcpy(out, current_cover_bytes, required);
    return true;
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

const char * gui_player_get_now_playing_genre(void) {
    return now_playing_genre;
}

int gui_player_get_now_playing_track_number(void) {
    return now_playing_track_number;
}


void gui_player_handle_auto_advance(void) {
    if (player_queue_model_current() < 0) return;
    reset_decoder_failure_tracking();
    int advanced_index = compute_auto_advance_index(player_queue_model_current());
    if (advanced_index >= 0) {
        commit_auto_advance();
        on_track_auto_advanced(advanced_index);
    }
}

void gui_player_handle_track_finished(void) {
    if (player_queue_model_current() < 0) return;
    reset_decoder_failure_tracking();
    int finished_index = compute_auto_advance_index(player_queue_model_current());
    if (finished_index >= 0) {
        commit_auto_advance();
        play_track_at_from_internal(finished_index, 0.0, false, false);
    } else {
        set_play_button_state(false);
    }
}

void gui_player_handle_playback_error_ex(audio_error_t err, uint64_t err_generation) {
    if (err == AUDIO_ERROR_NONE) return;
    if (player_queue_model_current() < 0) return;

    /* Stale-vs-race generation check: only reject an error OLDER than what
     * this function has last synced to -- playback_generation only ever
     * increases, so err_generation < current_playback_generation
     * unambiguously means this error is about a track already moved past.
     *
     * A plain != check is wrong here: the audio thread also bumps
     * playback_generation on its OWN via a crossfade/gapless promotion
     * (audio.c's track_advanced=true sites), which the GUI only learns
     * about by consuming audio_consume_track_advanced() -- and gui.c's
     * poll checks for an error FIRST, in a single if/else-if chain, so
     * that consume never runs this tick if an error is also pending. If
     * the newly-promoted track then fails within the same ~500ms poll
     * gap (easily possible -- the audio thread runs far more iterations
     * than that per poll), the error's generation is NEWER than what this
     * function has seen, not stale -- but a plain != check can't tell
     * "newer" from "older" and would silently drop a real failure while
     * audio_consume_error_ex() has already cleared it for good, leaving
     * the UI believing a track that never actually played is still
     * playing. Catch up to that pending promotion first (the same call
     * gui.c's own else-if branch would have made this tick, had the error
     * not taken priority) so the failure is attributed to the track it
     * actually happened on. */
    if (err_generation != 0 && current_playback_generation != 0 &&
        err_generation != current_playback_generation) {
        if (err_generation < current_playback_generation) return;
        if (!audio_consume_track_advanced()) return;
        gui_player_handle_auto_advance();
        if (current_playback_generation != err_generation) return;
    }

    if (err == AUDIO_ERROR_OUTPUT_FAILED) {
        /* Checkpoint confirmed position so pressing Play can retry cleanly */
        double pos = audio_get_resume_position_seconds();
        deferred_resume_position = (pos > 0.0) ? pos : 0.0;
        deferred_resume_pending = true;
        set_play_button_state(false);
        show_error_toast(TR("Playback error: audio output failed"));
        return;
    }

    if (err == AUDIO_ERROR_DECODER_FAILED || err == AUDIO_ERROR_FILE_UNAVAILABLE ||
        err == AUDIO_ERROR_UNSUPPORTED_FORMAT || err == AUDIO_ERROR_UNSUPPORTED_CHANNELS) {
        char failure_message[384];
        /* Record the failed track's physical path */
        const char * failed_path = playlist_path_at(player_queue_model_current());
        if (failed_path) record_failed_physical_path(failed_path);
        char remote_error[256] = {0};
        const char * error_text = TR(audio_error_description(err));
        if (failed_path && remote_track_path_is_remote(failed_path) &&
            remote_track_stream_error_for_path(failed_path, err_generation,
                                                remote_error, sizeof(remote_error)) &&
            remote_error[0]) error_text = TR(remote_error);

        consecutive_decoder_failure_skips++;
        int queue_count = player_queue_model_count();
        int max_skips = (queue_count < 5) ? queue_count : 5;
        if (consecutive_decoder_failure_skips >= max_skips) {
            set_play_button_state(false);
            deferred_resume_pending = false;
            deferred_resume_position = 0.0;
            reset_decoder_failure_tracking();
            snprintf(failure_message, sizeof(failure_message), TR("Playback stopped: %s"), error_text);
            show_error_toast(failure_message);
            return;
        }

        const char *failed_paths[MAX_FAILED_PHYSICAL_PATHS];
        for (int i = 0; i < failed_physical_paths_count; i++)
            failed_paths[i] = failed_physical_paths[i];
        int target_index = player_queue_model_advance_past_failed(
            current_settings.play_mode, failed_paths, failed_physical_paths_count);

        if (target_index < 0) {
            set_play_button_state(false);
            deferred_resume_pending = false;
            deferred_resume_position = 0.0;
            reset_decoder_failure_tracking();
            snprintf(failure_message, sizeof(failure_message), TR("Playback stopped: %s"), error_text);
            show_error_toast(failure_message);
            return;
        }

        deferred_resume_pending = false;
        deferred_resume_position = 0.0;
        snprintf(failure_message, sizeof(failure_message), TR("Skipped: %s"), error_text);
        show_error_toast(failure_message);
        play_track_at_from_internal(target_index, 0.0, false, false);
    }
}

void gui_player_handle_playback_error(audio_error_t err) {
    gui_player_handle_playback_error_ex(err, 0);
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

const lv_image_dsc_t * gui_player_get_current_cover_dsc(uint64_t * generation) {
    if (generation) *generation = current_cover_generation;
    if (!current_cover_bytes) return NULL;
    return &current_cover_dsc;
}
