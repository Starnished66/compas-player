#ifndef GUI_SHELL_INTERNAL_H
#define GUI_SHELL_INTERNAL_H

/* Internal to the gui_shell*.c files; never include from public headers. */

#include "gui_shell.h"
#include "../text/rendering/ui_label_text.h"
#include "../text/i18n/i18n.h"
#include "../../core/system/runtime/app_clock.h"
#include "../diagnostics/ui_frame_trace.h"
#include "../diagnostics/ui_snapshot_prefetch.h"
#include "../application/gui.h"
#include "../appearance/gui_theme.h"
#include "gui_notifications.h"
#include "../library/browse/gui_library.h"
#include "../../library/browser/files/file_browser.h"
#include "../player/screen/gui_queue.h"
#include "../player/screen/gui_player.h"
#include "../plugins/gui_plugins.h"
#include "../settings/gui_settings.h"
#include "../network/gui_network.h"
#include "../player/screen/gui_lyrics.h"
#include "../player/screen/gui_track_info.h"
#include "../text/rendering/gui_text_input.h"
#include "../text/rendering/gui_text_view.h"
#include "gui_navigation.h"
#include "../settings/lock/gui_lock_screen.h"
#include "../input/gesture_detector.h"
#include "../widgets/builders/screen_builders.h"
#include "../text/rendering/fallback_font.h"
#include "../effects/transition_compositor.h"
#include "src/core/lv_obj_draw_private.h"
#include "../../library/metadata/parsing/metadata.h"
#include "../../core/system/logging/db_log.h"
#include "../../audio/engine/audio.h"
#include "../input/hw_volume_coalesce.h"
#include "../../core/persistence/settings.h"
#include "../../core/persistence/bt_ssc_settings.h"
#include "../../core/persistence/bt_rc_trust_settings.h"
#include "../../core/themes/ui_customization.h"
#include "../assets/assets.h"
#include "../../hardware/device/device_config.h"
#include "../../hardware/power/battery.h"
#include "../../hardware/power/charge_limiter.h"
#include "../../network/wifi/wifi_status.h"
#include "../../network/wifi/wifi_control.h"
#include "../../network/radio/aic_radio.h"
#include "../../network/bluetooth/control/bluetooth_control.h"
#include "../../network/bluetooth/control/bt_ssc_policy.h"
#include "../../network/bluetooth/reconnect/bluetooth_reconnect.h"
#ifndef HOST_BUILD
#include "../../network/bluetooth/media/bt_media_player.h"
#endif
#include "../../hardware/usb/usb_audio_output.h"
#include "../../hardware/device/headphone_status.h"
#include "../../hardware/usb/usb_dac_bridge.h"
#include "../../hardware/usb/usb_mode_control.h"
#include "../../hardware/indicators/backlight.h"
#include "../../plugins/manager/plugin_manager.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

/* Macros shared by several files */
#if defined(BOARD_TEMPOTEC_V1)
/* V1 uses the same four-control Compas drawer, scaled to its 240x320 panel. */
#define QUICK_DRAWER_TOGGLE_ICON_PX BOARD_SCALE_PX(72)
#define QUICK_DRAWER_TOGGLE_CELL_W BOARD_SCALE_PX(112)
#define QUICK_DRAWER_TOGGLE_ROW_PITCH BOARD_SCALE_PY(245)
#define QUICK_DRAWER_CARD_H BOARD_SCALE_PY(322)
#define QUICK_DRAWER_ROW1_TOP BOARD_SCALE_PY(80)
#define QUICK_DRAWER_EXPANSION_MAX_HEIGHT BOARD_SCALE_PY(490)
#define QUICK_DRAWER_TOGGLE_STATE_GAP 20
#define QUICK_DRAWER_TOGGLE_CAPTION_GAP BOARD_SCALE_PY(5)
#endif
#if defined(BOARD_TEMPOTEC_V1)
#else
#define QUICK_DRAWER_TOGGLE_ICON_PX BOARD_SCALE_PX(84)
#define QUICK_DRAWER_TOGGLE_CELL_W BOARD_SCALE_PX(84)
#define QUICK_DRAWER_TOGGLE_ROW_PITCH BOARD_SCALE_PY(148)
#define QUICK_DRAWER_CARD_H BOARD_SCALE_PY(354)
#define QUICK_DRAWER_ROW1_TOP BOARD_SCALE_PY(71)
#define QUICK_DRAWER_EXPANSION_MAX_HEIGHT INT32_MAX
#define QUICK_DRAWER_TOGGLE_STATE_GAP BOARD_SCALE_PY(24)
#define QUICK_DRAWER_TOGGLE_CAPTION_GAP BOARD_SCALE_PY(2)
#endif
#define QUICK_DRAWER_TOGGLE_SLOTS (QD_TOGGLE_COUNT + PLUGIN_MAX_QUICK_TOGGLES)

