#include "player_volume_popup.h"
#include "../../application/gui.h"
#include "../screen/gui_player.h"
#include "../../shell/gui_shell.h"
#include "../../appearance/gui_theme.h"
#include "../../assets/assets.h"
#include "../../../audio/engine/audio.h"
#include "../../text/rendering/fallback_font.h"
#include "../../input/hw_volume_coalesce.h"
#include "../layout/player_layout_internal.h"
#include "../../../core/persistence/settings.h"

extern lv_obj_t * volume_slider;

lv_obj_t * volume_popup = NULL;
lv_obj_t * volume_popup_backdrop = NULL;

static lv_obj_t * volume_popup_track = NULL;
static lv_obj_t * volume_popup_speaker_icon = NULL;
static lv_obj_t * volume_popup_value_label = NULL;
static lv_timer_t * volume_popup_hide_timer = NULL;
static asset_decoded_image_t volume_popup_speaker_image;

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
    lv_area_increase(&area, 24, 24); /* matches build_volume_popup()'s hit area */
    return point.x >= area.x1 && point.x <= area.x2 &&
           point.y >= area.y1 && point.y <= area.y2;
}

/* Same card as the quick drawer's volume row (gui_shell.c): 413x73 rounded
 * panel, speaker icon, rail and a right-aligned numeric value, so the
 * hardware-button indicator reads exactly like the drawer's control. */
void player_volume_popup_init(void) {
    lv_obj_t * top = lv_layer_top();

    volume_popup = lv_obj_create(top);
    lv_obj_remove_style_all(volume_popup);
    lv_obj_set_size(volume_popup, BOARD_SCALE_PX(413), BOARD_SCALE_PY(73));
    lv_obj_align(volume_popup, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + BOARD_SCALE_PX(12));
    lv_obj_add_style(volume_popup, gui_shell_drawer_row_style(), 0);
    lv_obj_set_style_radius(volume_popup, BOARD_SCALE_PX(23), 0);
    lv_obj_remove_flag(volume_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(volume_popup, LV_OBJ_FLAG_HIDDEN);

    volume_popup_speaker_icon = lv_image_create(volume_popup);
    const void * speaker = asset_decoded_image_open(&volume_popup_speaker_image, "volume/vol.png")
                         ? asset_decoded_image_source(&volume_popup_speaker_image) : NULL;
    lv_image_set_src(volume_popup_speaker_icon, speaker ? speaker : asset_path("volume/vol.png"));
    lv_obj_add_style(volume_popup_speaker_icon, &style_theme_text_primary, 0);
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
    if (volume_popup_hide_timer && !dragged) {
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
    volume_popup_speaker_icon = NULL;
    volume_popup_value_label = NULL;
    asset_decoded_image_close(&volume_popup_speaker_image);
    volume_popup_track = NULL;
}

void player_volume_popup_refresh_assets(void) {
    if (!volume_popup) return;
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
