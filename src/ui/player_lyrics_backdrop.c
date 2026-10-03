#include "player_lyrics_backdrop.h"
#include "gui_navigation.h"
#include "player_layouts.h"
#include "transition_compositor.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BACKDROP_WARM_DELAY_MS 100
#define BACKDROP_FRAME_PERIOD_MS 16
#define BACKDROP_MAX_BYTES (1536U * 1024U)

static lv_obj_t * backdrop_wrapper;
static lv_obj_t * backdrop_dim;
static lv_obj_t * backdrop_area;
static lv_obj_t * backdrop_image;
static lv_draw_buf_t * backdrop_closed;
static lv_draw_buf_t * backdrop_open;
static lv_draw_buf_t * backdrop_frame;
static lv_timer_t * backdrop_warm_timer;
static lv_timer_t * backdrop_frame_timer;
static lv_anim_timeline_t * backdrop_timeline;
static lv_anim_timeline_t * backdrop_open_timeline;
static lv_anim_timeline_t * backdrop_close_timeline;
static player_layout_style_transition_t backdrop_open_dim_transition;
static player_layout_style_transition_t backdrop_open_area_transition;
static player_layout_style_transition_t backdrop_close_dim_transition;
static player_layout_style_transition_t backdrop_close_area_transition;
static lv_obj_t ** backdrop_children;
static bool * backdrop_children_hidden;
static uint32_t backdrop_child_capacity;
static uint32_t backdrop_child_count;
static bool backdrop_opening;
static bool backdrop_busy;
static bool backdrop_active;
static int32_t backdrop_last_mix = -1;

static bool transitions_match(const player_layout_style_transition_t * a,
                              const player_layout_style_transition_t * b) {
    if (a->start_time != b->start_time || a->duration != b->duration ||
        a->path_cb != b->path_cb) return false;
    if (a->path_cb == lv_anim_path_custom_bezier3) {
        return a->animation.parameter.bezier3.x1 == b->animation.parameter.bezier3.x1 &&
               a->animation.parameter.bezier3.y1 == b->animation.parameter.bezier3.y1 &&
               a->animation.parameter.bezier3.x2 == b->animation.parameter.bezier3.x2 &&
               a->animation.parameter.bezier3.y2 == b->animation.parameter.bezier3.y2;
    }
    return true;
}

static bool backdrop_timeline_targets_allowed(lv_anim_timeline_t * timeline,
                                               lv_obj_t * obj) {
    if (obj == backdrop_dim || obj == backdrop_area) {
        uint32_t bg_count = player_layouts_timeline_style_transition_count(
            timeline, obj, LV_STYLE_BG_OPA, LV_PART_MAIN);
        if (bg_count != 1 || player_layouts_timeline_animation_count_for_obj(timeline, obj) != 1)
            return false;
    }
    else if (player_layouts_timeline_has_animation_for_obj(timeline, obj)) return false;
    uint32_t children = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < children; ++i)
        if (!backdrop_timeline_targets_allowed(timeline, lv_obj_get_child(obj, i))) return false;
    return true;
}

static bool backdrop_ancestry_is_opaque_and_visible(lv_obj_t * obj) {
    for (lv_obj_t * current = obj; current; current = lv_obj_get_parent(current)) {
        if (lv_obj_has_flag(current, LV_OBJ_FLAG_HIDDEN) ||
            lv_obj_get_style_opa(current, LV_PART_MAIN) != LV_OPA_COVER) return false;
    }
    return true;
}