/* ---- Expanded area ----------------------------------------------------
 * The handle under the first toggle row drags down to reveal two more rows.
 * Both slider rails shift down by exactly the revealed height, and the
 * now-playing card hides for the duration -- there is no room for both.
 *
 * The rows live inside quick_drawer_expansion_box, whose HEIGHT is the
 * animation: LVGL clips children to their parent, so growing the box from
 * zero wipes the rows into view without touching their own geometry. One
 * resize per frame, rather than fading ~18 objects individually -- a
 * subtree opacity would force a composited layer every frame, which this
 * file already avoids for drawer motion (see quick_drawer_begin_bitmap_
 * motion()'s own snapshot reasoning). */
/* A toggle row is ~123px of content (84px icon + name + state caption), so
 * a 148 pitch leaves ~25px between rows where 128 left ~5. The taller panel
 * pays for it: fully expanded, the volume rail now ends at 724 against a
 * panel bottom of 785. */
/* 231, not 219: row 1's state caption ends at ~219, so the old value butted
 * row 2 straight against it with no gap at all. */
/* The now-playing card. Its cover frame fills it, so both read these. */
#define QUICK_DRAWER_PANEL_X BOARD_SCALE_PX(19)
#define QUICK_DRAWER_PANEL_W BOARD_SCALE_PX(442)
/* Share one physical inset on all three edges, including after board scaling. */
#define QUICK_DRAWER_CARD_INSET (BOARD_SCALE_PX(34) - QUICK_DRAWER_PANEL_X)
#define QUICK_DRAWER_CARD_W (QUICK_DRAWER_PANEL_W - 2 * QUICK_DRAWER_CARD_INSET)
#if defined(BOARD_TEMPOTEC_V1)
#else
#define quick_drawer_toggle_column_x(column, columns) \
    ((columns) > 4 ? BOARD_SCALE_PX(18 + 90 * (column)) \
                   : BOARD_SCALE_PX(quick_drawer_column_label_x[column]))
#define quick_drawer_toggle_state_y(row_y) \
    ((row_y) + QUICK_DRAWER_TOGGLE_ICON_PX + QUICK_DRAWER_TOGGLE_STATE_GAP)
#endif

/* Types shared by several files */
typedef enum {
    BT_CODEC_TYPE_NONE = 0,
    BT_CODEC_TYPE_SBC,
    BT_CODEC_TYPE_AAC,
    BT_CODEC_TYPE_APTX,
    BT_CODEC_TYPE_APTX_HD,
    BT_CODEC_TYPE_LDAC,
    BT_CODEC_TYPE_SSC,
    BT_CODEC_TYPE_UAT,
    BT_CODEC_TYPE_COUNT
} bt_codec_type_t;

/* One background worker thread. `active` is owned by the UI thread; the
 * worker's last action is atomic_store_explicit(&job.done, true,
 * memory_order_release), which the UI thread's reap pairs with an acquire. */
typedef struct {
    pthread_t th;
    atomic_bool done;
    bool active;
} shell_job_t;

/* The drawer's four "on" toggle icons are a filled circle in the stock
 * #009FF6 with a near-white glyph on top. LVGL image_recolor mixes every
 * pixel toward the recolor colour, so gui_theme_accent_style() would tint
 * the glyph as well -- the same COVER-flattening problem gui_player.c
 * documents for btn_play.png/btn_pause.png. Rewrite the circle in decoded
 * pixels instead, exactly as recolor_play_btn_accent() does there: keep the
 * glyph (and every anti-aliased step toward it) by mixing the accent toward
 * white in proportion to how white the source pixel already was. A pixel's
 * minimum channel is that measure -- 0 across the saturated stock blue, 223+
 * inside the near-white glyph -- so the circle lands on the exact accent and
 * the glyph stays the glyph. */
