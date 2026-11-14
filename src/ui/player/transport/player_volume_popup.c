#include "player_volume_popup.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../application/gui.h"
#include "../screen/gui_player.h"
#include "../../shell/gui_shell.h"
#include "../../network/gui_network.h"
#include "../../appearance/gui_theme.h"
#include "../../assets/assets.h"
#include "../../../audio/engine/audio.h"
#include "../../text/rendering/fallback_font.h"
#include "../../input/hw_volume_coalesce.h"
#include "../layout/player_layout_internal.h"
#include "../../../core/persistence/settings.h"
#include <string.h>

extern lv_obj_t * volume_slider;

lv_obj_t * volume_popup = NULL;
lv_obj_t * volume_popup_backdrop = NULL;

static lv_obj_t * volume_popup_track = NULL;
static lv_obj_t * volume_popup_output_button = NULL;
static lv_obj_t * volume_popup_output_icon = NULL;
static lv_obj_t * volume_popup_speaker_icon = NULL;
static lv_obj_t * volume_popup_value_label = NULL;
static lv_timer_t * volume_popup_hide_timer = NULL;
static asset_decoded_image_t volume_popup_speaker_image;
static const char * volume_popup_output_src = NULL;

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

static const char * volume_popup_output_icon_src(void) {
    return gui_shell_output_icon_src(topbar_icon("headphones"));
}

static void volume_popup_refresh_output_icon(void) {
    const char * src = volume_popup_output_icon_src();
    if (volume_popup_output_icon && volume_popup_output_src != src &&
        (!volume_popup_output_src || !src || strcmp(volume_popup_output_src, src) != 0)) {
        lv_image_set_src(volume_popup_output_icon, src);
        volume_popup_output_src = src;
    }
}

static void volume_popup_output_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_timer_pause(volume_popup_hide_timer);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_timer_reset(volume_popup_hide_timer);
        lv_timer_resume(volume_popup_hide_timer);
    } else if (code == LV_EVENT_CLICKED) {
        volume_popup_hide_timer_cb(NULL);
        gui_network_bt_open_output_menu();
    }
}

/* hw_volume_coalesce.h -- extracted from what used to be this file's own
 * private volume_hw_pending/volume_hw_apply_timer/volume_drag_active
 * machinery, so the quick drawer's own volume slider (gui_shell_drawer.c) can
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
        percent = audio_clamp_volume_percent((int) percent);
        if (percent != lv_slider_get_value(volume_popup_track))
            lv_slider_set_value(volume_popup_track, percent, LV_ANIM_OFF);
        hw_volume_coalesce_drag_update(&volume_popup_hv, (int) percent);
        if (volume_popup_value_label) lv_label_set_text_fmt(volume_popup_value_label, "%d", (int) percent);
        refresh_volume_topbar(percent);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        percent = audio_clamp_volume_percent((int) percent);
        if (percent != lv_slider_get_value(volume_popup_track))
            lv_slider_set_value(volume_popup_track, percent, LV_ANIM_OFF);
        percent = hw_volume_coalesce_drag_end(&volume_popup_hv, (int) percent);
        if (volume_popup_value_label) lv_label_set_text_fmt(volume_popup_value_label, "%d", (int) percent);
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
    area.x1 -= 24;
    area.x2 += 24;
    area.y1 -= 24;
    area.y2 += 24;
    if (point.x >= area.x1 && point.x <= area.x2 &&
        point.y >= area.y1 && point.y <= area.y2) return true;
    if (!volume_popup_output_button) return false;
    lv_obj_get_coords(volume_popup_output_button, &area);
    return point.x >= area.x1 && point.x <= area.x2 &&
           point.y >= area.y1 && point.y <= area.y2;
}

/* Widths follow the panel width, heights the panel height, so the column
 * keeps the same share of the screen on every board. */
#define VOLUME_PILL_W BOARD_SCALE_PX(84)
#define VOLUME_PILL_H BOARD_SCALE_PY(405)
#define VOLUME_BUTTON BOARD_SCALE_PX(76)
#define VOLUME_GAP BOARD_SCALE_PY(12)
#define VOLUME_CLICK_EXT 24

