#include "player_seekbar.h"
#include "lvgl/src/core/lv_obj_event_private.h"

#include "board_config.h"
#include "gui_navigation.h"
#include "gui_player.h"
#include "gui_theme.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    SEEKBAR_BARS,
    SEEKBAR_ENVELOPE,
    SEEKBAR_HALF,
} seekbar_style_t;

typedef struct {
    seekbar_style_t style;
    lv_color_t rail_color;
    lv_opa_t rail_opa;
    uint8_t bins[WAVEFORM_BINS];
    bool ready;
} seekbar_ctx_t;

typedef struct {
    int32_t previous_angle;
    int32_t unwrapped_angle;
} arc_pointer_ctx_t;

static bool is_slider(const lv_obj_t * obj)
{
    return obj && lv_obj_check_type(obj, &lv_slider_class);
}

static bool is_arc(const lv_obj_t * obj)
{
    return obj && lv_obj_check_type(obj, &lv_arc_class);
}

bool player_seekbar_is_supported(lv_obj_t * obj)
{
    return is_slider(obj) || is_arc(obj);
}

int32_t player_seekbar_get_value(lv_obj_t * obj)
{
    if (is_slider(obj)) return lv_slider_get_value(obj);
    if (is_arc(obj)) return lv_arc_get_value(obj);
    return 0;
}

void player_seekbar_set_value(lv_obj_t * obj, int32_t value)
{
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    if (is_slider(obj)) lv_slider_set_value(obj, value, LV_ANIM_OFF);
    else if (is_arc(obj)) lv_arc_set_value(obj, value);
}

static void arc_pointer_delete(lv_event_t * event)
{
    if (lv_event_get_code(event) == LV_EVENT_DELETE) free(lv_event_get_user_data(event));
}

static bool arc_is_full_circle(lv_obj_t * arc)
{
    return lv_arc_get_mode(arc) == LV_ARC_MODE_NORMAL &&
           lv_arc_get_bg_angle_start(arc) == 0 && lv_arc_get_bg_angle_end(arc) == 360;
}

static void arc_pointer_event(lv_event_t * event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_HIT_TEST) {
        lv_obj_t * arc = lv_event_get_target(event);
        lv_hit_test_info_t * info = lv_event_get_param(event);
        if (arc && info && info->point && arc_is_full_circle(arc) &&
            lv_screen_active() == gui_player_get_screen() &&
            gui_navigation_get_depth() > 1 &&
            info->point->x < BOARD_SCALE_PX(48)) {
            /* Keep the screen's left-edge back-swipe gutter available even
             * where it overlaps the seek ring. LVGL's own arc hit test runs
             * first, so the annulus and cover-hole behavior remain intact
             * everywhere else. */
            info->res = false;
        }
        return;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;
    lv_obj_t * arc = lv_event_get_target(event);
    arc_pointer_ctx_t * ctx = lv_event_get_user_data(event);
    if (!arc || !ctx || !arc_is_full_circle(arc)) return;
    lv_indev_t * indev = lv_indev_active();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);
    /* Match LVGL's arc geometry, including asymmetric main-part padding. */
    int32_t left_pad = lv_obj_get_style_pad_left(arc, LV_PART_MAIN);
    int32_t right_pad = lv_obj_get_style_pad_right(arc, LV_PART_MAIN);
    int32_t top_pad = lv_obj_get_style_pad_top(arc, LV_PART_MAIN);
    int32_t bottom_pad = lv_obj_get_style_pad_bottom(arc, LV_PART_MAIN);
    int32_t radius = LV_MIN(lv_obj_get_width(arc) - left_pad - right_pad,
                            lv_obj_get_height(arc) - top_pad - bottom_pad) / 2;
    lv_area_t bounds;
    lv_obj_get_coords(arc, &bounds);
    int32_t x = point.x - (bounds.x1 + radius + left_pad);
    int32_t y = point.y - (bounds.y1 + radius + top_pad);
    if (x == 0 && y == 0) return;
    int32_t angle = (int32_t)lv_atan2(y, x) - lv_arc_get_rotation(arc);
    angle %= 360;
    if (angle < 0) angle += 360;
    if (code == LV_EVENT_PRESSED) {
        ctx->unwrapped_angle = angle;
    }
    else {
        int32_t delta = angle - ctx->previous_angle;
        if (delta > 180) delta -= 360;
        else if (delta < -180) delta += 360;
        ctx->unwrapped_angle += delta;
    }
    ctx->previous_angle = angle;
    /* Keep the physical angle through the seam, clamping only its display. */
    int32_t display_angle = ctx->unwrapped_angle;
    if (display_angle < 0) display_angle = 0;
    if (display_angle > 360) display_angle = 360;
    player_seekbar_set_value(arc, (display_angle * 100 + 180) / 360);
}

