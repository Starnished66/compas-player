#include <limits.h>
#include "metadata.h"
#include "board_config.h"
#include "i18n.h"
#include "usb_mode_control.h"
#include "file_browser.h"
#include "remote_control.h"
#include "hiby_sys_server.h"
#include "bt_cover_art.h"
#include "backlight.h"
#include "led_control.h"
#include "charge_limiter.h"
#include "headphone_status.h"
#include "battery.h"

/* ---- Playback state and advance machinery ---- */
static char ** playlist = NULL;
static int playlist_count = 0;
static int playlist_index = -1;
static char transport_skip_directory[PATH_MAX];
static int transport_skip_seconds;
static bool transport_skip_manual(int direction);
static char now_playing_genre[128];
static int now_playing_track_number;
static int * playlist_lazy_sort_order = NULL;
static bool playlist_lazy_order_is_recency = false;
static struct tagcache_snapshot * playlist_lazy_snapshot = NULL;
static file_browser_index_t * playlist_lazy_file_index = NULL;
static int queued_pending_count = 0;
static int queue_sd_dirfd = -1;
static int queue_next_insert_index = -1;
static int consecutive_decoder_failure_skips = 0;
#define MAX_FAILED_PHYSICAL_PATHS 5
static char failed_physical_paths[MAX_FAILED_PHYSICAL_PATHS][PATH_MAX];
static int failed_physical_paths_count = 0;
extern bool plugin_now_playing_loaded;
static uint64_t current_playback_generation = 0;
static uint64_t metadata_ui_token, metadata_next_token, metadata_next_queue_revision;
static uint64_t metadata_ui_audio_generation, metadata_next_audio_generation;
static uint64_t favorite_ui_revision, metadata_favorite_revision;
static uint64_t favorite_request_token, favorite_request_revision;
static char favorite_request_path[PATH_MAX];
static bool favorite_track_known, favorite_toggle_pending;
static bool metadata_ui_notify, metadata_notify_ready;
static track_metadata_t metadata_notify_payload;
static char metadata_ui_path[PATH_MAX], metadata_next_current_path[PATH_MAX];
static int metadata_next_gain_mode;
static char armed_next_path[PATH_MAX];
static uint64_t armed_next_generation;
static int armed_next_gain_mode;
static bool next_metadata_cached;
static char next_metadata_cached_path[PATH_MAX];
static track_metadata_t next_metadata_cache;

static int open_sd_queue_dir(void);
static int open_queue_dir_from_snapshot(const struct tagcache_snapshot *snapshot);
static int open_queue_dir_from_file_index(const file_browser_index_t *index);
static void queue_save_cancel(void);
static bool queue_save_active(void);
static bool is_sd_card_path(const char *path);

#include "audio_helpers.h"
#include "hw_volume_coalesce.h"
#include "gui_player.h"
#include "app_clock.h"
#include "gui.h"
#include "gui_theme.h"
#include "gui_notifications.h"
#include "gui_library.h"
#include "gui_queue.h"
#include "playlist_files.h"
#include "favorite_writer.h"
#include "path_cache.h"
#include "cover_raster_cache.h"
#include "ui_wake.h"
#include "track_metadata_worker.h"
#include "playback_order.h"
#include "queue_resume.h"
#ifdef HOST_BUILD
#define MUSIC_ROOT_DIR "./music"
#else
#define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif
#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"
#define SUBSONIC_STREAM_CACHE_DIR "/tmp/subsonic_stream_cache"
#include "gui_lyrics.h"
#include "frosted_glass.h"
#include "player_layout.h"
#include "player_layouts.h"
#include "player_seekbar.h"
#include "player_cover_fade.h"
#include "player_lyrics_backdrop.h"
#include "player_timeline_labels.h"
#include "transition_compositor.h"
#include "lvgl/src/core/lv_obj_draw_private.h"
#include "lvgl/src/draw/sw/lv_draw_sw_private.h"
#include "artwork_coordinator.h"
#include "waveform.h"
#include "gui_track_info.h"
#include "gui_settings.h"
#include "gui_subsonic.h"
#include "gui_lock_screen.h"
#include "fallback_font.h"
#include "http_client.h"
#include "screen_builders.h"
static void notify_plugin_track_started(const track_metadata_t * meta, const char * path, double duration);

#include "metadata_db.h"
#include "cover_decode.h"
#include "db_log.h"
#include "airplay_control.h"
#include "albumart.h"
#include "audio.h"
#include "settings.h"
#include "assets.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "device_config.h"
#include "storage_paths.h"
#include "plugin_manager.h"
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
#include "metadata.h"
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

/* Player layout state, see the layout section above build_player_screen().
 * Declared here because the metadata and lyrics code read it. */
static bool player_layout_builtin = true;
static lv_obj_t * player_layout_root;
static lv_anim_timeline_t * player_timeline_lyrics_open;
static lv_anim_timeline_t * player_timeline_lyrics_close;
static lv_anim_timeline_t * player_timeline_track_change;
static lv_anim_timeline_t * player_timeline_screen_enter;
/* The enlarged touch targets the binder adds beside the layout's own
 * widgets, and the layout's own controls that the lyrics view hides. The
 * built-in layout hides every child of the screen other than the cover and
 * metadata instead, so it leaves the control list empty. */
#define PLAYER_MAX_HIT_TARGETS 8
#define PLAYER_MAX_LYRICS_CONTROLS 16
static lv_obj_t * player_hit_targets[PLAYER_MAX_HIT_TARGETS];
static unsigned player_hit_target_count;
static lv_obj_t * player_lyrics_controls[PLAYER_MAX_LYRICS_CONTROLS];
static unsigned player_lyrics_control_count;
/* A layout's `lyrics_area`: where the lyrics are shown instead of below the
 * metadata, and what they look like there (read once, at bind time). A layout
 * with an area owns the placement, so the app neither runs the C morph nor
 * hides the controls. */
static lv_obj_t * player_lyrics_area;
/* Optional custom-layout roles. The thumbnail shares the decoded cover;
 * the play circle provides a larger hit target around the stock play icon. */
static lv_obj_t * player_cover_thumbnail;
static lv_obj_t * player_play_circle;
static lv_draw_buf_t * player_timeline_cover_frame;
static const void * player_timeline_cover_source;
static bool player_timeline_cover_source_owned;
/* One exact endpoint raster, reused while its artwork and widget tree stay
 * unchanged. The active binding borrows this buffer; transient snapshots
 * for unsupported sources retain their original ownership. */
static lv_draw_buf_t * player_timeline_cover_cache;
static uint64_t player_timeline_cover_cache_generation;
static uint64_t player_timeline_cover_style_epoch;
static uint64_t player_timeline_cover_cache_epoch;
static bool player_timeline_cover_cache_antialias;
static bool player_timeline_cover_internal;
static lv_display_t * player_timeline_cover_cache_display;
static void player_timeline_cover_cache_invalidate(void);
static lv_timer_t * player_artwork_cache_timer;
/* Layouts without artist/album roles get hidden labels for the player
 * geometry code. They are not layout widgets: never laid out, scrolled,
 * styled or moved by the lyrics view. */
static bool player_artist_standin;
static bool player_album_standin;
static void player_now_playing_group_click_cb(lv_event_t * e);

static void player_lyrics_timeline_completed_cb(lv_anim_t * anim);

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

lv_obj_t * volume_popup = NULL;
lv_obj_t * volume_popup_backdrop = NULL;
lv_obj_t * more_menu_popup = NULL;
lv_obj_t * more_menu_popup_backdrop = NULL;

static lv_obj_t * volume_popup_track = NULL;
static lv_obj_t * volume_popup_speaker_icon = NULL;
static lv_obj_t * volume_popup_value_label = NULL;
static lv_timer_t * volume_popup_hide_timer = NULL;
static asset_decoded_image_t volume_popup_speaker_image;
/* Decoded copies of btn_play.png / btn_pause.png with the baked-in cyan
 * glyph rewritten to the current accent. Kept across widget teardown so
 * the quick-drawer button (destroyed later in gui_shell_teardown) never
 * holds a freed src pointer during reload. */
static asset_decoded_image_t play_btn_play_img;
static asset_decoded_image_t play_btn_pause_img;
static void load_play_btn_images(void);
static void player_fit_small_screen_icon(lv_obj_t * icon, int32_t reference_px);
static gui_popup_t delete_song_popup;
static lv_obj_t * delete_song_popup_title = NULL;

static lv_image_dsc_t current_cover_dsc;

static uint8_t * current_cover_bytes = NULL;
static int current_cover_for_index = -1;

/* Which generated cache file current_cover_bytes was decoded from. A rebuild
 * is an atomic rename, so the inode changes even when the size (fixed per
 * BMP size) and the mtime second do not. */
typedef struct {
    bool valid;
    dev_t dev;
    ino_t ino;
    time_t mtime;
    off_t size;
} cover_file_id_t;

static cover_file_id_t current_cover_file;

static bool cover_file_id_same(const cover_file_id_t * a, const cover_file_id_t * b) {
    return a->valid && b->valid && a->dev == b->dev && a->ino == b->ino &&
           a->mtime == b->mtime && a->size == b->size;
}