typedef enum {
    QD_TOGGLE_WIFI = 0,
    QD_TOGGLE_BT,
    QD_TOGGLE_SLEEP,
    QD_TOGGLE_CROSSFADE,
    /* Row 2, revealed by the expansion handle -- see build_quick_drawer()'s
     * own comment on the expanded area. */
    QD_TOGGLE_AIRPLAY,
    QD_TOGGLE_BT_DAC,
    QD_TOGGLE_DLNA,
    QD_TOGGLE_GAPLESS,
    QD_TOGGLE_RC,
    QD_TOGGLE_CAR_MODE,
    QD_TOGGLE_COUNT
} qd_toggle_t;

/* Stateless helpers */
#if defined(BOARD_TEMPOTEC_V1)

/* Keep four predictable columns at every board size. V1 uses 48px cells
 * with 16px native gutters; the wider boards retain the measured positions
 * (the first four columns come from the table below). */
static inline int32_t quick_drawer_toggle_column_x(int column, int columns) {
    (void) columns;
    return BOARD_SCALE_PX(18 + 110 * column);
}
static inline int32_t quick_drawer_toggle_state_y(int32_t row_y) {
    return row_y + QUICK_DRAWER_TOGGLE_ICON_PX + QUICK_DRAWER_TOGGLE_STATE_GAP;
}
static inline int32_t quick_drawer_expansion_visible_height(int32_t content_height) {
    return content_height > QUICK_DRAWER_EXPANSION_MAX_HEIGHT
         ? QUICK_DRAWER_EXPANSION_MAX_HEIGHT : content_height;
}
#endif
static inline bool image_file_source_matches(const void * current, const void * requested) {
    if (current == requested) return true;
    if (!current || !requested ||
        lv_image_src_get_type(current) != LV_IMAGE_SRC_FILE ||
        lv_image_src_get_type(requested) != LV_IMAGE_SRC_FILE)
        return false;
    /* asset_path() interns these file names, so keeping the current pointer
     * when two paths compare equal preserves its lifetime as well. */
    return strcmp((const char *) current, (const char *) requested) == 0;
}

/* Clears done, starts fn(arg); active only if the thread was created. */
static inline bool shell_job_start(shell_job_t * job, void * (*fn)(void *), void * arg) {
    atomic_store_explicit(&job->done, false, memory_order_relaxed);
    job->active = pthread_create(&job->th, NULL, fn, arg) == 0;
    return job->active;
}

/* Joins a finished worker and clears active; false while still running or
 * not started. */
static inline bool shell_job_reap(shell_job_t * job) {
    if (!job->active || !atomic_load_explicit(&job->done, memory_order_acquire)) return false;
    pthread_join(job->th, NULL);
    job->active = false;
    return true;
}

/* Waits for a running worker (teardown only). */
static inline void shell_job_cancel(shell_job_t * job) {
    if (!job->active) return;
    pthread_join(job->th, NULL);
    job->active = false;
}

/* External declarations */
extern player_settings_t current_settings;
extern void player_transition_cache_async_cb(void * user_data);
extern void toggle_play_pause(void);

/* Declarations */

/* Forward declarations -- defined later in this file with player screen transport buttons. */
void prev_btn_event_cb(lv_event_t * e);
void play_btn_event_cb(lv_event_t * e);
void next_btn_event_cb(lv_event_t * e);