bool player_seekbar_configure(lv_obj_t * obj)
{
    if (is_slider(obj)) {
        lv_slider_set_range(obj, 0, 100);
        return true;
    }
    if (!is_arc(obj)) return false;

    arc_pointer_ctx_t * ctx = NULL;
    uint32_t event_count = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < event_count; ++i) {
        lv_event_dsc_t * dsc = lv_obj_get_event_dsc(obj, i);
        if (lv_event_dsc_get_cb(dsc) == arc_pointer_event) {
            ctx = lv_event_dsc_get_user_data(dsc);
            break;
        }
    }
    if (!ctx) {
        ctx = calloc(1, sizeof(*ctx));
        if (!ctx) return false;
        lv_obj_add_event_cb(obj, arc_pointer_event, LV_EVENT_PRESSED, ctx);
        lv_obj_add_event_cb(obj, arc_pointer_event, LV_EVENT_PRESSING, ctx);
        lv_obj_add_event_cb(obj, arc_pointer_event, LV_EVENT_HIT_TEST, ctx);
        lv_obj_add_event_cb(obj, arc_pointer_delete, LV_EVENT_DELETE, ctx);
    }
    lv_arc_set_range(obj, 0, 100);
    /* Arc hit testing without ADV_HITTEST intercepts taps inside the ring's
     * hole. Its precise test rejects the hole so the cover beneath gets them. */
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_ADV_HITTEST |
                         LV_OBJ_FLAG_PRESS_LOCK);
    return true;
}

static lv_opa_t multiply_opa(lv_opa_t a, lv_opa_t b)
{
    return (lv_opa_t)(((uint16_t)a * b + 127u) / 255u);
}

static void draw_rect(lv_layer_t * layer, lv_area_t * area, lv_color_t color,
                      lv_opa_t opa, int32_t radius)
{
    if (opa == LV_OPA_TRANSP || area->x1 > area->x2 || area->y1 > area->y2) return;
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = opa;
    dsc.radius = radius;
    lv_draw_rect(layer, &dsc, area);
}

static int32_t slider_progress_x(const lv_obj_t * slider, const lv_area_t * bounds)
{
    int32_t min_value = lv_slider_get_min_value(slider);
    int32_t max_value = lv_slider_get_max_value(slider);
    int32_t span = max_value - min_value;
    if (span <= 0) return bounds->x1;
    int32_t value = lv_slider_get_value(slider) - min_value;
    if (value < 0) value = 0;
    if (value > span) value = span;
    return bounds->x1 + (int64_t)(lv_area_get_width(bounds) - 1) * value / span;
}

static void seekbar_draw(lv_event_t * event)
{
    lv_obj_t * slider = lv_event_get_target(event);
    seekbar_ctx_t * ctx = lv_event_get_user_data(event);
    if (!slider || !ctx || lv_event_get_code(event) != LV_EVENT_DRAW_MAIN_END) return;

    lv_area_t bounds;
    lv_obj_get_coords(slider, &bounds);
    int32_t width = lv_area_get_width(&bounds);
    int32_t height = lv_area_get_height(&bounds);
    if (width < 4 || height < 6) return;

    lv_layer_t * layer = lv_event_get_layer(event);
    /* LVGL draws an object with non-cover opacity into a temporary layer;
     * layer->opa already includes this slider and its parent chain. */
    lv_opa_t object_opa = layer->opa;
    lv_color_t accent = accent_lv_color();
    lv_color_t neutral = lv_color_mix(accent, ctx->rail_color, 56);
    int32_t progress_x = slider_progress_x(slider, &bounds);
    int32_t mid_y = bounds.y1 + (height - 1) / 2;

    if (!ctx->ready) {
        /* Keep a quiet neutral rail and a compact accent thumb until the
         * asynchronous waveform request returns. */
        int32_t rail_h = height >= 14 ? 3 : 2;
        lv_area_t rail = { bounds.x1, mid_y - rail_h / 2,
                           bounds.x2, mid_y + rail_h / 2 };
        lv_opa_t rail_opa = multiply_opa(ctx->rail_opa, object_opa);
        draw_rect(layer, &rail, neutral, rail_opa, rail_h / 2);
        lv_area_t played_rail = rail;
        played_rail.x2 = progress_x;
        if (played_rail.x2 > bounds.x2) played_rail.x2 = bounds.x2;
        draw_rect(layer, &played_rail, accent, rail_opa, rail_h / 2);

        int32_t x = progress_x;
        int32_t thumb_r = height >= 14 ? 5 : 4;
        lv_area_t thumb = { x - thumb_r, mid_y - thumb_r,
                            x + thumb_r, mid_y + thumb_r };
        if (thumb.x1 < bounds.x1) thumb.x1 = bounds.x1;
        if (thumb.x2 > bounds.x2) thumb.x2 = bounds.x2;
        if (thumb.y1 < bounds.y1) thumb.y1 = bounds.y1;
        if (thumb.y2 > bounds.y2) thumb.y2 = bounds.y2;
        draw_rect(layer, &thumb, accent, object_opa, thumb_r);
        return;
    }

    /* Limit draw submissions to 72 bars at 480px and 48 on compact screens. */
    int32_t count = width / 6;
    if (count > 72) count = 72;
    if (count > WAVEFORM_BINS) count = WAVEFORM_BINS;
    if (count < 1) count = 1;
    int32_t max_h = height - 2;
    int32_t baseline = ctx->style == SEEKBAR_HALF ? bounds.y2 - 1 : mid_y;

    for (int32_t i = 0; i < count; ++i) {
        int32_t first = i * WAVEFORM_BINS / count;
        int32_t last = (i + 1) * WAVEFORM_BINS / count;
        uint8_t amplitude = 0;
        for (int32_t j = first; j < last; ++j) {
            if (ctx->bins[j] > amplitude) amplitude = ctx->bins[j];
        }
        int32_t bar_h = (max_h * amplitude + 254) / 255;
        if (bar_h < 2) bar_h = 2;
        if (bar_h > max_h) bar_h = max_h;
        int32_t x1 = bounds.x1 + (int64_t)i * width / count;
        int32_t x2 = bounds.x1 + (int64_t)(i + 1) * width / count - 1;
        int32_t inset = ctx->style == SEEKBAR_ENVELOPE ? 0 : 1;
        x1 += inset;
        x2 -= inset;
        if (x1 > x2) x1 = x2;

        lv_area_t bar;
        if (ctx->style == SEEKBAR_HALF) {
            bar = (lv_area_t){ x1, baseline - bar_h + 1, x2, baseline };
        }
        else {
            int32_t top_h = (bar_h + 1) / 2;
            int32_t bottom_h = bar_h / 2;
            if (top_h < 1) top_h = 1;
            if (bottom_h < 1) bottom_h = 1;
            bar = (lv_area_t){ x1, mid_y - top_h + 1, x2,
                               mid_y + bottom_h };
        }
        if (bar.y1 < bounds.y1) bar.y1 = bounds.y1;
        if (bar.y2 > bounds.y2) bar.y2 = bounds.y2;
        draw_rect(layer, &bar, neutral, object_opa, ctx->style == SEEKBAR_ENVELOPE ? 1 : 2);
        lv_area_t played = bar;
        if (played.x2 > progress_x) played.x2 = progress_x;
        draw_rect(layer, &played, accent, object_opa,
                  ctx->style == SEEKBAR_ENVELOPE ? 1 : 2);
    }
    int32_t playhead_r = 1;
    lv_area_t playhead = { progress_x - playhead_r, bounds.y1,
                           progress_x + playhead_r, bounds.y2 };
    if (playhead.x1 < bounds.x1) playhead.x1 = bounds.x1;
    if (playhead.x2 > bounds.x2) playhead.x2 = bounds.x2;
    draw_rect(layer, &playhead, accent, object_opa, LV_RADIUS_CIRCLE);
}