/* The pill and Output button share one transparent top-layer column. */
void player_volume_popup_init(void) {
    volume_popup = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(volume_popup);
    lv_obj_set_size(volume_popup, VOLUME_PILL_W + 2 * VOLUME_CLICK_EXT,
                    VOLUME_PILL_H + VOLUME_GAP + VOLUME_BUTTON + 2 * VOLUME_CLICK_EXT);
    lv_obj_align(volume_popup, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(14) + VOLUME_CLICK_EXT, BOARD_SCALE_PY(10));
    lv_obj_remove_flag(volume_popup, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(volume_popup, LV_OBJ_FLAG_HIDDEN);

    const void * speaker = asset_decoded_image_open(&volume_popup_speaker_image, "volume/vol.png")
                         ? asset_decoded_image_source(&volume_popup_speaker_image) : NULL;
    fill_slider_t ui = build_fill_slider(volume_popup, &(fill_slider_cfg_t){
        .width = VOLUME_PILL_W, .height = VOLUME_PILL_H, .radius = BOARD_SCALE_PX(28),
        .max = 100, .value = gui_player_get_volume_percent(),
        .icon_src = speaker ? speaker : asset_path("volume/vol.png"),
        .value_format = "%d", .event_cb = volume_popup_track_event_cb
    });
    volume_popup_track = ui.slider;
    lv_obj_set_ext_click_area(volume_popup_track, VOLUME_CLICK_EXT);
    volume_popup_value_label = ui.label;
    volume_popup_speaker_icon = ui.icon;
    lv_obj_align(volume_popup_track, LV_ALIGN_TOP_MID, 0, VOLUME_CLICK_EXT);
    player_fit_small_screen_icon(volume_popup_speaker_icon, 30);

    volume_popup_output_button = lv_obj_create(volume_popup);
    lv_obj_remove_style_all(volume_popup_output_button);
    lv_obj_set_size(volume_popup_output_button, VOLUME_BUTTON, VOLUME_BUTTON);
    lv_obj_add_style(volume_popup_output_button, gui_shell_drawer_row_style(), 0);
    lv_obj_set_style_radius(volume_popup_output_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(volume_popup_output_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align_to(volume_popup_output_button, volume_popup_track, LV_ALIGN_OUT_BOTTOM_MID, 0, VOLUME_GAP);
    lv_obj_add_event_cb(volume_popup_output_button, volume_popup_output_event_cb, LV_EVENT_ALL, NULL);
    volume_popup_output_icon = lv_image_create(volume_popup_output_button);
    volume_popup_output_src = NULL;
    volume_popup_refresh_output_icon();
    lv_obj_add_style(volume_popup_output_icon, &style_theme_text_primary, 0);
    /* 28 px art drawn at 42 px on the 480-wide reference, smaller on narrower panels. */
    lv_image_set_scale(volume_popup_output_icon,
                       LV_SCALE_NONE * 3 * BOARD_SCREEN_WIDTH / (2 * BOARD_REFERENCE_WIDTH));
    lv_obj_center(volume_popup_output_icon);

    volume_popup_hide_timer = lv_timer_create(volume_popup_hide_timer_cb, 1500, NULL);
    lv_timer_pause(volume_popup_hide_timer);
    /* hw_volume_coalesce_drag_begin() creates its own timer lazily on the
     * first drag -- no eager creation needed here. */
}

void show_volume_popup(int32_t percent) {
    percent = audio_clamp_volume_percent(percent);
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
    /* Keep modal surfaces and the moving drawer above the pill. */
    if (!gui_popup_backdrop_visible() && !gui_shell_quick_drawer_visible())
        lv_obj_move_foreground(volume_popup);
    volume_popup_refresh_output_icon();
    if (volume_popup_hide_timer && !dragged &&
        !lv_obj_has_state(volume_popup_track, LV_STATE_PRESSED) &&
        !lv_obj_has_state(volume_popup_output_button, LV_STATE_PRESSED)) {
        lv_timer_reset(volume_popup_hide_timer);
        lv_timer_resume(volume_popup_hide_timer);
    }
}

void player_volume_popup_teardown(void) {
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
    volume_popup_output_button = NULL;
    volume_popup_output_icon = NULL;
    volume_popup_output_src = NULL;
    volume_popup_speaker_icon = NULL;
    volume_popup_value_label = NULL;
    asset_decoded_image_close(&volume_popup_speaker_image);
    volume_popup_track = NULL;
}

void player_volume_popup_refresh_assets(void) {
    if (!volume_popup) return;
    volume_popup_refresh_output_icon();
    asset_decoded_image_close(&volume_popup_speaker_image);
    const void * speaker = asset_decoded_image_open(&volume_popup_speaker_image, "volume/vol.png")
                         ? asset_decoded_image_source(&volume_popup_speaker_image) : NULL;
    if (volume_popup_speaker_icon)
        lv_image_set_src(volume_popup_speaker_icon, speaker ? speaker : asset_path("volume/vol.png"));
}

void player_volume_popup_cancel_pending(void) {
    hw_volume_coalesce_cancel(&volume_popup_hv);
}

bool player_volume_popup_is_dragged(void) {
    return volume_popup_track && lv_slider_is_dragged(volume_popup_track);
}