/* Shared state (owner file defines it) */
extern lv_obj_t * home_screen;
extern lv_obj_t * status_bar_band;
extern lv_obj_t * clock_topbar_group;
extern lv_obj_t * clock_topbar_digit[5];
extern lv_obj_t * clock_topbar_ampm;
extern lv_obj_t * volume_topbar_group;
extern lv_obj_t * volume_topbar_icon;
extern lv_obj_t * volume_topbar_digit[3];
extern lv_obj_t * volume_topbar_headphone;
extern lv_obj_t * quick_drawer_wifi_icon;
extern lv_obj_t * quick_drawer_bt_icon;
extern lv_obj_t * quick_drawer;
extern lv_obj_t * quick_drawer_brightness_icon;
extern asset_decoded_image_t quick_drawer_brightness_image;
extern lv_draw_buf_t * quick_drawer_motion_buf;
extern bool quick_drawer_bitmap_motion;
extern bool quick_drawer_direct_motion;
extern bool quick_drawer_snapshot_dirty;
extern bool quick_drawer_open;
extern bool quick_drawer_customization_reflow_pending;
extern lv_obj_t * quick_drawer_brightness_track;
extern lv_obj_t * quick_drawer_brightness_label;
extern lv_timer_t * brightness_hw_apply_timer;
extern lv_obj_t * quick_drawer_title_label;
extern lv_obj_t * quick_drawer_artist_label;
extern lv_obj_t * quick_drawer_album_label;
extern lv_obj_t * quick_drawer_format_label;
extern lv_obj_t * quick_drawer_cover_img;
extern lv_obj_t * quick_drawer_cover_frame;
extern lv_obj_t * quick_drawer_play_btn;
extern lv_obj_t * quick_drawer_volume_container;
#if defined(BOARD_TEMPOTEC_V1)
extern lv_obj_t * quick_drawer_brightness_container;
#endif
extern lv_obj_t * quick_drawer_volume_icon;
extern lv_obj_t * quick_drawer_eq_button;
extern lv_obj_t * quick_drawer_volume_track;
extern lv_obj_t * quick_drawer_volume_label;
extern lv_obj_t * quick_drawer_toggle_state[QUICK_DRAWER_TOGGLE_SLOTS];
extern lv_color_t shell_drawer_off_color;
extern lv_obj_t * quick_drawer_toggle_icon[QUICK_DRAWER_TOGGLE_SLOTS];
extern lv_obj_t * quick_drawer_toggle_name[QUICK_DRAWER_TOGGLE_SLOTS];
#if defined(BOARD_TEMPOTEC_V1)
#else
extern const int quick_drawer_column_label_x[];
#endif
extern lv_obj_t * quick_drawer_expansion_box;
extern lv_obj_t * quick_drawer_expansion_handle;
extern lv_obj_t * quick_drawer_card;
extern lv_obj_t * quick_drawer_airplay_icon;
extern lv_obj_t * quick_drawer_bt_dac_icon;
extern lv_obj_t * quick_drawer_dlna_icon;
extern lv_obj_t * quick_drawer_gapless_icon;
extern lv_obj_t * quick_drawer_rc_icon;
extern lv_obj_t * quick_drawer_car_mode_icon;
extern lv_obj_t * quick_drawer_plugin_icon[PLUGIN_MAX_QUICK_TOGGLES];
extern int quick_drawer_plugin_toggle_count;
extern int32_t quick_drawer_expansion_full;
extern int32_t quick_drawer_expansion_y;
extern bool quick_drawer_expanded;
extern bool quick_drawer_expansion_motion;
extern lv_timer_t * quick_drawer_expansion_start_timer;
extern int quick_drawer_shift_count;
extern lv_obj_t * battery_topbar_group;
extern lv_obj_t * battery_icon_frame;
extern lv_obj_t * wifi_icon;
extern lv_obj_t * bt_status_icon;
extern lv_obj_t * a2dp_status_icon;
extern lv_obj_t * usb_audio_status_icon;
extern lv_obj_t * play_pause_status_icon;
extern lv_obj_t * bt_codec_status_icon;
extern lv_style_t shell_drawer_bg_style, shell_drawer_panel_style, shell_drawer_row_style,
    shell_drawer_card_style, shell_drawer_handle_style, shell_topbar_glyph_style;
extern bool shell_theme_native;
extern lv_color_t shell_bar_color, shell_bar_glyph_color, shell_drawer_surface;
#if defined(BOARD_TEMPOTEC_V1)
extern int32_t topbar_v1_art_scale_percent;
extern bool topbar_v1_layout_signature_valid;
#endif
extern bool bt_startup_ready;
extern bool bt_is_a2dp_connected_ui;
extern lv_obj_t * quick_drawer_crossfade_icon;
extern bool sleep_timer_active;
extern lv_obj_t * quick_drawer_sleep_icon;