static bool backdrop_wrapper_geometry_supported(lv_obj_t * wrapper) {
    if (lv_obj_get_style_pad_left(wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_pad_right(wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_pad_top(wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_pad_bottom(wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_border_width(wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_layout(wrapper, LV_PART_MAIN) != LV_LAYOUT_NONE ||
        lv_obj_get_style_radius(wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_transform_width(wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_transform_height(wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_transform_scale_x(wrapper, LV_PART_MAIN) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_scale_y(wrapper, LV_PART_MAIN) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_rotation(wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_transform_skew_x(wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_transform_skew_y(wrapper, LV_PART_MAIN) != 0) return false;
    return true;
}

static bool backdrop_load_transitions(lv_anim_timeline_t * open_timeline,
                                      lv_anim_timeline_t * close_timeline) {
    bool dim_inside = false;
    bool area_inside = false;
    for (lv_obj_t * current = backdrop_dim; current; current = lv_obj_get_parent(current))
        if (current == backdrop_wrapper) dim_inside = true;
    for (lv_obj_t * current = backdrop_area; current; current = lv_obj_get_parent(current))
        if (current == backdrop_wrapper) area_inside = true;
    if (!dim_inside || !area_inside || backdrop_dim == backdrop_area ||
        !backdrop_wrapper_geometry_supported(backdrop_wrapper) ||
        !backdrop_ancestry_is_opaque_and_visible(backdrop_wrapper) ||
        lv_obj_get_style_bg_opa(backdrop_wrapper, LV_PART_MAIN) != LV_OPA_COVER ||
        !backdrop_timeline_targets_allowed(open_timeline, backdrop_wrapper) ||
        !backdrop_timeline_targets_allowed(close_timeline, backdrop_wrapper)) return false;
    if (!player_layouts_timeline_style_transition(open_timeline, backdrop_dim,
            LV_STYLE_BG_OPA, 0, &backdrop_open_dim_transition) ||
        !player_layouts_timeline_style_transition(open_timeline, backdrop_area,
            LV_STYLE_BG_OPA, 0, &backdrop_open_area_transition) ||
        !player_layouts_timeline_style_transition(close_timeline, backdrop_dim,
            LV_STYLE_BG_OPA, 0, &backdrop_close_dim_transition) ||
        !player_layouts_timeline_style_transition(close_timeline, backdrop_area,
            LV_STYLE_BG_OPA, 0, &backdrop_close_area_transition)) return false;
    const player_layout_style_transition_t * od = &backdrop_open_dim_transition;
    const player_layout_style_transition_t * oa = &backdrop_open_area_transition;
    const player_layout_style_transition_t * cd = &backdrop_close_dim_transition;
    const player_layout_style_transition_t * ca = &backdrop_close_area_transition;
    return transitions_match(od, oa) && transitions_match(cd, ca) &&
           od->path_cb && cd->path_cb &&
           od->start_value >= 0 && od->start_value <= LV_OPA_COVER &&
           od->end_value >= 0 && od->end_value <= LV_OPA_COVER &&
           oa->start_value >= 0 && oa->start_value <= LV_OPA_COVER &&
           oa->end_value >= 0 && oa->end_value <= LV_OPA_COVER &&
           cd->start_value == od->end_value && cd->end_value == od->start_value &&
           ca->start_value == oa->end_value && ca->end_value == oa->start_value;
}

static void backdrop_schedule_warm(void);

static void backdrop_free_frames(void) {
    if (backdrop_image) lv_image_set_src(backdrop_image, NULL);
    if (backdrop_closed) lv_draw_buf_destroy(backdrop_closed);
    if (backdrop_open) lv_draw_buf_destroy(backdrop_open);
    if (backdrop_frame) lv_draw_buf_destroy(backdrop_frame);
    backdrop_closed = NULL;
    backdrop_open = NULL;
    backdrop_frame = NULL;
    backdrop_last_mix = -1;
}

static void backdrop_cancel_warm(void) {
    if (!backdrop_warm_timer) return;
    lv_timer_delete(backdrop_warm_timer);
    backdrop_warm_timer = NULL;
}

static void backdrop_restore_children(void) {
    for (uint32_t i = 0; i < backdrop_child_count; ++i) {
        if (backdrop_children_hidden[i]) lv_obj_add_flag(backdrop_children[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(backdrop_children[i], LV_OBJ_FLAG_HIDDEN);
    }
    backdrop_child_count = 0;
    if (backdrop_image) lv_obj_add_flag(backdrop_image, LV_OBJ_FLAG_HIDDEN);
}

void player_lyrics_backdrop_end(void) {
    if (backdrop_frame_timer) {
        lv_timer_delete(backdrop_frame_timer);
        backdrop_frame_timer = NULL;
    }
    backdrop_busy = false;
    if (!backdrop_active) return;
    backdrop_active = false;
    backdrop_timeline = NULL;
    backdrop_restore_children();
    if (backdrop_wrapper) lv_obj_invalidate(backdrop_wrapper);
}

void player_lyrics_backdrop_teardown(void) {
    player_lyrics_backdrop_end();
    backdrop_busy = false;
    backdrop_cancel_warm();
    backdrop_free_frames();
    if (backdrop_image) lv_obj_delete(backdrop_image);
    backdrop_image = NULL;
    free(backdrop_children);
    free(backdrop_children_hidden);
    backdrop_children = NULL;
    backdrop_children_hidden = NULL;
    backdrop_child_capacity = 0;
    backdrop_wrapper = NULL;
    backdrop_dim = NULL;
    backdrop_area = NULL;
    backdrop_timeline = NULL;
    backdrop_open_timeline = NULL;
    backdrop_close_timeline = NULL;
}

static bool backdrop_snapshot_pair(void) {
    lv_obj_update_layout(backdrop_wrapper);
    int32_t width = lv_obj_get_width(backdrop_wrapper);
    int32_t height = lv_obj_get_height(backdrop_wrapper);
    if (width <= 0 || height <= 0) return false;
    uint64_t max_data = (uint64_t) width * (uint64_t) height * 2U;
    if (max_data * 3U > BACKDROP_MAX_BYTES) return false;

    lv_opa_t saved_dim = lv_obj_get_style_bg_opa(backdrop_dim, 0);
    lv_opa_t saved_area = lv_obj_get_style_bg_opa(backdrop_area, 0);
    lv_obj_set_style_bg_opa(backdrop_dim, backdrop_open_dim_transition.start_value, 0);
    lv_obj_set_style_bg_opa(backdrop_area, backdrop_open_area_transition.start_value, 0);
    backdrop_closed = lv_snapshot_take(backdrop_wrapper, LV_COLOR_FORMAT_RGB565);

    lv_obj_set_style_bg_opa(backdrop_dim, backdrop_open_dim_transition.end_value, 0);
    lv_obj_set_style_bg_opa(backdrop_area, backdrop_open_area_transition.end_value, 0);
    backdrop_open = lv_snapshot_take(backdrop_wrapper, LV_COLOR_FORMAT_RGB565);

    lv_obj_set_style_bg_opa(backdrop_dim, saved_dim, 0);
    lv_obj_set_style_bg_opa(backdrop_area, saved_area, 0);
    lv_obj_invalidate(backdrop_wrapper);
    if (!backdrop_closed || !backdrop_open ||
        backdrop_closed->header.cf != LV_COLOR_FORMAT_RGB565 ||
        backdrop_open->header.cf != LV_COLOR_FORMAT_RGB565 ||
        backdrop_closed->header.w != backdrop_open->header.w ||
        backdrop_closed->header.h != backdrop_open->header.h ||
        backdrop_closed->header.w != (uint32_t) width ||
        backdrop_closed->header.h != (uint32_t) height ||
        backdrop_closed->header.stride < (uint32_t) width * 2U ||
        backdrop_open->header.stride < (uint32_t) width * 2U ||
        (uint64_t) backdrop_closed->header.stride * backdrop_closed->header.h > backdrop_closed->data_size ||
        (uint64_t) backdrop_open->header.stride * backdrop_open->header.h > backdrop_open->data_size ||
        (uint64_t) backdrop_closed->data_size + backdrop_open->data_size > BACKDROP_MAX_BYTES) {
        backdrop_free_frames();
        return false;
    }

    backdrop_frame = lv_draw_buf_create(width, height, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (!backdrop_frame ||
        backdrop_frame->header.cf != LV_COLOR_FORMAT_RGB565 ||
        backdrop_frame->header.w != backdrop_closed->header.w ||
        backdrop_frame->header.h != backdrop_closed->header.h ||
        backdrop_frame->header.stride < (uint32_t) width * 2U ||
        (uint64_t) backdrop_frame->header.stride * backdrop_frame->header.h > backdrop_frame->data_size ||
        (uint64_t) backdrop_closed->data_size + backdrop_open->data_size + backdrop_frame->data_size > BACKDROP_MAX_BYTES) {
        backdrop_free_frames();
        return false;
    }
    lv_image_set_src(backdrop_image, backdrop_frame);
    lv_draw_buf_set_flag(backdrop_frame, LV_IMAGE_FLAGS_MODIFIABLE);
    return true;
}

static void backdrop_warm_timer_cb(lv_timer_t * timer) {
    (void) timer;
    backdrop_warm_timer = NULL;
    if (backdrop_busy || backdrop_active ||
        gui_navigation_transition_in_progress() || transition_compositor_is_active()) {
        backdrop_schedule_warm();
        return;
    }
    if (!backdrop_wrapper || !backdrop_image || backdrop_closed) return;
    backdrop_snapshot_pair();
}

static void backdrop_schedule_warm(void) {
    if (!backdrop_wrapper || !backdrop_dim || !backdrop_area || !backdrop_image ||
        backdrop_warm_timer || backdrop_closed || backdrop_busy) return;
    backdrop_warm_timer = lv_timer_create(backdrop_warm_timer_cb, BACKDROP_WARM_DELAY_MS, NULL);
    if (backdrop_warm_timer) lv_timer_set_repeat_count(backdrop_warm_timer, 1);
}

void player_lyrics_backdrop_bind(lv_obj_t * wrapper, lv_obj_t * dim, lv_obj_t * area,
                                 lv_anim_timeline_t * open_timeline,
                                 lv_anim_timeline_t * close_timeline) {
    player_lyrics_backdrop_teardown();
    if (!wrapper || !dim || !area ||
        lv_obj_get_style_bg_opa(wrapper, LV_PART_MAIN) != LV_OPA_COVER ||
        lv_obj_get_style_opa(wrapper, LV_PART_MAIN) != LV_OPA_COVER) return;
    backdrop_wrapper = wrapper;
    backdrop_dim = dim;
    backdrop_area = area;
    if (!backdrop_load_transitions(open_timeline, close_timeline)) {
        backdrop_wrapper = NULL;
        backdrop_dim = NULL;
        backdrop_area = NULL;
        return;
    }
    backdrop_open_timeline = open_timeline;
    backdrop_close_timeline = close_timeline;
    backdrop_child_capacity = lv_obj_get_child_count(wrapper) + 1U;
    backdrop_children = calloc(backdrop_child_capacity, sizeof(*backdrop_children));
    backdrop_children_hidden = calloc(backdrop_child_capacity, sizeof(*backdrop_children_hidden));
    backdrop_image = lv_image_create(wrapper);
    if (!backdrop_children || !backdrop_children_hidden || !backdrop_image) {
        player_lyrics_backdrop_teardown();
        return;
    }
    lv_obj_set_pos(backdrop_image, 0, 0);
    lv_obj_set_size(backdrop_image, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(backdrop_image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(backdrop_image, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_inner_align(backdrop_image, LV_IMAGE_ALIGN_STRETCH);
    backdrop_schedule_warm();
}

void player_lyrics_backdrop_invalidate(void) {
    if (!backdrop_wrapper) return;
    bool was_busy = backdrop_busy;
    player_lyrics_backdrop_end();
    backdrop_busy = was_busy;
    backdrop_cancel_warm();
    backdrop_free_frames();
    backdrop_schedule_warm();
}

void player_lyrics_backdrop_set_busy(bool busy) {
    backdrop_busy = busy;
    if (!busy) backdrop_schedule_warm();
}

static uint16_t backdrop_mix_pixel(uint16_t a, uint16_t b, uint32_t mix) {
    uint32_t inverse = 256U - mix;
    uint32_t rb_a = ((uint32_t) (a & 0xF800U) << 5) | (a & 0x001FU);
    uint32_t rb_b = ((uint32_t) (b & 0xF800U) << 5) | (b & 0x001FU);
    uint32_t rb = (rb_a * inverse + rb_b * mix + 0x00800080U) >> 8;
    uint32_t green = (((uint32_t) ((a >> 5) & 0x003FU) * inverse) +
                      ((uint32_t) ((b >> 5) & 0x003FU) * mix) + 0x80U) >> 8;
    return (uint16_t) (((rb >> 5) & 0xF800U) | (rb & 0x001FU) | (green << 5));
}

static void backdrop_render_mix(uint32_t mix) {
    if (!backdrop_frame || !backdrop_closed || !backdrop_open || !backdrop_image ||
        mix == (uint32_t) backdrop_last_mix) return;
    uint32_t width_bytes = backdrop_frame->header.w * 2U;
    const lv_draw_buf_t * endpoint = mix == 0U ? backdrop_closed : (mix == 256U ? backdrop_open : NULL);
    if (endpoint) {
        for (uint32_t y = 0; y < backdrop_frame->header.h; ++y) {
            const uint8_t * source_row = endpoint->data + (size_t) y * endpoint->header.stride;
            uint8_t * frame_row = backdrop_frame->data + (size_t) y * backdrop_frame->header.stride;
            memcpy(frame_row, source_row, width_bytes);
        }
        backdrop_last_mix = (int32_t) mix;
        lv_obj_invalidate(backdrop_image);
        return;
    }
    for (uint32_t y = 0; y < backdrop_frame->header.h; ++y) {
        const uint16_t * closed_row = (const uint16_t *) (backdrop_closed->data + (size_t) y * backdrop_closed->header.stride);
        const uint16_t * open_row = (const uint16_t *) (backdrop_open->data + (size_t) y * backdrop_open->header.stride);
        uint16_t * frame_row = (uint16_t *) (backdrop_frame->data + (size_t) y * backdrop_frame->header.stride);
        for (uint32_t x = 0; x < backdrop_frame->header.w; ++x)
            frame_row[x] = backdrop_mix_pixel(closed_row[x], open_row[x], mix);
    }
    backdrop_last_mix = (int32_t) mix;
    lv_obj_invalidate(backdrop_image);
}

static uint32_t backdrop_timeline_elapsed(lv_anim_timeline_t * timeline) {
    uint64_t elapsed = (uint64_t)lv_anim_timeline_get_playtime(timeline) *
                       lv_anim_timeline_get_progress(timeline) /
                       LV_ANIM_TIMELINE_PROGRESS_MAX;
    uint32_t playtime = lv_anim_timeline_get_playtime(timeline);
    if (elapsed > playtime) elapsed = playtime;
    return (uint32_t)elapsed;
}

static int32_t backdrop_transition_value(const player_layout_style_transition_t * transition,
                                         uint32_t timeline_elapsed) {
    if (timeline_elapsed < transition->start_time) return 0;
    uint32_t local = timeline_elapsed - transition->start_time;
    if (transition->duration == 0) return 1024;
    if (local >= transition->duration) return 1024;
    lv_anim_t animation = transition->animation;
    /* Evaluate the same path in normalized space to avoid opacity integer
     * rounding creating visible steps when endpoint deltas are small. */
    lv_anim_set_values(&animation, 0, 1024);
    lv_anim_set_duration(&animation, transition->duration);
    lv_anim_set_path_cb(&animation, transition->path_cb);
    animation.act_time = (int32_t)local;
    return transition->path_cb(&animation);
}

static bool backdrop_initial_opacity_matches(lv_obj_t * obj,
                                              const player_layout_style_transition_t * transition) {
    int32_t rendered = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
    int32_t expected = transition->start_value;
    if (transition->start_time > 0 && !transition->animation.early_apply) {
        /* During a delayed animation without early_apply, LVGL keeps the
         * authored style value visible until start_time. It must already be
         * the endpoint represented by the cached first frame. */
        return rendered == expected;
    }
    /* With no delay, or with early_apply, the visible start must also match. */
    return rendered == expected;
}

static bool backdrop_initial_state_matches(bool opening) {
    const player_layout_style_transition_t * dim_transition = opening ?
        &backdrop_open_dim_transition : &backdrop_close_dim_transition;
    const player_layout_style_transition_t * area_transition = opening ?
        &backdrop_open_area_transition : &backdrop_close_area_transition;
    return backdrop_initial_opacity_matches(backdrop_dim, dim_transition) &&
           backdrop_initial_opacity_matches(backdrop_area, area_transition);
}

static void backdrop_frame_timer_cb(lv_timer_t * timer) {
    (void) timer;
    if (!backdrop_active || !backdrop_timeline || !backdrop_frame) return;
    const player_layout_style_transition_t * dim_transition = backdrop_opening ?
        &backdrop_open_dim_transition : &backdrop_close_dim_transition;
    const player_layout_style_transition_t * area_transition = backdrop_opening ?
        &backdrop_open_area_transition : &backdrop_close_area_transition;
    const player_layout_style_transition_t * transition =
        dim_transition->start_value != dim_transition->end_value ? dim_transition : area_transition;
    int32_t progress = backdrop_transition_value(transition,
                                                  backdrop_timeline_elapsed(backdrop_timeline));
    int32_t mix = (int32_t)(((int64_t)progress * 256) / 1024);
    if (mix < 0) mix = 0;
    if (mix > 256) mix = 256;
    if (!backdrop_opening) mix = 256 - mix;
    backdrop_render_mix((uint32_t)mix);
}

bool player_lyrics_backdrop_begin(bool opening) {
    if (!backdrop_wrapper || !backdrop_image || !backdrop_closed || !backdrop_open ||
        !backdrop_frame || backdrop_active || backdrop_busy) return false;
    int32_t old_open_dim_start = backdrop_open_dim_transition.start_value;
    int32_t old_open_dim_end = backdrop_open_dim_transition.end_value;
    int32_t old_open_area_start = backdrop_open_area_transition.start_value;
    int32_t old_open_area_end = backdrop_open_area_transition.end_value;
    if (!backdrop_load_transitions(backdrop_open_timeline, backdrop_close_timeline)) return false;
    if (old_open_dim_start != backdrop_open_dim_transition.start_value ||
        old_open_dim_end != backdrop_open_dim_transition.end_value ||
        old_open_area_start != backdrop_open_area_transition.start_value ||
        old_open_area_end != backdrop_open_area_transition.end_value) {
        backdrop_free_frames();
        backdrop_schedule_warm();
        return false;
    }
    if (!backdrop_initial_state_matches(opening)) return false;
    uint32_t child_count = lv_obj_get_child_count(backdrop_wrapper);
    if (child_count > backdrop_child_capacity) return false;
    lv_timer_t * timer = lv_timer_create(backdrop_frame_timer_cb, BACKDROP_FRAME_PERIOD_MS, NULL);
    if (!timer) return false;

    backdrop_child_count = 0;
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t * child = lv_obj_get_child(backdrop_wrapper, i);
        if (child == backdrop_image) continue;
        backdrop_children[backdrop_child_count] = child;
        backdrop_children_hidden[backdrop_child_count] = lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN);
        backdrop_child_count++;
    }
    backdrop_opening = opening;
    backdrop_timeline = opening ? backdrop_open_timeline : backdrop_close_timeline;
    backdrop_active = true;
    backdrop_busy = true;
    backdrop_frame_timer = timer;
    backdrop_render_mix(opening ? 0U : 256U);
    for (uint32_t i = 0; i < backdrop_child_count; ++i)
        lv_obj_add_flag(backdrop_children[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(backdrop_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(backdrop_wrapper);
    return true;
}