static void seekbar_delete(lv_event_t * event)
{
    if (lv_event_get_code(event) == LV_EVENT_DELETE) free(lv_event_get_user_data(event));
}

bool player_seekbar_attach(lv_obj_t * slider, const char * style)
{
    if (!is_slider(slider) || !style) return false;
    seekbar_style_t selected;
    if (strcmp(style, "waveform_bars") == 0) selected = SEEKBAR_BARS;
    else if (strcmp(style, "waveform_envelope") == 0) selected = SEEKBAR_ENVELOPE;
    else if (strcmp(style, "waveform_half") == 0) selected = SEEKBAR_HALF;
    else return false;

    seekbar_ctx_t * ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return false;
    ctx->style = selected;
    ctx->rail_color = lv_obj_get_style_bg_color(slider, LV_PART_MAIN);
    ctx->rail_opa = lv_obj_get_style_bg_opa(slider, LV_PART_MAIN);
    lv_obj_add_event_cb(slider, seekbar_draw, LV_EVENT_DRAW_MAIN_END, ctx);
    lv_obj_add_event_cb(slider, seekbar_delete, LV_EVENT_DELETE, ctx);

    /* The slider remains the input target. Only its built-in paint is hidden;
     * custom drawing stays inside the same object bounds. */
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
    /* The player theme also outlines the native knob. Hide that whole part,
     * otherwise a ring remains on top of the waveform's playhead. */
    lv_obj_set_style_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
    return true;
}

void player_seekbar_update(lv_obj_t * slider, const waveform_data_t * data)
{
    if (!slider) return;
    uint32_t event_count = lv_obj_get_event_count(slider);
    for (uint32_t i = 0; i < event_count; ++i) {
        lv_event_dsc_t * dsc = lv_obj_get_event_dsc(slider, i);
        if (lv_event_dsc_get_cb(dsc) == seekbar_draw) {
            seekbar_ctx_t * ctx = lv_event_dsc_get_user_data(dsc);
            uint8_t bins[WAVEFORM_BINS] = {0};
            bool ready = data && data->ready;
            if (ready) memcpy(bins, data->bins, sizeof(bins));
            if (ctx->ready != ready || memcmp(ctx->bins, bins, sizeof(bins)) != 0) {
                ctx->ready = ready;
                memcpy(ctx->bins, bins, sizeof(bins));
                lv_obj_invalidate(slider);
            }
            return;
        }
    }
}