/* Shared functions (owner file defines them) */
void refresh_quick_drawer_brightness(void);
void quick_drawer_size_toggle_caption(lv_obj_t * label);
void quick_drawer_register_shift_obj(lv_obj_t * obj, int32_t base_y);
void quick_drawer_apply_expansion(int32_t y);
void quick_drawer_release_motion_cache(void);
void quick_drawer_prepare_motion_cache(void);
void quick_drawer_begin_expansion_composition(void);
void quick_drawer_expansion_anim_cb(void * var, int32_t v);
void quick_drawer_cancel_expansion_start(void);
void quick_drawer_animate_expansion(bool expand);
void quick_drawer_reset_expansion(void);
void quick_drawer_expansion_handle_cb(lv_event_t * e);
void quick_drawer_volume_event_cb(lv_event_t * e);
void quick_drawer_refresh_volume(void);
void quick_drawer_set_toggle_state_text(int index, bool enabled, const char * text);
void quick_drawer_set_toggle_state(int index, bool enabled);
void quick_drawer_fit_cover(void);
const char * bt_codec_get_asset(bt_codec_type_t type);
void shell_theme_styles_init(void);
lv_color_t shell_drawer_on_color(void);
void shell_set_topbar_glyphs(lv_color_t color, bool recolor);
void topbar_add_glyph_style(lv_obj_t * parent);
void quick_drawer_retint_toggle_states(lv_color_t old_off);
void style_topbar_text(lv_obj_t * label, const lv_font_t * font);
const lv_font_t * topbar_digit_font(void);
const lv_font_t * topbar_ampm_font(void);
const char * topbar_icon(const char * name);
void layout_battery_topbar_image(lv_obj_t * image);
void layout_volume_topbar_image(lv_obj_t * image);
void layout_headphone_topbar_image(lv_obj_t * image);
void layout_a2dp_topbar_image(lv_obj_t * image);
void layout_usb_audio_topbar_image(lv_obj_t * image);
void layout_play_pause_topbar_image(lv_obj_t * image);
void layout_radio_topbar_image(lv_obj_t * image);
void layout_bluetooth_topbar_image(lv_obj_t * image);
void layout_wifi_connected_topbar_image(lv_obj_t * image);
void layout_codec_topbar_image(lv_obj_t * image);
void build_status_bar(void);
bool topbar_local_style_is(lv_obj_t * obj, lv_style_prop_t prop, int32_t value);
bool topbar_set_image(lv_obj_t * image, const char * src);
void refresh_play_pause_topbar(void);
void quick_drawer_sync_volume(int32_t percent);
void refresh_headphone_icon(void);
void apply_topbar_visibility_preferences(void);
int topbar_clock_position(void);
void reflow_topbar_layout(void);
void sync_topbar_status_icon_positions(void);
void load_quick_drawer_toggle_images(void);
const void * quick_drawer_toggle_src(qd_toggle_t t, bool on);
const void * quick_drawer_plugin_toggle_src(int index, bool on);
bool quick_drawer_set_toggle_image(lv_obj_t * image, const void * source);
bool quick_drawer_apply_toggle_image(qd_toggle_t id, lv_obj_t * icon, bool on);
void quick_drawer_layout_transport_icon(lv_obj_t * image, int32_t reference_x);
void quick_drawer_dismiss_for_navigation(void);
void poll_wifi_status(void);
void refresh_wifi_icon(void);
void poll_wifi_status_screen_off(void);
void invalidate_bt_codec_status_cache(void);
void start_refresh_bt_icon(void);
void sync_bt_codec_status_icon(void);
void poll_bt_monitor_lifecycle(void);
void poll_refresh_bt_icon(void);
void poll_usb_audio_output(void);
void quick_drawer_crossfade_event_cb(lv_event_t * e);
void quick_drawer_sleep_event_cb(lv_event_t * e);
void poll_sleep_timer(void);
void quick_drawer_anim_y_cb(void * var, int32_t v);
int32_t quick_drawer_motion_y(void);
void quick_drawer_rebuild_snapshot(void);
bool quick_drawer_begin_bitmap_motion(void);
void quick_drawer_finish_bitmap_motion(void);
#if defined(BOARD_TEMPOTEC_V1)
void quick_drawer_expansion_scroll_event_cb(lv_event_t * e);
#endif
void quick_drawer_wifi_long_press_cb(lv_event_t * e);
void quick_drawer_bt_long_press_cb(lv_event_t * e);
void quick_drawer_wifi_pressed_cb(lv_event_t * e);
void quick_drawer_bt_pressed_cb(lv_event_t * e);
void poll_wifi_toggle(void);
void poll_bt_toggle(void);
void start_bt_dac_startup_reapply_if_needed(void);
void poll_bt_dac_startup_reapply(void);
void start_bt_source_codec_reconcile_if_needed(void);
void poll_bt_source_codec_reconcile(void);
void poll_bt_reconnect(void);
void poll_bt_apply_output_settings(void);
void poll_bt_dac_session(void);
void brightness_hw_apply_timer_cb(lv_timer_t * timer);
void quick_drawer_brightness_changed_cb(lv_event_t * e);
void quick_drawer_make_toggle_labels(int slot, int32_t x, int32_t row_y,
                                            const char * name_text);