static uint8_t * current_reflection_bytes = NULL;
static lv_image_dsc_t current_reflection_dsc;
static uint64_t current_cover_generation;
static struct {
    bool valid;
    const uint8_t * cover_bytes;
    uint64_t cover_generation;
    int blur_radius, blur_passes, darken_num, darken_den;
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

void poll_volume_popup_timeout(void) {
    if (volume_popup && !lv_obj_has_flag(volume_popup, LV_OBJ_FLAG_HIDDEN)) {
        /* handled by timer */
    }
}


/* Transient volume popup (Task #28 / closes Task #17): built once, hidden,
 * on the top layer -- same reasoning as the status bar, but shown only for
 * a couple of seconds after the hardware volume buttons change the level,
 * then auto-hidden, instead of a permanently visible slider. */

static void volume_popup_hide_timer_cb(lv_timer_t * timer) {
    (void) timer;
    lv_obj_add_flag(volume_popup, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(volume_popup_hide_timer);
}

/* hw_volume_coalesce.h -- extracted from what used to be this file's own
 * private volume_hw_pending/volume_hw_apply_timer/volume_drag_active
 * machinery, so the quick drawer's own volume slider (gui_shell.c) can
 * reuse the exact same, already-proven debounce instead of a second,
 * simpler, worse copy of it. Behavior here is unchanged. */
static hw_volume_coalesce_t volume_popup_hv = { .pending = -1 };

/* Handles touch/drag interactions on the popup volume slider. Its knob
 * follows LVGL directly; secondary displays and hardware consume the
 * coalesced value above. */
static void volume_popup_track_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t percent = lv_slider_get_value(lv_event_get_target(e));

    if (code == LV_EVENT_PRESSED) {
        hw_volume_coalesce_drag_begin(&volume_popup_hv);
        /* Stop the 1.5s auto-hide countdown while a finger's still on it --
         * otherwise a slow drag could get hidden out from under the user
         * mid-interaction. */
        lv_timer_pause(volume_popup_hide_timer);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        hw_volume_coalesce_drag_update(&volume_popup_hv, (int) percent);
        if (volume_popup_value_label) lv_label_set_text_fmt(volume_popup_value_label, "%d", (int) percent);
        refresh_volume_topbar(percent);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        hw_volume_coalesce_drag_end(&volume_popup_hv, (int) percent);
        if (volume_slider) lv_slider_set_value(volume_slider, percent, LV_ANIM_OFF);
        gui_player_remember_volume_percent(percent);
        settings_save_async(&current_settings);
        lv_timer_reset(volume_popup_hide_timer);
        lv_timer_resume(volume_popup_hide_timer);
    }
}

/* The stock rail sprites are 360 px wide. LVGL centers a background image
 * at its native size instead of scaling it to the part, so they protrude
 * from the 360 px volume rail and the dynamically narrower brightness rail.
 * Paint those two rails natively; knobs use the shared vector ring style. */
void configure_native_slider_rail(lv_obj_t * slider) {
    lv_obj_set_style_bg_image_src(slider, NULL, LV_PART_MAIN);
    lv_obj_set_style_bg_image_src(slider, NULL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_make(132, 134, 132), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
}

bool gui_player_volume_control_hit_test(lv_point_t point) {
    if (!volume_popup_track || !volume_popup ||
        lv_obj_has_flag(volume_popup, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_area_t area;
    lv_obj_get_coords(volume_popup_track, &area);
    lv_area_increase(&area, 24, 24); /* matches build_volume_popup()'s hit area */
    return point.x >= area.x1 && point.x <= area.x2 &&
           point.y >= area.y1 && point.y <= area.y2;
}

/* Same card as the quick drawer's volume row (gui_shell.c): 413x73 rounded
 * panel, speaker icon, rail and a right-aligned numeric value, so the
 * hardware-button indicator reads exactly like the drawer's control. */
static void build_volume_popup(void) {
    lv_obj_t * top = lv_layer_top();

    volume_popup = lv_obj_create(top);
    lv_obj_remove_style_all(volume_popup);
    lv_obj_set_size(volume_popup, BOARD_SCALE_PX(413), BOARD_SCALE_PY(73));
    lv_obj_align(volume_popup, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + BOARD_SCALE_PX(12));
    lv_obj_set_style_bg_color(volume_popup, lv_color_hex(0x151b17), 0);
    lv_obj_set_style_bg_opa(volume_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(volume_popup, BOARD_SCALE_PX(23), 0);
    lv_obj_remove_flag(volume_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(volume_popup, LV_OBJ_FLAG_HIDDEN);

    volume_popup_speaker_icon = lv_image_create(volume_popup);
    const void * speaker = asset_decoded_image_open(&volume_popup_speaker_image, "volume/vol.png")
                         ? asset_decoded_image_source(&volume_popup_speaker_image) : NULL;
    lv_image_set_src(volume_popup_speaker_icon, speaker ? speaker : asset_path("volume/vol.png"));
    player_fit_small_screen_icon(volume_popup_speaker_icon, 30);
    lv_obj_align(volume_popup_speaker_icon, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(21), 0);

    /* Drawer geometry relative to its card (x=34): track at 103, value
     * right edge at 428, a 16px gap before the widest value ("100"). */
    lv_point_t value_size;
    lv_text_get_size(&value_size, "100", &app_font_player_meta, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t track_w = BOARD_SCALE_PX(428) - value_size.x - BOARD_SCALE_PX(16) - BOARD_SCALE_PX(103);
    if (track_w > BOARD_SCALE_PX(300)) track_w = BOARD_SCALE_PX(300);
    if (track_w < BOARD_SCALE_PX(120)) track_w = BOARD_SCALE_PX(120);

    volume_popup_value_label = lv_label_create(volume_popup);
    lv_obj_add_style(volume_popup_value_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(volume_popup_value_label, &app_font_player_meta, 0);
    lv_obj_align(volume_popup_value_label, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(19), 0);
    lv_label_set_text_fmt(volume_popup_value_label, "%d", (int) gui_player_get_volume_percent());

    volume_popup_track = lv_slider_create(volume_popup);
    lv_obj_set_size(volume_popup_track, track_w, SLIDER_TRACK_HEIGHT);
    lv_obj_align(volume_popup_track, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(69), 0);
    lv_slider_set_range(volume_popup_track, 0, 100);
    lv_obj_set_style_bg_color(volume_popup_track, lv_color_black(), LV_PART_MAIN);
    lv_obj_add_style(volume_popup_track, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(volume_popup_track, gui_theme_accent_knob_style(), LV_PART_KNOB);
    configure_native_slider_rail(volume_popup_track);
    lv_obj_set_style_bg_opa(volume_popup_track, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_width(volume_popup_track, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_set_style_height(volume_popup_track, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_add_event_cb(volume_popup_track, volume_popup_track_event_cb, LV_EVENT_ALL, NULL);

    /* Forgiving vertical capture area for fast drags; the hit test in
     * gui_player_volume_control_hit_test() uses the same 24px margin. */
    lv_obj_set_ext_click_area(volume_popup_track, 24);

    volume_popup_hide_timer = lv_timer_create(volume_popup_hide_timer_cb, 1500, NULL);
    lv_timer_pause(volume_popup_hide_timer);
    /* hw_volume_coalesce_drag_begin() creates its own timer lazily on the
     * first drag -- no eager creation needed here. */
}

/* Generates a "frosted mirror" look behind the transport controls
 * (player_overlay_panel, built in build_player_screen()): a heavily
 * blurred, darkened, vertically-mirrored copy of the album art,
 * replacing the flat buttom.png placeholder. Generated fresh from the
 * per-track RGB565 buffer decoded by cover_decode_to_rgb565(). */
static inline int32_t player_x(int32_t px) { return BOARD_SCALE_PX(px); }
static inline int32_t player_y(int32_t px) { return (int32_t) (((int64_t) px * BOARD_SCREEN_HEIGHT + 400) / 800); }
static inline int32_t player_s(int32_t px) {
    int32_t sx = player_x(px), sy = player_y(px);
    return sx < sy ? sx : sy;
}

static void player_fit_small_screen_icon(lv_obj_t * icon, int32_t reference_px) {
#if defined(BOARD_R3II_2025)
    /* R3 stock transport PNGs still have the reference board's dimensions.
     * Contain scales their paint AND layout box, including later source swaps
     * for pressed/play/pause states. XML layouts retain their own sizing. */
    int32_t size = player_s(reference_px);
    lv_obj_set_size(icon, size, size);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
#else
    (void) icon;
    (void) reference_px;
#endif
}

#define REFLECTION_WIDTH BOARD_SCREEN_WIDTH
/* compute_reflection_bytes() keeps the existing 60x100 bounded work image
 * and delegates its crop/blur/upscale pipeline to frosted_glass.c. */
#define REFLECTION_BLUR_RADIUS 5
#define REFLECTION_BLUR_PASSES 3
/* Kept as an integer ratio (channel * NUM / DEN) rather than a float --
 * matches this codebase's general preference for integer arithmetic on the
 * embedded target, and there's no accuracy need here that would justify a
 * float. */
#define REFLECTION_DARKEN_NUM 1
#define REFLECTION_DARKEN_DEN 2

/* "Match album art": the cover's most prominent vivid hue. Samples a grid,
 * weights each colorful pixel by saturation x value into 15-degree hue
 * buckets, takes the strongest three-bucket window, and lifts its average
 * so it still reads as an accent on dark and light surfaces. False for a
 * gray or near-black cover. Runs on the cover worker. */
static bool cover_accent_from_rgb565(const uint16_t * px, uint32_t * out_rgb) {
    enum { BUCKETS = 24, BUCKET_DEG = 15, STEP = 7 };
    uint64_t weight[BUCKETS] = {0}, hue_off[BUCKETS] = {0}, sat[BUCKETS] = {0}, val[BUCKETS] = {0};
    uint32_t samples = 0;
    for (int y = STEP / 2; y < COVER_ART_HEIGHT; y += STEP) {
        for (int x = STEP / 2; x < COVER_ART_WIDTH; x += STEP) {
            uint16_t c = px[y * COVER_ART_WIDTH + x];
            uint8_t r = (uint8_t) (((c >> 11) << 3) | (c >> 13));
            uint8_t g = (uint8_t) ((((c >> 5) & 0x3F) << 2) | ((c >> 9) & 0x03));
            uint8_t b = (uint8_t) (((c & 0x1F) << 3) | ((c >> 2) & 0x07));
            lv_color_hsv_t hsv = lv_color_rgb_to_hsv(r, g, b);
            samples++;
            if (hsv.s < 25 || hsv.v < 25) continue;
            uint32_t w = (uint32_t) hsv.s * hsv.v;
            int k = (hsv.h % 360) / BUCKET_DEG;
            weight[k] += w;
            hue_off[k] += (uint64_t) w * (hsv.h % 360 - k * BUCKET_DEG);
            sat[k] += (uint64_t) w * hsv.s;
            val[k] += (uint64_t) w * hsv.v;
        }
    }
    int best = 0;
    uint64_t best_w = 0;
    for (int k = 0; k < BUCKETS; k++) {
        uint64_t w = weight[(k + BUCKETS - 1) % BUCKETS] + weight[k] + weight[(k + 1) % BUCKETS];
        if (w > best_w) { best_w = w; best = k; }
    }
    /* At least a tenth of the cover moderately colorful (s x v of 4000). */
    if (best_w == 0 || best_w < (uint64_t) samples * 400) return false;

    uint64_t hue_acc = 0, sat_acc = 0, val_acc = 0;
    for (int d = 0; d < 3; d++) {
        int k = (best - 1 + d + BUCKETS) % BUCKETS;
        hue_acc += weight[k] * (uint64_t) (d * BUCKET_DEG) + hue_off[k];
        sat_acc += sat[k];
        val_acc += val[k];
    }
    int h = (best - 1) * BUCKET_DEG + (int) (hue_acc / best_w);
    h = ((h % 360) + 360) % 360;
    int sv = (int) (sat_acc / best_w);
    int vv = (int) (val_acc / best_w);
    if (sv < 50) sv = 50;
    if (sv > 90) sv = 90;
    if (vv < 80) vv = 80;
    lv_color_t rgb = lv_color_hsv_to_rgb((uint16_t) h, (uint8_t) sv, (uint8_t) vv);
    *out_rgb = ((uint32_t) rgb.red << 16) | ((uint32_t) rgb.green << 8) | rgb.blue;
    return true;
}

uint8_t * compute_reflection_bytes(const uint8_t * cover_bytes, int blur_radius, int blur_passes, int darken_num, int darken_den) {
    if (darken_den <= 0) darken_den = 1;
    if (blur_radius < 0) blur_radius = 0;
    if (blur_passes < 0) blur_passes = 0;
    if (darken_num < 0) darken_num = 0;

    /* Preserve the historical reflection dimensions, center crop, blur,
     * darkening, spatial dither, and unmirrored source pixel orientation. */
    return frosted_glass_blur_rgb565((const uint16_t *) cover_bytes,
                                     COVER_ART_WIDTH, COVER_ART_HEIGHT,
                                     REFLECTION_WIDTH, BOARD_SCREEN_HEIGHT,
                                     blur_radius, blur_passes,
                                     darken_num, darken_den, false);
}

typedef struct {
    bool flat;
    bool has_bg_color;
    uint32_t bg_color;
    int blur_radius;
    int blur_passes;
    int darken_num;
    int darken_den;
} player_frost_params_t;

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

    return params;
}

static bool reflection_matches(const player_frost_params_t * params) {
    return current_reflection_bytes && current_reflection_key.valid &&
           current_reflection_key.cover_bytes == current_cover_bytes &&
           current_reflection_key.cover_generation == current_cover_generation &&
           current_reflection_key.blur_radius == params->blur_radius &&
           current_reflection_key.blur_passes == params->blur_passes &&
           current_reflection_key.darken_num == params->darken_num &&
           current_reflection_key.darken_den == params->darken_den;
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
    current_reflection_bytes = compute_reflection_bytes(current_cover_bytes, params.blur_radius, params.blur_passes, params.darken_num, params.darken_den);
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

/* Cover art decoding (cover_decode_to_rgb565()) and reflection blur
 * computation (compute_reflection_bytes()) are offloaded to a background
 * pthread to avoid blocking the UI thread. Frost parameters (radius,
 * passes, darken ratio, or flat styling) are snapshotted on the UI thread
 * into each request before launching the worker thread, avoiding data races
 * against live player_layout_config modifications. launch_cover_decode_req()
 * spawns the worker thread, and poll_cover_decode() (called periodically
 * from update_timer_cb) applies the decoded RGB565 buffers or flat styling
 * to the image widgets once ready. */
typedef struct {
    int for_index;
    unsigned int generation;
    uint64_t bt_art_token;
    /* Skip the generated-cache fast path and the negative cache: set for the
     * re-read after a cover reload, whose tag-derived key may name a cache
     * file the reload did not delete. */
    bool force_source;
    /* Filled at thread start from current_cover_file (see load_generated_player_cover()). */
    cover_file_id_t shown_cover;
    uint8_t * picture_data; /* owned download buffer for streamed artwork */
    uint32_t picture_size;

    /* Set instead of picture_data/picture_size for a Subsonic stream's cover
     * art (see launch_cover_decode_from_url()) -- there's no local file to
     * have already extracted embedded art from, so the compressed image
     * bytes are fetched here, on the same background thread, before falling
     * into the same decode path local art already uses. Empty string for
     * every other caller. */
    char stream_url[1536];
    bool stream_verify_tls;
    /* Local track identifying its generated album cache. The lookup and
     * file read stay on this worker thread. Artist/
     * album come from the already-parsed track (or the DB), never from a
     * second metadata_read() -- that re-parse OOMs on huge ID3/APIC tags. */
    char local_track_path[PATH_MAX];
    char artist[128];
    char album[128];
    char album_artist[128];

    /* Resolved frosted-glass parameters snapshotted from player_layout_config
     * on the UI thread before launching the worker thread. */
    bool flat;
    bool has_bg_color;
    uint32_t bg_color;
    int blur_radius;
    int blur_passes;
    int darken_num;
    int darken_den;
} cover_decode_request_t;

static pthread_t cover_decode_thread;
static bool cover_decode_active = false;
static atomic_bool cover_decode_done_flag = false;

/* Result fields, written by cover_decode_thread_func() on the background
 * thread and consumed (freed or applied) by poll_cover_decode() on the main
 * thread once cover_decode_done_flag is seen true -- never touched by both
 * at once, same "background thread writes then sets the flag last, main
 * thread only reads after seeing the flag" contract as every other _done_
 * flag in this file. */
static int cover_decode_result_for_index;
static unsigned int cover_decode_result_generation;
static audio_current_format_info_t cover_decode_result_format;
/* UI-thread cache, keyed by path rather than a mutable playlist index. */
static audio_current_format_info_t player_cached_format;
static bool cover_decode_result_ok;
static uint16_t * cover_decode_result_pixels;    /* COVER_ART_WIDTH x COVER_ART_HEIGHT RGB565, owned */
static uint8_t * cover_decode_result_reflection; /* REFLECTION_WIDTH x BOARD_SCREEN_HEIGHT RGB565, owned */
static bool cover_decode_result_flat;
static bool cover_decode_result_has_bg_color;
static uint32_t cover_decode_result_bg_color;
/* Snapshotted at request-launch time, same as the 3 fields above -- carried
 * through so poll_cover_decode() can tell whether player_layout_config
 * changed while this decode was in flight (see its own comment). */
static int cover_decode_result_blur_radius;
static int cover_decode_result_blur_passes;
static int cover_decode_result_darken_num;
static int cover_decode_result_darken_den;
static bool cover_decode_result_accent_valid;
static uint32_t cover_decode_result_accent;
/* The file the result came from, and whether it is the one already shown
 * (then ok is true with no pixels and no reflection). */
static cover_file_id_t cover_decode_result_file;
static bool cover_decode_result_same;

/* Helper called only from the UI thread when constructing a fresh decode request.
 * Resolves current layout frost parameters once and snapshots them into the request. */
static void cover_decode_request_snapshot_frost(cover_decode_request_t * req) {
    player_frost_params_t p = resolve_player_frost_params();
    req->flat = p.flat;
    req->has_bg_color = p.has_bg_color;
    req->bg_color = p.bg_color;
    req->blur_radius = p.blur_radius;
    req->blur_passes = p.blur_passes;
    req->darken_num = p.darken_num;
    req->darken_den = p.darken_den;
}


/* Holds at most one superseded request -- see launch_cover_decode_req()'s own
 * comment on why a track change that arrives while a decode is already in
 * flight can't just be dropped (that would leave the cover permanently
 * stuck showing an earlier track's art). */
static bool cover_decode_pending_valid = false;
static cover_decode_request_t cover_decode_pending;
static atomic_uint cover_decode_generation;
static atomic_bool cover_reload_active;
/* True while a cover decode worker body runs (it may write generated cache
 * files); a cover reload waits for this off the UI thread before deleting. */
static atomic_bool cover_decode_running;
static bool last_local_cover_request_valid;
static cover_decode_request_t last_local_cover_request;

/* Bound reads even if a generated cache file was replaced or corrupted.
 * The expected 480x480 BMP occupies 691254 bytes. */
#define EXTERNAL_COVER_MAX_BYTES (4U * 1024U * 1024U)

static void albumart_info_from_path_tags(const char * track_path, const char * artist, const char * album,
                                          const char * album_artist, albumart_info_t * info) {
    memset(info, 0, sizeof(*info));
    snprintf(info->path, sizeof(info->path), "%s", track_path ? track_path : "");
    snprintf(info->artist, sizeof(info->artist), "%s", artist ? artist : "");
    snprintf(info->album, sizeof(info->album), "%s", album ? album : "");
    snprintf(info->albumartist, sizeof(info->albumartist), "%s", album_artist ? album_artist : "");
}

void albumart_info_from_song_row(const song_row_t * song, albumart_info_t * info) {
    albumart_info_from_path_tags(song->path, song->tags.artist, song->tags.album, song->tags.album_artist, info);
}

/* Session-scoped negative cache for load_cached_player_cover()'s slow path:
 * a track with no art anywhere (no sidecar, no embedded picture) or with
 * tags too empty for albumart_store_rgb565()'s artist+album requirement
 * can never produce a persistable cache entry, so without this,
 * gui_library_generate_player_cover()'s full fork+isolated-extract fallback
 * would silently re-run on every single play of that track -- exactly the
 * per-play cost this feature exists to eliminate. Small, bounded, path-hash
 * keyed (FNV-1a, same convention already used for cache filenames
 * elsewhere in this codebase); cleared by an app restart and after every
 * library publication, so a database update or metadata refresh retries
 * art added mid-session without per-play mtime tracking. */
#define PLAYER_COVER_NEGATIVE_CACHE_SIZE 32
static uint64_t player_cover_negative_cache[PLAYER_COVER_NEGATIVE_CACHE_SIZE];
static int player_cover_negative_cache_next = 0;
/* Bumped by any library publication; the decode worker, the table's only
 * user, empties it on seeing a new value so refreshed art is retried. */
static atomic_uint player_cover_negative_epoch;
static unsigned player_cover_negative_seen_epoch;

void gui_player_invalidate_cover_negatives(void) {
    atomic_fetch_add_explicit(&player_cover_negative_epoch, 1u, memory_order_relaxed);
}

static uint64_t player_cover_path_hash(const char * path) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char * p = (const unsigned char *) path; *p; p++)
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    return hash;
}

static bool player_cover_decode_cancelled(void * user_data) {
    unsigned int generation = (unsigned int) (uintptr_t) user_data;
    return atomic_load_explicit(&cover_decode_generation, memory_order_relaxed) != generation;
}

static bool player_cover_negative_cache_hit(const char * path) {
    unsigned epoch = atomic_load_explicit(&player_cover_negative_epoch, memory_order_relaxed);
    if (epoch != player_cover_negative_seen_epoch) {
        memset(player_cover_negative_cache, 0, sizeof(player_cover_negative_cache));
        player_cover_negative_cache_next = 0;
        player_cover_negative_seen_epoch = epoch;
    }
    uint64_t h = player_cover_path_hash(path);
    for (int i = 0; i < PLAYER_COVER_NEGATIVE_CACHE_SIZE; i++)
        if (player_cover_negative_cache[i] == h) return true;
    return false;
}

static void player_cover_negative_cache_add(const char * path) {
    player_cover_negative_cache[player_cover_negative_cache_next] = player_cover_path_hash(path);
    player_cover_negative_cache_next = (player_cover_negative_cache_next + 1) % PLAYER_COVER_NEGATIVE_CACHE_SIZE;
}

/* Local playback uses the generated 480px BMP. Never decode the original
 * embedded/sidecar image directly here: cache generation owns that
 * expensive work -- normally the background warmer, or, for a track it
 * hasn't reached yet, the one-time-per-session fallback below. */
/* The generated player-size cache for these tags. require_fresh compares its
 * stored source mtime with this track's; a reload's fallback skips that, as
 * the file was just rebuilt, possibly from a sibling track with another mtime. */
/* shown is the file already on screen: when it is this same file, nothing
 * is decoded and *out_same is set instead of *out_pixels. The file is
 * identified before it is read, so a rebuild racing the read can only cause
 * an extra decode later, never a stale cover. */
/* Generated file identity, independent of the queue slot and layout.
 * Sharing its JPEG keeps same-album changes on the existing pixel fast path. */
static uint64_t bt_cover_file_key(const cover_file_id_t * id) {
    if (!id->valid) return 0;
    uint64_t values[] = { (uint64_t) id->dev, (uint64_t) id->ino,
                          (uint64_t) id->mtime, (uint64_t) id->size };
    uint64_t key = 14695981039346656037ULL;
    for (size_t i = 0; i < sizeof(values); i++) {
        key ^= ((const unsigned char *) values)[i];
        key *= 1099511628211ULL;
    }
    return key ? key : 1;
}

static size_t player_raster_cache_memory_floor(void) {
    return artwork_reserve_bytes_for_priority(ARTWORK_PRIO_WARMER) + 1024u * 1024u;
}

static bool load_generated_player_cover(const albumart_info_t * info, const char * track_path,
                                        unsigned int generation, uint64_t bt_art_token, bool require_fresh,
                                        const cover_file_id_t * shown, cover_file_id_t * out_id,
                                        bool * out_same, uint16_t ** out_pixels) {
    char found[PATH_MAX];
    bool have = require_fresh
        ? albumart_generated_cache_fresh(info, ALBUMART_PLAYER_CACHE_SIZE, ALBUMART_PLAYER_CACHE_SIZE,
                                         found, sizeof(found))
        : albumart_generated_cache_find(info, ALBUMART_PLAYER_CACHE_SIZE, ALBUMART_PLAYER_CACHE_SIZE,
                                        found, sizeof(found));
    if (!have) return false;
    struct stat st = {0};
    cover_file_id_t id = { .valid = false };
    if (stat(found, &st) == 0) {
        id = (cover_file_id_t) { true, st.st_dev, st.st_ino, st.st_mtime, st.st_size };
    }
    if (shown && cover_file_id_same(&id, shown) &&
        bt_cover_art_reuse(bt_art_token, bt_cover_file_key(&id))) {
        *out_id = id;
        *out_same = true;
        DB_LOG("ART_PLAYER", "cache_same path=%s file=%s", track_path, found);
        return true;
    }
    const char *cache_flag = getenv("COMPAS_ARTWORK_REUSE");
    bool reuse = cache_flag && strcmp(cache_flag, "1") == 0 && id.valid;
    cover_raster_key_t key = { .file = st, .width = COVER_ART_WIDTH, .height = COVER_ART_HEIGHT };
    if (reuse && system_get_mem_available_bytes() < player_raster_cache_memory_floor()) {
        cover_raster_cache_clear();
        reuse = false;
    }
    if (reuse) {
        *out_pixels = cover_raster_cache_get(&key);
        if (*out_pixels) {
            *out_id = id;
            DB_LOG("ART_PLAYER", "raster_reuse path=%s bytes=%zu", track_path, cover_raster_cache_bytes());
            return true;
        }
    }
    uint8_t * data = NULL;
    uint32_t size = 0;
    if (albumart_load_file_ex(found, &data, &size, EXTERNAL_COVER_MAX_BYTES,
                              ARTWORK_PRIO_PLAYER) != ALBUMART_LOAD_OK)
        return false;
    cover_decode_result_t result = cover_decode_to_rgb565_ex(data, size,
        COVER_ART_WIDTH, COVER_ART_HEIGHT, ARTWORK_PRIO_PLAYER,
        player_cover_decode_cancelled, (void *) (uintptr_t) generation, out_pixels);
    free(data);
    if (result != COVER_DECODE_OK) return false;
    /* A replaced cache file may race the read. Only retain a raster if its
     * complete identity still matches after decoding. */
    struct stat after;
    if (reuse && stat(found, &after) == 0 &&
        st.st_dev == after.st_dev && st.st_ino == after.st_ino && st.st_size == after.st_size &&
        st.st_mtim.tv_sec == after.st_mtim.tv_sec && st.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
        st.st_ctim.tv_sec == after.st_ctim.tv_sec && st.st_ctim.tv_nsec == after.st_ctim.tv_nsec &&
        system_get_mem_available_bytes() >= player_raster_cache_memory_floor())
        cover_raster_cache_put(&key, *out_pixels);
    *out_id = id;
    DB_LOG("ART_PLAYER", "cache_hit path=%s file=%s", track_path, found);
    return true;
}

static bool load_cached_player_cover(const char * track_path, const char * artist, const char * album,
                                      const char * album_artist, unsigned int generation,
                                      uint64_t bt_art_token, bool force_source, const cover_file_id_t * shown,
                                      cover_file_id_t * out_id, bool * out_same, uint16_t ** out_pixels) {
    if (player_cover_decode_cancelled((void *) (uintptr_t) generation)) return false;
    albumart_info_t info;
    albumart_info_from_path_tags(track_path, artist, album, album_artist, &info);
    if (!force_source && load_generated_player_cover(&info, track_path, generation, bt_art_token, true,
                                                     shown, out_id, out_same, out_pixels))
        return true;

    if (player_cover_decode_cancelled((void *) (uintptr_t) generation)) return false;
    if (!force_source && player_cover_negative_cache_hit(track_path)) {
        DB_LOG("ART_PLAYER", "no_art_known path=%s", track_path);
        return false;
    }

    /* Cache miss -- this track's album hasn't been warmed yet (freshly
     * added, or the background scan just hasn't reached it). Generate and
     * persist the cache now so every later play of this track hits the
     * fast path above. */
    bool no_art_confirmed = false;
    if (gui_library_generate_player_cover(track_path, artist, album, album_artist,
                                          player_cover_decode_cancelled,
                                          (void *) (uintptr_t) generation,
                                          out_pixels, &no_art_confirmed)) {
        DB_LOG("ART_PLAYER", "cache_miss_generated path=%s", track_path);
        return true;
    }
    if (player_cover_decode_cancelled((void *) (uintptr_t) generation)) return false;
    /* A forced re-read prefers the track's own art, but a track without any
     * may still have album art the warmer rebuilt from a sibling track. */
    if (force_source && load_generated_player_cover(&info, track_path, generation, bt_art_token, false,
                                                    NULL, out_id, out_same, out_pixels))
        return true;
    /* Only remember a confirmed "no art anywhere" result -- a transient one
     * (coordinator busy, isolated-helper fork/timeout) must retry on the
     * next play, not get silently and permanently suppressed. */
    DB_LOG("ART_PLAYER", "cache_miss_failed path=%s confirmed=%d", track_path, no_art_confirmed);
    if (no_art_confirmed) player_cover_negative_cache_add(track_path);
    return false;
}

/* Read native FLAC STREAMINFO without opening a second decoder. This is
 * bounded I/O and avoids shared codec initialization or whole-file scans. */
static bool player_read_flac_format(const char * path, audio_current_format_info_t * out) {
    unsigned char header[42];
    FILE * f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(header, 1, sizeof(header), f);
    fclose(f);
    if (n != sizeof(header) || memcmp(header, "fLaC", 4) != 0 ||
        (header[4] & 0x7f) != 0 || header[5] != 0 || header[6] != 0 || header[7] != 34)
        return false;
    /* STREAMINFO bytes 10..13 pack rate (20 bits), channels (3), depth (5). */
    unsigned int rate = ((unsigned int) header[18] << 12) |
                        ((unsigned int) header[19] << 4) | (header[20] >> 4);
    unsigned int depth = (((header[20] & 1u) << 4) | (header[21] >> 4)) + 1;
    if (!rate || depth < 4) return false;
    memset(out, 0, sizeof(*out));
    out->valid = true;
    snprintf(out->path, sizeof(out->path), "%s", path);
    out->source_sample_rate = rate;
    out->source_bit_depth = depth;
    return true;
}

/* Player-size JPEG/PNG cover. Album thumbnails use a 4 MiB stack. */
/* Weak: tests that #include this file link without main.c, which
 * defines the real hook. */
extern void install_thread_crash_altstack(void) __attribute__((weak));

#define COVER_DECODE_THREAD_STACK_SIZE (256 * 1024)

static void * cover_decode_thread_func(void * arg) {
#ifdef __linux__
    (void) prctl(PR_SET_NAME, "cover");
#endif
    if (install_thread_crash_altstack) install_thread_crash_altstack(); /* see its own comment (main.c) */
    cover_decode_request_t * req = (cover_decode_request_t *) arg;
    uint16_t * pixels = NULL;
    bool ok = false;
    bool same = false;
    cover_file_id_t file_id = { .valid = false };

    memset(&cover_decode_result_format, 0, sizeof(cover_decode_result_format));
    if (req->local_track_path[0]) {
        if (!audio_get_current_format_info(&cover_decode_result_format) ||
            strcmp(cover_decode_result_format.path, req->local_track_path) != 0) {
            memset(&cover_decode_result_format, 0, sizeof(cover_decode_result_format));
            /* Restored paused tracks have no decoder yet. Probe off the UI
             * thread; never start playback merely to populate the badge. */
            player_read_flac_format(req->local_track_path, &cover_decode_result_format);
        }
    }

    if (req->stream_url[0] != '\0') {
        int status = 0;
        uint8_t * body = NULL;
        size_t body_size = 0;
        if (http_get_to_buffer(req->stream_url, req->stream_verify_tls, &status, &body, &body_size) && status == 200) {
            req->picture_data = body;
            req->picture_size = (uint32_t) body_size;
        } else {
            free(body);
            /* Left NULL/0 -- decode below no-ops, same as "no embedded art"
             * for a local file. No error surfaced beyond that; a failed
             * cover-art fetch isn't worth blocking or retrying playback
             * over. */
        }
        ok = req->picture_data && req->picture_size > 0 &&
             cover_decode_to_rgb565(req->picture_data, req->picture_size, COVER_ART_WIDTH, COVER_ART_HEIGHT, &pixels);
        free(req->picture_data);
        req->picture_data = NULL;
    } else if (req->local_track_path[0]) {
        ok = load_cached_player_cover(req->local_track_path, req->artist, req->album,
                                       req->album_artist, req->generation, req->bt_art_token, req->force_source,
                                       &req->shown_cover, &file_id, &same, &pixels);
    }

    bool cancelled = player_cover_decode_cancelled((void *) (uintptr_t) req->generation);
    if (cancelled) {
        free(pixels);
        pixels = NULL;
        ok = false;
        same = false;
    }

    if (ok && pixels)
        bt_cover_art_publish_rgb565(req->bt_art_token, pixels, COVER_ART_WIDTH, COVER_ART_HEIGHT,
                                      bt_cover_file_key(&file_id));
    else if (!ok && !cancelled)
        bt_cover_art_clear(req->bt_art_token);

    uint32_t accent = 0;
    bool accent_valid = ok && pixels && cover_accent_from_rgb565(pixels, &accent);

    uint8_t * reflection = NULL;
    if (ok && pixels && !req->flat) {
        reflection = compute_reflection_bytes((const uint8_t *) pixels,
                                              req->blur_radius,
                                              req->blur_passes,
                                              req->darken_num,
                                              req->darken_den);
    }

    cover_decode_result_for_index = req->for_index;
    cover_decode_result_generation = req->generation;
    cover_decode_result_ok = ok;
    cover_decode_result_pixels = pixels;
    cover_decode_result_reflection = reflection;
    cover_decode_result_flat = req->flat;
    cover_decode_result_has_bg_color = req->has_bg_color;
    cover_decode_result_bg_color = req->bg_color;
    cover_decode_result_blur_radius = req->blur_radius;
    cover_decode_result_blur_passes = req->blur_passes;
    cover_decode_result_darken_num = req->darken_num;
    cover_decode_result_darken_den = req->darken_den;
    cover_decode_result_accent_valid = accent_valid;
    cover_decode_result_accent = accent;
    cover_decode_result_file = file_id;
    cover_decode_result_same = same;
    free(req);
    atomic_store_explicit(&cover_decode_running, false, memory_order_release);
    atomic_store_explicit(&cover_decode_done_flag, true, memory_order_release);
    ui_wake_notify(); /* written last -- poll_cover_decode() only checks this flag */
    return NULL;
}

/* Takes ownership of picture_data (a malloc()'d buffer from metadata_read(),
 * or NULL). If a decode is already running, this doesn't launch a second
 * one (cover_decode_to_rgb565()/compute_reflection_bytes() are reentrant --
 * no shared state -- but there's no value in two decodes racing when only
 * the newest one's result will ever get applied) -- it instead replaces
 * cover_decode_pending (freeing whatever was queued there before), and
 * poll_cover_decode() launches that pending request the moment the active
 * one finishes. A rapid sequence of track changes only ever actually
 * decodes the first and the final one, which is exactly the set of results
 * that matters. */
static void launch_cover_decode_req(cover_decode_request_t r) {
    if (r.generation == 0) {
        r.bt_art_token = bt_cover_art_begin_track(playlist_path_at(r.for_index));
        r.generation = atomic_fetch_add_explicit(&cover_decode_generation, 1u,
                                                  memory_order_relaxed) + 1u;
    }
    if (r.local_track_path[0]) {
        last_local_cover_request = r;
        last_local_cover_request_valid = true;
    } else if (r.stream_url[0]) {
        last_local_cover_request_valid = false;
    }

    if (atomic_load_explicit(&cover_reload_active, memory_order_relaxed)) {
        free(cover_decode_pending.picture_data);
        cover_decode_pending = r;
        cover_decode_pending_valid = true;
        return;
    }
    if (cover_decode_active) {
        free(cover_decode_pending.picture_data);
        cover_decode_pending = r;
        cover_decode_pending_valid = true;
        return;
    }

    cover_decode_request_t * req = malloc(sizeof(*req));
    if (!req) {
        /* Free owned picture_data and skip decoding if request allocation
         * fails under memory pressure. */
        free(r.picture_data);
        return;
    }
    *req = r;
    /* Snapshotted here, not when the request was made: a pending request
     * may start after other covers were applied. */
    req->shown_cover = current_cover_bytes ? current_cover_file : (cover_file_id_t) { .valid = false };
    atomic_store_explicit(&cover_decode_done_flag, false, memory_order_relaxed);
    cover_decode_active = true;
    /* Marked before the thread exists, so a reload worker can never see
     * "idle" while a created-but-unscheduled decode is about to run. */
    atomic_store_explicit(&cover_decode_running, true, memory_order_release);
    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    bool attr_initialized = pthread_attr_init(&attr) == 0;
    if (attr_initialized &&
        pthread_attr_setstacksize(&attr, COVER_DECODE_THREAD_STACK_SIZE) == 0)
        attr_ptr = &attr;
    int create_rc = pthread_create(&cover_decode_thread, attr_ptr, cover_decode_thread_func, req);
    if (attr_initialized) pthread_attr_destroy(&attr);
    if (create_rc != 0) {
        free(req->picture_data);
        free(req);
        cover_decode_active = false;
        atomic_store_explicit(&cover_decode_running, false, memory_order_release);
    }
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
    const void * thumbnail_target = player_timeline_cover_frame && source == player_timeline_cover_frame ?
                                    player_timeline_cover_source : source;
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
        backlight_screen_is_on() && !cover_decode_active &&
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
    if (available < player_raster_cache_memory_floor())
        cover_raster_cache_clear();
    lv_draw_sw_image_cache_service(available, idle);
    lv_draw_sw_image_cache_get_intermediate_stats(&stats);
    if (timer && (!idle || !stats.pending)) lv_timer_pause(timer);
    if (!timer && idle && stats.pending) {
        if (!player_artwork_cache_timer)
            player_artwork_cache_timer = lv_timer_create(player_artwork_cache_service, 20, NULL);
        else lv_timer_resume(player_artwork_cache_timer);
    }
}

/* Called every tick from update_timer_cb. Applies the finished decode to
 * cover_img/player_overlay_panel -- unless playlist_index has already moved
 * on to a different track by the time this lands (another launch_cover_
 * decode() call superseded it via cover_decode_pending, in which case that
 * one is now either running or about to be), in which case the result is
 * just discarded rather than briefly flashing a stale track's art. */
void poll_cover_decode(void) {
    /* Pressure checks also run during transitions. The ordinary maintenance
     * poll starts optional idle work; a short-lived timer continues slices. */
    player_artwork_cache_service(NULL);

    /* Slides display owned snapshots. Applying artwork now cannot update
     * those frames, but image/layout work can delay their presentation.
     * Leave the completed result owned by the worker until the next poll. */
    if (gui_navigation_transition_in_progress() || gui_player_lyrics_animation_in_progress()) return;
    if (!cover_decode_active || !atomic_load_explicit(&cover_decode_done_flag, memory_order_acquire)) return;
    cover_decode_active = false;
    pthread_join(cover_decode_thread, NULL);

    bool result_generation_current = cover_decode_result_generation ==
        atomic_load_explicit(&cover_decode_generation, memory_order_relaxed);

    if (result_generation_current && cover_decode_result_format.valid &&
        strcmp(cover_decode_result_format.path, playlist_path_at(playlist_index)) == 0) {
        player_cached_format = cover_decode_result_format;
        refresh_format_badge();
    }

    if (!result_generation_current || cover_decode_result_for_index != playlist_index) {
        free(cover_decode_result_pixels);
        free(cover_decode_result_reflection);
    } else if (cover_decode_result_ok && cover_decode_result_same && current_cover_bytes &&
               cover_file_id_same(&cover_decode_result_file, &current_cover_file)) {
        /* Same cached file as the cover on screen (next track of the same
         * album): the pixels, reflection, lock screen, drawer and accent
         * already match. Only the owning queue slot moves on. */
        free(cover_decode_result_pixels);
        free(cover_decode_result_reflection);
        current_cover_for_index = cover_decode_result_for_index;
        gui_player_refresh_frosted_background();
        gui_lyrics_on_cover_changed(playlist_index);
    } else if (cover_decode_result_same) {
        /* The shown cover changed after this worker started, so the result
         * has no pixels to apply; decode this track's cover properly. */
        if (last_local_cover_request_valid && !cover_decode_pending_valid) {
            cover_decode_request_t retry = last_local_cover_request;
            retry.generation = 0;
            retry.picture_data = NULL;
            cover_decode_pending = retry;
            cover_decode_pending_valid = true;
        }
    } else if (!cover_decode_result_ok) {
        free(cover_decode_result_reflection);
        player_timeline_cover_cache_invalidate();
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
        }
        free(old_cover_bytes);
        gui_lock_screen_refresh(true);
        gui_theme_set_cover_accent(false, 0);
        player_transition_mark_dirty(); /* cover_img just changed to the placeholder -- see the cache's own doc comment */
    } else {
        player_timeline_cover_cache_invalidate();
        lv_image_cache_drop(&current_cover_dsc);
        free(current_cover_bytes);
        current_cover_generation++;
        current_cover_bytes = (uint8_t *) cover_decode_result_pixels;
        current_cover_for_index = cover_decode_result_for_index;
        current_cover_file = cover_decode_result_file; /* invalid unless it came from a generated cache file */

        /* lv_image_header_t.magic must be set to LV_IMAGE_HEADER_MAGIC;
         * otherwise LVGL's bin decoder treats a 0 magic value as a legacy
         * header format and overwrites header->cf with header->magic (0),
         * corrupting the pixel color format interpretation. */
        memset(&current_cover_dsc, 0, sizeof(current_cover_dsc));
        current_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        current_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        current_cover_dsc.header.flags |= LV_IMAGE_FLAGS_USER3;
        current_cover_dsc.header.w = COVER_ART_WIDTH;
        current_cover_dsc.header.h = COVER_ART_HEIGHT;
        current_cover_dsc.header.stride = COVER_ART_WIDTH * 2;
        current_cover_dsc.data = current_cover_bytes;
        current_cover_dsc.data_size = (uint32_t) COVER_ART_WIDTH * COVER_ART_HEIGHT * 2;
        lv_image_set_src(cover_img, &current_cover_dsc);
        fit_cover_img_to_card();
        player_lyrics_backdrop_invalidate();
        gui_shell_refresh_quick_drawer_cover();
        gui_lock_screen_refresh(true);

        /* The reflection this decode computed was built from frost params
         * snapshotted when the decode was LAUNCHED. If a plugin changed
         * player_layout_config while this decode was in flight (a rare
         * race, e.g. plugin.set_player_layout()/refresh_theme() firing
         * mid-decode), that stale snapshot must not silently overwrite the
         * live config the plugin just applied -- compare against the
         * current live params and only trust the worker's precomputed
         * result when they still match. */
        player_frost_params_t live_params = resolve_player_frost_params();
        bool snapshot_is_stale = (live_params.flat != cover_decode_result_flat) ||
            (live_params.flat
                ? (live_params.has_bg_color != cover_decode_result_has_bg_color ||
                   (live_params.has_bg_color && live_params.bg_color != cover_decode_result_bg_color))
                : (live_params.blur_radius != cover_decode_result_blur_radius ||
                   live_params.blur_passes != cover_decode_result_blur_passes ||
                   live_params.darken_num != cover_decode_result_darken_num ||
                   live_params.darken_den != cover_decode_result_darken_den));

        if (snapshot_is_stale) {
            /* Discard the worker's now-outdated result and recompute
             * synchronously against the live config and the cover art
             * just installed above -- a one-off UI-thread recompute here
             * is the same acceptable tradeoff gui_player_refresh_frosted_
             * background()'s own live-refresh callers already make. */
            free(cover_decode_result_reflection);
            gui_player_refresh_frosted_background();
        } else if (cover_decode_result_flat) {
            free(cover_decode_result_reflection);
            apply_player_flat_background(cover_decode_result_has_bg_color, cover_decode_result_bg_color);
        } else {
            drop_current_reflection();
            current_reflection_bytes = cover_decode_result_reflection;
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
        gui_lyrics_on_cover_changed(playlist_index);
        gui_theme_set_cover_accent(cover_decode_result_accent_valid, cover_decode_result_accent);
        player_transition_mark_dirty(); /* cover_img/player_overlay_panel's reflection just changed -- see the cache's own doc comment */
    }

    cover_decode_result_pixels = NULL;
    cover_decode_result_reflection = NULL;

    if (cover_decode_pending_valid) {
        cover_decode_pending_valid = false;
        launch_cover_decode_req(cover_decode_pending); /* the whole request, not a rebuilt one -- must carry stream_url too, see that field's own comment */
        /* Clear picture_data to avoid double-freeing if another track
         * change occurs while the handed-off decode thread is running. */
        cover_decode_pending.picture_data = NULL;
        cover_decode_pending.picture_size = 0;
    }
}

void gui_player_begin_cover_reload(void) {
    /* New decodes are held (launch_cover_decode_req() parks them in
     * cover_decode_pending) until gui_player_finish_cover_reload(). One
     * already running, local or streamed, finishes normally: the reload
     * worker waits for gui_player_cover_decode_idle() before deleting any
     * file, so it cannot write an old cover back afterwards, and the UI
     * thread never blocks on an isolated extraction here. */
    atomic_store_explicit(&cover_reload_active, true, memory_order_relaxed);
}

bool gui_player_cover_decode_idle(void) {
    return !atomic_load_explicit(&cover_decode_running, memory_order_acquire);
}

void gui_player_finish_cover_reload(void) {
    atomic_store_explicit(&cover_reload_active, false, memory_order_relaxed);
    /* Re-read the playing track's cover whatever album was reloaded: the
     * player's tag-derived key can differ from the library's, and one cached
     * read is cheap. The result replaces (and frees) the old cover; a pending
     * request for a newer track wins instead. */
    cover_decode_request_t req;
    if (cover_decode_pending_valid) {
        req = cover_decode_pending;
    } else if (last_local_cover_request_valid && last_local_cover_request.for_index == playlist_index &&
               strcmp(last_local_cover_request.local_track_path, playlist_path_at(playlist_index)) == 0) {
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
    memset(&cover_decode_pending, 0, sizeof(cover_decode_pending));
    cover_decode_pending_valid = false;
    req.generation = 0;
    launch_cover_decode_req(req);
}

void gui_player_cancel_cover_reload(void) {
    atomic_store_explicit(&cover_reload_active, false, memory_order_relaxed);
    atomic_fetch_add_explicit(&cover_decode_generation, 1u, memory_order_relaxed);
    if (cover_decode_pending_valid) {
        free(cover_decode_pending.picture_data);
        memset(&cover_decode_pending, 0, sizeof(cover_decode_pending));
        cover_decode_pending_valid = false;
    }
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
        favorite_writer_submit(playlist_path_at(playlist_index), favorite_is_set);
    }
}

/* Remote Control changed some favorite: reload the now-playing heart from
 * the writer, which also sees this player's own unsaved taps. */
void gui_player_refresh_favorite(void) {
    if (playlist_index < 0 || playlist_index >= playlist_count) return;
    const char * current = playlist_path_at(playlist_index);
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
    if (playlist_index < 0 || playlist_index >= playlist_count) return;

    const char * path = playlist_path_at(playlist_index);
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
 * single 4-state cycle either way, so both surfaces stay in sync and
 * neither reimplements the mode-advance/persist/re-arm logic. */
void cycle_play_mode(void) {
    play_mode_t mode = (play_mode_t) current_settings.play_mode;
    mode = (play_mode_t) ((mode + 1) % 4);
    gui_player_set_play_mode(mode);
}

void gui_player_set_play_mode(int mode_value) {
    play_mode_t mode = (play_mode_t) mode_value;
    if (mode < PLAY_MODE_SEQUENTIAL || mode > PLAY_MODE_SHUFFLE) return;
    current_settings.play_mode = (int) mode;
    settings_save_async(&current_settings);

    if (order_icon) lv_image_set_src(order_icon, asset_path(play_mode_icon_asset(mode)));
    gui_shell_update_quick_drawer_play_mode((int) mode);

    /* What comes after the current track changes with the mode (e.g.
     * entering Shuffle picks a random next instead of index+1) -- re-arm the
     * gapless/crossfade target immediately rather than waiting for whatever
     * was armed under the old mode to turn out wrong. */
    if (playlist_index >= 0) arm_next_track_for_audio(playlist_index);
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
    if (!path || !is_http_url(path) || index < 0 || index >= playlist_count ||
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
    /* Resolved once -- this is playlist_index's first real touch on every
     * track-start (play_track_at_from()/on_track_auto_advanced() both call
     * this immediately after setting playlist_index), so it's also where a
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
    }
    /* Stream and remote catalog text reaches the rows too: same single-line,
     * non-empty rules as file tags. */
    if (is_remote_track || is_subsonic_stream) metadata_normalize_text_tags(out_meta);

    return !is_remote_track && !is_subsonic_stream && !is_http_url(path);
}

static void apply_loaded_track_metadata(int index, track_metadata_t * out_meta, bool favorite) {
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
    if (is_remote_track && remote_meta.artwork_url[0]) {
        launch_cover_decode_from_url(index, remote_meta.artwork_url, remote_meta.verify_tls);
    } else if (is_subsonic_stream && subsonic_meta->cover_url[0]) {
        launch_cover_decode_from_url(index, subsonic_meta->cover_url, subsonic_meta->verify_tls);
    } else if (is_remote_track) {
        /* No artwork_url and nothing local to fall back to -- unlike the
         * plain "no embedded picture" case below, launch_cover_decode_
         * from_track() must not be tried: path is the synthetic remote://
         * key, not a real file. A no-art request also supersedes pending
         * older decodes and clears the visible cover when it completes. */
        cover_decode_request_t no_art = { .for_index = index };
        cover_decode_request_snapshot_frost(&no_art);
        launch_cover_decode_req(no_art);
    } else {
        free(out_meta->picture_data);
        out_meta->picture_data = NULL;
        launch_cover_decode_from_track(index, path, out_meta->artist, out_meta->album, out_meta->album_artist);
    }

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


    if (metadata_favorite_revision == favorite_ui_revision) apply_loaded_favorite(favorite);
    player_transition_mark_dirty();
}

/* Establish the new UI identity immediately; file/DB enrichment arrives
 * through an owned scalar snapshot and never resets live playback progress. */
static void request_track_metadata(int index, bool notify, bool probe, bool audio_tags) {
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
    snprintf(metadata_ui_path, sizeof(metadata_ui_path), "%s", path);
    metadata_ui_notify = notify;
    metadata_notify_ready = false;
    plugin_now_playing_loaded = false;
    metadata_ui_audio_generation = audio_get_playback_generation();
    metadata_favorite_revision = ++favorite_ui_revision;
    track_metadata_submit(TRACK_METADATA_CURRENT, &request);
    /* Cancel a cover result even if a new queue reuses the same slot. */
    atomic_fetch_add_explicit(&cover_decode_generation, 1u, memory_order_relaxed);
    free(cover_decode_pending.picture_data);
    memset(&cover_decode_pending, 0, sizeof(cover_decode_pending));
    cover_decode_pending_valid = false;
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
        lv_label_set_text_fmt(song_count_label, "%d/%d", index + 1, playlist_count);
        lv_obj_remove_flag(song_count_label, LV_OBJ_FLAG_HIDDEN);
    }
    player_transition_mark_dirty(); /* title/artist/format badge/progress reset above all just changed player_screen's own content -- see the cache's own doc comment */
    player_timeline_play(player_timeline_track_change, NULL, NULL, NULL);
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

static void delete_song_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_delete_song_popup();
    if (playlist_index < 0 || playlist_index >= playlist_count) return;

    char * to_delete = strdup(playlist_path_at(playlist_index));
    int del_index = playlist_index;

    /* What to play next is decided before removing the entry: the track
     * shifting into del_index, or the new last track if the deleted one
     * was last. */
    free(playlist[del_index]);
    for (int i = del_index; i < playlist_count - 1; i++) playlist[i] = playlist[i + 1];
    /* Kept in lockstep -- see playlist_lazy_sort_order's own comment. */
    if (playlist_lazy_sort_order) {
        for (int i = del_index; i < playlist_count - 1; i++) playlist_lazy_sort_order[i] = playlist_lazy_sort_order[i + 1];
    }
    playlist_count--;

    if (playlist_count == 0) {
        audio_stop();
        plugin_manager_notify_stopped();
        free(playlist);
        playlist = NULL;
        playlist_index = -1;
        free(playlist_lazy_sort_order);
        playlist_lazy_sort_order = NULL;
        clear_player_source();
        set_play_button_state(false);
        player_set_track_display_text(NULL, NULL, NULL);
        now_playing_genre[0] = '\0';
        now_playing_track_number = 0;
        nav_pop(); /* nothing left to show on the player screen */
    } else {
        int new_index = (del_index < playlist_count) ? del_index : playlist_count - 1;
        play_track_at(new_index);
    }

    unlink(to_delete); /* only after playback has moved off it */
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
                                             lv_color_make(255, 120, 120), delete_song_confirm_cb, NULL, TR("Cancel"),
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
    if (playlist_index < 0) return;
    open_add_to_playlist_for(playlist_path_at(playlist_index));
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

static void more_menu_eq_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    nav_push(gui_settings_get_eq_screen());
}

static void more_menu_delete_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    if (playlist_index < 0 || playlist_index >= playlist_count) return;

    lv_label_set_text_fmt(delete_song_popup_title, TR("Delete %s?\nThis cannot be undone."), basename_of(playlist_path_at(playlist_index)));
    gui_popup_show(&delete_song_popup);
}

static void more_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (playlist_index < 0) return;

    lv_obj_remove_flag(more_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(more_menu_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(more_menu_popup_backdrop);
    lv_obj_move_foreground(more_menu_popup);
}

static void build_more_menu_popup(void) {
    const menu_popup_row_t rows[] = {
        { TR("Queue"), more_menu_queue_cb, false },
        { TR("Add to Playlist"), more_menu_add_to_playlist_cb, false },
        { TR("Information"), more_menu_information_cb, false },
        { TR("EQ"), more_menu_eq_cb, false },
        { TR("Delete"), more_menu_delete_cb, true },
        { TR("Cancel"), more_menu_popup_backdrop_cb, false, true },
    };
    more_menu_popup = build_menu_popup(rows, (int) (sizeof(rows) / sizeof(rows[0])), more_menu_popup_backdrop_cb,
                                        &more_menu_popup_backdrop);
}

/* Song context menu moved to gui_queue.c. */

typedef struct {
    lv_obj_t * obj;
    int32_t x, y, w, h;
    int32_t compact_x, compact_y, compact_w, compact_h;
    int32_t height_style;
    int32_t normal_pad_left;
    int32_t centered_text_inset;
    int32_t translate_x, translate_y;
    lv_label_long_mode_t long_mode;
} player_lyrics_geometry_t;

static bool player_lyrics_open;
static bool player_lyrics_animating;
/* Keep preparation damage queued until the timeline starts on a fresh tick. */
static lv_display_t * player_lyrics_start_display;
static bool player_lyrics_start_refresh_requested;

static void player_lyrics_release_start_refresh(void) {
    lv_display_t * display = player_lyrics_start_display;
    bool requested = player_lyrics_start_refresh_requested;
    player_lyrics_start_display = NULL;
    player_lyrics_start_refresh_requested = false;
    if (!display) return;
    lv_timer_t * refresh = lv_display_get_refr_timer(display);
    if (refresh && requested && gui_display_refresh_allowed()) {
        lv_timer_resume(refresh);
        lv_timer_ready(refresh);
    }
}
static bool player_timeline_cover_antialias_saved;
static bool player_timeline_cover_antialias;
static lv_obj_t * player_timeline_cover_fade;
static bool player_timeline_cover_fade_was_hidden;
static int32_t player_timeline_cover_w;
static int32_t player_timeline_cover_h;
static bool player_timeline_cover_clip_corner;
static bool player_timeline_backdrop_covers_cover;
bool gui_player_lyrics_animation_in_progress(void) { return player_lyrics_animating; }
static void player_lyrics_update_thumbnail_clickable(void) {
    if (!player_cover_thumbnail) return;
    if (player_lyrics_open && !player_lyrics_animating)
        lv_obj_add_flag(player_cover_thumbnail, LV_OBJ_FLAG_CLICKABLE);
    else
        lv_obj_remove_flag(player_cover_thumbnail, LV_OBJ_FLAG_CLICKABLE);
}

/* The custom XML timelines resize the card every frame. Keep one composited
 * cover (including optional fade overlays) and scale that during motion,
 * then restore the original live source at the end. */
static void player_timeline_cover_snapshot_restore(void) {
    if (!player_timeline_cover_frame) {
        /* A live cover has no temporary antialias setting to restore. */
        player_timeline_cover_antialias_saved = false;
        return;
    }
    bool was_internal = player_timeline_cover_internal;
    player_timeline_cover_internal = true;
    lv_draw_buf_t * frame = player_timeline_cover_frame;
    const void * source = player_timeline_cover_source;
    bool source_owned = player_timeline_cover_source_owned;
    if (cover_img && lv_image_get_src(cover_img) == frame)
        lv_image_set_src(cover_img, source);
    /* The track may have changed while the timeline was active. Clear the
     * cache state before fitting so the thumbnail follows that newer source. */
    player_timeline_cover_frame = NULL;
    player_timeline_cover_source = NULL;
    player_timeline_cover_source_owned = false;
    fit_cover_img_to_card();
    if (cover_card) lv_obj_set_style_clip_corner(cover_card, player_timeline_cover_clip_corner, 0);
    if (player_timeline_cover_fade && !player_timeline_cover_fade_was_hidden)
        lv_obj_remove_flag(player_timeline_cover_fade, LV_OBJ_FLAG_HIDDEN);
    player_timeline_cover_fade_was_hidden = false;
    if (frame != player_timeline_cover_cache) {
        lv_image_cache_drop(frame);
        lv_draw_buf_destroy(frame);
    }
    if (source_owned) lv_free((void *) source);
    if (player_timeline_cover_antialias_saved && cover_img) {
        lv_image_set_antialias(cover_img, player_timeline_cover_antialias);
        player_timeline_cover_antialias_saved = false;
    }
    player_timeline_cover_internal = was_internal;
}

static void player_timeline_cover_cache_invalidate(void) {
    player_timeline_cover_snapshot_restore();
    if (player_timeline_cover_cache) {
        lv_image_cache_drop(player_timeline_cover_cache);
        lv_draw_buf_destroy(player_timeline_cover_cache);
        player_timeline_cover_cache = NULL;
    }
}

static void player_timeline_cover_style_changed_cb(lv_event_t * e) {
    (void) e;
    if (player_timeline_cover_internal || player_lyrics_animating) return;
    ++player_timeline_cover_style_epoch;
    player_timeline_cover_cache_invalidate();
}

/* Retention is deliberately narrower than the one-shot transition cache:
 * only a plain opaque bitmap can be reused at rest without baking live
 * decorations, inherited recolor, or translucent child blend rounding. */
static void cover_img_tap_cb(lv_event_t * e);

static bool player_timeline_cover_has_only_owned_events(lv_obj_t * object) {
    for (uint32_t i = 0; i < lv_obj_get_event_count(object); ++i) {
        lv_event_dsc_t * event = lv_obj_get_event_dsc(object, i);
        lv_event_cb_t callback = lv_event_dsc_get_cb(event);
        if (callback != cover_img_tap_cb && callback != player_cover_size_changed_cb &&
            callback != player_timeline_cover_style_changed_cb) return false;
    }
    return true;
}

static bool player_timeline_cover_can_retain(void) {
    if (!cover_card || !cover_img || !current_cover_bytes ||
        current_cover_dsc.header.cf != LV_COLOR_FORMAT_RGB565 ||
        !player_timeline_cover_has_only_owned_events(cover_card) ||
        !player_timeline_cover_has_only_owned_events(cover_img) ||
        lv_obj_get_ext_draw_size(cover_card) != 0 ||
        lv_obj_get_style_color_filter_dsc(cover_card, 0) ||
        lv_obj_get_style_color_filter_dsc(cover_img, 0) ||
        lv_obj_get_style_color_filter_opa(cover_card, 0) != LV_OPA_TRANSP ||
        lv_obj_get_style_color_filter_opa(cover_img, 0) != LV_OPA_TRANSP ||
        (lv_obj_get_child_count(cover_card) != 1 &&
         (lv_obj_get_child_count(cover_card) != 2 || !player_timeline_cover_fade ||
          lv_obj_get_child(cover_card, 0) != cover_img ||
          lv_obj_get_child(cover_card, 1) != player_timeline_cover_fade ||
          lv_obj_get_parent(player_timeline_cover_fade) != cover_card ||
          !player_cover_fade_cache_eligible(player_timeline_cover_fade) ||
          (player_timeline_cover_frame
           ? (!lv_obj_has_flag(player_timeline_cover_fade, LV_OBJ_FLAG_HIDDEN) ||
              player_timeline_cover_fade_was_hidden)
           : lv_obj_has_flag(player_timeline_cover_fade, LV_OBJ_FLAG_HIDDEN)))) ||
        lv_obj_has_flag(cover_card, LV_OBJ_FLAG_OVERFLOW_VISIBLE) ||
        lv_obj_get_parent(cover_img) != cover_card ||
        lv_obj_get_child_count(cover_img) != 0 ||
        lv_obj_get_layer_type(cover_card) != LV_LAYER_TYPE_NONE ||
        lv_obj_get_layer_type(cover_img) != LV_LAYER_TYPE_NONE ||
        lv_obj_get_style_opa_recursive(cover_card, 0) != LV_OPA_COVER ||
        lv_obj_get_style_bg_opa(cover_card, 0) != LV_OPA_TRANSP ||
        lv_obj_get_style_radius(cover_card, 0) != 0 ||
        lv_obj_get_style_border_width(cover_card, 0) ||
        lv_obj_get_style_shadow_width(cover_card, 0) ||
        lv_obj_get_style_outline_width(cover_card, 0) ||
        lv_obj_get_style_bg_image_src(cover_card, 0) ||
        lv_obj_get_style_layout(cover_card, 0) != LV_LAYOUT_NONE ||
        lv_obj_get_style_pad_left(cover_card, 0) || lv_obj_get_style_pad_right(cover_card, 0) ||
        lv_obj_get_style_pad_top(cover_card, 0) || lv_obj_get_style_pad_bottom(cover_card, 0) ||
        lv_obj_get_style_transform_width(cover_card, 0) || lv_obj_get_style_transform_height(cover_card, 0) ||
        lv_obj_get_style_transform_rotation(cover_card, 0) ||
        lv_obj_get_style_transform_scale_x(cover_card, 0) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_scale_y(cover_card, 0) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_skew_x(cover_card, 0) ||
        lv_obj_get_style_transform_skew_y(cover_card, 0) ||
        lv_obj_get_style_opa(cover_img, 0) != LV_OPA_COVER ||
        lv_obj_get_style_image_opa(cover_img, 0) != LV_OPA_COVER ||
        lv_obj_get_style_image_recolor_opa(cover_img, 0) != LV_OPA_TRANSP ||
        lv_obj_get_style_recolor_recursive(cover_img, 0).alpha != 0 ||
        lv_obj_get_style_bg_opa(cover_img, 0) != LV_OPA_TRANSP ||
        lv_obj_get_style_border_width(cover_img, 0) ||
        lv_obj_get_style_shadow_width(cover_img, 0) ||
        lv_obj_get_style_outline_width(cover_img, 0) ||
        lv_obj_get_style_bg_image_src(cover_img, 0) ||
        lv_obj_get_style_blend_mode(cover_img, 0) != LV_BLEND_MODE_NORMAL ||
        lv_image_get_blend_mode(cover_img) != LV_BLEND_MODE_NORMAL ||
        lv_image_get_bitmap_map_src(cover_img) ||
        lv_obj_get_style_radius(cover_img, 0) ||
        lv_obj_get_style_translate_x(cover_img, 0) || lv_obj_get_style_translate_y(cover_img, 0) ||
        lv_obj_get_style_transform_width(cover_img, 0) || lv_obj_get_style_transform_height(cover_img, 0) ||
        lv_obj_get_style_pad_left(cover_img, 0) || lv_obj_get_style_pad_right(cover_img, 0) ||
        lv_obj_get_style_pad_top(cover_img, 0) || lv_obj_get_style_pad_bottom(cover_img, 0) ||
        lv_obj_get_style_transform_rotation(cover_img, 0) ||
        lv_obj_get_style_transform_scale_x(cover_img, 0) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_scale_y(cover_img, 0) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_skew_x(cover_img, 0) ||
        lv_obj_get_style_transform_skew_y(cover_img, 0) ||
        lv_image_get_rotation(cover_img) ||
        lv_image_get_offset_x(cover_img) || lv_image_get_offset_y(cover_img)) return false;
    lv_image_align_t inner_align = lv_image_get_inner_align(cover_img);
    /* LVGL defaults to CENTER. At intrinsic widget dimensions it is identical
     * to DEFAULT; other inner alignment geometries remain live. */
    if (inner_align != LV_IMAGE_ALIGN_DEFAULT &&
        (inner_align != LV_IMAGE_ALIGN_CENTER ||
         lv_obj_get_width(cover_img) != lv_image_get_src_width(cover_img) ||
         lv_obj_get_height(cover_img) != lv_image_get_src_height(cover_img))) return false;
    lv_point_t pivot;
    lv_image_get_pivot(cover_img, &pivot);
    return pivot.x == lv_image_get_src_width(cover_img) / 2 &&
           pivot.y == lv_image_get_src_height(cover_img) / 2;
}

/* Drawing-only properties do not all emit STYLE_CHANGED. Revalidate the
 * narrow retained binding before the display builds draw tasks as well. */
static bool player_timeline_display_refreshing;
static void player_timeline_cover_cache_refresh_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_REFR_START) player_timeline_display_refreshing = true;
    else if (code == LV_EVENT_REFR_READY || code == LV_EVENT_DELETE) player_timeline_display_refreshing = false;
    if (code == LV_EVENT_REFR_REQUEST) {
        lv_display_t * display = lv_event_get_target(e);
        if (display == player_lyrics_start_display) {
            /* LVGL's built-in request handler resumes this timer first. Keep
             * the barrier intact without dropping or disabling invalidation. */
            lv_timer_t * refresh = lv_display_get_refr_timer(display);
            player_lyrics_start_refresh_requested = true;
            if (refresh) lv_timer_pause(refresh);
            return;
        }
        /* Damage produced after the refresh timer was visited should be
         * presented on the next handler pass. During a refresh, keep LVGL's
         * normal scheduling so layout invalidations do not queue an extra
         * presentation of the same animation sample. */
        if (!player_timeline_display_refreshing && player_lyrics_animating &&
            player_timeline_lyrics_open && player_timeline_lyrics_close &&
            player_screen && lv_display_get_screen_active(display) == player_screen) {
            lv_timer_t * timer = lv_display_get_refr_timer(display);
            if (timer) lv_timer_ready(timer);
        }
        return;
    }
    if (code == LV_EVENT_REFR_READY) return;
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        if (lv_event_get_target(e) == player_lyrics_start_display) {
            player_lyrics_start_display = NULL;
            player_lyrics_start_refresh_requested = false;
        }
        if (lv_event_get_target(e) == player_timeline_cover_cache_display)
            player_timeline_cover_cache_display = NULL;
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_INVALIDATE_AREA) {
        /* XML lyrics move several overlapping widgets. One display region
         * avoids repeatedly drawing the same backdrop and copying fragmented
         * damage between the direct framebuffer pages. */
        lv_display_t * display = lv_event_get_target(e);
        if (player_lyrics_animating && player_timeline_lyrics_open && player_timeline_lyrics_close &&
            player_screen && lv_display_get_screen_active(display) == player_screen) {
            lv_area_t * area = lv_event_get_param(e);
            if (area) {
                area->x1 = 0;
                area->y1 = 0;
                area->x2 = lv_display_get_horizontal_resolution(display) - 1;
                area->y2 = lv_display_get_vertical_resolution(display) - 1;
            }
        }
        return;
    }
    if (player_timeline_cover_internal || player_lyrics_animating ||
        !player_timeline_cover_cache || player_timeline_cover_frame != player_timeline_cover_cache) return;
    if (!player_timeline_cover_can_retain() ||
        player_timeline_cover_cache_generation != current_cover_generation ||
        player_timeline_cover_cache_epoch != player_timeline_cover_style_epoch ||
        lv_image_get_src(cover_img) != player_timeline_cover_frame ||
        lv_obj_get_width(cover_card) != (int32_t) player_timeline_cover_cache->header.w ||
        lv_obj_get_height(cover_card) != (int32_t) player_timeline_cover_cache->header.h ||
        lv_image_get_antialias(cover_img)) {
        /* A caller explicitly enabling antialias on the kept image should
         * retain that setting when the original source is rebound. */
        if (cover_img && lv_image_get_antialias(cover_img)) player_timeline_cover_antialias = true;
        player_timeline_cover_cache_invalidate();
    }
}

/* A card snapshot replaces only static cover pixels. Geometry can animate
 * on the card itself; drawing changes or other children must stay live.
 * Drawing-property animations, including radius, retain live rendering. */
static bool player_timeline_cover_cache_eligible(lv_anim_timeline_t * timeline) {
    if (!timeline || !cover_card || !cover_img || lv_obj_get_parent(cover_img) != cover_card ||
        lv_obj_get_child_count(cover_img) != 0 ||
        lv_obj_get_style_layout(cover_card, LV_PART_MAIN) != LV_LAYOUT_NONE ||
        lv_obj_get_style_pad_left(cover_card, 0) || lv_obj_get_style_pad_right(cover_card, 0) ||
        lv_obj_get_style_pad_top(cover_card, 0) || lv_obj_get_style_pad_bottom(cover_card, 0) ||
        lv_obj_get_style_border_width(cover_card, 0) ||
        lv_obj_get_style_outline_width(cover_card, 0) || lv_obj_get_style_shadow_width(cover_card, 0) ||
        lv_obj_get_style_opa(cover_card, 0) != LV_OPA_COVER ||
        lv_obj_get_style_transform_width(cover_card, 0) || lv_obj_get_style_transform_height(cover_card, 0) ||
        lv_obj_get_style_transform_rotation(cover_card, 0) ||
        lv_obj_get_style_transform_scale_x(cover_card, 0) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_scale_y(cover_card, 0) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_skew_x(cover_card, 0) ||
        lv_obj_get_style_transform_skew_y(cover_card, 0)) return false;
    uint32_t children = lv_obj_get_child_count(cover_card);
    for (uint32_t i = 0; i < children; ++i) {
        lv_obj_t * child = lv_obj_get_child(cover_card, i);
        if (child != cover_img && child != player_timeline_cover_fade &&
            lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN) &&
            !player_layouts_timeline_has_animation_for_obj(timeline, child)) continue;
        if ((child != cover_img && child != player_timeline_cover_fade) ||
            (lv_obj_get_child_count(child) != 0 &&
             (child != player_timeline_cover_fade || !player_cover_fade_cache_eligible(child))) ||
            player_layouts_timeline_has_animation_for_obj(timeline, child)) return false;
        if (child == player_timeline_cover_fade && lv_obj_get_child_count(child) &&
            player_layouts_timeline_has_animation_for_obj(timeline, lv_obj_get_child(child, 0))) return false;
    }
    uint32_t count = player_layouts_timeline_style_animation_count(timeline);
    uint32_t card_count = 0;
    for (uint32_t i = 0; i < count; ++i) {
        player_layout_style_transition_t animation;
        if (!player_layouts_timeline_get_style_animation(timeline, i, &animation)) return false;
        if (animation.target != cover_card) continue;
        if (animation.selector != LV_PART_MAIN ||
            (animation.prop != LV_STYLE_WIDTH && animation.prop != LV_STYLE_HEIGHT &&
             animation.prop != LV_STYLE_TRANSLATE_X && animation.prop != LV_STYLE_TRANSLATE_Y &&
             animation.prop != LV_STYLE_OPA)) return false;
        ++card_count;
    }
    return card_count == player_layouts_timeline_animation_count_for_obj(timeline, cover_card);
}

static bool player_timeline_cover_snapshot_is_opaque(const lv_draw_buf_t * frame) {
    if (!frame || !frame->data || frame->header.cf != LV_COLOR_FORMAT_ARGB8888 ||
        frame->header.stride < (uint64_t) frame->header.w * 4U ||
        (uint64_t) frame->header.stride * frame->header.h > frame->data_size) return false;
    for (uint32_t y = 0; y < frame->header.h; ++y) {
        const uint8_t * row = frame->data + (size_t) y * frame->header.stride;
        for (uint32_t x = 0; x < frame->header.w; ++x)
            if (row[(size_t) x * 4U + 3U] != LV_OPA_COVER) return false;
    }
    return true;
}

static void player_timeline_cover_snapshot_take(lv_anim_timeline_t * timeline) {
    if (!cover_img || !cover_card || player_timeline_cover_frame ||
        !player_timeline_cover_cache_eligible(timeline)) return;
    const void * source = lv_image_get_src(cover_img);
    if (!source) return;
    player_timeline_cover_source = source;
    lv_image_src_t source_type = lv_image_src_get_type(source);
    player_timeline_cover_source_owned = source_type == LV_IMAGE_SRC_FILE || source_type == LV_IMAGE_SRC_SYMBOL;
    if (player_timeline_cover_source_owned) {
        player_timeline_cover_source = lv_strdup(source);
        if (!player_timeline_cover_source) {
            player_timeline_cover_source_owned = false;
            return;
        }
    }

    int32_t current_w = lv_obj_get_width(cover_card);
    int32_t current_h = lv_obj_get_height(cover_card);
    int32_t snapshot_w = player_timeline_cover_w > 0 ? player_timeline_cover_w : current_w;
    int32_t snapshot_h = player_timeline_cover_h > 0 ? player_timeline_cover_h : current_h;
    bool retain = source == &current_cover_dsc && player_timeline_cover_can_retain() &&
        snapshot_w > 0 && snapshot_h > 0 &&
        (uint64_t) lv_draw_buf_width_to_stride(snapshot_w, LV_COLOR_FORMAT_ARGB8888) * snapshot_h +
            LV_DRAW_BUF_ALIGN - 1U + sizeof(lv_draw_buf_t) <= 1024U * 1024U;
    bool hit = retain && player_timeline_cover_cache &&
        player_timeline_cover_cache_generation == current_cover_generation &&
        player_timeline_cover_cache_epoch == player_timeline_cover_style_epoch &&
        player_timeline_cover_cache_antialias == lv_image_get_antialias(cover_img) &&
        player_timeline_cover_cache->header.w == (uint32_t) snapshot_w &&
        player_timeline_cover_cache->header.h == (uint32_t) snapshot_h;
    bool was_internal = player_timeline_cover_internal;
    player_timeline_cover_internal = true;
    player_timeline_cover_clip_corner = lv_obj_get_style_clip_corner(cover_card, 0);
    int32_t card_radius = lv_obj_get_style_radius(cover_card, 0);
    lv_color_format_t format = player_timeline_cover_clip_corner && card_radius != 0 ?
                               LV_COLOR_FORMAT_ARGB8888 : LV_COLOR_FORMAT_RGB565;
    if (hit) {
        player_timeline_cover_frame = player_timeline_cover_cache;
    } else {
        lv_obj_set_size(cover_card, snapshot_w, snapshot_h);
        lv_obj_update_layout(cover_card);
        fit_cover_img_to_card();
        if (retain && !player_timeline_cover_can_retain()) {
            retain = false;
            format = LV_COLOR_FORMAT_ARGB8888;
        }
        if (retain) {
            /* An undecorated identity RGB565 image fills every card pixel.
             * Capture it natively instead of allocating ARGB, checking every
             * alpha byte and converting the same opaque pixels back to 565. */
            bool native_opaque = snapshot_w == (int32_t)current_cover_dsc.header.w &&
                snapshot_h == (int32_t)current_cover_dsc.header.h &&
                lv_image_get_scale_x(cover_img) == LV_SCALE_NONE &&
                lv_image_get_scale_y(cover_img) == LV_SCALE_NONE &&
                lv_obj_get_x(cover_img) == 0 && lv_obj_get_y(cover_img) == 0;
            bool managed_fade = player_timeline_cover_can_retain() && player_timeline_cover_fade &&
                lv_obj_get_parent(player_timeline_cover_fade) == cover_card &&
                player_cover_fade_cache_eligible(player_timeline_cover_fade);
            if (managed_fade) {
                /* Prove the underlying cover opaque before adding the tint.
                 * A8 blending into ARGB can round an opaque alpha down; taking
                 * the combined snapshot directly in RGB565 preserves native
                 * tint rounding once its underlying pixels are proven opaque. */
                format = LV_COLOR_FORMAT_ARGB8888;
                bool fade_was_hidden = lv_obj_has_flag(player_timeline_cover_fade, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(player_timeline_cover_fade, LV_OBJ_FLAG_HIDDEN);
                lv_draw_buf_t * proof = lv_snapshot_take(cover_card, LV_COLOR_FORMAT_ARGB8888);
                if (!fade_was_hidden) lv_obj_remove_flag(player_timeline_cover_fade, LV_OBJ_FLAG_HIDDEN);
                native_opaque = proof && proof->header.w == (uint32_t) snapshot_w &&
                    proof->header.h == (uint32_t) snapshot_h &&
                    player_timeline_cover_snapshot_is_opaque(proof);
                if (proof) lv_draw_buf_destroy(proof);
                if (!native_opaque) retain = false;
            }
            if (retain)
                player_timeline_cover_frame = lv_snapshot_take(cover_card,
                    native_opaque ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_ARGB8888);
            bool opaque = player_timeline_cover_frame &&
                (native_opaque || player_timeline_cover_snapshot_is_opaque(player_timeline_cover_frame));
            if (opaque && !native_opaque) {
                uint32_t stride = lv_draw_buf_width_to_stride(snapshot_w, LV_COLOR_FORMAT_RGB565);
                transition_compositor_blend_argb8888_over_rgb565(player_timeline_cover_frame->data, stride,
                    player_timeline_cover_frame->data, player_timeline_cover_frame->header.stride,
                    snapshot_w, snapshot_h);
                if (!lv_draw_buf_reshape(player_timeline_cover_frame, LV_COLOR_FORMAT_RGB565,
                                         snapshot_w, snapshot_h, stride)) opaque = false;
                else lv_draw_buf_clear_flag(player_timeline_cover_frame, LV_IMAGE_FLAGS_PREMULTIPLIED);
            }
            if (!opaque) {
                if (player_timeline_cover_frame) lv_draw_buf_destroy(player_timeline_cover_frame);
                player_timeline_cover_frame = NULL;
                retain = false;
            }
        }
        if (!player_timeline_cover_frame) player_timeline_cover_frame = lv_snapshot_take(cover_card, format);
        lv_obj_set_size(cover_card, current_w, current_h);
        lv_obj_update_layout(cover_card);
        if (retain && player_timeline_cover_frame) {
            if (player_timeline_cover_cache) {
                lv_image_cache_drop(player_timeline_cover_cache);
                lv_draw_buf_destroy(player_timeline_cover_cache);
            }
            player_timeline_cover_cache = player_timeline_cover_frame;
            player_timeline_cover_cache_generation = current_cover_generation;
            player_timeline_cover_cache_epoch = player_timeline_cover_style_epoch;
            player_timeline_cover_cache_antialias = lv_image_get_antialias(cover_img);
        }
    }
#ifdef UI_PERF_TRACE
    printf("PERF player_cover_snapshot retained=%d hit=%d\n", retain, hit);
#endif
    if (!player_timeline_cover_frame) {
        if (player_timeline_cover_source_owned) lv_free((void *) player_timeline_cover_source);
        player_timeline_cover_source = NULL;
        player_timeline_cover_source_owned = false;
        fit_cover_img_to_card();
        player_timeline_cover_internal = was_internal;
        return;
    }

    if (player_timeline_cover_frame == player_timeline_cover_cache &&
        player_timeline_cover_frame->header.cf == LV_COLOR_FORMAT_RGB565)
        lv_draw_buf_set_flag(player_timeline_cover_frame, LV_IMAGE_FLAGS_USER3);
    lv_image_set_src(cover_img, player_timeline_cover_frame);
    fit_cover_img_to_card();
    if (player_cover_thumbnail) {
        lv_image_set_src(player_cover_thumbnail, player_timeline_cover_source);
        lv_image_set_inner_align(player_cover_thumbnail, LV_IMAGE_ALIGN_CONTAIN);
    }
    lv_obj_set_style_clip_corner(cover_card, false, 0);
    /* The image now contains every child captured in the snapshot. Hide the
     * live fade child to prevent drawing it a second time over the cache. */
    player_timeline_cover_fade_was_hidden = player_timeline_cover_fade &&
        lv_obj_has_flag(player_timeline_cover_fade, LV_OBJ_FLAG_HIDDEN);
    if (player_timeline_cover_fade && !player_timeline_cover_fade_was_hidden)
        lv_obj_add_flag(player_timeline_cover_fade, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_antialias(cover_img, false);
    player_timeline_cover_internal = was_internal;
}
#define PLAYER_LYRICS_GEOMETRY_OBJECT_COUNT 4
static player_lyrics_geometry_t player_lyrics_geometry[PLAYER_LYRICS_GEOMETRY_OBJECT_COUNT];
/* Cover first, then the metadata labels the layout has; the title is required
 * but artist and album are optional, so this is 2..4. */
static unsigned player_lyrics_object_count;
static lv_obj_t * player_lyrics_hidden[32];
static unsigned player_lyrics_hidden_count;
static int32_t player_lyrics_content_top;
static bool player_lyrics_generated_timelines;

#ifdef UI_PERF_TRACE
#define PLAYER_LYRICS_PERF_FRAME_CAPACITY 64
typedef struct {
    unsigned sequence;
    uint64_t refresh_start_us;
    uint64_t render_start_us;
    uint64_t render_ready_us;
    uint64_t last_flush_start_us;
    uint64_t last_flush_finish_us;
    uint64_t present_finish_us;
    uint64_t last_flush_us;
    uint64_t flush_total_us;
    uint64_t present_gap_us;
    uint64_t refresh_ready_us;
    int32_t cover_width;
    int32_t cover_height;
    unsigned flush_count;
    bool flush_is_last;
} player_lyrics_perf_frame_t;

typedef struct {
    uint64_t refresh_start_us;
    uint64_t render_start_us;
    uint64_t last_refresh_start_us;
    uint64_t last_present_finish_us;
    uint64_t trace_start_us;
    uint64_t prep_elapsed_us;
    uint64_t runner_start_us;
    uint32_t runner_start_tick;
    uint64_t render_total_us;
    uint64_t render_max_us;
    uint64_t refresh_total_us;
    uint64_t refresh_max_us;
    uint64_t max_gap_us;
    player_lyrics_perf_frame_t frames[PLAYER_LYRICS_PERF_FRAME_CAPACITY];
    unsigned frame_record_head;
    unsigned frame_record_count;
    unsigned dropped_frame_records;
    unsigned total_frame_records;
    unsigned active_frame_index;
    unsigned render_count;
    unsigned refresh_count;
    unsigned presentation_count;
    unsigned finish_refreshes_without_present;
    bool trace_open;
    bool trace_is_timeline;
    bool refresh_started;
    bool render_started;
    bool active_frame;
    bool refresh_had_presentation;
    bool finish_pending;
} player_lyrics_perf_stats_t;

static player_lyrics_perf_stats_t player_lyrics_perf_stats;
static lv_display_t * player_lyrics_perf_registered_display;
static bool player_lyrics_perf_trace_active;

static void player_lyrics_perf_log_summary(bool final_presented, const char * reason);

static void player_lyrics_perf_display_event_cb(lv_event_t * e) {
    if (!player_lyrics_perf_trace_active) return;

    uint64_t now_us = ui_perf_now_us();
    switch (lv_event_get_code(e)) {
        case LV_EVENT_REFR_START:
            if (player_lyrics_perf_stats.last_refresh_start_us) {
                uint64_t gap_us = now_us - player_lyrics_perf_stats.last_refresh_start_us;
                if (gap_us > player_lyrics_perf_stats.max_gap_us)
                    player_lyrics_perf_stats.max_gap_us = gap_us;
            }
            player_lyrics_perf_stats.last_refresh_start_us = now_us;
            player_lyrics_perf_stats.refresh_start_us = now_us;
            player_lyrics_perf_stats.refresh_started = true;
            player_lyrics_perf_stats.refresh_had_presentation = false;
            player_lyrics_perf_stats.active_frame = false;
            break;
        case LV_EVENT_RENDER_START:
            player_lyrics_perf_stats.render_start_us = now_us;
            player_lyrics_perf_stats.render_started = true;
            if (player_lyrics_perf_stats.frame_record_count < PLAYER_LYRICS_PERF_FRAME_CAPACITY) {
                player_lyrics_perf_stats.active_frame_index =
                    (player_lyrics_perf_stats.frame_record_head + player_lyrics_perf_stats.frame_record_count) %
                    PLAYER_LYRICS_PERF_FRAME_CAPACITY;
                player_lyrics_perf_stats.frame_record_count++;
            } else {
                player_lyrics_perf_stats.active_frame_index = player_lyrics_perf_stats.frame_record_head;
                player_lyrics_perf_stats.frame_record_head =
                    (player_lyrics_perf_stats.frame_record_head + 1U) % PLAYER_LYRICS_PERF_FRAME_CAPACITY;
                player_lyrics_perf_stats.dropped_frame_records++;
            }
            player_lyrics_perf_stats.total_frame_records++;
            player_lyrics_perf_stats.active_frame = true;
            player_lyrics_perf_frame_t * frame =
                &player_lyrics_perf_stats.frames[player_lyrics_perf_stats.active_frame_index];
            memset(frame, 0, sizeof(*frame));
            frame->sequence = player_lyrics_perf_stats.total_frame_records;
            frame->refresh_start_us = player_lyrics_perf_stats.refresh_start_us;
            frame->render_start_us = now_us;
            if (cover_card) {
                frame->cover_width = lv_obj_get_width(cover_card);
                frame->cover_height = lv_obj_get_height(cover_card);
            }
            break;
        case LV_EVENT_RENDER_READY:
            if (player_lyrics_perf_stats.render_started) {
                uint64_t render_us = now_us - player_lyrics_perf_stats.render_start_us;
                player_lyrics_perf_stats.render_total_us += render_us;
                if (render_us > player_lyrics_perf_stats.render_max_us)
                    player_lyrics_perf_stats.render_max_us = render_us;
                player_lyrics_perf_stats.render_count++;
                player_lyrics_perf_stats.render_started = false;
            }
            if (player_lyrics_perf_stats.active_frame) {
                player_lyrics_perf_stats.frames[player_lyrics_perf_stats.active_frame_index].render_ready_us = now_us;
            }
            break;
        case LV_EVENT_FLUSH_START:
            if (player_lyrics_perf_stats.active_frame) {
                player_lyrics_perf_frame_t * frame =
                    &player_lyrics_perf_stats.frames[player_lyrics_perf_stats.active_frame_index];
                frame->last_flush_start_us = now_us;
                frame->flush_is_last = lv_display_flush_is_last((lv_display_t *) lv_event_get_target(e));
            }
            break;
        case LV_EVENT_FLUSH_FINISH:
            if (player_lyrics_perf_stats.active_frame) {
                player_lyrics_perf_frame_t * frame =
                    &player_lyrics_perf_stats.frames[player_lyrics_perf_stats.active_frame_index];
                frame->last_flush_finish_us = now_us;
                frame->last_flush_us = frame->last_flush_start_us && now_us >= frame->last_flush_start_us
                    ? now_us - frame->last_flush_start_us : 0;
                frame->flush_total_us += frame->last_flush_us;
                frame->flush_count++;
                if (frame->flush_is_last) frame->present_finish_us = now_us;
                if (frame->flush_is_last) player_lyrics_perf_stats.refresh_had_presentation = true;
            }
            break;
        case LV_EVENT_REFR_READY:
            if (player_lyrics_perf_stats.refresh_started) {
                uint64_t refresh_us = now_us - player_lyrics_perf_stats.refresh_start_us;
                player_lyrics_perf_stats.refresh_total_us += refresh_us;
                if (refresh_us > player_lyrics_perf_stats.refresh_max_us)
                    player_lyrics_perf_stats.refresh_max_us = refresh_us;
                player_lyrics_perf_stats.refresh_count++;
                player_lyrics_perf_stats.refresh_started = false;
            }
            if (player_lyrics_perf_stats.active_frame) {
                player_lyrics_perf_frame_t * frame =
                    &player_lyrics_perf_stats.frames[player_lyrics_perf_stats.active_frame_index];
                frame->refresh_ready_us = now_us;
                if (frame->present_finish_us) {
                    player_lyrics_perf_stats.presentation_count++;
                    if (player_lyrics_perf_stats.last_present_finish_us &&
                        frame->present_finish_us >= player_lyrics_perf_stats.last_present_finish_us) {
                        frame->present_gap_us = frame->present_finish_us -
                                                player_lyrics_perf_stats.last_present_finish_us;
                    }
                    player_lyrics_perf_stats.last_present_finish_us = frame->present_finish_us;
                }
                player_lyrics_perf_stats.active_frame = false;
            }
            if (player_lyrics_perf_stats.finish_pending) {
                if (player_lyrics_perf_stats.refresh_had_presentation) {
                    player_lyrics_perf_log_summary(true, "settled_frame");
                } else if (++player_lyrics_perf_stats.finish_refreshes_without_present >= 2U) {
                    player_lyrics_perf_log_summary(false, "no_final_flush");
                }
            }
            break;
        default:
            break;
    }
}

static void player_lyrics_perf_register_display_events(void) {
    lv_display_t * disp = lv_display_get_default();
    if (!disp || disp == player_lyrics_perf_registered_display) return;
    lv_display_add_event_cb(disp, player_lyrics_perf_display_event_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, player_lyrics_perf_display_event_cb, LV_EVENT_REFR_READY, NULL);
    lv_display_add_event_cb(disp, player_lyrics_perf_display_event_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(disp, player_lyrics_perf_display_event_cb, LV_EVENT_RENDER_READY, NULL);
    lv_display_add_event_cb(disp, player_lyrics_perf_display_event_cb, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(disp, player_lyrics_perf_display_event_cb, LV_EVENT_FLUSH_FINISH, NULL);
    player_lyrics_perf_registered_display = disp;
}

static void player_lyrics_perf_reset(bool open, bool timeline) {
    if (player_lyrics_perf_trace_active) {
        player_lyrics_perf_log_summary(false,
            player_lyrics_perf_stats.finish_pending ? "interrupted_before_final_flush" : "interrupted");
    }
    memset(&player_lyrics_perf_stats, 0, sizeof(player_lyrics_perf_stats));
    player_lyrics_perf_stats.trace_start_us = ui_perf_now_us();
    player_lyrics_perf_stats.trace_open = open;
    player_lyrics_perf_stats.trace_is_timeline = timeline;
    player_lyrics_perf_trace_active = true;
}

static void player_lyrics_perf_finish_pending(void) {
    if (!player_lyrics_perf_trace_active) return;
    player_lyrics_perf_stats.finish_pending = true;
    player_lyrics_perf_stats.finish_refreshes_without_present = 0;
}

static void player_lyrics_perf_abort(const char * reason) {
    if (!player_lyrics_perf_trace_active) return;
    player_lyrics_perf_log_summary(false, reason);
}

static void player_lyrics_perf_log_summary(bool final_presented, const char * reason) {
    const player_lyrics_perf_stats_t * s = &player_lyrics_perf_stats;
    uint64_t present_gaps[PLAYER_LYRICS_PERF_FRAME_CAPACITY];
    unsigned present_gap_count = 0;
    for (unsigned i = 0; i < s->frame_record_count; ++i) {
        unsigned index = (s->frame_record_head + i) % PLAYER_LYRICS_PERF_FRAME_CAPACITY;
        uint64_t gap = s->frames[index].present_gap_us;
        if (!gap) continue;
        unsigned insert = present_gap_count;
        while (insert > 0 && present_gaps[insert - 1] > gap) {
            present_gaps[insert] = present_gaps[insert - 1];
            insert--;
        }
        present_gaps[insert] = gap;
        present_gap_count++;
    }
    uint64_t present_gap_p95_us = present_gap_count
        ? present_gaps[((present_gap_count * 95U + 99U) / 100U) - 1U] : 0;
    uint64_t present_gap_p99_us = present_gap_count
        ? present_gaps[((present_gap_count * 99U + 99U) / 100U) - 1U] : 0;
    printf("PERF player_lyrics_%s direction=%s end=%s final_presented=%d presentations=%u count=%u prep_us=%llu render_avg_us=%llu render_max_us=%llu refresh_count=%u refresh_avg_us=%llu refresh_max_us=%llu maxgap_us=%llu present_gap_p95_us=%llu present_gap_p99_us=%llu frame_records=%u frame_records_total=%u frame_records_discarded=%u\n",
           s->trace_is_timeline ? "timeline" : "morph",
           s->trace_open ? "open" : "close", reason, final_presented, s->presentation_count, s->render_count,
           (unsigned long long) s->prep_elapsed_us,
           (unsigned long long) (s->render_count ? s->render_total_us / s->render_count : 0),
           (unsigned long long) s->render_max_us, s->refresh_count,
           (unsigned long long) (s->refresh_count ? s->refresh_total_us / s->refresh_count : 0),
           (unsigned long long) s->refresh_max_us,
           (unsigned long long) s->max_gap_us,
           (unsigned long long) present_gap_p95_us,
           (unsigned long long) present_gap_p99_us,
           s->frame_record_count, s->total_frame_records, s->dropped_frame_records);
    printf("PERF player_lyrics_start runner_us=%llu runner_tick=%u\n",
           (unsigned long long) s->runner_start_us, s->runner_start_tick);
    for (unsigned i = 0; i < s->frame_record_count; ++i) {
        unsigned index = (s->frame_record_head + i) % PLAYER_LYRICS_PERF_FRAME_CAPACITY;
        const player_lyrics_perf_frame_t * frame = &s->frames[index];
        uint64_t render_us = frame->render_start_us && frame->render_ready_us >= frame->render_start_us
            ? frame->render_ready_us - frame->render_start_us : 0;
        uint64_t raster_us = render_us > frame->flush_total_us
            ? render_us - frame->flush_total_us : 0;
        uint64_t refresh_us = frame->refresh_ready_us >= frame->refresh_start_us
            ? frame->refresh_ready_us - frame->refresh_start_us : 0;
        printf("PERF player_lyrics_frame seq=%u refresh_start_us=%llu cover_w=%d cover_h=%d raster_us=%llu flush_count=%u flush_total_us=%llu flush_last_us=%llu present_finish_us=%llu present_gap_us=%llu total_us=%llu\n",
               frame->sequence,
               (unsigned long long) (frame->refresh_start_us >= s->trace_start_us
                   ? frame->refresh_start_us - s->trace_start_us : 0),
               frame->cover_width, frame->cover_height,
               (unsigned long long) raster_us, frame->flush_count,
               (unsigned long long) frame->flush_total_us,
               (unsigned long long) frame->last_flush_us,
               (unsigned long long) (frame->present_finish_us >= s->trace_start_us
                   ? frame->present_finish_us - s->trace_start_us : 0),
               (unsigned long long) frame->present_gap_us,
               (unsigned long long) refresh_us);
    }
    player_lyrics_perf_trace_active = false;
    player_lyrics_perf_stats.finish_pending = false;
}
#endif

static void player_lyrics_restore_controls(void) {
    for (unsigned i = 0; i < player_lyrics_hidden_count; ++i)
        lv_obj_remove_flag(player_lyrics_hidden[i], LV_OBJ_FLAG_HIDDEN);
    player_lyrics_hidden_count = 0;
}

static void player_lyrics_hide_object(lv_obj_t * obj) {
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (player_lyrics_hidden_count >= sizeof(player_lyrics_hidden) / sizeof(player_lyrics_hidden[0])) return;
    player_lyrics_hidden[player_lyrics_hidden_count++] = obj;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void player_lyrics_unhide_object(lv_obj_t * obj) {
    for (unsigned i = 0; i < player_lyrics_hidden_count; ++i) {
        if (player_lyrics_hidden[i] != obj) continue;
        player_lyrics_hidden[i] = player_lyrics_hidden[--player_lyrics_hidden_count];
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        return;
    }
}

/* Screen rectangle of the layout's lyrics area, after layout. */
static bool player_lyrics_area_rect(lv_area_t * area) {
    if (!player_lyrics_area || !player_screen) return false;
    lv_obj_update_layout(player_screen);
    lv_obj_get_coords(player_lyrics_area, area);
    return true;
}

static void player_lyrics_set_open(bool open, bool animate);

static void player_lyrics_area_empty_tap(void) {
    player_lyrics_set_open(false, true);
}

/* Shows the lyrics in the area. Touch targets over it would take the taps
 * meant for the lyrics, so those stay off while the others come back. */
static void player_lyrics_show_in_area(const lv_area_t * area) {
    for (unsigned i = 0; i < player_hit_target_count; ++i) {
        lv_area_t hit;
        lv_obj_get_coords(player_hit_targets[i], &hit);
        bool overlaps = hit.x1 <= area->x2 && hit.x2 >= area->x1 && hit.y1 <= area->y2 && hit.y2 >= area->y1;
        if (overlaps) player_lyrics_hide_object(player_hit_targets[i]);
        else player_lyrics_unhide_object(player_hit_targets[i]);
    }
    gui_lyrics_show_embedded_area(player_screen, area->x1, area->y1, lv_area_get_width(area),
                                  lv_area_get_height(area));
}

/* Without timelines, a layout with an area just shows and hides the lyrics. */
static void player_lyrics_set_open_area(bool open) {
    player_lyrics_object_count = 0;
    player_lyrics_open = open;
    player_lyrics_animating = false;
    lv_draw_sw_image_cache_set_transition_active(false);
    player_lyrics_update_thumbnail_clickable();
    lv_area_t area;
    if (open && player_lyrics_area_rect(&area)) {
        player_lyrics_show_in_area(&area);
    } else {
        gui_lyrics_hide_embedded();
        player_lyrics_restore_controls();
    }
    player_transition_mark_dirty();
}

/* A layout that ships both `lyrics_open` and `lyrics_close` timelines animates
 * the transition itself; native layouts supply equivalent measured properties. The timelines
 * own the geometry, so all this does is sequence them with what must stay
 * native: disabling the touch targets, hiding the controls once they have
 * animated away, and embedding the lyrics where the cover and metadata ended
 * up. The shared runner completes when its native timeline animation finishes. */
static lv_timer_t * player_lyrics_timeline_timer;

static void player_lyrics_timeline_cancel_timer(void) {
    player_lyrics_release_start_refresh();
    if (player_artwork_cache_timer) lv_timer_pause(player_artwork_cache_timer);
    lv_draw_sw_image_cache_set_transition_active(false);
    if (player_lyrics_timeline_timer) {
        lv_timer_delete(player_lyrics_timeline_timer);
        player_lyrics_timeline_timer = NULL;
    }
}

static void player_lyrics_timeline_finish(void) {
    player_lyrics_release_start_refresh();
    lv_draw_sw_image_cache_set_transition_active(false);
#ifdef UI_PERF_TRACE
    lv_draw_sw_image_cache_stats_t cache_stats;
    lv_draw_sw_image_cache_get_intermediate_stats(&cache_stats);
    printf("PERF player_intermediate_cache hits=%llu misses=%llu observations=%llu builds=%llu build_us_total=%llu build_us_max=%llu slice_us_max=%llu evictions=%llu bytes=%u entries=%u candidates=%u qualified_candidates=%u pending_rows=%u hit_pixels=%llu qualifications=%llu displacements=%llu service_calls=%llu idle_blocks=%llu transition_blocks=%llu pressure_blocks=%llu no_candidate=%llu source_rejections=%llu\n",
        (unsigned long long) cache_stats.hits, (unsigned long long) cache_stats.misses,
        (unsigned long long) cache_stats.observations, (unsigned long long) cache_stats.builds,
        (unsigned long long) cache_stats.build_us_total, (unsigned long long) cache_stats.build_us_max,
        (unsigned long long) cache_stats.slice_us_max,
        (unsigned long long) cache_stats.evictions, cache_stats.bytes, cache_stats.entries, cache_stats.candidates,
        cache_stats.qualified_candidates, cache_stats.pending_rows,
        (unsigned long long) cache_stats.hit_pixels,
        (unsigned long long) cache_stats.candidate_qualifications,
        (unsigned long long) cache_stats.candidate_displacements,
        (unsigned long long) cache_stats.service_calls,
        (unsigned long long) cache_stats.service_blocked_idle,
        (unsigned long long) cache_stats.service_blocked_transition,
        (unsigned long long) cache_stats.service_blocked_pressure,
        (unsigned long long) cache_stats.service_no_candidate,
        (unsigned long long) cache_stats.source_rejections);
#endif
    bool was_internal = player_timeline_cover_internal;
    player_timeline_cover_internal = true;
#ifdef UI_PERF_TRACE
    if (player_lyrics_animating || player_lyrics_perf_trace_active) player_lyrics_perf_finish_pending();
#endif
    player_lyrics_animating = false;
    lv_anim_timeline_t * finished = player_lyrics_open ? player_timeline_lyrics_open : player_timeline_lyrics_close;
    if (finished) {
        lv_anim_timeline_pause(finished);
        lv_anim_timeline_set_progress(finished, LV_ANIM_TIMELINE_PROGRESS_MAX);
    }
    player_timeline_labels_end();
    if (player_lyrics_generated_timelines) {
        for (unsigned i = 1; i < player_lyrics_object_count; ++i) {
            player_lyrics_geometry_t * g = &player_lyrics_geometry[i];
            lv_label_set_long_mode(g->obj, g->long_mode);
            if (!player_lyrics_open) {
                lv_obj_set_style_pad_left(g->obj, g->normal_pad_left, 0);
                lv_obj_set_height(g->obj, g->height_style);
                lv_obj_set_style_text_align(g->obj, LV_TEXT_ALIGN_CENTER, 0);
            }
        }
    }
    player_lyrics_backdrop_end(true);
    player_lyrics_backdrop_set_busy(false);
    /* Timeline style endpoints and restored labels may still have deferred
     * geometry. Resolve it before deciding whether the full-size frame fits. */
    if (player_screen) lv_obj_update_layout(player_screen);
    bool keep_cover = !player_lyrics_open && player_timeline_cover_frame &&
        player_timeline_cover_can_retain() &&
        player_timeline_cover_frame == player_timeline_cover_cache &&
        player_timeline_cover_cache_generation == current_cover_generation &&
        player_timeline_cover_cache_epoch == player_timeline_cover_style_epoch &&
        player_timeline_cover_source == &current_cover_dsc &&
        lv_image_get_src(cover_img) == player_timeline_cover_frame &&
        lv_obj_get_width(cover_card) == (int32_t) player_timeline_cover_frame->header.w &&
        lv_obj_get_height(cover_card) == (int32_t) player_timeline_cover_frame->header.h;
#ifdef UI_PERF_TRACE
    printf("PERF player_cover_finish retained_binding=%d eligible=%d card_w=%d card_h=%d image_w=%d image_h=%d source_w=%d source_h=%d\n",
           keep_cover, player_timeline_cover_can_retain(),
           cover_card ? lv_obj_get_width(cover_card) : 0, cover_card ? lv_obj_get_height(cover_card) : 0,
           cover_img ? lv_obj_get_width(cover_img) : 0, cover_img ? lv_obj_get_height(cover_img) : 0,
           cover_img ? lv_image_get_src_width(cover_img) : 0, cover_img ? lv_image_get_src_height(cover_img) : 0);
#endif
    if (!keep_cover) player_timeline_cover_snapshot_restore();
    player_lyrics_update_thumbnail_clickable();
    lv_area_t lyrics_rect;
    if (player_lyrics_open && player_lyrics_area_rect(&lyrics_rect)) {
        player_lyrics_show_in_area(&lyrics_rect);
    } else if (player_lyrics_open) {
        for (unsigned i = 0; i < player_lyrics_control_count; ++i) player_lyrics_hide_object(player_lyrics_controls[i]);
        player_lyrics_hide_object(favorite_circle);
        int32_t bottom = 0;
        lv_obj_t * kept[] = { cover_card, song_title_label,
                              player_artist_standin ? NULL : artist_label,
                              player_album_standin ? NULL : album_label };
        for (unsigned i = 0; i < sizeof(kept) / sizeof(kept[0]); ++i) {
            lv_area_t area;
            if (!kept[i]) continue;
            lv_obj_get_coords(kept[i], &area);
            if (area.y2 + 1 > bottom) bottom = area.y2 + 1;
        }
        player_lyrics_content_top = bottom + player_y(16);
        gui_lyrics_show_embedded(player_screen, player_lyrics_content_top,
                                BOARD_SCREEN_HEIGHT - player_lyrics_content_top - player_y(32));
    } else {
        player_lyrics_restore_controls();
    }
    fit_cover_img_to_card();
    player_transition_mark_dirty();
    player_timeline_cover_internal = was_internal;
}

static void player_lyrics_timeline_completed_cb(lv_anim_t * anim) {
    lv_anim_timeline_t * completed = anim ? (lv_anim_timeline_t *)anim->var : NULL;
    lv_anim_timeline_t * expected = player_lyrics_open ? player_timeline_lyrics_open : player_timeline_lyrics_close;
    if (!player_lyrics_animating || !completed || completed != expected) return;
    player_lyrics_timeline_finish();
}

static uint32_t player_lyrics_timeline_fallback_delay(lv_anim_timeline_t * timeline, uint32_t playtime) {
    if (!timeline || playtime == LV_ANIM_PLAYTIME_INFINITE) return LV_ANIM_PLAYTIME_INFINITE;
    uint32_t repeat_count = lv_anim_timeline_get_repeat_count(timeline);
    if (repeat_count == LV_ANIM_REPEAT_INFINITE) return LV_ANIM_PLAYTIME_INFINITE;
    uint32_t repeats = repeat_count ? repeat_count : 1;
    uint64_t total = (uint64_t)playtime * repeats;
    uint32_t repeat_delay = lv_anim_timeline_get_repeat_delay(timeline);
    if (repeats > 1) {
        uint64_t repeat_time = (uint64_t)repeat_delay * (repeats - 1U);
        if (total > UINT64_MAX - repeat_time) return LV_ANIM_PLAYTIME_INFINITE;
        total += repeat_time;
    }
    uint32_t delay = lv_anim_timeline_get_delay(timeline);
    if (total > UINT64_MAX - delay) return LV_ANIM_PLAYTIME_INFINITE;
    total += delay;
    if (total >= LV_ANIM_PLAYTIME_INFINITE) return LV_ANIM_PLAYTIME_INFINITE;
    return (uint32_t)total;
}

static void player_lyrics_timeline_finish_timer_cb(lv_timer_t * timer) {
    lv_anim_timeline_t * completed = lv_timer_get_user_data(timer);
    player_lyrics_timeline_timer = NULL; /* repeat count 1: LVGL deletes it */
    lv_anim_timeline_t * expected = player_lyrics_open ? player_timeline_lyrics_open : player_timeline_lyrics_close;
    if (player_lyrics_animating && completed && completed == expected) player_lyrics_timeline_finish();
}

static void player_lyrics_timeline_start(void) {
    if (!player_screen) return;
    lv_anim_timeline_t * timeline = player_lyrics_open ? player_timeline_lyrics_open : player_timeline_lyrics_close;
    bool completion_attached = false;
    bool runner_available = false;
    uint32_t playtime = player_timeline_play(timeline, player_lyrics_timeline_completed_cb,
                                             &completion_attached, &runner_available);
    if (completion_attached) return;
    if (!runner_available) {
        /* lv_anim_start() can fail to allocate its runner. The style endpoint
         * can still be applied synchronously and must not leave lyrics busy. */
        if (timeline) lv_anim_timeline_set_progress(timeline, LV_ANIM_TIMELINE_PROGRESS_MAX);
        player_lyrics_timeline_finish();
        return;
    }

    /* Preserve a runner's preexisting callback. Only this uncommon conflict
     * needs a timer fallback. Infinite timelines have no completion event. */
    uint32_t finish_delay = player_lyrics_timeline_fallback_delay(timeline, playtime);
    if (finish_delay == LV_ANIM_PLAYTIME_INFINITE) return;
    player_lyrics_timeline_timer = lv_timer_create(player_lyrics_timeline_finish_timer_cb, finish_delay, timeline);
    if (!player_lyrics_timeline_timer) {
        /* The runner may still be live (and may own a preexisting callback).
         * Stop it before jumping to the endpoint and finishing synchronously. */
        lv_anim_timeline_pause(timeline);
        lv_anim_timeline_set_progress(timeline, LV_ANIM_TIMELINE_PROGRESS_MAX);
        player_lyrics_timeline_finish();
        return;
    }
    lv_timer_set_repeat_count(player_lyrics_timeline_timer, 1);
}

static void player_lyrics_timeline_start_timer_cb(lv_timer_t * timer) {
    (void) timer;
    player_lyrics_timeline_timer = NULL; /* repeat count 1: LVGL deletes it */
#ifdef UI_PERF_TRACE
    player_lyrics_perf_stats.runner_start_us = ui_perf_now_us() - player_lyrics_perf_stats.trace_start_us;
    player_lyrics_perf_stats.runner_start_tick = lv_tick_get();
#endif
    player_lyrics_timeline_start();
    player_lyrics_release_start_refresh();
}

static void player_lyrics_set_open_timeline(bool open, bool animate) {
    bool was_internal = player_timeline_cover_internal;
    player_timeline_cover_internal = true;
#ifdef UI_PERF_TRACE
    if (animate && !player_lyrics_generated_timelines) player_lyrics_perf_reset(open, true);
#endif
    player_lyrics_timeline_cancel_timer();
    lv_anim_timeline_pause(player_timeline_lyrics_open);
    lv_anim_timeline_pause(player_timeline_lyrics_close);
    player_timeline_labels_teardown();
    player_lyrics_backdrop_end(false);
    /* A second request may interrupt a running timeline. Release its cached
     * cover before the next timeline rewrites the same image source. */
    player_timeline_cover_snapshot_restore();
    if (!player_lyrics_generated_timelines) player_lyrics_object_count = 0;
    if (open) {
        lv_area_t area;
        if (player_lyrics_area_rect(&area)) {
            int32_t width = lv_area_get_width(&area);
            gui_lyrics_prepare_layout_for_width(width);
            gui_lyrics_prepare_embedded(player_screen, area.x1, area.y1, width,
                                        lv_area_get_height(&area), true);
        } else {
            gui_lyrics_prepare_layout();
            gui_lyrics_prepare_embedded(player_screen, 0, 0, BOARD_SCREEN_WIDTH,
                                        BOARD_SCREEN_HEIGHT, false);
        }
        lv_obj_update_layout(player_screen);
    } else {
        gui_lyrics_hide_embedded();
        /* Authored controls animate back in. Native controls retain their
         * existing hidden-until-complete behavior. */
        if (!player_lyrics_generated_timelines) player_lyrics_restore_controls();
    }
    for (unsigned i = 0; i < player_hit_target_count; ++i) player_lyrics_hide_object(player_hit_targets[i]);
    player_lyrics_open = open;

    lv_anim_timeline_t * timeline = open ? player_timeline_lyrics_open : player_timeline_lyrics_close;
    if (!animate) {
        player_lyrics_animating = false;
        lv_anim_timeline_set_progress(timeline, 0);
        lv_anim_timeline_set_progress(timeline, LV_ANIM_TIMELINE_PROGRESS_MAX);
        player_lyrics_timeline_finish();
        player_timeline_cover_internal = was_internal;
        return;
    }
    player_layouts_timeline_refresh_timing(player_timeline_lyrics_open);
    player_layouts_timeline_refresh_timing(player_timeline_lyrics_close);
    /* A static frosted backdrop can be cached separately from the moving
     * cover. Only a cache containing that cover replaces its snapshot. */
    bool cached_backdrop = player_lyrics_backdrop_begin(open) && player_timeline_backdrop_covers_cover;
    if (!cached_backdrop) {
        bool antialias = lv_image_get_antialias(cover_img);
        player_timeline_cover_snapshot_take(timeline);
        if (player_timeline_cover_frame) {
            player_timeline_cover_antialias = antialias;
            player_timeline_cover_antialias_saved = true;
            lv_image_set_antialias(cover_img, false);
        }
    }
    player_timeline_labels_begin(timeline);
#ifdef UI_PERF_TRACE
    player_lyrics_perf_stats.prep_elapsed_us = ui_perf_now_us() - player_lyrics_perf_stats.trace_start_us;
#endif
    const void * intermediate_source = NULL;
    if (!cached_backdrop && cover_img) {
        const void * source = lv_image_get_src(cover_img);
        if (source == &current_cover_dsc ||
            (player_timeline_cover_cache && source == player_timeline_cover_cache &&
             player_timeline_cover_frame == player_timeline_cover_cache &&
             player_timeline_cover_cache->header.cf == LV_COLOR_FORMAT_RGB565)) intermediate_source = source;
    }
    /* An unsupported transition does not consume this pool. Preserve prior
     * entries until their owning source is invalidated or replaced. */
    if (intermediate_source) lv_draw_sw_image_cache_set_intermediate_source(intermediate_source);
    lv_draw_sw_image_cache_set_transition_active(intermediate_source != NULL);
    player_lyrics_animating = true;
    if (!cached_backdrop) player_lyrics_backdrop_set_busy(true);
    player_lyrics_update_thumbnail_clickable();
    /* Preparation and snapshot rendering can be expensive while LVGL's
     * animation clock is frozen. Start on a fresh tick so that work does not
     * consume the first frame or shorten the visible transition. */
    player_lyrics_timeline_timer = lv_timer_create(player_lyrics_timeline_start_timer_cb, 1, NULL);
    if (!player_lyrics_timeline_timer) {
        lv_anim_timeline_set_progress(timeline, LV_ANIM_TIMELINE_PROGRESS_MAX);
        player_lyrics_timeline_finish();
        player_timeline_cover_internal = was_internal;
        return;
    }
    lv_timer_set_repeat_count(player_lyrics_timeline_timer, 1);
    lv_display_t * display = lv_obj_get_display(player_screen);
    lv_timer_t * refresh = lv_display_get_refr_timer(display);
    if (refresh && display == player_timeline_cover_cache_display &&
        !player_timeline_display_refreshing && gui_display_refresh_allowed() &&
        lv_display_get_screen_active(display) == player_screen) {
        player_lyrics_start_display = display;
        player_lyrics_start_refresh_requested = !lv_timer_get_paused(refresh);
        lv_timer_pause(refresh);
    }
    player_timeline_cover_internal = was_internal;
}

/* Native layouts supply measured geometry; playback, raster proxies and
 * lifecycle are shared with authored property timelines. */
static void player_lyrics_generated_release(bool keep_backdrop) {
    if (!player_lyrics_generated_timelines) return;
    player_timeline_labels_teardown();
    if (keep_backdrop && player_layout_builtin && player_overlay_panel)
        player_lyrics_backdrop_bind(player_overlay_panel, NULL, NULL, NULL, NULL);
    else player_lyrics_backdrop_teardown();
    player_layouts_destroy_style_timeline(player_timeline_lyrics_open);
    player_layouts_destroy_style_timeline(player_timeline_lyrics_close);
    player_timeline_lyrics_open = NULL;
    player_timeline_lyrics_close = NULL;
    player_lyrics_generated_timelines = false;
}

static bool player_lyrics_generated_prepare(bool opening) {
    player_lyrics_generated_release(true);
    if (opening) {
        lv_obj_update_layout(player_screen);
        lv_obj_t * objects[PLAYER_LYRICS_GEOMETRY_OBJECT_COUNT] = { cover_card, song_title_label };
        unsigned count = 2;
        if (artist_label && !player_artist_standin) objects[count++] = artist_label;
        if (album_label && !player_album_standin) objects[count++] = album_label;
        player_lyrics_object_count = count;
        int32_t cover_size = player_s(112);
        int32_t left = player_x(24), top = player_y(48);
        int32_t text_x = left + cover_size + player_x(16), text_y = top;
        for (unsigned i = 0; i < count; ++i) {
            player_lyrics_geometry_t * g = &player_lyrics_geometry[i];
            memset(g, 0, sizeof(*g));
            g->obj = objects[i];
            g->x = lv_obj_get_x(g->obj); g->y = lv_obj_get_y(g->obj);
            g->w = lv_obj_get_width(g->obj); g->h = lv_obj_get_height(g->obj);
            g->height_style = lv_obj_get_style_height(g->obj, 0);
            g->normal_pad_left = lv_obj_get_style_pad_left(g->obj, 0);
            g->translate_x = lv_obj_get_style_translate_x(g->obj, 0);
            g->translate_y = lv_obj_get_style_translate_y(g->obj, 0);
            g->compact_x = i ? text_x : left;
            g->compact_y = i ? text_y : top;
            g->compact_w = i ? BOARD_SCREEN_WIDTH - text_x - left : cover_size;
            g->compact_h = i ? g->h : cover_size;
            if (!player_layout_builtin) {
                lv_area_t area;
                lv_obj_get_coords(g->obj, &area);
                g->compact_x -= area.x1 - g->x;
                g->compact_y -= area.y1 - g->y;
            }
            if (i) { g->long_mode = lv_label_get_long_mode(g->obj); text_y += g->compact_h + player_y(6); }
        }
        player_timeline_cover_w = player_lyrics_geometry[0].w;
        player_timeline_cover_h = player_lyrics_geometry[0].h;
    }
    player_layout_style_transition_t transitions[PLAYER_LYRICS_GEOMETRY_OBJECT_COUNT * 5] = {0};
    uint32_t count = 0;
    for (unsigned i = 0; i < player_lyrics_object_count; ++i) {
        player_lyrics_geometry_t * g = &player_lyrics_geometry[i];
        if (i) {
            lv_point_t text_size;
            lv_text_get_size(&text_size, lv_label_get_text(g->obj), lv_obj_get_style_text_font(g->obj, 0),
                            lv_obj_get_style_text_letter_space(g->obj, 0),
                            lv_obj_get_style_text_line_space(g->obj, 0), LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
            int32_t available = g->w - g->normal_pad_left - lv_obj_get_style_pad_right(g->obj, 0);
            g->centered_text_inset = text_size.x < available ? (available - text_size.x) / 2 : 0;
        }
        const lv_style_prop_t props[] = { LV_STYLE_TRANSLATE_X, LV_STYLE_TRANSLATE_Y,
                                         LV_STYLE_WIDTH, LV_STYLE_HEIGHT,
                                         i ? LV_STYLE_PAD_LEFT : LV_STYLE_RADIUS };
        const int32_t starts[] = { g->translate_x, g->translate_y, g->w, g->h,
                                  i ? g->normal_pad_left + g->centered_text_inset :
                                      lv_obj_get_style_radius(g->obj, 0) };
        const int32_t ends[] = { g->translate_x + g->compact_x - g->x,
                                g->translate_y + g->compact_y - g->y,
                                g->compact_w, g->compact_h,
                                i ? g->normal_pad_left : lv_obj_get_style_radius(g->obj, 0) };
        /* A native card keeps its radius in pixels while it changes size.
         * Declaring that drawing constraint retains the live rounded clipping
         * through the same eligibility rules used by authored timelines. */
        for (unsigned j = 0; j < 5U; ++j) {
            player_layout_style_transition_t * t = &transitions[count++];
            t->target = g->obj; t->selector = LV_PART_MAIN; t->prop = props[j];
            t->start_value = starts[j]; t->end_value = ends[j];
            t->duration = 240; t->path_cb = lv_anim_path_ease_in_out;
            t->animation.early_apply = true;
        }
    }
    lv_anim_timeline_t * open = player_layouts_create_style_timeline(transitions, count);
    for (uint32_t i = 0; i < count; ++i) {
        int32_t start = transitions[i].start_value;
        transitions[i].start_value = transitions[i].end_value; transitions[i].end_value = start;
    }
    lv_anim_timeline_t * close = open ? player_layouts_create_style_timeline(transitions, count) : NULL;
    if (!open || !close) {
        player_layouts_destroy_style_timeline(open);
        player_layouts_destroy_style_timeline(close);
        player_timeline_lyrics_open = NULL;
        player_timeline_lyrics_close = NULL;
        player_lyrics_backdrop_teardown();
        /* Keep the geometry provider active for endpoint/style restoration
         * even when no timeline could be allocated. */
        player_lyrics_generated_timelines = true;
        return false;
    }
    player_timeline_lyrics_open = open;
    player_timeline_lyrics_close = close;
    player_lyrics_generated_timelines = true;
    if (player_layout_builtin && player_overlay_panel)
        player_lyrics_backdrop_bind(player_overlay_panel, NULL, NULL, open, close);
    for (unsigned i = 1; i < player_lyrics_object_count; ++i) {
        player_lyrics_geometry_t * g = &player_lyrics_geometry[i];
        lv_label_set_long_mode(g->obj, LV_LABEL_LONG_CLIP);
        lv_obj_set_height(g->obj, g->h);
        lv_obj_set_style_text_align(g->obj, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_pad_left(g->obj, g->normal_pad_left +
                                 (opening ? g->centered_text_inset : 0), 0);
    }
    return true;
}

static void player_lyrics_native_hide_controls(void) {
    if (player_layout_builtin) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(player_screen); ++i) {
            lv_obj_t * child = lv_obj_get_child(player_screen, i);
            if (child == player_overlay_panel || child == cover_card || child == song_title_label ||
                child == album_label || child == artist_label) continue;
            player_lyrics_hide_object(child);
        }
    } else {
        for (unsigned i = 0; i < player_hit_target_count; ++i) player_lyrics_hide_object(player_hit_targets[i]);
        for (unsigned i = 0; i < player_lyrics_control_count; ++i) player_lyrics_hide_object(player_lyrics_controls[i]);
    }
    player_lyrics_hide_object(favorite_circle);
}

/* Allocation failure still changes view safely, without a second animation
 * implementation. Only endpoint geometry is applied directly. */
static void player_lyrics_generated_endpoint(bool open) {
    for (unsigned i = 0; i < player_lyrics_object_count; ++i) {
        player_lyrics_geometry_t * g = &player_lyrics_geometry[i];
        lv_obj_set_style_translate_x(g->obj, g->translate_x + (open ? g->compact_x - g->x : 0), 0);
        lv_obj_set_style_translate_y(g->obj, g->translate_y + (open ? g->compact_y - g->y : 0), 0);
        lv_obj_set_size(g->obj, open ? g->compact_w : g->w, open ? g->compact_h : g->h);
        if (i) {
            lv_obj_set_style_text_align(g->obj, open ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_pad_left(g->obj, g->normal_pad_left, 0);
        }
    }
    player_lyrics_open = open;
    player_lyrics_timeline_finish();
}

static void player_lyrics_set_open(bool open, bool animate) {
    if (!player_screen || (animate && player_lyrics_animating)) return;
    if (gui_anims_off()) animate = false;
#ifdef UI_PERF_TRACE
    if (!animate && player_lyrics_perf_trace_active)
        player_lyrics_perf_abort(player_lyrics_animating ? "interrupted" : "screen_unloaded");
#endif
    if (open == player_lyrics_open && !player_lyrics_animating) return;
    bool native = player_lyrics_generated_timelines ||
                  (!player_timeline_lyrics_open || !player_timeline_lyrics_close);
    if (native && !player_lyrics_area) {
#ifdef UI_PERF_TRACE
        /* Include responsive geometry and timeline construction in preparation. */
        if (animate) player_lyrics_perf_reset(open, true);
#endif
        player_lyrics_timeline_cancel_timer();
        player_timeline_labels_teardown();
        player_timeline_cover_snapshot_restore();
        bool was_internal = player_timeline_cover_internal;
        player_timeline_cover_internal = true;
        bool ready = player_lyrics_generated_prepare(open);
        if (open) player_lyrics_native_hide_controls();
        else gui_lyrics_hide_embedded();
        player_timeline_cover_internal = was_internal;
        if (!ready) { player_lyrics_generated_endpoint(open); return; }
    }
    if (player_timeline_lyrics_open && player_timeline_lyrics_close)
        player_lyrics_set_open_timeline(open, animate);
    else if (player_lyrics_area)
        player_lyrics_set_open_area(open);
}

static void cover_img_tap_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (player_lyrics_open)
        player_lyrics_set_open(false, true);
    else if (current_settings.lyrics_enabled)
        player_lyrics_set_open(true, true);
}

static void player_lyrics_screen_unloaded_cb(lv_event_t * e) {
    (void) e;
    player_lyrics_set_open(false, false);
}


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

/* Long-press seeking on next/prev buttons (fast-forward/rewind).
 *
 * `transport_seek_target_seconds` accumulates seek offsets persistently
 * so rapid repeat events advance smoothly without re-reading intermediate
 * positions before earlier seeks complete.
 *
 * Seeking triggers on LV_EVENT_LONG_PRESSED and repeats on
 * LV_EVENT_LONG_PRESSED_REPEAT. Target positions are clamped short of EOF
 * by TRANSPORT_SEEK_EOF_GUARD_SECONDS to prevent auto-advancing into the
 * next track while holding.
 *
 * If playback generation changes during a hold (indicating a track
 * transition), `transport_seek_hold_cancelled` stops further seeks until
 * the next press.
 *
 * `dir` is +1 for forward seeking and -1 for rewinding. */
#define TRANSPORT_SEEK_STEP_SECONDS 3.0
#define TRANSPORT_SEEK_EOF_GUARD_SECONDS 0.5

/* Shared math for both the touch long-press repeat below and the physical
 * Next button's own hold (gui_player_hw_next_seek_steps()) -- each input
 * source keeps its own target_seconds/playback_generation/hold_cancelled
 * triple so a hold on one doesn't interfere with a hold on the other. */
static void apply_transport_seek_step(double dir, int step_count, bool is_first, double * target_seconds,
                                       uint64_t * playback_generation, bool * hold_cancelled) {
    if (step_count <= 0) return;
    uint64_t gen = audio_get_playback_generation();
    if (is_first) {
        *target_seconds = audio_get_position_seconds();
        *playback_generation = gen;
        *hold_cancelled = false;
    } else if (gen != *playback_generation) {
        *hold_cancelled = true;
    }
    if (*hold_cancelled) return;
    *target_seconds += dir * TRANSPORT_SEEK_STEP_SECONDS * (double) step_count;
    if (*target_seconds < 0.0) *target_seconds = 0.0;
    double duration = audio_get_duration_seconds();
    double max_target = duration > TRANSPORT_SEEK_EOF_GUARD_SECONDS ? duration - TRANSPORT_SEEK_EOF_GUARD_SECONDS : 0.0;
    if (*target_seconds > max_target) *target_seconds = max_target;
    audio_seek(*target_seconds);
}

static double transport_seek_target_seconds;
static uint64_t transport_seek_playback_generation;
static bool transport_seek_hold_cancelled;

static void transport_seek_repeat_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_LONG_PRESSED && code != LV_EVENT_LONG_PRESSED_REPEAT) return;
    double dir = (double) (intptr_t) lv_event_get_user_data(e);
    apply_transport_seek_step(dir, 1, code == LV_EVENT_LONG_PRESSED, &transport_seek_target_seconds,
                               &transport_seek_playback_generation, &transport_seek_hold_cancelled);
}

/* Separate held targets for each direction retain repeat accumulation while
 * sharing touch seeking's playback-generation and end-of-track guards. */
static double hw_seek_target_seconds[2];
static uint64_t hw_seek_playback_generation[2];
static bool hw_seek_hold_cancelled[2];

void gui_player_hw_seek_steps(int direction, int step_count, bool is_first) {
    if ((direction != 1 && direction != -1) || step_count <= 0) return;
    unsigned slot = direction < 0;
    apply_transport_seek_step((double)direction, step_count, is_first,
                              &hw_seek_target_seconds[slot],
                              &hw_seek_playback_generation[slot],
                              &hw_seek_hold_cancelled[slot]);
}

/* LV_EVENT_CLICKED still fires on release even after a long press (LVGL's
 * own documented behavior -- "regardless of long press"), so a hold-then-
 * release would otherwise ALSO skip/restart the track right after fast-
 * forwarding/rewinding it. Same fix already established in this codebase
 * for the exact same problem (quick_drawer_wifi_long_press_fired/
 * quick_drawer_bt_long_press_fired in gui_shell.c): a flag set here on
 * LV_EVENT_LONG_PRESSED, checked and cleared at the top of the CLICKED
 * handler to skip that one release's normal tap action.
 *
 * Bug caught in review: that flag was previously only ever cleared inside
 * the CLICKED handler -- but a press that ends via LV_EVENT_PRESS_LOST
 * (finger dragged off the object before release) never gets a matching
 * CLICKED at all (confirmed against lv_indev.c: PRESS_LOST and RELEASED/
 * CLICKED are emitted from disjoint code paths, never both for the same
 * press). A long-hold-then-drag-off would leave the flag stuck true
 * forever, silently discarding the very next legitimate short tap on that
 * same button. transport_long_press_cancel_cb() below clears it on
 * PRESS_LOST too so a cancelled hold can't outlive its own press. */
static bool next_btn_long_press_fired = false;
static bool prev_btn_long_press_fired = false;

static void transport_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    bool * fired = (bool *) lv_event_get_user_data(e);
    *fired = true;
}

static void transport_long_press_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_PRESS_LOST) return;
    bool * fired = (bool *) lv_event_get_user_data(e);
    *fired = false;
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
    if (player_lyrics_area && player_lyrics_open) {
        player_lyrics_set_open(false, true);
        return;
    }
    nav_pop();
}

static lv_timer_t * pending_progress_seek_timer = NULL;
/* The actual seek target -- audio.c binds this percentage to the current
 * playback generation and converts it only after that generation's decoder
 * publishes total_frames, so a slow track start cannot reuse the previous
 * track's duration. */
static double pending_progress_seek_percent = 0.0;
/* Best-effort ESTIMATE of where that percent lands, in seconds -- used only
 * by the "has the seek landed yet" poll heuristic below, never passed to
 * audio_seek()/audio_seek_percent() itself. Fine if this is occasionally
 * stale (same track-switch race as above): the only consequence is the
 * progress UI falling back to its normal 8-tick timeout instead of
 * detecting the landed position early, not an incorrect seek. */
static double pending_progress_seek_seconds = 0.0;
bool user_seeking = false;
/* True from finger-up until audio position catches the requested seek, so
 * the 500ms progress poll does not yank the knob back to the pre-seek
 * playhead while the playback thread is applying it. */
static bool progress_awaiting_seek = false;
static int progress_awaiting_seek_ticks = 0;

static void cancel_pending_progress_seek(void) {
    if (pending_progress_seek_timer) {
        lv_timer_delete(pending_progress_seek_timer);
        pending_progress_seek_timer = NULL;
    }
}

static void pending_progress_seek_timer_cb(lv_timer_t * timer) {
    (void) timer;
    pending_progress_seek_timer = NULL;
    audio_seek_percent(pending_progress_seek_percent);
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
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * slider = lv_event_get_target(e);

    if (code == LV_EVENT_PRESSED) {
        user_seeking = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        double duration = audio_get_duration_seconds();
        int32_t percent = player_seekbar_get_value(slider);
        pending_progress_seek_percent = (double) percent;
        pending_progress_seek_seconds = duration * ((double) percent / 100.0);
        displayed_progress_percent = percent;
        progress_awaiting_seek = (duration > 0.0);
        progress_awaiting_seek_ticks = 0;
        cancel_pending_progress_seek();
        pending_progress_seek_timer = lv_timer_create(pending_progress_seek_timer_cb, 150, NULL);
        lv_timer_set_repeat_count(pending_progress_seek_timer, 1);
        user_seeking = false;
    }
}

/* ---- Player layouts: built-in creator and role binder ----
 *
 * The Player screen is assembled in two steps so that its widget tree can come
 * from more than one place. A layout (the built-in creator below, a component
 * exported to C by LVGL's UI Editor, or an LVGL XML file; see
 * player_layouts.h) only creates widgets, geometry and styling and names the
 * ones the app cares about. player_bind_widgets() then finds those widgets by
 * name and attaches everything that is behavior: events, touch targets, the
 * exported globals the rest of the app reads. The role names are the public
 * contract documented in docs/PLAYER_LAYOUTS.md. */

static void player_init_overlay_panel(lv_obj_t * panel) {
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_size(panel, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_add_style(panel, &style_theme_screen_bg, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t * player_create_hidden_label(lv_obj_t * parent) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, "");
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    return label;
}

static lv_obj_t * player_create_hidden_background_img(lv_obj_t * panel) {
    lv_obj_t * img = lv_image_create(panel);
    lv_obj_set_name_static(img, "background_img");
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return img;
}

lv_obj_t * player_layout_create_builtin(lv_obj_t * scr) {
    /* Full-screen fallback surface and background reflection image */
    lv_obj_t * overlay_panel = lv_obj_create(scr);
    lv_obj_set_name_static(overlay_panel, "overlay_panel");
    player_init_overlay_panel(overlay_panel);
    player_create_hidden_background_img(overlay_panel);

    /* Cover card (clipped, crisp, unblurred) */
    lv_obj_t * card = lv_obj_create(scr);
    lv_obj_set_name_static(card, "cover_card");
    lv_obj_remove_style_all(card);
    /* Centered directly from the resolved card size rather than a fixed
     * offset literal -- that would only center a player_s(350) card when
     * player_x and player_s agree, which is true on R1 but not on the other
     * boards. 350x350 matches COVER_ART_WIDTH/HEIGHT (gui.h) exactly, so the
     * reference board needs no runtime scale at all in fit_cover_img_to_card(). */
    lv_obj_set_pos(card, (BOARD_SCREEN_WIDTH - player_s(350)) / 2, player_y(152));
    lv_obj_set_size(card, player_s(350), player_s(350));
    lv_obj_set_style_radius(card, player_s(10), 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_set_style_bg_opa(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * cover = lv_image_create(card);
    lv_obj_set_name_static(cover, "cover_img");
    lv_image_set_src(cover, asset_path("playing_plane/default_cover_565.png"));

    lv_obj_t * fav_circle = lv_obj_create(card);
    lv_obj_set_name_static(fav_circle, "favorite_circle");
    lv_obj_remove_style_all(fav_circle);
    lv_obj_set_pos(fav_circle, player_s(296), player_s(296)); /* 352-44-12=296 from each edge */
    lv_obj_set_size(fav_circle, player_s(44), player_s(44));
    lv_obj_set_style_radius(fav_circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(fav_circle, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(fav_circle, LV_OPA_40, 0);
    lv_obj_add_style(fav_circle, gui_theme_accent_outline_style(), 0);
    lv_obj_remove_flag(fav_circle, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * fav_icon = lv_image_create(fav_circle);
    lv_obj_set_name_static(fav_icon, "favorite_icon");
    lv_image_set_src(fav_icon, asset_path("playing_plane/collect_out.png"));
    player_fit_small_screen_icon(fav_icon, 40);
    lv_obj_add_style(fav_icon, &icon_press_style, LV_STATE_PRESSED);
    lv_obj_center(fav_icon);

    /* The supplied reference is the R1 480x800 composition. Keep the
     * existing compact typography on shorter panels so the three metadata
     * lines do not collide with the cover there. */
    const bool reference_player = BOARD_SCREEN_HEIGHT >= 800;
    const lv_font_t * player_title_font = &app_font_player_title;
    const lv_font_t * player_meta_font = &app_font_player_meta;

    lv_obj_t * title = lv_label_create(scr);
    lv_obj_set_name_static(title, "title");
    lv_label_set_text(title, TR("No track loaded"));
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, player_title_font, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_x(title, player_x(22));
    lv_obj_set_width(title, BOARD_SCREEN_WIDTH - 2 * player_x(22));

    lv_obj_t * album = lv_label_create(scr);
    lv_obj_set_name_static(album, "album");
    lv_label_set_text(album, "");
    lv_obj_add_style(album, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(album, player_meta_font, 0);
    lv_obj_set_style_text_align(album, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_x(album, player_x(22));
    /* Content height so circular marquee does not fall back to vertical scrolling. */
    lv_obj_set_width(album, BOARD_SCREEN_WIDTH - 2 * player_x(22));
    lv_obj_set_height(album, LV_SIZE_CONTENT);

    lv_obj_t * artist = lv_label_create(scr);
    lv_obj_set_name_static(artist, "artist");
    lv_label_set_text(artist, "");
    lv_obj_add_style(artist, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(artist, player_meta_font, 0);
    lv_obj_set_style_text_align(artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_x(artist, player_x(22));
    /* Circular marquee falls back to vertical scrolling when text exceeds
     * a fixed height. Fit the actual font metrics (including loaded fallback
     * fonts) while retaining the fixed width for horizontal overflow. */
    lv_obj_set_width(artist, BOARD_SCREEN_WIDTH - 2 * player_x(22));
    lv_obj_set_height(artist, LV_SIZE_CONTENT);

    /* Back/dismiss button. build_header_back_button()'s shared 64x64 default
     * (TITLE_ROW_HEIGHT, used by every other screen's header) would reach
     * further into the album label's row than the symmetric 44x44 "more"
     * target on the opposite corner -- shrink to match it instead of
     * needlessly widening this corner's overlap with the metadata block. */
    lv_obj_t * dismiss = build_header_back_button(scr, NULL);
    lv_obj_set_name_static(dismiss, "dismiss_btn");
    lv_obj_set_size(dismiss, player_s(44), player_s(44));
    lv_obj_set_pos(dismiss, player_x(22), player_y(62));
    /* The reference keeps the status bar unobstructed and has no visible
     * back arrow on the Player. Retain the transparent hit target so the
     * established tap-to-dismiss behavior remains available. */
    lv_obj_t * dismiss_arrow = lv_obj_get_child(dismiss, 0);
    if (dismiss_arrow) lv_obj_add_flag(dismiss_arrow, LV_OBJ_FLAG_HIDDEN);

    /* Quality pill */
    lv_obj_t * pill = lv_obj_create(scr);
    lv_obj_set_name_static(pill, "quality_pill");
    lv_obj_remove_style_all(pill);
    /* This label is intentionally not marquee-animated: the pill grows to
     * the complete, short format string and therefore never clips codec,
     * bit-depth, or sample-rate information. */
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(pill, player_s(36), 0);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, player_y(518));
    lv_obj_set_style_pad_hor(pill, player_s(14), 0);
    lv_obj_set_style_pad_ver(pill, player_s(6), 0);
    lv_obj_set_style_pad_column(pill, player_s(10), 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x000000), 0);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * quality_waveform = lv_image_create(pill);
    lv_image_set_src(quality_waveform, asset_path("playing_plane/quality_waveform.png"));
    lv_image_set_scale(quality_waveform, (player_s(20) * LV_SCALE_NONE) / 20);
    lv_obj_set_size(quality_waveform, player_s(20), player_s(20));
    lv_obj_add_style(quality_waveform, gui_theme_accent_style(), 0);
    lv_obj_set_style_bg_opa(quality_waveform, LV_OPA_TRANSP, 0);
    lv_obj_set_style_image_recolor_opa(quality_waveform, LV_OPA_COVER, 0);

    lv_obj_t * format_badge = lv_label_create(pill);
    lv_obj_set_name_static(format_badge, "format_badge");
    lv_label_set_text(format_badge, "");
    lv_obj_add_style(format_badge, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(format_badge, &app_font_16, 0);
    lv_obj_set_width(format_badge, LV_SIZE_CONTENT);

    /* Progress bar (native rail, not the old fixed PNG sprites) */
    lv_obj_t * slider = lv_slider_create(scr);
    lv_obj_set_name_static(slider, "progress_slider");
    lv_obj_set_pos(slider, player_x(30), player_y(575));
    lv_obj_set_size(slider, player_x(420), SLIDER_TRACK_HEIGHT);
    configure_native_slider_rail(slider);
    lv_obj_add_style(slider, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(slider, gui_theme_accent_knob_style(), LV_PART_KNOB);
    lv_obj_set_style_width(slider, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_set_style_height(slider, SLIDER_KNOB_SIZE, LV_PART_KNOB);

    /* Time row */
    lv_obj_t * time_row = lv_obj_create(scr);
    lv_obj_set_pos(time_row, player_x(30), player_y(610));
    lv_obj_set_size(time_row, player_x(420), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(time_row, 0, 0);
    lv_obj_set_style_border_width(time_row, 0, 0);
    lv_obj_set_style_pad_all(time_row, 0, 0);
    lv_obj_remove_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * pos = lv_label_create(time_row);
    lv_obj_set_name_static(pos, "pos_label");
    lv_label_set_text(pos, "0:00");
    lv_obj_add_style(pos, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(pos, player_meta_font, 0);

    lv_obj_t * dur = lv_label_create(time_row);
    lv_obj_set_name_static(dur, "dur_label");
    lv_label_set_text(dur, "--:--");
    lv_obj_add_style(dur, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(dur, player_meta_font, 0);

    /* Current position in the active queue, overlaid on the cover. */
    lv_obj_t * count = lv_label_create(card);
    lv_obj_set_name_static(count, "song_count");
    lv_label_set_text(count, "--/--");
    lv_obj_add_style(count, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(count, &app_font_16, 0);
    lv_obj_set_style_bg_color(count, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(count, LV_OPA_50, 0);
    lv_obj_set_style_radius(count, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(count, player_s(9), 0);
    lv_obj_set_style_pad_ver(count, player_s(5), 0);
    lv_obj_set_pos(count, player_s(10), player_s(10));
    lv_obj_remove_flag(count, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(count, LV_OBJ_FLAG_HIDDEN);

    /* Transport row: order, previous, play/pause, next, more. Cover taps are
     * the sole lyrics actuator, leaving the final slot for song actions. */
    lv_obj_t * controls_row = lv_obj_create(scr);
    lv_obj_remove_style_all(controls_row);
    const int32_t transport_top = player_y(645);
    lv_obj_set_pos(controls_row, 0, transport_top);
    lv_obj_set_size(controls_row, BOARD_SCREEN_WIDTH, player_y(94));
    lv_obj_remove_flag(controls_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * order = lv_image_create(controls_row);
    lv_obj_set_name_static(order, "order_btn");
    lv_image_set_src(order, asset_path(play_mode_icon_asset((play_mode_t) current_settings.play_mode)));
    player_fit_small_screen_icon(order, 40);
    lv_obj_add_style(order, &icon_press_style, LV_STATE_PRESSED);

    lv_obj_t * prev = lv_image_create(controls_row);
    lv_obj_set_name_static(prev, "prev_btn");
    lv_image_set_src(prev, asset_path("playing_plane/btn_prev.png"));
    player_fit_small_screen_icon(prev, 40);

    /* btn_play.png/btn_pause.png already contain their own filled accent
     * circle -- no separate outline ring needed here, unlike favorite_circle/
     * quality_pill which wrap plain glyphs/text with no ring of their own. */
    lv_obj_t * play = lv_image_create(controls_row);
    lv_obj_set_name_static(play, "play_btn");
    load_play_btn_images();
    lv_image_set_src(play, gui_player_play_btn_image_src(audio_is_playing()));
    player_fit_small_screen_icon(play, 84);
    lv_obj_add_style(play, &icon_press_style, LV_STATE_PRESSED);

    lv_obj_t * more = lv_image_create(controls_row);
    lv_obj_set_name_static(more, "more_btn");
    lv_image_set_src(more, asset_path("playing_plane/ic_more.png"));
    player_fit_small_screen_icon(more, 40);
    lv_obj_add_style(more, &icon_press_style, LV_STATE_PRESSED);

    lv_obj_t * next = lv_image_create(controls_row);
    lv_obj_set_name_static(next, "next_btn");
    lv_image_set_src(next, asset_path("playing_plane/btn_next.png"));
    player_fit_small_screen_icon(next, 40);

    lv_obj_update_layout(controls_row);
    int32_t row_h = lv_obj_get_height(controls_row);
    lv_obj_set_pos(play, player_x(240) - lv_obj_get_width(play) / 2, (row_h - lv_obj_get_height(play)) / 2);
    lv_obj_set_pos(order, player_x(57) - lv_obj_get_width(order) / 2, (row_h - lv_obj_get_height(order)) / 2);
    lv_obj_set_pos(prev, player_x(142) - lv_obj_get_width(prev) / 2, (row_h - lv_obj_get_height(prev)) / 2);
    lv_obj_set_pos(more, player_x(423) - lv_obj_get_width(more) / 2, (row_h - lv_obj_get_height(more)) / 2);
    lv_obj_set_pos(next, player_x(338) - lv_obj_get_width(next) / 2, (row_h - lv_obj_get_height(next)) / 2);

    lv_obj_t * volume = lv_slider_create(scr);
    lv_obj_set_name_static(volume, "volume_slider");
    lv_obj_add_flag(volume, LV_OBJ_FLAG_HIDDEN);

    return scr;
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
    if (player_hit_target_count < PLAYER_MAX_HIT_TARGETS) player_hit_targets[player_hit_target_count++] = hit;
    return hit;
}

static void player_add_lyrics_control(lv_obj_t * obj) {
    if (obj && player_lyrics_control_count < PLAYER_MAX_LYRICS_CONTROLS)
        player_lyrics_controls[player_lyrics_control_count++] = obj;
}

static void player_screen_loaded_cb(lv_event_t * e) {
    (void) e;
    player_timeline_play(player_timeline_screen_enter, NULL, NULL, NULL);
}

/* Wires a screen built by a layout to the app. Roles and their contract:
 * docs/PLAYER_LAYOUTS.md. Returns false, with no global assigned and nothing
 * attached, when a required role is missing or of the wrong type, so the
 * caller can discard the tree and fall back to the built-in layout. */
static bool player_bind_widgets(lv_obj_t * scr) {
    const bool builtin = player_layout_builtin;

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
    player_timeline_cover_fade = r_cover_fade &&
        lv_obj_get_parent(r_cover_fade) == r_cover_card ? r_cover_fade : NULL;
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
    if (r_artist) {
        lv_obj_add_flag(r_artist, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r_artist, player_now_playing_group_click_cb,
                            LV_EVENT_CLICKED, (void *) (intptr_t) 0);
    }
    if (r_album) {
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
    player_lyrics_area = r_lyrics_area;
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
        lyrics_look.on_empty_tap = player_lyrics_area_empty_tap;
    }
    if (r_lyrics_active) {
        lyrics_look.has_active = true;
        lyrics_look.active = lv_obj_get_style_text_color(r_lyrics_active, LV_PART_MAIN);
    }
    gui_lyrics_set_look(&lyrics_look);
    if (r_lyrics_toggle) {
        lv_obj_add_flag(r_lyrics_toggle, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r_lyrics_toggle, cover_img_tap_cb, LV_EVENT_CLICKED, NULL);
    }
    if (player_cover_thumbnail) {
        lv_obj_add_event_cb(player_cover_thumbnail, cover_img_tap_cb, LV_EVENT_CLICKED, NULL);
        player_lyrics_update_thumbnail_clickable();
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
            if (primary[i]) lv_obj_add_style(primary[i], &style_theme_text_primary, 0);
        for (unsigned i = 0; i < sizeof(muted) / sizeof(muted[0]); ++i)
            if (muted[i]) lv_obj_add_style(muted[i], &style_theme_text_muted, 0);
        lv_obj_add_style(r_progress, gui_theme_accent_style(), LV_PART_INDICATOR);
        lv_obj_add_style(r_progress, gui_theme_accent_knob_style(), LV_PART_KNOB);
        if (r_fav_circle) lv_obj_add_style(r_fav_circle, gui_theme_accent_outline_style(), 0);
        lv_obj_t * pressable[] = { r_order, r_play, r_more, r_fav_icon };
        for (unsigned i = 0; i < sizeof(pressable) / sizeof(pressable[0]); ++i)
            if (pressable[i]) lv_obj_add_style(pressable[i], &icon_press_style, LV_STATE_PRESSED);

        /* The built-in creator sets these while it builds, because it sizes
         * its transport row from them. */
        load_play_btn_images();
        lv_image_set_src(r_play, gui_player_play_btn_image_src(audio_is_playing()));
        if (r_order) lv_image_set_src(r_order, asset_path(play_mode_icon_asset((play_mode_t) current_settings.play_mode)));
        if (!lv_image_get_src(r_cover_img)) lv_image_set_src(r_cover_img, asset_path("playing_plane/default_cover_565.png"));
    }

    lv_obj_add_flag(r_cover_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r_cover_img, cover_img_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(r_cover_card, player_cover_size_changed_cb, LV_EVENT_SIZE_CHANGED, NULL);
    if (r_cover_fade) {
        lv_obj_update_layout(scr);
        player_timeline_cover_w = lv_obj_get_width(r_cover_card);
        player_timeline_cover_h = lv_obj_get_height(r_cover_card);
        lv_color_t fade_color = lv_obj_get_style_bg_color(player_layout_root, LV_PART_MAIN);
        if (!player_cover_fade_attach(r_cover_fade, fade_color)) {
            /* If the mask cannot be allocated, preserve metadata contrast. */
            lv_obj_set_style_bg_color(r_cover_fade, fade_color, 0);
            lv_obj_set_style_bg_opa(r_cover_fade, LV_OPA_60, 0);
        }
    } else {
        lv_obj_update_layout(scr);
        player_timeline_cover_w = lv_obj_get_width(r_cover_card);
        player_timeline_cover_h = lv_obj_get_height(r_cover_card);
    }


    lv_obj_t * cached_cover_objects[] = { r_cover_card, r_cover_img };
    for (unsigned i = 0; i < sizeof(cached_cover_objects) / sizeof(cached_cover_objects[0]); ++i) {
        if (!cached_cover_objects[i]) continue;
        lv_obj_add_event_cb(cached_cover_objects[i], player_timeline_cover_style_changed_cb,
                            LV_EVENT_STYLE_CHANGED, NULL);
        lv_obj_add_event_cb(cached_cover_objects[i], player_timeline_cover_style_changed_cb,
                            LV_EVENT_SIZE_CHANGED, NULL);
    }
    lv_display_t * cache_display = lv_obj_get_display(scr);
    if (cache_display != player_timeline_cover_cache_display) {
        if (player_timeline_cover_cache_display)
            lv_display_remove_event_cb_with_user_data(player_timeline_cover_cache_display,
                                                      player_timeline_cover_cache_refresh_cb, NULL);
        lv_display_add_event_cb(cache_display, player_timeline_cover_cache_refresh_cb, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(cache_display, player_timeline_cover_cache_refresh_cb, LV_EVENT_REFR_READY, NULL);
        lv_display_add_event_cb(cache_display, player_timeline_cover_cache_refresh_cb, LV_EVENT_REFR_REQUEST, NULL);
        lv_display_add_event_cb(cache_display, player_timeline_cover_cache_refresh_cb, LV_EVENT_INVALIDATE_AREA, NULL);
        lv_display_add_event_cb(cache_display, player_timeline_cover_cache_refresh_cb, LV_EVENT_DELETE, NULL);
        player_timeline_cover_cache_display = cache_display;
    }

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
    lv_slider_set_value(r_volume, (int32_t) (audio_get_volume() * 100.0f), LV_ANIM_OFF);

    /* Part D: Hit targets */
    lv_obj_update_layout(scr);
    player_hit_target_count = 0;

    int32_t transport_center_y = 0;
    if (builtin) transport_center_y = player_y(645) + lv_obj_get_height(lv_obj_get_parent(r_play)) / 2;

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
        lv_obj_add_event_cb(prev_hit, transport_long_press_cb, LV_EVENT_LONG_PRESSED, &prev_btn_long_press_fired);
        lv_obj_add_event_cb(prev_hit, transport_long_press_cancel_cb, LV_EVENT_PRESS_LOST, &prev_btn_long_press_fired);
        lv_obj_add_event_cb(prev_hit, transport_seek_repeat_cb, LV_EVENT_LONG_PRESSED, (void *) (intptr_t) -1);
        lv_obj_add_event_cb(prev_hit, transport_seek_repeat_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *) (intptr_t) -1);
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
        lv_obj_add_event_cb(next_hit, transport_long_press_cb, LV_EVENT_LONG_PRESSED, &next_btn_long_press_fired);
        lv_obj_add_event_cb(next_hit, transport_long_press_cancel_cb, LV_EVENT_PRESS_LOST, &next_btn_long_press_fired);
        lv_obj_add_event_cb(next_hit, transport_seek_repeat_cb, LV_EVENT_LONG_PRESSED, (void *) (intptr_t) 1);
        lv_obj_add_event_cb(next_hit, transport_seek_repeat_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *) (intptr_t) 1);
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
        if (player_hit_target_count < PLAYER_MAX_HIT_TARGETS) player_hit_targets[player_hit_target_count++] = favorite_hit;
#ifdef UI_HITBOX_DEBUG
        lv_obj_set_style_border_width(favorite_hit, 3, 0);
        lv_obj_set_style_border_color(favorite_hit, lv_palette_main(LV_PALETTE_PINK), 0);
        lv_obj_set_style_border_opa(favorite_hit, LV_OPA_COVER, 0);
#endif
    }

    /* Controls the lyrics view hides, for layouts whose tree is not a flat
     * list of the screen's children. Never a container of the cover or the
     * metadata, which stay visible. */
    player_lyrics_control_count = 0;
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
        player_timeline_lyrics_open = player_layouts_find_timeline(player_layout_root, "lyrics_open");
        player_timeline_lyrics_close = player_layouts_find_timeline(player_layout_root, "lyrics_close");
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
    player_timeline_backdrop_covers_cover = backdrop && player_obj_is_ancestor_of(backdrop, r_cover_card);
    player_lyrics_backdrop_bind(backdrop, r_lyrics_dim, r_lyrics_area,
                                player_timeline_lyrics_open, player_timeline_lyrics_close);

    finalize_screen_navigation(scr);
    lv_obj_add_event_cb(scr, player_lyrics_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    fit_cover_img_to_card();
    return true;
}

/* Clears every widget global and cached pointer derived from the tree. Used
 * by teardown and when a layout is discarded. */
static void player_reset_widget_globals(void) {
    lv_draw_sw_image_cache_set_transition_active(false);
    lv_draw_sw_image_cache_set_intermediate_source(NULL);
#ifdef UI_PERF_TRACE
    player_lyrics_perf_abort("screen_unloaded");
#endif
    player_timeline_cover_cache_invalidate();
    player_lyrics_open = false;
    player_lyrics_animating = false;
    player_timeline_cover_antialias_saved = false;
    player_timeline_cover_fade = NULL;
    player_timeline_cover_w = 0;
    player_timeline_cover_h = 0;
    player_timeline_backdrop_covers_cover = false;
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
    player_timeline_lyrics_open = NULL;
    player_timeline_lyrics_close = NULL;
    player_timeline_track_change = NULL;
    player_timeline_screen_enter = NULL;
    player_hit_target_count = 0;
    player_lyrics_control_count = 0;
    player_lyrics_area = NULL;
    player_cover_thumbnail = NULL;
    player_play_circle = NULL;
    gui_lyrics_set_look(NULL);
    player_artist_standin = false;
    player_album_standin = false;
    player_lyrics_object_count = 0;
    player_lyrics_hidden_count = 0;
}

static lv_obj_t * build_player_screen(uint32_t screen_width, uint32_t screen_height) {
    (void) screen_width;
    (void) screen_height;
#ifdef UI_PERF_TRACE
    player_lyrics_perf_register_display_events();
#endif
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    player_layouts_init(player_layout_create_builtin);
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
            player_timeline_cover_cache_invalidate();
            lv_obj_clean(scr);
            player_reset_widget_globals();
            player_layouts_release();
        }
    }
    if (!built) {
        player_layout_builtin = true;
        player_layout_root = NULL;
        player_layout_create_builtin(scr);
        if (!player_bind_widgets(scr)) {
            lv_obj_delete(scr);
            return NULL;
        }
    }
    return scr;
}



void show_volume_popup(int32_t percent) {
    if (!volume_popup || !volume_popup_track) return;
    /* A BT/remote/hw-button echo writing the slider under a live finger
     * snaps the knob back to the last applied value, then the next indev
     * sample jumps it forward again. */
    bool dragged = lv_slider_is_dragged(volume_popup_track);
    if (!dragged) {
        lv_slider_set_value(volume_popup_track, percent, LV_ANIM_OFF);
        if (volume_popup_value_label) lv_label_set_text_fmt(volume_popup_value_label, "%d", (int) percent);
    }
    lv_obj_remove_flag(volume_popup, LV_OBJ_FLAG_HIDDEN);
    if (volume_popup_hide_timer && !dragged) {
        lv_timer_reset(volume_popup_hide_timer);
        lv_timer_resume(volume_popup_hide_timer);
    }
}

static const char * format_codec_name(audio_codec_t codec) {
    switch (codec) {
        case AUDIO_CODEC_FLAC: return "FLAC";
        case AUDIO_CODEC_MP3: return "MP3";
        case AUDIO_CODEC_PCM: return "PCM";
        case AUDIO_CODEC_DSD: return "DSD";
        case AUDIO_CODEC_AAC: return "AAC";
        case AUDIO_CODEC_ALAC: return "ALAC";
        case AUDIO_CODEC_APE: return "APE";
        case AUDIO_CODEC_WMA: return "WMA";
        case AUDIO_CODEC_OPUS: return "Opus";
        case AUDIO_CODEC_VORBIS: return "Vorbis";
        case AUDIO_CODEC_WAVPACK: return "WavPack";
        case AUDIO_CODEC_UNKNOWN: break;
    }
    return "";
}

/* A stream's URL is not a format label. Only known catalog/local hint
 * values may supply a fallback while its decoder is still opening. */
static const char * stream_format_hint_name(const char * hint) {
    const char * name = format_codec_name(info_codec_from_hint(hint));
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
    const char * path = playlist_index >= 0 ? playlist_path_at(playlist_index) : NULL;
    if (!path || !path[0]) {
        if (format_badge_label) lv_label_set_text(format_badge_label, "");
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
    if (is_http_url(path) || is_remote_track) {
        const char * name = format_codec_name(codec);
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
    if (format_badge_label && strcmp(lv_label_get_text(format_badge_label), text) != 0)
        lv_label_set_text(format_badge_label, text);
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
    if (playlist_count <= 0 || playlist_index < 0 || playlist_index >= playlist_count ||
        !now_playing_path[0]) return;
    /* Metadata remains visible in the lyrics view, but its cover/metadata
     * area belongs to lyrics navigation while that view is open or moving. */
    if (player_lyrics_open || player_lyrics_animating ||
        gui_navigation_transition_in_progress()) return;
    /* Resolve against the current path at tap time. The library helper first
     * confirms that path is a local indexed song, so remote display metadata
     * cannot accidentally match a similarly named local group. */
    bool album = (intptr_t) lv_event_get_user_data(e) != 0;
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


/* Shuffle "bag": a permutation of 0..playlist_count-1 walked front-to-back
 * rather than picking a fresh random index every time, so every track plays
 * exactly once before any repeats -- a bare `rand() % count` on every
 * advance can (and eventually will) replay the same track twice in a row or
 * leave others unplayed for a long stretch, which reads as broken shuffle
 * rather than random. Regenerated (and reshuffled) whenever it's stale --
 * see ensure_shuffle_order_current(). */
static int * shuffle_order = NULL;
static int shuffle_order_count = 0; /* playlist_count this bag was generated for -- staleness check */
static int shuffle_pos = -1;        /* index into shuffle_order such that shuffle_order[shuffle_pos] == playlist_index */

/* Precomputed reshuffled continuation bag for the shuffle-wrap case.
 * Invalidated on playlist change by ensure_shuffle_order_current(). */
static int * pending_shuffle_order = NULL;
static uint64_t queue_revision = 1;




/* Shared style object driving the app-wide accent color (sliders, checked
 * switches, the selected EQ band) -- one lv_style_t whose bg_color gets
 * updated in place whenever the user picks a new color, so every widget
 * that has this style attached re-renders automatically without having to
 * walk and restyle each one individually. */
/* style_accent defined in gui_theme.c */
/* Plain light-gray text style for the *unselected* EQ band labels -- kept
 * as its own shared style (rather than a one-off lv_obj_set_style_text_color)
 * so toggling selection is just swapping which shared style is attached,
 * with no risk of a local per-object style override taking priority over
 * style_accent and silently defeating the highlight. */
/* accent colors and styles moved to gui_theme.c */

/* Generic back-stack, replacing the old pairwise hardcoded back targets
 * (settings always -> browser, eq always -> settings). Every screen's back
 * button and the left-to-right swipe gesture just call nav_pop(); forward
 * navigation calls nav_push(). Root (gui_shell_get_home_screen()) is seeded once in
 * gui_init() and is never popped past. */
#define NAV_STACK_MAX 16

/* Navigation stack and transitions moved to gui_navigation.c */

/* Persistent status bar (clock + battery/wifi icons), built once on LVGL's
 * top layer rather than duplicated into every build_XXX_screen() -- objects
 * on the top layer are drawn over whichever screen is currently active, on
 * every screen, automatically. Left non-clickable throughout so touches
 * still fall through to the real screen underneath (confirmed by reading
 * lv_indev.c's search order: layer_top is checked before the active screen,
 * but only objects with LV_OBJ_FLAG_CLICKABLE ever hit-test positive, so a
 * plain informational bar like this never intercepts anything). Battery
 * percentage (battery_get_percent()), wifi connection state
 * (wifi_get_status()), and the clock are all real, live data -- the
 * battery reads -1 on host (no /sys/class/power_supply there), in which
 * case the label is just left blank and only the plain icon shows; wifi
 * always reads disconnected on host too, since there's no wlan0
 * wpa_supplicant instance to query there, same honest "no data" treatment
 * either way. Battery percentage is rendered the same sprite-digit way as
 * the clock/volume readouts (see battery_topbar_group below), not an
 * lv_label, for the same size/style match. */
/* Status bar and Quick Drawer moved to gui_shell.c */


void reset_decoder_failure_tracking(void) {
    consecutive_decoder_failure_skips = 0;
    failed_physical_paths_count = 0;
}

static bool is_failed_physical_path(const char * path) {
    if (!path || !path[0]) return false;
    for (int i = 0; i < failed_physical_paths_count; i++) {
        if (strcmp(failed_physical_paths[i], path) == 0) return true;
    }
    return false;
}

static void record_failed_physical_path(const char * path) {
    if (!path || !path[0]) return;
    if (is_failed_physical_path(path)) return;
    if (failed_physical_paths_count < MAX_FAILED_PHYSICAL_PATHS) {
        size_t len = strlen(path);
        if (len < sizeof(failed_physical_paths[failed_physical_paths_count])) {
            memcpy(failed_physical_paths[failed_physical_paths_count], path, len + 1);
            failed_physical_paths_count++;
        }
    }
}

void free_playlist(void) {
    ++metadata_ui_token;
    ++metadata_next_token;
    ++favorite_ui_revision;
    queue_revision++;
    queued_pending_count = 0;
    queue_next_insert_index = -1;
    free(shuffle_order); shuffle_order = NULL; shuffle_order_count = 0; shuffle_pos = -1;
    free(pending_shuffle_order); pending_shuffle_order = NULL;
    reset_decoder_failure_tracking();
    current_playback_generation = 0;
    for (int i = 0; i < playlist_count; i++) free(playlist[i]); /* free(NULL) (an unresolved lazy slot) is a safe no-op */
    free(playlist);
    playlist = NULL;
    playlist_count = 0;
    playlist_index = -1;
    if (queue_sd_dirfd >= 0) { close(queue_sd_dirfd); queue_sd_dirfd = -1; }
    free(playlist_lazy_sort_order);
    playlist_lazy_sort_order = NULL;
    if (playlist_lazy_snapshot) metadata_db_snapshot_close(playlist_lazy_snapshot);
    playlist_lazy_snapshot = NULL;
    if (playlist_lazy_file_index) file_browser_index_close(playlist_lazy_file_index);
    playlist_lazy_file_index = NULL;
}

static void queue_pin_sd_dir_if_needed(void) {
    if (queue_sd_dirfd >= 0) return;
    for (int i = 0; i < playlist_count; i++) {
        const char * p = playlist[i];
        if (p && is_sd_card_path(p)) {
            queue_sd_dirfd = open_sd_queue_dir();
            return;
        }
    }
}

/* Defined further down, with the rest of the lazy-All-Songs-queue
 * machinery -- forward-declared here since every reader of playlist[]'s
 * actual STRING CONTENT
 * between here and there (favorite toggle, gapless preload, play_track_at_
 * from, ...) must resolve through it rather than indexing playlist[]
 * directly, or a lazy All-Songs queue's unresolved NULL slots would crash
 * them. See playlist_lazy_sort_order's own comment for the full picture. */

const char * play_mode_icon_asset(play_mode_t mode) {
    switch (mode) {
        case PLAY_MODE_REPEAT_ALL: return "playing_plane/loop.png";
        case PLAY_MODE_REPEAT_ONE: return "playing_plane/single.png";
        case PLAY_MODE_SHUFFLE:    return "playing_plane/random.png";
        default:                   return "playing_plane/order.png";
    }
}

/* Fisher-Yates shuffle of a fresh 0..count-1 permutation. */
static void fisher_yates_shuffle(int * arr, int count) {
    for (int i = count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

/* (Re)generates shuffle_order if it's stale (playlist size changed, or it
 * was never generated) and points shuffle_pos at wherever the currently
 * playing track landed in the new order -- switching into shuffle mode, or
 * the playlist changing size mid-shuffle, should never itself jump away
 * from whatever's already playing. */
static void ensure_shuffle_order_current(void) {
    if (shuffle_order && shuffle_order_count == playlist_count) return;

    static bool rng_seeded = false;
    if (!rng_seeded) {
        srand((unsigned int) time(NULL));
        rng_seeded = true;
    }

    free(shuffle_order);
    shuffle_order = malloc(sizeof(int) * (size_t) playlist_count);
    if (!shuffle_order) { shuffle_order_count = 0; shuffle_pos = -1; return; }
    for (int i = 0; i < playlist_count; i++) shuffle_order[i] = i;
    fisher_yates_shuffle(shuffle_order, playlist_count);
    shuffle_order_count = playlist_count;

    /* A pending wrap-continuation order (see compute_auto_advance_index()'s
     * own comment) sized for whatever playlist_count was true when it was
     * generated is no longer valid once the playlist itself has changed --
     * without this, a stale pending_shuffle_order could later get promoted
     * by commit_auto_advance() and paired with the NEW (different)
     * playlist_count, a real out-of-bounds read the first time shuffle_pos
     * advances past the old, smaller array's real size. */
    free(pending_shuffle_order);
    pending_shuffle_order = NULL;

    shuffle_pos = 0;
    if (playlist_index >= 0) {
        for (int i = 0; i < playlist_count; i++) {
            if (shuffle_order[i] == playlist_index) {
                int first = shuffle_order[0];
                shuffle_order[0] = shuffle_order[i];
                shuffle_order[i] = first;
                shuffle_pos = 0;
                break;
            }
        }
    }
    /* Explicit additions must occupy the next slots in a fresh bag too. */
    for (int n = 0; n < queued_pending_count; n++) {
        int physical = playlist_index + 1 + n;
        int found = 0;
        while (found < playlist_count && shuffle_order[found] != physical) found++;
        if (found == playlist_count) continue;
        memmove(shuffle_order + found, shuffle_order + found + 1, (size_t) (playlist_count - found - 1) * sizeof(int));
        if (found <= shuffle_pos) shuffle_pos--;
        int at = shuffle_pos + 1 + n;
        memmove(shuffle_order + at + 1, shuffle_order + at, (size_t) (playlist_count - at - 1) * sizeof(int));
        shuffle_order[at] = physical;
    }
}

static void queue_shuffle_insert(int physical, int added) {
    int old_count = playlist_count - added;
    if (shuffle_order && shuffle_order_count == old_count) {
        int * grown = realloc(shuffle_order, (size_t) playlist_count * sizeof(int));
        if (grown) {
            shuffle_order = grown;
            int at = shuffle_pos + 1 + physical - playlist_index - 1;
            if (at < 0) at = 0;
            if (at > old_count) at = old_count;
            playback_order_insert(shuffle_order, old_count, physical, at, added);
            shuffle_order_count = playlist_count;
        } else { free(shuffle_order); shuffle_order = NULL; shuffle_order_count = 0; }
    }
    free(pending_shuffle_order); pending_shuffle_order = NULL;
}

static void queue_shuffle_remove(int physical) {
    playback_order_note_removed(shuffle_order, &shuffle_order_count, &shuffle_pos, physical, playlist_count);
    free(pending_shuffle_order); pending_shuffle_order = NULL;
}


/* What track to move to when the current one finishes naturally (auto-
 * advance) -- as opposed to compute_manual_step_index() below, for an
 * explicit Prev/Next tap. Returns -1 for "stop, end of queue" (only
 * possible in Sequential mode).
 *
 * Deliberately read-only (never mutates shuffle_pos/shuffle_order) --
 * arm_next_track_for_audio() calls this speculatively, well before the
 * transition it's asking about is actually confirmed (gapless preload/
 * crossfade prep), so it must be safe to call more than once for the same
 * `index` and always get the same answer. commit_auto_advance() below is
 * the only thing allowed to actually advance the shuffle state, called
 * exactly once at the point a transition is confirmed real. */
int compute_auto_advance_index(int index) {
    if (index < 0 || playlist_count <= 0) return -1;

    /* A queued song always plays next, ahead of play_mode entirely (Repeat
     * One included -- queueing something is an explicit override of
     * whatever's currently looping) -- see queued_pending_count's own
     * comment. */
    if (queued_pending_count > 0 && index + 1 < playlist_count) return index + 1;

    switch ((play_mode_t) current_settings.play_mode) {
        case PLAY_MODE_REPEAT_ONE:
            return index;
        case PLAY_MODE_REPEAT_ALL:
            return (index + 1) % playlist_count;
        case PLAY_MODE_SHUFFLE: {
            ensure_shuffle_order_current();
            if (!shuffle_order) return -1;
            int peek_pos = shuffle_pos + 1;
            if (peek_pos < playlist_count) return shuffle_order[peek_pos];

            /* Bag exhausted -- precompute the reshuffled continuation now
             * rather than waiting for commit_auto_advance(), so whatever
             * gets armed for gapless preload here and whatever that commit
             * later confirms are guaranteed to be the same track.
             *
             * Precomputed once per wrap so repeated calls for the same
             * transition return a consistent answer. commit_auto_advance()
             * consumes and clears pending_shuffle_order when the wrap is
             * confirmed. */
            if (!pending_shuffle_order) {
                pending_shuffle_order = malloc(sizeof(int) * (size_t) playlist_count);
                if (!pending_shuffle_order) return -1;
                memcpy(pending_shuffle_order, shuffle_order, sizeof(int) * (size_t) playlist_count);
                fisher_yates_shuffle(pending_shuffle_order, playlist_count);
            }
            return pending_shuffle_order[0];
        }
        case PLAY_MODE_SEQUENTIAL:
        default:
            return (index + 1 < playlist_count) ? index + 1 : -1;
    }
}

static bool is_failed_physical_track_cb(int index, void * userdata) {
    (void) userdata;
    const char * path = playlist_path_at(index);
    return is_failed_physical_path(path);
}

static void commit_decoder_failure_advance_plan(const failure_advance_plan_t * plan) {
    if (!plan || plan->target_index < 0) return;

    if (plan->queued_consumed > 0) {
        if (plan->queued_consumed >= queued_pending_count) {
            queued_pending_count = 0;
            queue_next_insert_index = -1;
        } else {
            queued_pending_count -= plan->queued_consumed;
        }
    }

    if ((play_mode_t) current_settings.play_mode == PLAY_MODE_SHUFFLE && plan->shuffle_steps > 0) {
        if (plan->shuffle_wrapped && pending_shuffle_order) {
            free(shuffle_order);
            shuffle_order = pending_shuffle_order;
            pending_shuffle_order = NULL;
            shuffle_order_count = playlist_count;
            shuffle_pos = (shuffle_pos + plan->shuffle_steps) % playlist_count;
        } else {
            shuffle_pos += plan->shuffle_steps;
            if (shuffle_pos >= playlist_count) shuffle_pos = playlist_count - 1;
        }
    }
}

/* Call exactly once, right when an auto-advance computed by
 * compute_auto_advance_index() actually happens (the playback thread really
 * moved on, or the hard-restart fallback is about to play that index after
 * a true end-of-playlist/failed-file) -- advances shuffle_pos, swapping in
 * the reshuffled bag precomputed above if a wrap happened. No-op outside
 * Shuffle mode, where compute_auto_advance_index() is already pure/stateless. */
void commit_auto_advance(void) {
    /* Matches compute_auto_advance_index()'s own queue-priority check --
     * a queue-jump never steps through the shuffle bag (shuffle_pos is
     * untouched), so it's handled here first and returns before any of the
     * shuffle bookkeeping below. */
    if (queued_pending_count > 0) {
        if (shuffle_order && shuffle_order_count == playlist_count) shuffle_pos++;
        queued_pending_count--;
        if (queued_pending_count == 0) queue_next_insert_index = -1;
        return;
    }

    if ((play_mode_t) current_settings.play_mode != PLAY_MODE_SHUFFLE) return;

    int peek_pos = shuffle_pos + 1;
    if (peek_pos < playlist_count) {
        shuffle_pos = peek_pos;
        return;
    }
    if (pending_shuffle_order) {
        free(shuffle_order);
        shuffle_order = pending_shuffle_order;
        pending_shuffle_order = NULL;
        shuffle_order_count = playlist_count;
        shuffle_pos = 0;
    }
}

/* What track an explicit Prev/Next button tap should move to. direction is
 * +1 (Next) or -1 (Prev). Repeat One doesn't affect manual skipping (only
 * auto-advance-at-end) -- a deliberate Next tap should never mean "restart
 * this same track". Returns -1 for "no-op, already at that edge". */
int compute_manual_step_index(int index, int direction) {
    if (index < 0 || playlist_count <= 0) return -1;
    if (direction < 0 && queued_pending_count > 0) {
        queued_pending_count = 0; queue_next_insert_index = -1;
    }

    /* Same queue-priority override as compute_auto_advance_index()/
     * commit_auto_advance() -- a manual Next tap should land on a queued
     * song too, not skip past it into whatever play_mode would otherwise
     * pick. Every caller of this function (direction=+1 case) is a real,
     * one-shot commit (touchscreen/hw button/BT remote/phone remote Next,
     * each behind its own edge-triggered "consume" flag) -- never a
     * speculative preview call -- so decrementing here is safe. */
    if (direction > 0 && queued_pending_count > 0 && index + 1 < playlist_count) {
        if (shuffle_order && shuffle_order_count == playlist_count) shuffle_pos++;
        queued_pending_count--;
        if (queued_pending_count == 0) queue_next_insert_index = -1;
        return index + 1;
    }

    if ((play_mode_t) current_settings.play_mode == PLAY_MODE_SHUFFLE) {
        ensure_shuffle_order_current();
        if (!shuffle_order) return -1;
        int new_pos = shuffle_pos + direction;
        if (new_pos < 0) return -1; /* no history to go back past */
        if (new_pos >= playlist_count) {
            fisher_yates_shuffle(shuffle_order, playlist_count);
            new_pos = 0;
        }
        shuffle_pos = new_pos;
        return shuffle_order[shuffle_pos];
    }

    if ((play_mode_t) current_settings.play_mode == PLAY_MODE_REPEAT_ALL) {
        return (index + direction + playlist_count) % playlist_count;
    }

    int next = index + direction;
    return (next >= 0 && next < playlist_count) ? next : -1;
}

/* Splits a full path into a display title (filename, no extension) and the
 * name of its containing folder. No tag/metadata parsing yet, so the
 * filename is the best "song title" available. */
void get_display_names(const char * path, char * title_out, size_t title_size,
                               char * folder_out, size_t folder_size) {
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
static void recolor_play_btn_accent(lv_draw_buf_t * buf, lv_color_t accent)
{
    if (!buf || !buf->data || !buf->header.w || !buf->header.h ||
        buf->header.cf != LV_COLOR_FORMAT_ARGB8888) return;
    uint32_t w = buf->header.w;
    uint32_t h = buf->header.h;
    uint32_t stride = buf->header.stride;
    uint8_t ar = accent.red;
    uint8_t ag = accent.green;
    uint8_t ab = accent.blue;
    for (uint32_t y = 0; y < h; y++) {
        lv_color32_t * row = (lv_color32_t *) (buf->data + y * stride);
        for (uint32_t x = 0; x < w; x++) {
            if (row[x].alpha == 0) continue;
            /* Normalize to the image ellipse. The outer quarter of its
             * radius contains the ring, outside either center glyph.
             * Preserve alpha so antialiased edges remain smooth. */
            int32_t dx = ((int32_t) (2 * x + 1) - (int32_t) w) * 256 / (int32_t) w;
            int32_t dy = ((int32_t) (2 * y + 1) - (int32_t) h) * 256 / (int32_t) h;
            if (dx * dx + dy * dy >= 192 * 192) {
                row[x].red = ar;
                row[x].green = ag;
                row[x].blue = ab;
                continue;
            }
            uint8_t minc = row[x].red;
            if (row[x].green < minc) minc = row[x].green;
            if (row[x].blue < minc) minc = row[x].blue;
            uint8_t t = (uint8_t) (255 - minc);
            /* Disc interior is white / near-white; the glyph is the stock
             * cyan. Skip the disc so only the mark is retinted. */
            if (t < 16) continue;
            row[x].red   = (uint8_t) (255 - ((uint16_t) (255 - ar) * t) / 255);
            row[x].green = (uint8_t) (255 - ((uint16_t) (255 - ag) * t) / 255);
            row[x].blue  = (uint8_t) (255 - ((uint16_t) (255 - ab) * t) / 255);
        }
    }
}

static void load_play_btn_images(void)
{
    lv_color_t accent = accent_lv_color();
    if (asset_decoded_image_prepare_retint(&play_btn_play_img, "playing_plane/btn_play.png"))
        recolor_play_btn_accent((lv_draw_buf_t *) play_btn_play_img.decoder.decoded, accent);
    if (asset_decoded_image_prepare_retint(&play_btn_pause_img, "playing_plane/btn_pause.png"))
        recolor_play_btn_accent((lv_draw_buf_t *) play_btn_pause_img.decoder.decoded, accent);
}

const void * gui_player_play_btn_image_src(bool is_playing)
{
    const void * src = asset_decoded_image_source(is_playing ? &play_btn_pause_img : &play_btn_play_img);
    if (src) return src;
    return asset_path(is_playing ? "playing_plane/btn_pause.png" : "playing_plane/btn_play.png");
}

void refresh_play_btn_icon(void)
{
    load_play_btn_images();
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

/* resolve_replaygain moved to gui_player.c */

/* Tells audio.c what comes after `index` (per the current play mode -- see
 * compute_auto_advance_index()) so its playback thread can gapless-handoff
 * or crossfade into it near `index`'s natural end without a GUI round-trip.
 * Must be re-called (from on_track_auto_advanced) every time the thread
 * advances on its own, or the chain of automatic transitions stops after
 * one hop. */
void arm_next_track_for_audio(int index) {
    ++metadata_next_token;
    queue_revision++;
    if (current_settings.play_mode == PLAY_MODE_SHUFFLE) {
        ensure_shuffle_order_current();
        if (shuffle_order) {
            for (int i = 0; i < playlist_count; i++) if (shuffle_order[i] == index) { shuffle_pos = i; break; }
        }
    }
    /* Gapless off: arm nothing, exactly as if this were the end of the
     * queue. The playback thread then reaches a true EOF, and
     * gui_player_handle_track_finished() computes the same next index and
     * opens it fresh -- which reopens the output device, producing the gap
     * between tracks that turning this off is asking for. Auto-advance
     * itself is unaffected; only the seamless handoff is given up. */
    int next_index = compute_auto_advance_index(index);
    if (next_index < 0 || !current_settings.gapless_enabled) {
        armed_next_path[0] = '\0';
        audio_set_next_track(NULL, false, 0.0, false, 0.0);
        return;
    }
    const char * next_path = playlist_path_at(next_index);
    uint64_t generation = audio_get_playback_generation();
    if (armed_next_generation != generation || armed_next_gain_mode != current_settings.replaygain_mode ||
        strcmp(armed_next_path, next_path) != 0) {
        armed_next_path[0] = '\0';
        audio_set_next_track(NULL, false, 0.0, false, 0.0);
    }
    track_metadata_request_t request = { .token = ++metadata_next_token };
    snprintf(request.path, sizeof(request.path), "%s", next_path);
    request.read_file = seed_track_metadata(next_index, &request.metadata);
    metadata_next_queue_revision = queue_revision;
    metadata_next_audio_generation = audio_get_playback_generation();
    snprintf(metadata_next_current_path, sizeof(metadata_next_current_path), "%s", playlist_path_at(index));
    metadata_next_gain_mode = current_settings.replaygain_mode;
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

void gui_player_poll_track_metadata(void) {
    poll_track_started_notification();
    track_metadata_result_t result;
    if (track_metadata_take(TRACK_METADATA_FAVORITE, &result) &&
        result.request.token == favorite_request_token && favorite_request_revision == favorite_ui_revision &&
        playlist_index >= 0 && playlist_index < playlist_count &&
        strcmp(result.request.path, playlist_path_at(playlist_index)) == 0 &&
        strcmp(result.request.path, favorite_request_path) == 0)
        apply_loaded_favorite(result.favorite);
    if (track_metadata_take(TRACK_METADATA_NEXT, &result) &&
        result.request.token == metadata_next_token && queue_revision == metadata_next_queue_revision &&
        audio_get_playback_generation() == metadata_next_audio_generation &&
        playlist_index >= 0 && playlist_index < playlist_count &&
        strcmp(playlist_path_at(playlist_index), metadata_next_current_path) == 0) {
        next_metadata_cached = true;
        snprintf(next_metadata_cached_path, sizeof(next_metadata_cached_path), "%s", result.request.path);
        next_metadata_cache = result.request.metadata;
        /* Resolve with the mode captured when this NEXT request was queued. */
        track_metadata_t * tags = &result.request.metadata;
        bool album = metadata_next_gain_mode == 2 && tags->has_replaygain_album;
        bool gain = metadata_next_gain_mode != 0 && (album || tags->has_replaygain);
        bool album_peak = album && tags->has_replaygain_album_peak;
        bool peak = metadata_next_gain_mode != 0 && (album_peak || tags->has_replaygain_peak);
        if (audio_set_next_track_for_generation(result.request.path, gain,
            album ? tags->replaygain_album_gain_db : tags->replaygain_gain_db, peak,
            album_peak ? tags->replaygain_album_peak : tags->replaygain_peak, metadata_next_audio_generation)) {
            snprintf(armed_next_path, sizeof(armed_next_path), "%s", result.request.path);
            armed_next_generation = metadata_next_audio_generation;
            armed_next_gain_mode = metadata_next_gain_mode;
        }
    }
    if (!track_metadata_take(TRACK_METADATA_CURRENT, &result)) return;
    if (result.request.token != metadata_ui_token ||
        playlist_index < 0 || playlist_index >= playlist_count || !player_screen ||
        strcmp(result.request.path, playlist_path_at(playlist_index)) != 0 ||
        strcmp(result.request.path, metadata_ui_path) != 0) return;
    apply_loaded_track_metadata(playlist_index, &result.request.metadata, result.favorite);
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

void gui_player_queue_add_many(const char * const * paths, int count) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    if (count <= 0) return;
    if (count > INT_MAX - playlist_count || count > INT_MAX - queued_pending_count) return;

    char ** copies = calloc((size_t) count, sizeof(*copies));
    if (!copies) return;
    for (int i = 0; i < count; i++) {
        copies[i] = paths[i] ? strdup(paths[i]) : NULL;
        if (!copies[i]) {
            for (int j = 0; j < count; j++) free(copies[j]);
            free(copies);
            return;
        }
    }

    if (!gui_player_has_active_track()) {
        free_playlist();
        clear_player_source();
        playlist = copies; playlist_count = count; playlist_index = 0;
        queued_pending_count = count - 1;
        queue_next_insert_index = count > 1 ? count : -1;
        request_track_metadata(0, false, false, false);
        set_play_button_state(false);
        deferred_resume_pending = true; deferred_resume_position = 0.0;
        snprintf(current_settings.last_track, sizeof(current_settings.last_track), "%s", playlist[0]);
        current_settings.last_position = 0;
        current_settings.last_source_kind = 0;
        current_settings.last_source_name[0] = '\0';
        settings_save_async(&current_settings);
        show_info_toast(TR("Queue ready. Press Play to start."));
        return;
    }
    int pos = (queue_next_insert_index >= 0 && queue_next_insert_index <= playlist_count)
        ? queue_next_insert_index : playlist_index + 1;
    char ** grown = realloc(playlist, sizeof(*playlist) * (size_t) (playlist_count + count));
    if (!grown) {
        for (int i = 0; i < count; i++) free(copies[i]);
        free(copies);
        return;
    }
    playlist = grown;
    int * grown_order = playlist_lazy_sort_order
        ? realloc(playlist_lazy_sort_order, sizeof(*playlist_lazy_sort_order) * (size_t) (playlist_count + count))
        : NULL;
    if (playlist_lazy_sort_order && !grown_order) {
        for (int i = 0; i < count; i++) free(copies[i]);
        free(copies);
        return;
    }
    if (grown_order) playlist_lazy_sort_order = grown_order;

    memmove(&playlist[pos + count], &playlist[pos],
            sizeof(*playlist) * (size_t) (playlist_count - pos));
    if (playlist_lazy_sort_order) {
        memmove(&playlist_lazy_sort_order[pos + count], &playlist_lazy_sort_order[pos],
                sizeof(*playlist_lazy_sort_order) * (size_t) (playlist_count - pos));
    }
    for (int i = 0; i < count; i++) {
        playlist[pos + i] = copies[i];
        if (playlist_lazy_sort_order) playlist_lazy_sort_order[pos + i] = -1;
    }
    free(copies);
    playlist_count += count;
    queued_pending_count += count;
    queue_next_insert_index = pos + count;
    queue_shuffle_insert(pos, count);
    if (song_count_label) lv_label_set_text_fmt(song_count_label, "%d/%d", playlist_index + 1, playlist_count);
    arm_next_track_for_audio(playlist_index);

    char msg[128];
    snprintf(msg, sizeof(msg), TR_N("Added %d song to queue", "Added %d songs to queue", count), count);
    show_info_toast(msg);
}

void queue_remove_song_at_offset(int offset) {
    if (offset < 0 || offset >= queued_pending_count || playlist_index < 0) return;
    int pos = playlist_index + 1 + offset;
    queue_shuffle_remove(pos);
    free(playlist[pos]);
    memmove(&playlist[pos], &playlist[pos + 1], sizeof(char *) * (size_t) (playlist_count - pos - 1));
    if (playlist_lazy_sort_order) {
        memmove(&playlist_lazy_sort_order[pos], &playlist_lazy_sort_order[pos + 1],
                sizeof(int) * (size_t) (playlist_count - pos - 1));
    }
    playlist_count--;
    queued_pending_count--;
    queue_next_insert_index = queued_pending_count > 0 ? playlist_index + 1 + queued_pending_count : -1;
    if (song_count_label) lv_label_set_text_fmt(song_count_label, "%d/%d", playlist_index + 1, playlist_count);
    arm_next_track_for_audio(playlist_index);
    show_info_toast(TR("Removed from queue"));
}

void queue_clear_pending(void) {
    while (queued_pending_count > 0) queue_remove_song_at_offset(queued_pending_count - 1);
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
 * re-init isn't automatic -- see TESTING.md). With Bluetooth actually off,
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
    if (index < 0 || index >= playlist_count) return;

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
        show_error_toast(block_reason);
        return;
    }

    cancel_pending_progress_seek();
    user_seeking = false;
    progress_awaiting_seek = false;
    deferred_resume_pending = false;
    deferred_resume_position = 0.0;
    playlist_index = index;

    const char * path = playlist_path_at(index);
    track_metadata_t seed;
    bool local = seed_track_metadata(index, &seed);
    track_metadata_count_play(path);
    next_metadata_cached = false;
    if (local) audio_play_file_at_with_replaygain_mode(path, start_seconds, current_settings.replaygain_mode);
    else {
        bool has_gain, has_peak;
        double gain_db, peak;
        resolve_replaygain(&seed, &has_gain, &gain_db, &has_peak, &peak);
        audio_play_file_at(path, start_seconds, has_gain, gain_db, has_peak, peak);
    }
    current_playback_generation = audio_get_playback_generation();
    request_track_metadata(index, true, false, true);
    arm_next_track_for_audio(index);

    set_play_button_state(true);
    /* Manual next/previous and a deferred startup resume can already be on
     * the player screen.  Do not stack a duplicate copy of the same screen
     * just because playback is being (re)started there. */
    if (push_nav && lv_screen_active() != player_screen) nav_push(player_screen);

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
    if (index < 0 || index >= playlist_count) return;

    cancel_pending_progress_seek();
    user_seeking = false;
    progress_awaiting_seek = false;
    playlist_index = index;

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
    playlist = new_playlist;
    playlist_count = count;
    queue_pin_sd_dir_if_needed();
    /* A brand new playback context (a fresh song tapped from any list)
     * invalidates whatever was queued against the OLD playlist[] -- those
     * array positions no longer mean anything once the array itself has
     * been replaced. Matches every other music app: starting something new
     * clears "Up Next". */
    queued_pending_count = 0;
    queue_next_insert_index = -1;
    play_track_at(selected_index);
}

/* Same as on_file_selected() above, but for a tap that needs to start
 * partway into the track rather than at 0:00 -- currently only CUE track
 * playback (see cue_track_row_click_cb()): each of new_playlist's entries
 * is the SAME physical audio file repeated once per CUE track, and
 * start_seconds is the tapped track's own INDEX 01 offset within it. */
void on_file_selected_at(char ** new_playlist, int count, int selected_index, double start_seconds) {
    free_playlist();
    playlist = new_playlist;
    playlist_count = count;
    queue_pin_sd_dir_if_needed();
    queued_pending_count = 0;
    queue_next_insert_index = -1;
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

/* Wraps on_file_selected() as file_browser_init()'s select_cb, rather than
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
    free_playlist();
    playlist = slots; playlist_count = (int) playable_count;
    playlist_lazy_sort_order = order; playlist_lazy_order_is_recency = false;
    playlist_lazy_file_index = index;
    for (unsigned i = 0; i < playable_count; i++) order[i] = (int) i;
    queue_sd_dirfd = pinned_queue_dirfd;
    queued_pending_count = 0; queue_next_insert_index = -1;
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

void toggle_play_pause(void) {
    reset_decoder_failure_tracking();
    if (playlist_index < 0) return; /* nothing loaded yet */
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
        play_track_at_from_internal(playlist_index, audio_get_resume_position_seconds(), true, true);
        return;
    }
    if (deferred_resume_pending) {
        double start_seconds = deferred_resume_position;
        deferred_resume_pending = false;
        deferred_resume_position = 0.0;
        play_track_at_from(playlist_index, start_seconds);
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
    /* See transport_long_press_cb()'s own comment -- LV_EVENT_CLICKED still
     * fires on release even after a long press, so a hold-to-rewind session
     * would otherwise ALSO restart/skip to the previous track right after. */
    if (prev_btn_long_press_fired) {
        prev_btn_long_press_fired = false;
        return;
    }
    reset_decoder_failure_tracking();
    if (playlist_index < 0) return;

    if (transport_skip_manual(-1)) return;
    if (audio_get_position_seconds() > PREV_BUTTON_REWIND_THRESHOLD_SECONDS) {
        audio_seek(0.0);
        return;
    }

    gui_player_step_manual(-1);
}

void next_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (next_btn_long_press_fired) {
        next_btn_long_press_fired = false;
        return;
    }
    reset_decoder_failure_tracking();
    if (playlist_index < 0) return;
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
    if (gui_player_has_active_track()) arm_next_track_for_audio(playlist_index);
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
    if (gui_player_has_active_track()) arm_next_track_for_audio(playlist_index);
    gui_settings_sync_gapless_toggle();
    gui_shell_refresh_quick_drawer_expansion_toggles();
}

void gui_player_set_replaygain_mode(int mode) {
    if (mode < 0 || mode > 2 || current_settings.replaygain_mode == mode) return;
    current_settings.replaygain_mode = mode;
    settings_save_async(&current_settings);
    /* The already prepared successor captured the old gain mode. Rebuild it
     * now so the new setting takes effect at the next transition. */
    if (playlist_index >= 0) arm_next_track_for_audio(playlist_index);
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

/* refresh_clock_label() re-syncs the AM/PM sprite's own hidden flag itself
 * -- called here so toggling this switch is reflected immediately, same
 * reasoning as battery_percent_switch_event_cb() just above. */
void clock_24h_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.clock_24h = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    app_clock_get_persistence(&current_settings.clock_manual_epoch,
                              &current_settings.clock_system_reference);
    settings_save_async(&current_settings);
    refresh_clock_label();
}

/* Log-scale mapping so the slider gives fine control at low frequencies
 * (where the ear is more sensitive to small Hz changes) instead of wasting
 * most of the slider's travel on the top octave. */



void gui_player_init(uint32_t screen_width, uint32_t screen_height) {
    track_metadata_worker_start();
    /* Pay thread and stack setup during initialization, not inside the first
     * favorite tap where it would visibly block the LVGL event callback. */
    favorite_writer_start();
    build_volume_popup();
    build_delete_song_popup();
    build_more_menu_popup();
    gui_track_info_init();
    player_screen = build_player_screen(screen_width, screen_height);
    player_restore_reload_label_text();
    if (!gui_player_has_active_track() && song_title_label)
        lv_label_set_text(song_title_label, TR("No track loaded"));
    /* A rebuilt label can need the same live values written again even if
     * playback has not crossed a second or progress percentage boundary. */
    displayed_progress_percent = -1;
    displayed_position_second = -1;
    displayed_duration_second = -1;
    if (song_count_label && gui_player_has_active_track()) {
        lv_label_set_text_fmt(song_count_label, "%d/%d", playlist_index + 1, playlist_count);
        lv_obj_remove_flag(song_count_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (gui_player_has_active_track()) {
        track_metadata_request_t request = { .token = ++metadata_ui_token, .probe = deferred_resume_pending };
        snprintf(request.path, sizeof(request.path), "%s", playlist_path_at(playlist_index));
        request.read_file = seed_track_metadata(playlist_index, &request.metadata);
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
    gui_player_refresh_frosted_background();
}

/* For gui_reload.c's in-process UI reload -- deletes every screen/popup this
 * module owns so gui_player_init() can rebuild them from a clean slate
 * without leaking the old objects. volume_popup/delete_song_popup/
 * more_menu_popup and their backdrops are built directly on lv_layer_top()
 * (see build_confirm_popup()'s own comment), not as children of
 * player_screen, so deleting player_screen alone would not reach them. */
void gui_player_teardown(void) {
    ++metadata_ui_token;
    ++favorite_ui_revision;
    /* Also cancel a pending fallback completion after an already-settled
     * transition, before any timeline or widget can be replaced. */
    player_lyrics_timeline_cancel_timer();
    if (player_artwork_cache_timer) {
        lv_timer_delete(player_artwork_cache_timer);
        player_artwork_cache_timer = NULL;
    }
    lv_draw_sw_image_cache_set_transition_active(false);
    lv_draw_sw_image_cache_set_intermediate_source(NULL);
    player_lyrics_set_open(false, false);
    player_timeline_labels_teardown();
    player_lyrics_backdrop_teardown();
    player_lyrics_generated_release(false);
    /* build_volume_popup() creates this with an unguarded lv_timer_create()
     * -- for gui_reload.c's in-process UI reload, delete it here or the old
     * (leaked) timer keeps firing volume_popup_hide_timer_cb() forever,
     * which itself references the GLOBAL volume_popup_hide_timer variable
     * (by then reassigned to the NEW timer) -- so the leaked OLD timer
     * would keep incorrectly pausing/resetting the NEW one on every tick,
     * never itself getting paused, instead of a clean single timer. */
    if (volume_popup_hide_timer) { lv_timer_del(volume_popup_hide_timer); volume_popup_hide_timer = NULL; }
    /* hw_volume_coalesce_teardown() can't re-read a live slider itself (it
     * doesn't know which widget owns the drag) -- that's a deliberate
     * caller-side step, done here exactly like before the extraction. */
    if (volume_popup_hv.drag_active && volume_popup_track)
        hw_volume_coalesce_drag_update(&volume_popup_hv, (int) lv_slider_get_value(volume_popup_track));
    int volume_teardown_pending = volume_popup_hv.pending;
    hw_volume_coalesce_teardown(&volume_popup_hv);
    if (volume_teardown_pending >= 0) {
        gui_player_remember_volume_percent(volume_teardown_pending);
        settings_save(&current_settings); /* Final teardown checkpoint must reach storage. */
    }
    if (volume_popup) { lv_obj_delete(volume_popup); volume_popup = NULL; }
    volume_popup_speaker_icon = NULL;
    volume_popup_value_label = NULL;
    asset_decoded_image_close(&volume_popup_speaker_image);
    volume_popup_track = NULL;
    gui_popup_teardown(&delete_song_popup);
    if (more_menu_popup) { lv_obj_delete(more_menu_popup); more_menu_popup = NULL; }
    if (more_menu_popup_backdrop) { lv_obj_delete(more_menu_popup_backdrop); more_menu_popup_backdrop = NULL; }
    gui_track_info_teardown();
    /* Capture while the old labels are still alive. The audio state and
     * current track continue across a UI-only reload. */
    player_capture_reload_label_text();
    /* Unbind retained cover pixels while their widgets are still alive. */
    player_timeline_cover_cache_invalidate();
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

void gui_player_refresh_static_assets(void) {
    asset_decoded_image_close(&play_btn_play_img);
    asset_decoded_image_close(&play_btn_pause_img);
    refresh_play_btn_icon();
    if (!volume_popup) return;
    asset_decoded_image_close(&volume_popup_speaker_image);
    const void * speaker = asset_decoded_image_open(&volume_popup_speaker_image, "volume/vol.png")
                         ? asset_decoded_image_source(&volume_popup_speaker_image) : NULL;
    if (volume_popup_speaker_icon)
        lv_image_set_src(volume_popup_speaker_icon, speaker ? speaker : asset_path("volume/vol.png"));
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

void gui_player_update_progress(void) {
    gui_player_poll_confirmed_playback();
    if (player_waveform_seekbar && progress_slider) {
        const char * path = gui_player_get_current_track_path();
        waveform_data_t data;
        waveform_request(path);
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

    if (progress_awaiting_seek) {
        if (fabs(position - pending_progress_seek_seconds) < 0.4 ||
            ++progress_awaiting_seek_ticks >= 8)
            progress_awaiting_seek = false;
        else
            return;
    }

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
    return cover_decode_active || gui_player_queue_write_busy() || queue_save_active();
}

void gui_player_cancel_background_work(void) {
    queue_save_cancel();
    if (cover_decode_active) {
        pthread_join(cover_decode_thread, NULL);
        cover_decode_active = false;
        free(cover_decode_result_pixels);
        free(cover_decode_result_reflection);
        cover_decode_result_pixels = NULL;
        cover_decode_result_reflection = NULL;
        atomic_store_explicit(&cover_decode_done_flag, false, memory_order_relaxed);
    }
    if (atomic_load_explicit(&cover_reload_active, memory_order_relaxed))
        gui_player_cancel_cover_reload();
}

const char * playlist_path_at(int index) {
    if (index < 0 || index >= playlist_count) return "";
    if (playlist[index]) return playlist[index];
    if (!playlist_lazy_sort_order) return "";
    int sort_pos = playlist_lazy_sort_order[index];
    char resolved[PATH_MAX];
    bool got = playlist_lazy_snapshot && metadata_db_snapshot_path_at(playlist_lazy_snapshot, sort_pos,
                                                                       resolved, sizeof(resolved));
    if (!got && playlist_lazy_file_index)
        got = file_browser_index_playable_path_at(playlist_lazy_file_index, (unsigned) sort_pos,
                                                  resolved, sizeof(resolved));
    /* A missed read must stay unresolved. Caching "" makes the slot look
     * empty forever and a later play would save that as the resume path. */
    if (!got || !resolved[0]) return "";
    char *copy = strdup(resolved);
    if (!copy) return "";
    playlist[index] = copy;
    return copy;
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
    free_playlist();
    playlist_lazy_snapshot = snapshot;
    playlist = new_pl;
    playlist_lazy_sort_order = new_order;
    queue_sd_dirfd = open_queue_dir_from_snapshot(playlist_lazy_snapshot);
    playlist_count = count;
    for (int i = 0; i < count; i++) playlist_lazy_sort_order[i] = i;
    playlist_lazy_order_is_recency = false;
    queued_pending_count = 0;
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
    free_playlist();
    playlist_lazy_snapshot = snapshot;
    playlist = new_pl;
    playlist_lazy_sort_order = new_order;
    queue_sd_dirfd = open_sd_queue_dir();
    playlist_count = count;
    for (int i = 0; i < count; i++) playlist_lazy_sort_order[i] = i;
    playlist_lazy_order_is_recency = true;
    queued_pending_count = 0;
    play_track_at(selected_index);
}

static bool resume_playlist_needs_lazy_order = false;
static file_browser_index_t *resume_file_index = NULL;
#ifdef HOST_BUILD
#define QUEUE_RESUME_PATH "./.compas/queue.bin"
#define SD_QUEUE_RESUME_PATH "./music/.compas/queue.bin"
#define SD_QUEUE_RESUME_DIR "./music/.compas"
#define QUEUE_RESUME_LEGACY_PATH "./open_hiby_player_queue.bin"
#define SD_QUEUE_RESUME_LEGACY_PATH "./music/.open_hiby_player/queue.bin"
#else
#define QUEUE_RESUME_PATH "/usr/data/.compas/queue.bin"
#define SD_QUEUE_RESUME_PATH "/data/mnt/sd_0/.compas/queue.bin"
#define SD_QUEUE_RESUME_DIR "/data/mnt/sd_0/.compas"
#define QUEUE_RESUME_LEGACY_PATH "/usr/data/open_hiby_player_queue.bin"
#define SD_QUEUE_RESUME_LEGACY_PATH "/data/mnt/sd_0/.open_hiby_player/queue.bin"
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

typedef struct {
    int target_dirfd;
    char target_name[NAME_MAX];
    bool target_is_sd;
    queue_resume_t queue;
} checkpoint_worker_ctx_t;

static checkpoint_worker_ctx_t checkpoint_ctx;
static pthread_t checkpoint_thread;
static bool checkpoint_running;
static atomic_bool checkpoint_done;
static atomic_bool checkpoint_urgent_pending;
static atomic_bool checkpoint_large_lazy_cleanup_pending;
static atomic_bool checkpoint_failure_pending;

static bool queue_checkpoint_is_large_lazy(void) {
    return playlist_lazy_sort_order != NULL &&
           playlist_count > LAZY_QUEUE_CHECKPOINT_MAX_ENTRIES;
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

static void remove_queue_checkpoint_at(int dirfd, const char * name) {
    if (dirfd >= 0 && name) (void) unlinkat(dirfd, name, 0);
}

static void remove_legacy_sd_checkpoint(int compas_dirfd) {
    int rootfd = openat(compas_dirfd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rootfd < 0) return;
    int legacyfd = openat(rootfd, LEGACY_STORAGE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (legacyfd >= 0) { (void) unlinkat(legacyfd, "queue.bin", 0); close(legacyfd); }
    close(rootfd);
}

static void retire_legacy_checkpoint(const checkpoint_worker_ctx_t * ctx) {
    if (!ctx) return;
    if (!ctx->target_is_sd) {
        (void) unlink(QUEUE_RESUME_LEGACY_PATH);
        return;
    }
    remove_legacy_sd_checkpoint(ctx->target_dirfd);
}

/* Plain uint64_t, not atomic: this target's toolchain lacks a native 64-bit
 * atomic instruction and needs libatomic for one (confirmed -- linking
 * atomic_uint_least64_t here fails with "undefined reference to
 * __atomic_store_8" on this project's mipsel cross-compiler, which isn't
 * linked against libatomic), so an 8-byte atomic isn't a free change here
 * the way the existing 1-byte atomic_bool usage in this file is. The
 * checkpoint worker thread can write checkpoint_revision_sd to 0 on failure
 * at the same time gui_player_handle_sd_unmount() resets it to 0 on the UI
 * thread on a confirmed unmount -- both only ever write the same value (0),
 * so this is a logically harmless race, accepted rather than pulling in a
 * new link dependency to make it formally well-defined. */
static uint64_t checkpoint_revision_internal;
static double checkpoint_position_internal = -1;
static uint64_t checkpoint_revision_sd;
static double checkpoint_position_sd = -1;

static void * queue_checkpoint_worker(void * arg) {
    checkpoint_worker_ctx_t * ctx = (checkpoint_worker_ctx_t *) arg;
    bool wrote = queue_resume_write_at(ctx->target_dirfd, ctx->target_name, &ctx->queue);
    if (!wrote) {
        /* Retry on the next checkpoint; never replace a valid file on failure. */
        atomic_store(&checkpoint_failure_pending, true);
    } else {
        atomic_store(&checkpoint_failure_pending, false);
        retire_legacy_checkpoint(ctx);
    }
    queue_resume_free(&ctx->queue);
    if (atomic_load(&checkpoint_large_lazy_cleanup_pending)) {
        remove_queue_checkpoint_at(ctx->target_dirfd, ctx->target_name);
    }
    close(ctx->target_dirfd);
    ctx->target_dirfd = -1;
    atomic_store(&checkpoint_done, true);
    return NULL;
}

bool gui_player_queue_write_busy(void) {
    return checkpoint_running && !atomic_load(&checkpoint_done);
}

/* UI code may poll this and present one persistent status indicator; the flag
 * is deliberately level-triggered so a failed read-only card does not create
 * a toast on every periodic checkpoint attempt. */
bool gui_player_queue_checkpoint_failed(void) {
    return atomic_load(&checkpoint_failure_pending);
}

void gui_player_queue_checkpoint(void) {
    if (queue_checkpoint_is_large_lazy()) {
        /* Set this before observing the worker: if it is just finishing, the
         * worker will remove anything it wrote; the join path below covers the
         * race where it has already published checkpoint_done. */
        atomic_store(&checkpoint_large_lazy_cleanup_pending, true);
        atomic_store(&checkpoint_urgent_pending, false);
        if (checkpoint_running) {
            if (!atomic_load(&checkpoint_done)) return;
            pthread_join(checkpoint_thread, NULL);
            checkpoint_running = false;
        }
        int internal_dirfd = open_checkpoint_target_dir(QUEUE_RESUME_PATH);
        if (internal_dirfd >= 0) { remove_queue_checkpoint_at(internal_dirfd, "queue.bin"); close(internal_dirfd); }
        if (queue_sd_dirfd >= 0) remove_queue_checkpoint_at(queue_sd_dirfd, "queue.bin");
        (void) unlink(QUEUE_RESUME_LEGACY_PATH);
        if (queue_sd_dirfd >= 0) remove_legacy_sd_checkpoint(queue_sd_dirfd);
        checkpoint_revision_internal = 0;
        checkpoint_position_internal = -1;
        checkpoint_revision_sd = 0;
        checkpoint_position_sd = -1;
        atomic_store(&checkpoint_large_lazy_cleanup_pending, false);
        return;
    }

    if (checkpoint_running) {
        if (!atomic_load(&checkpoint_done)) return;
        pthread_join(checkpoint_thread, NULL);
        checkpoint_running = false;
        if (atomic_load(&checkpoint_failure_pending)) {
            checkpoint_revision_internal = 0;
            checkpoint_revision_sd = 0;
        }
    }
    atomic_store(&checkpoint_large_lazy_cleanup_pending, false);
    if (!gui_player_has_active_track()) return;
    double position = deferred_resume_pending ? deferred_resume_position : audio_get_resume_position_seconds();

    bool is_sd_persistable = true;
    for (int i = 0; i < playlist_count; i++) {
        const char * p = playlist_path_at(i);
        if (!p || !p[0] || !is_sd_card_path(p) ||
            strncmp(p, SUBSONIC_STREAM_CACHE_DIR, strlen(SUBSONIC_STREAM_CACHE_DIR)) == 0) {
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

    if (is_sd_persistable && sd_mounted) {
        target_name = "queue.bin";
        target_path = SD_QUEUE_RESUME_PATH;
        if (target_dirfd >= 0) close(target_dirfd);
        /* A restored queue.bin fills playlist[] without going through
         * on_file_selected(), which is the only place that used to pin this
         * directory. Leaving it at -1 made every boot checkpoint fail before
         * any write, and the toast blamed a read-only card. */
        if (queue_sd_dirfd < 0) queue_sd_dirfd = open_sd_queue_dir();
        target_dirfd = queue_sd_dirfd >= 0 ? dup(queue_sd_dirfd) : -1;
        last_rev = &checkpoint_revision_sd;
        last_pos = &checkpoint_position_sd;
    }

    if (queue_revision == *last_rev && fabs(position - *last_pos) < 1.0) { if (target_dirfd >= 0) close(target_dirfd); return; }
    if (target_dirfd < 0) { *last_rev = 0; atomic_store(&checkpoint_failure_pending, true); return; }

    queue_resume_t s = { .count = playlist_count, .current = playlist_index, .pending = queued_pending_count,
        .mode = current_settings.play_mode, .shuffle_pos = shuffle_pos, .position = position };
    s.paths = calloc((size_t) s.count, sizeof(char *));
    if (!s.paths) { close(target_dirfd); return; }
    bool ok = true;
    for (int i = 0; i < s.count; i++) {
        const char * path = playlist_path_at(i);
        if (!path || !path[0] || remote_track_path_is_remote(path) ||
            strncmp(path, SUBSONIC_STREAM_CACHE_DIR, strlen(SUBSONIC_STREAM_CACHE_DIR)) == 0) { ok = false; break; }
        s.paths[i] = strdup(path);
        if (!s.paths[i]) { ok = false; break; }
    }
    if (ok && current_settings.play_mode == PLAY_MODE_SHUFFLE) {
        ensure_shuffle_order_current();
        s.shuffle_pos = shuffle_pos;
        s.order = malloc((size_t) s.count * sizeof(int));
        ok = s.order && shuffle_order;
        if (ok) memcpy(s.order, shuffle_order, (size_t) s.count * sizeof(int));
        if (ok && pending_shuffle_order) {
            s.continuation = malloc((size_t) s.count * sizeof(int));
            ok = s.continuation != NULL;
            if (ok) memcpy(s.continuation, pending_shuffle_order, (size_t) s.count * sizeof(int));
        }
    }
    if (!ok) { queue_resume_free(&s); close(target_dirfd); return; }

    checkpoint_ctx.target_dirfd = target_dirfd;
    snprintf(checkpoint_ctx.target_name, sizeof(checkpoint_ctx.target_name), "%s", target_name);
    checkpoint_ctx.target_is_sd = is_sd_persistable && sd_mounted;
    checkpoint_ctx.queue = s;
    *last_rev = queue_revision;
    *last_pos = position;

    atomic_store(&checkpoint_done, false);
    checkpoint_running = pthread_create(&checkpoint_thread, NULL, queue_checkpoint_worker, &checkpoint_ctx) == 0;
    if (!checkpoint_running) { queue_resume_free(&checkpoint_ctx.queue); close(target_dirfd); checkpoint_ctx.target_dirfd = -1; *last_rev = 0; }
}

/* Like gui_player_queue_checkpoint(), but guarantees the request is not silently
 * dropped if a previous checkpoint's background worker is still writing: it marks
 * an urgent-pending flag instead, which gui_player_queue_poll_urgent() below
 * retries on the very next UI poll tick once the in-flight worker finishes,
 * rather than waiting for the next scheduled 30-second/1-second periodic tick. */
void gui_player_queue_checkpoint_urgent(void) {
    if (checkpoint_running && !atomic_load(&checkpoint_done)) {
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
    if (checkpoint_running && !atomic_load(&checkpoint_done)) return; /* still busy */
    atomic_store(&checkpoint_urgent_pending, false);
    gui_player_queue_checkpoint();
}

void gui_player_queue_flush(void) {
    if (checkpoint_running) {
        pthread_join(checkpoint_thread, NULL);
        checkpoint_running = false;
    }
    gui_player_queue_checkpoint();
    if (checkpoint_running) {
        pthread_join(checkpoint_thread, NULL);
        checkpoint_running = false;
    }
}

void gui_player_handle_sd_unmount(void) {
    atomic_store(&sd_card_absent_immediate, true);
    checkpoint_revision_sd = 0;
    checkpoint_position_sd = -1;

    /* Deliberately do NOT join checkpoint_thread here, even if one is still
     * running: this runs on the UI thread (via poll_sd_card_hotplug()'s
     * 500ms timer), and the worker may be blocked inside a write()/fsync()
     * to the very card that's being pulled out from under it -- joining
     * unconditionally here would risk freezing the whole UI on a stalled/
     * hung SD write. This is safe to skip: the worker only ever touches its
     * own deep-copied checkpoint_ctx.queue snapshot (built synchronously
     * before pthread_create()), never the live playlist[] this function is
     * about to free, so there's no actual data race to guard against by
     * waiting for it here. It will finish (or fail harmlessly, retried on
     * the next checkpoint) on its own; checkpoint_running/checkpoint_done
     * are already correctly rechecked, non-blockingly, by the next call to
     * gui_player_queue_checkpoint(). */

    /* Lazy song queues belong to the SD database even when no path has been
     * resolved yet. Never resolve every entry of a 300K queue on the UI
     * thread just to decide whether it must be cleared. */
    bool has_sd_playlist = playlist_lazy_sort_order != NULL;
    for (int i = 0; !has_sd_playlist && i < playlist_count; i++) {
        if (is_sd_card_path(playlist[i])) {
            has_sd_playlist = true;
            break;
        }
    }
    if (!has_sd_playlist) return;

    const char * cur_path = playlist_path_at(playlist_index);
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

    if (!queue_resume_read(SD_QUEUE_RESUME_PATH, &restored_queue) &&
        !queue_resume_read(SD_QUEUE_RESUME_LEGACY_PATH, &restored_queue)) return false;

    if (restored_queue.count <= 0 || restored_queue.current < 0 || restored_queue.current >= restored_queue.count) {
        queue_resume_free(&restored_queue);
        return false;
    }

    bool all_sd = true;
    for (int i = 0; i < restored_queue.count; i++) {
        const char * path = restored_queue.paths[i];
        if (!path || !path[0] || !is_sd_card_path(path) ||
            strncmp(path, SUBSONIC_STREAM_CACHE_DIR, strlen(SUBSONIC_STREAM_CACHE_DIR)) == 0) {
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
            const char * cur = playlist_path_at(playlist_index);
            if (!is_sd_card_path(cur)) return false;
        }
    }

    char ** resume_playlist = NULL;
    int resume_count = 0, resume_index = 0;
    double position = 0.0;
    if (!build_sd_card_resume_playlist(&resume_playlist, &resume_count, &resume_index, &position)) return false;

    /* install_saved_resume_playlist() only swaps the queue bookkeeping
     * (free_playlist() + replace playlist[]/playlist_count) -- it never
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
         * Without it, playlist_index is set but nothing is "current" in the
         * audio subsystem yet, so toggle_play_pause() would be a no-op. */
        playlist_index = resume_index;
        request_track_metadata(resume_index, false, false, false);
        set_play_button_state(false);
        deferred_resume_pending = true; deferred_resume_position = 0.0;
    }
    return true;
}

bool gui_player_queue_restart_displayed(int selected) {
    if (playlist_count <= 0 || selected < 0 || selected >= playlist_count) return false;
    int *order = NULL, count = 0, current = 0;
    uint64_t revision = 0;
    if (!gui_player_queue_snapshot(&order, &count, &current, &revision) || count != playlist_count) {
        free(order);
        return false;
    }
    char **next_paths = NULL;
    int *next_ranks = NULL;
    if (!playback_order_resequence(order, count, playlist, playlist_lazy_sort_order, &next_paths, &next_ranks)) {
        free(order);
        return false;
    }
    free(order);
    free(playlist);
    free(playlist_lazy_sort_order);
    playlist = next_paths;
    playlist_lazy_sort_order = next_ranks;
    playlist_index = selected;
    queued_pending_count = 0;
    queue_next_insert_index = -1;
    free(shuffle_order);
    shuffle_order = NULL;
    shuffle_order_count = 0;
    shuffle_pos = -1;
    free(pending_shuffle_order);
    pending_shuffle_order = NULL;
    return true;
}

bool build_saved_resume_playlist(char *** out_playlist, int * out_count, int * out_index) {
    resume_playlist_needs_lazy_order = false;
    if (resume_file_index) {
        file_browser_index_close(resume_file_index);
        resume_file_index = NULL;
    }
    queue_resume_free(&restored_queue);
    have_restored_queue = false;
    if (queue_resume_read(QUEUE_RESUME_PATH, &restored_queue) ||
        queue_resume_read(QUEUE_RESUME_LEGACY_PATH, &restored_queue)) {
        bool local = true;
        for (int i = 0; i < restored_queue.count; i++) {
            const char * path = restored_queue.paths[i];
            if (remote_track_path_is_remote(path) || strncmp(path, SUBSONIC_STREAM_CACHE_DIR, strlen(SUBSONIC_STREAM_CACHE_DIR)) == 0)
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
    free_playlist();
    playlist_lazy_snapshot = resume_snapshot;
    playlist_lazy_file_index = file_index;
    playlist = resume_playlist;
    playlist_count = resume_count;
    playlist_lazy_sort_order = new_order;
    playlist_lazy_order_is_recency = false;
    if (playlist_lazy_file_index) queue_sd_dirfd = open_queue_dir_from_file_index(playlist_lazy_file_index);
    else if (playlist_lazy_snapshot) queue_sd_dirfd = open_queue_dir_from_snapshot(playlist_lazy_snapshot);
    if (have_restored_queue) {
        queued_pending_count = restored_queue.pending;
        queue_next_insert_index = queued_pending_count ? restored_queue.current + 1 + queued_pending_count : -1;
        current_settings.play_mode = restored_queue.mode;
        shuffle_order = restored_queue.order; restored_queue.order = NULL;
        pending_shuffle_order = restored_queue.continuation; restored_queue.continuation = NULL;
        shuffle_order_count = shuffle_order ? resume_count : 0;
        shuffle_pos = restored_queue.shuffle_pos;
        queue_resume_free(&restored_queue);
        have_restored_queue = false;
    }
    return true;
}

void prepare_deferred_resume(int index, double start_seconds) {
    playlist_index = index;
    set_play_button_state(false);
    deferred_resume_pending = true;
    deferred_resume_position = start_seconds;
    request_track_metadata(index, false, true, false);

    nav_push(player_screen);
}


int gui_player_get_playlist_count(void) {
    return playlist_count;
}

int gui_player_get_playlist_index(void) {
    return playlist_index;
}

bool gui_player_has_active_track(void) {
    return playlist_index >= 0 && playlist_index < playlist_count;
}

const char * gui_player_get_current_track_path(void) {
    if (playlist_index < 0 || playlist_index >= playlist_count) return "";
    return playlist_path_at(playlist_index);
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

int gui_player_get_queued_count(void) {
    return queued_pending_count;
}

const char * gui_player_get_queued_path_at(int offset) {
    if (offset < 0 || offset >= queued_pending_count || playlist_index < 0) return "";
    int pos = playlist_index + 1 + offset;
    if (pos >= playlist_count) return "";
    return playlist_path_at(pos);
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

uint64_t gui_player_queue_revision(void) { return queue_revision; }

bool gui_player_queue_snapshot(int ** order, int * count, int * current, uint64_t * revision) {
    *order = NULL; *count = playlist_count; *current = playlist_index; *revision = queue_revision;
    if (!playlist_count) return true;
    int * result = malloc((size_t) playlist_count * sizeof(int));
    if (!result) return false;
    if (current_settings.play_mode == PLAY_MODE_SHUFFLE) {
        ensure_shuffle_order_current();
        if (!shuffle_order) { free(result); return false; }
        memcpy(result, shuffle_order, (size_t) playlist_count * sizeof(int));
        *current = shuffle_pos;
    } else for (int i = 0; i < playlist_count; i++) result[i] = i;
    *order = result;
    return true;
}

/* Publish the displayed Up Next order only when the queue revision changes.
 * The remote listener owns its own copy, so it never reads playlist state
 * from its worker thread. This also seeds the endpoint the first time
 * Remote Control is enabled after boot/restoration. */
void gui_player_sync_remote_queue(void) {
    static uint64_t published_revision = 0;
    if (queue_revision == published_revision) return;
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
    if (revision != queue_revision || offset < 0) return -1;
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
    if (revision != queue_revision) return false;
    gui_player_queue_clear_all();
    return revision != queue_revision;
}

/* Materialize a displayed cycle before editing. The audio owns its decoder;
 * rearranging path slots does not restart the current track. */
static bool queue_materialize_order(void) {
    int * order = NULL, count, current;
    uint64_t revision;
    if (!gui_player_queue_snapshot(&order, &count, &current, &revision)) return false;
    if (!count) { free(order); return true; }
    if (playlist_lazy_sort_order) {
        /* Keep the database row mapping as the queue's storage.  Reorder the
         * row ids in displayed order and leave all paths unresolved; this
         * makes edit/select/clear work on huge lazy queues without copying the
         * library into RAM. */
        int * mapped = malloc((size_t) count * sizeof(*mapped));
        char ** mapped_paths = calloc((size_t) count, sizeof(*mapped_paths));
        if (!mapped || !mapped_paths) { free(mapped); free(mapped_paths); free(order); return false; }
        for (int i = 0; i < count; i++) {
            mapped[i] = playlist_lazy_sort_order[order[i]];
            mapped_paths[i] = playlist[order[i]];
        }
        free(order);
        free(playlist);
        free(playlist_lazy_sort_order);
        playlist = mapped_paths;
        playlist_lazy_sort_order = mapped;
        playlist_index = current;
        if (shuffle_order && shuffle_order_count == count) {
            for (int i = 0; i < count; i++) shuffle_order[i] = i;
            shuffle_pos = current;
        }
        free(pending_shuffle_order); pending_shuffle_order = NULL;
        queue_next_insert_index = queued_pending_count ? current + 1 + queued_pending_count : -1;
        return true;
    }
    char ** paths = calloc((size_t) count, sizeof(*paths));
    if (!paths) { free(order); return false; }
    for (int i = 0; i < count; i++) {
        const char * path = playlist_path_at(order[i]);
        if (!path || !path[0]) { free(paths); free(order); return false; }
        paths[i] = playlist[order[i]];
    }
    free(playlist); playlist = paths;
    free(playlist_lazy_sort_order); playlist_lazy_sort_order = NULL;
    playlist_index = current;
    if (shuffle_order && shuffle_order_count == count) {
        for (int i = 0; i < count; i++) shuffle_order[i] = i;
        shuffle_pos = current;
    }
    free(pending_shuffle_order); pending_shuffle_order = NULL;
    queue_next_insert_index = queued_pending_count ? current + 1 + queued_pending_count : -1;
    free(order);
    return true;
}

bool gui_player_queue_edit(uint64_t revision, int from, int to) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    if (revision != queue_revision) return false;
    int * order = NULL, count, current;
    uint64_t actual;
    if (!gui_player_queue_snapshot(&order, &count, &current, &actual)) return false;
    free(order);
    if (from <= current || from >= count || (to >= 0 && (to <= current || to >= count))) return false;
    if (!queue_materialize_order()) return false;
    if (playlist_lazy_sort_order) {
        int moved = playlist_lazy_sort_order[from];
        char * moved_path = playlist[from];
        if (to < 0) {
            /* Materialize left an identity bag of the old length. Remove
             * this row from it before the count changes, or the next arm
             * rebuilds a random bag. */
            queue_shuffle_remove(from);
            memmove(&playlist_lazy_sort_order[from], &playlist_lazy_sort_order[from + 1],
                    (size_t) (playlist_count - from - 1) * sizeof(*playlist_lazy_sort_order));
            playlist_count--;
            free(playlist[from]);
            memmove(&playlist[from], &playlist[from + 1], (size_t) (playlist_count - from) * sizeof(*playlist));
            char ** paths_shrunk = realloc(playlist, (size_t) playlist_count * sizeof(*playlist));
            if (paths_shrunk || playlist_count == 0) playlist = paths_shrunk;
            int * shrunk = realloc(playlist_lazy_sort_order, (size_t) playlist_count * sizeof(*playlist_lazy_sort_order));
            if (shrunk || playlist_count == 0) playlist_lazy_sort_order = shrunk;
            if (from <= playlist_index + queued_pending_count) queued_pending_count--;
        } else {
            if (from < to) memmove(&playlist_lazy_sort_order[from], &playlist_lazy_sort_order[from + 1],
                                    (size_t) (to - from) * sizeof(*playlist_lazy_sort_order));
            else memmove(&playlist_lazy_sort_order[to + 1], &playlist_lazy_sort_order[to],
                          (size_t) (from - to) * sizeof(*playlist_lazy_sort_order));
            playlist_lazy_sort_order[to] = moved;
            if (from < to) memmove(&playlist[from], &playlist[from + 1], (size_t) (to - from) * sizeof(*playlist));
            else memmove(&playlist[to + 1], &playlist[to], (size_t) (from - to) * sizeof(*playlist));
            playlist[to] = moved_path;
            int pending_end = playlist_index + queued_pending_count;
            if (from <= pending_end && to > pending_end) queued_pending_count = to - playlist_index;
            else if (from > pending_end && to <= pending_end) queued_pending_count++;
        }
        queue_next_insert_index = queued_pending_count ? playlist_index + 1 + queued_pending_count : -1;
        arm_next_track_for_audio(playlist_index);
        return true;
    }
    char * path = playlist[from];
    int pending_end = playlist_index + queued_pending_count;
    if (to < 0) {
        queue_shuffle_remove(from);
        memmove(playlist + from, playlist + from + 1, (size_t) (playlist_count - from - 1) * sizeof(*playlist));
        free(path); playlist_count--;
        if (from <= pending_end) queued_pending_count--;
    } else {
        if (from < to) memmove(playlist + from, playlist + from + 1, (size_t) (to - from) * sizeof(*playlist));
        else memmove(playlist + to + 1, playlist + to, (size_t) (from - to) * sizeof(*playlist));
        playlist[to] = path;
        /* Crossing the priority boundary explicitly schedules that prefix. */
        if (from <= pending_end && to > pending_end) queued_pending_count = to - playlist_index;
        else if (from > pending_end && to <= pending_end) queued_pending_count++;
    }
    queue_next_insert_index = queued_pending_count ? playlist_index + 1 + queued_pending_count : -1;
    arm_next_track_for_audio(playlist_index);
    return true;
}

bool gui_player_queue_select(uint64_t revision, int index) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    if (revision != queue_revision || index < 0 || index >= playlist_count) return false;
    if (!queue_materialize_order()) return false;
    if (index > playlist_index && index <= playlist_index + queued_pending_count)
        queued_pending_count -= index - playlist_index;
    else if (index != playlist_index) queued_pending_count = 0;
    queue_next_insert_index = queued_pending_count ? index + 1 + queued_pending_count : -1;
    if (shuffle_order) shuffle_pos = index;
    play_track_at(index);
    return true;
}

/* Clears the ENTIRE queue -- played history and upcoming alike -- leaving
 * only the single track actively playing, at position 0. Not a full stop:
 * there is no "go idle, nothing loaded" path anywhere in this player (every
 * screen that starts playback always hands it a full playlist to build
 * one), and clearing the queue was never a request to stop the track
 * already sounding out of the speaker, only to empty everything around it
 * -- the same convention other media players' own "Clear queue" actions
 * follow for the same reason. queue_materialize_order() above already
 * normalizes playlist[]/shuffle_order[] to plain identity order against
 * playlist_index, so the only bookkeeping left here is collapsing both
 * down to that one surviving element. */
void gui_player_queue_clear_all(void) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    if (!gui_player_has_active_track()) return;
    if (playlist_lazy_sort_order) {
        const char * current_path = playlist_path_at(playlist_index);
        if (!current_path || !current_path[0]) return;
        char * keep = strdup(current_path);
        if (!keep) return;
        char ** replacement = calloc(1, sizeof(*replacement));
        if (!replacement) { free(keep); return; }
        replacement[0] = keep;
        for (int i = 0; i < playlist_count; i++) free(playlist[i]);
        free(playlist); playlist = replacement;
        free(playlist_lazy_sort_order); playlist_lazy_sort_order = NULL;
        if (playlist_lazy_snapshot) metadata_db_snapshot_close(playlist_lazy_snapshot);
        playlist_lazy_snapshot = NULL;
        if (playlist_lazy_file_index) file_browser_index_close(playlist_lazy_file_index);
        playlist_lazy_file_index = NULL;
        playlist_count = 1; playlist_index = 0;
        free(shuffle_order); shuffle_order = NULL; shuffle_order_count = 0;
        free(pending_shuffle_order); pending_shuffle_order = NULL;
        queued_pending_count = 0; queue_next_insert_index = -1;
        arm_next_track_for_audio(playlist_index);
        return;
    }
    if (!queue_materialize_order()) return;
    for (int i = 0; i < playlist_count; i++) {
        if (i != playlist_index) free(playlist[i]);
    }
    playlist[0] = playlist[playlist_index];
    playlist_count = 1;
    playlist_index = 0;
    if (shuffle_order) {
        shuffle_order[0] = 0;
        shuffle_order_count = 1;
        shuffle_pos = 0;
    }
    queued_pending_count = 0; queue_next_insert_index = -1;
    arm_next_track_for_audio(playlist_index);
}

void gui_player_queue_play_next(const char * path) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    int old_count = playlist_count;
    int old_insert = queue_next_insert_index;
    queue_next_insert_index = playlist_index + 1;
    queue_add_song(path);
    if (playlist_count == old_count) queue_next_insert_index = old_insert;
    else queue_next_insert_index = playlist_index + 1 + queued_pending_count;
}

typedef struct {
    int *order, *ranks;
    char **explicit_paths;
    metadata_db_snapshot_t *snapshot;
    file_browser_index_t *file_index;
    atomic_bool *cancel;
    char resolved[PATH_MAX];
} queue_save_provider_ctx_t;
static bool queue_save_path_provider(void * context, int index, const char ** path) {
    queue_save_provider_ctx_t * ctx = context;
    if (ctx->cancel && atomic_load(ctx->cancel)) return false;
    int physical = ctx->order[index];
    const char * p = ctx->explicit_paths[physical];
    if (!p && ctx->snapshot &&
        !metadata_db_snapshot_path_at(ctx->snapshot, ctx->ranks[physical],
                                      ctx->resolved, sizeof(ctx->resolved))) return false;
    if (!p && ctx->file_index &&
        !file_browser_index_playable_path_at(ctx->file_index, (unsigned) ctx->ranks[physical],
                                             ctx->resolved, sizeof(ctx->resolved))) return false;
    if (!p && !ctx->snapshot && !ctx->file_index) return false;
    if (!p) p = ctx->resolved;
    if (!p || !p[0] || remote_track_path_is_remote(p) ||
        strncmp(p, SUBSONIC_STREAM_CACHE_DIR, strlen(SUBSONIC_STREAM_CACHE_DIR)) == 0) return false;
    *path = p;
    return true;
}

typedef struct queue_save_worker_state {
    int dirfd, count;
    char name[NAME_MAX], leaf[NAME_MAX];
    queue_save_provider_ctx_t provider;
    atomic_bool done;
    atomic_bool cancel;
    bool ok;
    pthread_t thread;
} queue_save_worker_state_t;
static queue_save_worker_state_t *queue_save_worker;
static bool queue_save_active(void) { return queue_save_worker != NULL; }

static void *queue_save_worker_func(void *arg) {
    queue_save_worker_state_t *w = arg;
    w->ok = playlist_files_write_new_stream_at(w->dirfd, w->name, w->count,
                                               queue_save_path_provider, &w->provider,
                                               w->leaf, sizeof(w->leaf));
    atomic_store(&w->done, true);
    return NULL;
}
static void queue_save_cancel(void) {
    if (!queue_save_worker) return;
    atomic_store(&queue_save_worker->cancel, true);
    pthread_join(queue_save_worker->thread, NULL);
    if (queue_save_worker->dirfd >= 0) close(queue_save_worker->dirfd);
    if (queue_save_worker->provider.snapshot) metadata_db_snapshot_close(queue_save_worker->provider.snapshot);
    if (queue_save_worker->provider.file_index) file_browser_index_close(queue_save_worker->provider.file_index);
    for (int i = 0; i < queue_save_worker->count; i++) free(queue_save_worker->provider.explicit_paths[i]);
    free(queue_save_worker->provider.explicit_paths); free(queue_save_worker->provider.ranks);
    free(queue_save_worker->provider.order); free(queue_save_worker); queue_save_worker = NULL;
}

bool gui_player_queue_save_as(const char * name) {
    if (queue_save_worker) return false;
    int * order = NULL, count, current;
    uint64_t revision;
    if (!gui_player_queue_snapshot(&order, &count, &current, &revision)) return false;
    queue_save_worker_state_t *w = calloc(1, sizeof(*w));
    if (!w) { free(order); return false; }
    if (!name || !name[0] || strlen(name) >= sizeof(w->name)) { free(w); free(order); return false; }
    w->count = count; snprintf(w->name, sizeof(w->name), "%s", name);
    atomic_store(&w->cancel, false);
    w->provider.cancel = &w->cancel;
    w->provider.order = order; w->provider.ranks = calloc((size_t) count, sizeof(int));
    w->provider.explicit_paths = calloc((size_t) count, sizeof(char *));
    if (!w->provider.ranks || !w->provider.explicit_paths) { free(w->provider.ranks); free(w->provider.explicit_paths); free(order); free(w); return false; }
    for (int i = 0; i < count; i++) {
        w->provider.ranks[i] = playlist_lazy_sort_order ? playlist_lazy_sort_order[i] : -1;
        if (playlist[i]) {
            w->provider.explicit_paths[i] = strdup(playlist[i]);
            if (!w->provider.explicit_paths[i]) {
                for (int j = 0; j < i; j++) free(w->provider.explicit_paths[j]);
                free(w->provider.explicit_paths); free(w->provider.ranks); free(order); free(w); return false;
            }
        }
    }
    if (playlist_lazy_snapshot) {
        w->provider.snapshot = metadata_db_snapshot_retain(playlist_lazy_snapshot);
        if (!w->provider.snapshot) { for (int i = 0; i < count; i++) free(w->provider.explicit_paths[i]); free(w->provider.explicit_paths); free(w->provider.ranks); free(order); free(w); return false; }
    }
    if (playlist_lazy_file_index && !file_browser_index_retain(playlist_lazy_file_index, &w->provider.file_index)) {
        if (w->provider.snapshot) metadata_db_snapshot_close(w->provider.snapshot);
        for (int i = 0; i < count; i++) free(w->provider.explicit_paths[i]); free(w->provider.explicit_paths); free(w->provider.ranks); free(order); free(w); return false;
    }
    bool has_sd = false;
    for (int i = 0; i < count; i++) if (playlist[i] && is_sd_card_path(playlist[i])) has_sd = true;
    if (has_sd && queue_sd_dirfd < 0) {
        if (w->provider.snapshot) metadata_db_snapshot_close(w->provider.snapshot);
        if (w->provider.file_index) file_browser_index_close(w->provider.file_index);
        for (int i = 0; i < count; i++) free(w->provider.explicit_paths[i]);
        free(w->provider.explicit_paths); free(w->provider.ranks); free(order); free(w);
        return false;
    }
    if (queue_sd_dirfd >= 0) {
        int parent = openat(queue_sd_dirfd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent >= 0) (void) mkdirat(parent, "Playlists", 0755);
        w->dirfd = parent >= 0 ? openat(parent, "Playlists", O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
        if (parent >= 0) close(parent);
    } else w->dirfd = open(PLAYLISTS_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (w->dirfd < 0 || pthread_create(&w->thread, NULL, queue_save_worker_func, w) != 0) {
        if (w->dirfd >= 0) close(w->dirfd);
        if (w->provider.snapshot) metadata_db_snapshot_close(w->provider.snapshot);
        if (w->provider.file_index) file_browser_index_close(w->provider.file_index);
        for (int i = 0; i < count; i++) free(w->provider.explicit_paths[i]);
        free(w->provider.explicit_paths); free(w->provider.ranks); free(order); free(w); return false;
    }
    queue_save_worker = w;
    return true;
}

bool gui_player_queue_save_as_poll(bool *done, bool *ok) {
    if (!queue_save_worker) { if (done) *done = true; if (ok) *ok = false; return false; }
    if (!atomic_load(&queue_save_worker->done)) { if (done) *done = false; return true; }
    queue_save_worker_state_t *w = queue_save_worker;
    pthread_join(w->thread, NULL);
    struct stat job_dir, current_dir;
    bool same_destination = fstat(w->dirfd, &job_dir) == 0 &&
                            stat(PLAYLISTS_DIR, &current_dir) == 0 &&
                            job_dir.st_dev == current_dir.st_dev && job_dir.st_ino == current_dir.st_ino;
    bool cache_bound = false;
    if (same_destination) {
        int parent = openat(w->dirfd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent >= 0) { cache_bound = path_cache_bind_directory_fd(parent); close(parent); }
    }
    bool result = w->ok && same_destination && cache_bound && metadata_db_storage_current();
    if (result) { char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s", PLAYLISTS_DIR, w->leaf); metadata_db_playlist_insert_one(path); }
    if (done) *done = true;
    if (ok) *ok = result;
    if (w->provider.snapshot) metadata_db_snapshot_close(w->provider.snapshot);
    if (w->dirfd >= 0) close(w->dirfd);
    if (w->provider.file_index) file_browser_index_close(w->provider.file_index);
    for (int i = 0; i < w->count; i++) free(w->provider.explicit_paths[i]);
    free(w->provider.explicit_paths); free(w->provider.ranks); free(w->provider.order); free(w); queue_save_worker = NULL;
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
    if (playlist_index < 0) return;
    int next_idx = compute_manual_step_index(playlist_index, direction);
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
    return volume_slider ? lv_slider_get_value(volume_slider) : (int32_t)(audio_get_volume() * 100.0f);
}

void gui_player_set_volume_percent(int32_t percent) {
    /* External/hardware writes are newer than any coalesced drag sample. */
    hw_volume_coalesce_cancel(&volume_popup_hv);
    if (volume_slider) lv_slider_set_value(volume_slider, percent, LV_ANIM_OFF);
}

void gui_player_remember_volume_percent(int32_t percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
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
        int32_t percent = (int32_t) (volume * 100.0f + 0.5f);
        gui_player_set_volume_percent(percent);
        audio_set_volume(volume);
        refresh_volume_topbar(percent);
        settings_save_async(&current_settings);
    }
    gui_settings_sync_car_mode();
    gui_shell_refresh_quick_drawer_expansion_toggles();
}

bool gui_player_volume_is_being_adjusted(void) {
    return (volume_popup_track && lv_slider_is_dragged(volume_popup_track)) ||
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
    if (playlist_index < 0) return;
    reset_decoder_failure_tracking();
    int advanced_index = compute_auto_advance_index(playlist_index);
    if (advanced_index >= 0) {
        commit_auto_advance();
        on_track_auto_advanced(advanced_index);
    }
}

void gui_player_handle_track_finished(void) {
    if (playlist_index < 0) return;
    reset_decoder_failure_tracking();
    int finished_index = compute_auto_advance_index(playlist_index);
    if (finished_index >= 0) {
        commit_auto_advance();
        play_track_at_from_internal(finished_index, 0.0, false, false);
    } else {
        set_play_button_state(false);
    }
}

void gui_player_handle_playback_error_ex(audio_error_t err, uint64_t err_generation) {
    if (err == AUDIO_ERROR_NONE) return;
    if (playlist_index < 0) return;

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
        char failure_message[96];
        /* Record the failed track's physical path */
        const char * failed_path = playlist_path_at(playlist_index);
        if (failed_path) record_failed_physical_path(failed_path);

        consecutive_decoder_failure_skips++;
        int max_skips = (playlist_count < 5) ? playlist_count : 5;
        if (consecutive_decoder_failure_skips >= max_skips) {
            set_play_button_state(false);
            deferred_resume_pending = false;
            deferred_resume_position = 0.0;
            reset_decoder_failure_tracking();
            snprintf(failure_message, sizeof(failure_message), TR("Playback stopped: %s"),
                     TR(audio_error_description(err)));
            show_error_toast(failure_message);
            return;
        }

        if ((play_mode_t) current_settings.play_mode == PLAY_MODE_SHUFFLE) {
            ensure_shuffle_order_current();
            if (!pending_shuffle_order) {
                pending_shuffle_order = malloc(sizeof(int) * (size_t) playlist_count);
                if (pending_shuffle_order) {
                    memcpy(pending_shuffle_order, shuffle_order, sizeof(int) * (size_t) playlist_count);
                    fisher_yates_shuffle(pending_shuffle_order, playlist_count);
                }
            }
        }

        failure_advance_plan_t plan = compute_decoder_failure_advance_plan(
            playlist_index,
            playlist_count,
            (int) current_settings.play_mode,
            queued_pending_count,
            shuffle_order,
            shuffle_pos,
            pending_shuffle_order,
            is_failed_physical_track_cb,
            NULL
        );

        if (plan.target_index < 0) {
            set_play_button_state(false);
            deferred_resume_pending = false;
            deferred_resume_position = 0.0;
            reset_decoder_failure_tracking();
            snprintf(failure_message, sizeof(failure_message), TR("Playback stopped: %s"),
                     TR(audio_error_description(err)));
            show_error_toast(failure_message);
            return;
        }

        commit_decoder_failure_advance_plan(&plan);
        deferred_resume_pending = false;
        deferred_resume_position = 0.0;
        snprintf(failure_message, sizeof(failure_message), TR("Skipped: %s"), TR(audio_error_description(err)));
        show_error_toast(failure_message);
        play_track_at_from_internal(plan.target_index, 0.0, false, false);
    }
}

void gui_player_handle_playback_error(audio_error_t err) {
    gui_player_handle_playback_error_ex(err, 0);
}


void gui_player_sync_topbar_visibility(lv_obj_t * screen) {
    if (player_dismiss_btn) {
        if (player_lyrics_open || player_lyrics_animating ||
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