void build_quick_drawer(void);
bool quick_drawer_customization_enabled(void);
void quick_drawer_reflow_customization(void);
void topbar_load_volume_warn_threshold(void);
void quick_drawer_motion_teardown_cancel(void);
void quick_drawer_brightness_teardown_flush(void);
void quick_drawer_motion_teardown_image(void);
void quick_drawer_volume_teardown_flush(void);
void quick_drawer_motion_teardown_state(void);
void quick_drawer_teardown_widgets(void);
void topbar_layout_teardown(void);
void topbar_teardown(void);
void quick_drawer_poll_snapshot_wake(void);
#ifndef HOST_BUILD
void wifi_resume(bool wifi_was_on);
void bt_resume(bool bt_was_on);
void wifi_suspend_launch(bool wifi_was_on);
void bt_suspend_launch(bool bt_was_on);
#endif
void gui_shell_gestures_start(void);
void gui_shell_gestures_reset_expansion_flags(void);
/* Swipe gestures (gui_shell_swipes.c), polled in order by
 * gui_shell_gestures.c's raw-touch timer. */
/* Swipe-up-to-Home tracking -- same live per-tick overlay as player_swipe_*
 * below, but vertical and sliding up from the bottom edge. Eligibility
 * (band/overlay/lyrics/lock-screen exclusions) is still gesture_detector.h's
 * gesture_home_state_is_eligible() -- gui_lock_screen.c's own independent
 * swipe-up-to-dismiss still uses gesture_home_state_poll()/state_t directly,
 * so that machinery stays in place; this only replaces how gui_shell_gestures.c
 * itself consumes eligibility, trading a fixed post-threshold instant cut
 * for the same candidate/tracking live-drag shape as the other two gestures. */
typedef struct {
    bool candidate;
    bool tracking;
    /* One-tick present deferral. UI_PERF_TRACE showed begin_slide_transition_ex()
     * (17-34ms) and the first overlay frame's present (~16ms) landing in the
     * same poll tick as the deadzone confirm, a 33-50ms stall right as the
     * gesture starts. Set only at the instant tracking begins; the tracking
     * update clears it and skips presenting that tick, deferring frame 0. */
    bool just_confirmed;
    int32_t start_x, start_y;
    int32_t last_v;        /* last sampled offset, for per-tick velocity */
    int32_t last_velocity; /* sticky: last nonzero per-tick delta */
    slide_transition_ctx_t * ctx;
} swipe_state_t;
extern swipe_state_t home_swipe, player_swipe, back_swipe;
extern bool back_swipe_owns_press;
/* Drawer-drag press state owned by gui_shell_gestures.c, read by the swipes. */
extern bool quick_drawer_drag_tracking;
extern bool quick_drawer_was_pressed;
bool player_swipe_press_excluded(lv_point_t p);
bool shell_screen_blocks_swipe(lv_obj_t * screen);
void gui_shell_swipes_poll_press(lv_point_t p, int32_t h);
void gui_shell_swipes_poll_deadzones(lv_indev_t * indev, lv_point_t p, bool pressed);
void gui_shell_swipes_poll_tracking(lv_point_t p, int32_t h, bool pressed);
void gui_shell_swipes_poll_release(int32_t h, bool pressed);
void gui_shell_swipes_clear_candidates(void);
void gui_shell_swipes_reset_home(void);
void gui_shell_swipes_reset_player_back(void);
bool gui_shell_swipes_busy(void);
bool bt_workers_active(void);
bool wifi_background_active(void);
bool bt_monitor_or_reconnect_active(void);
void bt_background_cancel_monitors(void);
void wifi_background_cancel_status(void);
void bt_background_cancel_toggle(void);
void wifi_background_cancel_toggle(void);
void bt_background_cancel_workers(void);

#endif /* GUI_SHELL_INTERNAL_H */
