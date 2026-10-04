#include "player_lyrics_backdrop.h"
#include "gui_navigation.h"
#include "player_layouts.h"
#include "transition_compositor.h"
#include "lvgl/src/core/lv_obj_draw_private.h"
#include "lvgl/src/misc/lv_area_private.h"
#include "lvgl/src/misc/lv_event_private.h"
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
static bool backdrop_observer_attached;
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
static bool backdrop_static;
static bool backdrop_static_bound;
static bool backdrop_static_retained;
static bool backdrop_tearing_down;
static int32_t backdrop_last_mix = -1;
static lv_display_t * backdrop_cache_display;

typedef struct {
    lv_obj_t * image_obj;
    const lv_image_dsc_t * source;
    const void * data;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t data_size;
    lv_color_format_t color_format;
    lv_color_t recolor;
    lv_opa_t recolor_opa;
    bool antialias;
    bool valid;
} backdrop_static_cache_key_t;

static backdrop_static_cache_key_t backdrop_static_cache_key;

static void backdrop_liveness_delete_cb(lv_event_t * event);
static void backdrop_teardown_internal(lv_obj_t * deleting_obj);
static void backdrop_cache_observer_update(void);
static void backdrop_cache_refresh_cb(lv_event_t * event);
static void backdrop_cache_display_delete_cb(lv_event_t * event);

static bool backdrop_track_object(lv_obj_t * obj, lv_obj_t ** pointer_slot) {
    return obj && pointer_slot &&
           lv_obj_add_event_cb(obj, backdrop_liveness_delete_cb, LV_EVENT_DELETE, pointer_slot) != NULL;
}

static void backdrop_untrack_object(lv_obj_t * obj, lv_obj_t ** pointer_slot) {
    if (obj && pointer_slot)
        lv_obj_remove_event_cb_with_user_data(obj, backdrop_liveness_delete_cb, pointer_slot);
}

static void backdrop_untrack_saved_children(void) {
    for (uint32_t i = 0; i < backdrop_child_count; ++i) {
        lv_obj_t * child = backdrop_children[i];
        if (child) backdrop_untrack_object(child, &backdrop_children[i]);
        backdrop_children[i] = NULL;
    }
}

static void backdrop_static_cache_key_clear(void) {
    if (backdrop_static_cache_key.image_obj)
        backdrop_untrack_object(backdrop_static_cache_key.image_obj,
                                &backdrop_static_cache_key.image_obj);
    memset(&backdrop_static_cache_key, 0, sizeof(backdrop_static_cache_key));
}

static void backdrop_liveness_delete_cb(lv_event_t * event) {
    if (lv_event_get_code(event) != LV_EVENT_DELETE) return;
    lv_obj_t * obj = lv_event_get_target(event);
    lv_obj_t ** pointer_slot = lv_event_get_user_data(event);
    bool wrapper_deleted = pointer_slot == &backdrop_wrapper;
    bool image_deleted = pointer_slot == &backdrop_image;
    bool transition_child_deleted = obj == backdrop_dim || obj == backdrop_area;

    if (pointer_slot && *pointer_slot == obj) *pointer_slot = NULL;
    if (wrapper_deleted) {
        /* The wrapper's children will be deleted by LVGL immediately after
         * this event. Do not dereference any of them during teardown. */
        backdrop_dim = NULL;
        backdrop_area = NULL;
        if (backdrop_image) backdrop_untrack_object(backdrop_image, &backdrop_image);
        backdrop_image = NULL;
        backdrop_teardown_internal(obj);
    }
    else if (image_deleted || transition_child_deleted) {
        if (obj == backdrop_dim) backdrop_dim = NULL;
        if (obj == backdrop_area) backdrop_area = NULL;
        backdrop_teardown_internal(obj);
    }
}

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
    if (!timeline) return false;
    /* Non-normal blending depends on the destination. The animated pair
     * cannot preserve its changing result, and a transparent static snapshot
     * cannot reproduce blending against the live screen behind it. */
    if (lv_obj_get_style_blend_mode(obj, LV_PART_MAIN) != LV_BLEND_MODE_NORMAL ||
        (lv_obj_check_type(obj, &lv_image_class) && lv_image_get_blend_mode(obj) != LV_BLEND_MODE_NORMAL))
        return false;
    if (backdrop_static) {
        if (player_layouts_timeline_has_animation_for_obj(timeline, obj)) return false;
    }
    else if (obj == backdrop_dim || obj == backdrop_area) {
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
    if (lv_obj_get_ext_draw_size(wrapper) != 0 ||
        lv_obj_get_style_pad_left(wrapper, LV_PART_MAIN) != 0 ||
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

static bool backdrop_has_custom_draw_event(lv_obj_t * obj) {
    uint32_t count = lv_obj_get_event_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        lv_event_dsc_t * dsc = lv_obj_get_event_dsc(obj, i);
        if (!dsc) continue;
        uint32_t filter = dsc->filter & ~(uint32_t)LV_EVENT_PREPROCESS;
        if (filter == LV_EVENT_ALL || filter == LV_EVENT_COVER_CHECK ||
            filter == LV_EVENT_REFR_EXT_DRAW_SIZE || filter == LV_EVENT_DRAW_MAIN_BEGIN ||
            filter == LV_EVENT_DRAW_MAIN || filter == LV_EVENT_DRAW_MAIN_END ||
            filter == LV_EVENT_DRAW_POST_BEGIN || filter == LV_EVENT_DRAW_POST ||
            filter == LV_EVENT_DRAW_POST_END || filter == LV_EVENT_DRAW_TASK_ADDED) return true;
    }
    return false;
}

static lv_obj_t * backdrop_static_source_image(void) {
    if (!backdrop_wrapper) return NULL;
    lv_obj_t * candidate = NULL;
    uint32_t count = lv_obj_get_child_count(backdrop_wrapper);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t * child = lv_obj_get_child(backdrop_wrapper, i);
        if (child == backdrop_image) continue;
        if (candidate || !lv_obj_check_type(child, &lv_image_class)) return NULL;
        candidate = child;
    }
    return candidate;
}

static bool backdrop_static_cache_key_read(backdrop_static_cache_key_t * key) {
    if (!key || !backdrop_static || !backdrop_wrapper || !backdrop_image) return false;
    lv_obj_update_layout(backdrop_wrapper);
    lv_obj_t * image = backdrop_static_source_image();
    if (!image || lv_obj_get_child_count(image) != 0 ||
        (!backdrop_static_bound && lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN)) ||
        lv_obj_get_style_opa(backdrop_wrapper, LV_PART_MAIN) != LV_OPA_COVER ||
        lv_obj_get_layer_type(backdrop_wrapper) != LV_LAYER_TYPE_NONE ||
        lv_obj_get_width(backdrop_wrapper) <= 0 || lv_obj_get_height(backdrop_wrapper) <= 0 ||
        lv_obj_get_x(image) != 0 || lv_obj_get_y(image) != 0 ||
        lv_obj_get_width(image) != lv_obj_get_width(backdrop_wrapper) ||
        lv_obj_get_height(image) != lv_obj_get_height(backdrop_wrapper) ||
        lv_obj_get_ext_draw_size(backdrop_wrapper) != 0 ||
        lv_obj_get_ext_draw_size(image) != 0 ||
        lv_obj_get_style_layout(backdrop_wrapper, LV_PART_MAIN) != LV_LAYOUT_NONE ||
        lv_obj_get_style_pad_left(backdrop_wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_pad_right(backdrop_wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_pad_top(backdrop_wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_pad_bottom(backdrop_wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_pad_left(image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_pad_right(image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_pad_top(image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_pad_bottom(image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_border_width(backdrop_wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_border_width(image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_shadow_width(backdrop_wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_shadow_width(image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_outline_width(backdrop_wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_outline_width(image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_radius(backdrop_wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_radius(image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_clip_corner(backdrop_wrapper, LV_PART_MAIN) ||
        lv_obj_get_style_transform_width(backdrop_wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_transform_height(backdrop_wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_translate_x(backdrop_wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_translate_y(backdrop_wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_transform_scale_x(backdrop_wrapper, LV_PART_MAIN) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_scale_y(backdrop_wrapper, LV_PART_MAIN) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_rotation(backdrop_wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_transform_skew_x(backdrop_wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_transform_skew_y(backdrop_wrapper, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_transform_width(image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_transform_height(image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_translate_x(image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_translate_y(image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_transform_scale_x(image, LV_PART_MAIN) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_scale_y(image, LV_PART_MAIN) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_rotation(image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_transform_skew_x(image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_transform_skew_y(image, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_opa(image, LV_PART_MAIN) != LV_OPA_COVER ||
        lv_obj_get_layer_type(image) != LV_LAYER_TYPE_NONE ||
        lv_obj_get_style_opa_recursive(image, LV_PART_MAIN) != LV_OPA_COVER ||
        lv_obj_get_style_image_opa(image, LV_PART_MAIN) != LV_OPA_COVER ||
        lv_obj_get_style_recolor_recursive(backdrop_wrapper, LV_PART_MAIN).alpha != 0 ||
        lv_obj_get_style_bg_image_src(backdrop_wrapper, LV_PART_MAIN) ||
        lv_obj_get_style_bg_image_src(image, LV_PART_MAIN) ||
        lv_obj_get_style_blend_mode(backdrop_wrapper, LV_PART_MAIN) != LV_BLEND_MODE_NORMAL ||
        lv_obj_get_style_blend_mode(image, LV_PART_MAIN) != LV_BLEND_MODE_NORMAL ||
        lv_image_get_blend_mode(image) != LV_BLEND_MODE_NORMAL ||
        lv_image_get_scale_x(image) != LV_SCALE_NONE || lv_image_get_scale_y(image) != LV_SCALE_NONE ||
        lv_image_get_rotation(image) != 0 ||
        lv_image_get_offset_x(image) != 0 || lv_image_get_offset_y(image) != 0 ||
        (lv_image_get_inner_align(image) != LV_IMAGE_ALIGN_DEFAULT &&
         lv_image_get_inner_align(image) != LV_IMAGE_ALIGN_CENTER &&
         lv_image_get_inner_align(image) != LV_IMAGE_ALIGN_STRETCH) ||
        lv_image_get_bitmap_map_src(image) || backdrop_has_custom_draw_event(backdrop_wrapper) ||
        backdrop_has_custom_draw_event(image)) return false;

    const void * src = lv_image_get_src(image);
    if (!src || lv_image_src_get_type(src) != LV_IMAGE_SRC_VARIABLE) return false;
    const lv_image_dsc_t * dsc = src;
    if (dsc->header.magic != LV_IMAGE_HEADER_MAGIC || dsc->header.cf != LV_COLOR_FORMAT_RGB565 ||
        (dsc->header.flags & (LV_IMAGE_FLAGS_COMPRESSED | LV_IMAGE_FLAGS_CUSTOM_DRAW)) ||
        dsc->header.w != (uint32_t) lv_obj_get_width(backdrop_wrapper) ||
        dsc->header.h != (uint32_t) lv_obj_get_height(backdrop_wrapper) || !dsc->data ||
        dsc->header.stride < dsc->header.w * 2U ||
        (uint64_t) dsc->header.stride * dsc->header.h > dsc->data_size) return false;

    memset(key, 0, sizeof(*key));
    key->image_obj = image;
    key->source = dsc;
    key->data = dsc->data;
    key->width = dsc->header.w;
    key->height = dsc->header.h;
    key->stride = dsc->header.stride;
    key->data_size = dsc->data_size;
    key->color_format = dsc->header.cf;
    key->recolor = lv_obj_get_style_image_recolor(image, LV_PART_MAIN);
    key->recolor_opa = lv_obj_get_style_image_recolor_opa(image, LV_PART_MAIN);
    key->antialias = lv_image_get_antialias(image);
    key->valid = true;
    return true;
}

static bool backdrop_static_cache_key_matches(void) {
    backdrop_static_cache_key_t current;
    if (!backdrop_static_retained || !backdrop_static_cache_key.valid ||
        !backdrop_static_cache_key_read(&current)) return false;
    const backdrop_static_cache_key_t * saved = &backdrop_static_cache_key;
    return current.image_obj == saved->image_obj && current.source == saved->source &&
           current.data == saved->data && current.width == saved->width &&
           current.height == saved->height && current.stride == saved->stride &&
           current.data_size == saved->data_size && current.color_format == saved->color_format &&
           lv_color_eq(current.recolor, saved->recolor) &&
           current.recolor_opa == saved->recolor_opa && current.antialias == saved->antialias;
}

static bool backdrop_static_snapshot_retained(void) {
    backdrop_static_cache_key_t key;
    if (!backdrop_ancestry_is_opaque_and_visible(backdrop_wrapper) ||
        !backdrop_static_cache_key_read(&key) ||
        (uint64_t) key.width * key.height * 2U > BACKDROP_MAX_BYTES) return false;
    lv_draw_buf_t * snapshot = lv_snapshot_take(backdrop_wrapper, LV_COLOR_FORMAT_RGB565);
    if (!snapshot || snapshot->header.cf != LV_COLOR_FORMAT_RGB565 ||
        snapshot->header.w != key.width || snapshot->header.h != key.height ||
        snapshot->header.stride < key.width * 2U ||
        (uint64_t) snapshot->header.stride * snapshot->header.h > snapshot->data_size ||
        snapshot->data_size > BACKDROP_MAX_BYTES) {
        if (snapshot) lv_draw_buf_destroy(snapshot);
        return false;
    }
    backdrop_closed = snapshot;
    backdrop_static_cache_key_clear();
    backdrop_static_cache_key = key;
    if (!backdrop_track_object(key.image_obj, &backdrop_static_cache_key.image_obj)) {
        backdrop_static_cache_key_clear();
        backdrop_closed = NULL;
        lv_draw_buf_destroy(snapshot);
        return false;
    }
    backdrop_static_retained = true;
    lv_image_set_src(backdrop_image, backdrop_closed);
    return true;
}

static void backdrop_static_unbind(void) {
    if (!backdrop_static_bound) return;
    for (uint32_t i = 0; i < backdrop_child_count; ++i) {
        lv_obj_t * child = backdrop_children[i];
        if (!backdrop_wrapper || !child || lv_obj_get_parent(child) != backdrop_wrapper)
            continue;
        if (backdrop_children_hidden[i]) lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(child, LV_OBJ_FLAG_HIDDEN);
    }
    backdrop_untrack_saved_children();
    backdrop_child_count = 0;
    if (backdrop_image) lv_obj_add_flag(backdrop_image, LV_OBJ_FLAG_HIDDEN);
    backdrop_static_bound = false;
    backdrop_cache_observer_update();
}

static void backdrop_cache_display_delete_cb(lv_event_t * event) {
    if (lv_event_get_code(event) == LV_EVENT_DELETE &&
        lv_event_get_target(event) == backdrop_cache_display) backdrop_cache_display = NULL;
}

static void backdrop_cache_observer_update(void) {
    bool wanted = backdrop_static_retained && backdrop_static_bound && backdrop_wrapper;
    lv_display_t * display = wanted ? lv_obj_get_display(backdrop_wrapper) : NULL;
    if (backdrop_cache_display && (!wanted || backdrop_cache_display != display)) {
        lv_display_remove_event_cb_with_user_data(backdrop_cache_display, backdrop_cache_refresh_cb, NULL);
        lv_display_remove_event_cb_with_user_data(backdrop_cache_display, backdrop_cache_display_delete_cb, NULL);
        backdrop_cache_display = NULL;
    }
    if (!display || backdrop_cache_display == display) return;
    backdrop_cache_display = display;
    lv_display_add_event_cb(display, backdrop_cache_refresh_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(display, backdrop_cache_display_delete_cb, LV_EVENT_DELETE, NULL);
}

static void backdrop_cache_refresh_cb(lv_event_t * event) {
    (void) event;
    if (!backdrop_static_retained || !backdrop_static_bound || !backdrop_wrapper) return;
    lv_display_t * display = lv_obj_get_display(backdrop_wrapper);
    lv_obj_t * wrapper_screen = lv_obj_get_screen(backdrop_wrapper);
    if (!display || (wrapper_screen != lv_display_get_screen_active(display) &&
                     wrapper_screen != lv_display_get_screen_prev(display))) return;
    if (backdrop_static_retained && !backdrop_static_cache_key_matches())
        player_lyrics_backdrop_invalidate();
}

static bool backdrop_load_transitions(lv_anim_timeline_t * open_timeline,
                                      lv_anim_timeline_t * close_timeline) {
    if (!backdrop_wrapper || !backdrop_wrapper_geometry_supported(backdrop_wrapper) ||
        !backdrop_ancestry_is_opaque_and_visible(backdrop_wrapper) ||
        (!backdrop_static && lv_obj_get_style_bg_opa(backdrop_wrapper, LV_PART_MAIN) != LV_OPA_COVER) ||
        !backdrop_timeline_targets_allowed(open_timeline, backdrop_wrapper) ||
        !backdrop_timeline_targets_allowed(close_timeline, backdrop_wrapper)) return false;
    if (backdrop_static) return true;
    bool dim_inside = false;
    bool area_inside = false;
    for (lv_obj_t * current = backdrop_dim; current; current = lv_obj_get_parent(current)) {
        if (current == backdrop_wrapper) dim_inside = true;
        if (current == backdrop_wrapper) break;
        if (lv_obj_get_layer_type(current) != LV_LAYER_TYPE_NONE) return false;
    }
    for (lv_obj_t * current = backdrop_area; current; current = lv_obj_get_parent(current)) {
        if (current == backdrop_wrapper) area_inside = true;
        if (current == backdrop_wrapper) break;
        if (lv_obj_get_layer_type(current) != LV_LAYER_TYPE_NONE) return false;
    }
    if (!dim_inside || !area_inside || backdrop_dim == backdrop_area) return false;
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
static void backdrop_timeline_observer_cb(lv_anim_timeline_t * timeline, uint32_t act_time, void * user_data);

static bool backdrop_capture_children(uint32_t child_count) {
    backdrop_child_count = 0;
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t * child = lv_obj_get_child(backdrop_wrapper, i);
        if (child == backdrop_image) continue;
        uint32_t slot = backdrop_child_count++;
        backdrop_children[slot] = child;
        backdrop_children_hidden[slot] = lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN);
        if (!backdrop_track_object(child, &backdrop_children[slot])) {
            backdrop_untrack_saved_children();
            backdrop_child_count = 0;
            return false;
        }
    }
    return true;
}

static void backdrop_free_frames(void) {
    if (backdrop_image) lv_image_set_src(backdrop_image, NULL);
    if (backdrop_closed) lv_draw_buf_destroy(backdrop_closed);
    if (backdrop_open) lv_draw_buf_destroy(backdrop_open);
    if (backdrop_frame) lv_draw_buf_destroy(backdrop_frame);
    backdrop_closed = NULL;
    backdrop_open = NULL;
    backdrop_frame = NULL;
    backdrop_last_mix = -1;
    backdrop_static_retained = false;
    backdrop_cache_observer_update();
    backdrop_static_cache_key_clear();
}

static void backdrop_cancel_warm(void) {
    if (!backdrop_warm_timer) return;
    lv_timer_delete(backdrop_warm_timer);
    backdrop_warm_timer = NULL;
}

static void backdrop_restore_children(void) {
    for (uint32_t i = 0; i < backdrop_child_count; ++i) {
        lv_obj_t * child = backdrop_children[i];
        if (!backdrop_wrapper || !child || lv_obj_get_parent(child) != backdrop_wrapper)
            continue;
        if (backdrop_children_hidden[i]) lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(child, LV_OBJ_FLAG_HIDDEN);
    }
    backdrop_untrack_saved_children();
    backdrop_child_count = 0;
    if (backdrop_image) lv_obj_add_flag(backdrop_image, LV_OBJ_FLAG_HIDDEN);
}

void player_lyrics_backdrop_end(bool keep_static) {
    if (backdrop_observer_attached && backdrop_timeline) {
        /* A direct set_progress to the endpoint may not have advanced act_time
         * since the last child exec callback. Flush that final sample before
         * releasing the proxy. */
        if (backdrop_wrapper)
            player_layouts_timeline_observer_sync(backdrop_timeline);
        player_layouts_timeline_observer_detach(backdrop_timeline, backdrop_timeline_observer_cb, NULL);
        backdrop_observer_attached = false;
    }
    if (backdrop_frame_timer) {
        lv_timer_delete(backdrop_frame_timer);
        backdrop_frame_timer = NULL;
    }
    backdrop_busy = false;
    bool was_active = backdrop_active;
    if (backdrop_active) {
        backdrop_active = false;
        backdrop_timeline = NULL;
    }
    bool retain_binding = keep_static && backdrop_static && backdrop_static_bound &&
                          backdrop_static_retained && backdrop_static_cache_key_matches();
    if (backdrop_static_bound) {
        if (!retain_binding) backdrop_static_unbind();
    }
    else if (was_active && !backdrop_static) {
        backdrop_restore_children();
    }
    if (backdrop_wrapper && !retain_binding)
        lv_obj_invalidate(backdrop_wrapper);
}

static void backdrop_teardown_internal(lv_obj_t * deleting_obj) {
    if (backdrop_tearing_down) return;
    backdrop_tearing_down = true;
    player_lyrics_backdrop_end(false);
    backdrop_busy = false;
    backdrop_cancel_warm();
    if (backdrop_cache_display) {
        lv_display_remove_event_cb_with_user_data(backdrop_cache_display, backdrop_cache_refresh_cb, NULL);
        lv_display_remove_event_cb_with_user_data(backdrop_cache_display, backdrop_cache_display_delete_cb, NULL);
        backdrop_cache_display = NULL;
    }
    backdrop_free_frames();
    backdrop_untrack_saved_children();
    if (backdrop_wrapper && backdrop_wrapper != deleting_obj)
        backdrop_untrack_object(backdrop_wrapper, &backdrop_wrapper);
    if (backdrop_dim && backdrop_dim != deleting_obj)
        backdrop_untrack_object(backdrop_dim, &backdrop_dim);
    if (backdrop_area && backdrop_area != deleting_obj)
        backdrop_untrack_object(backdrop_area, &backdrop_area);
    if (backdrop_image && backdrop_image != deleting_obj) {
        lv_obj_t * image = backdrop_image;
        backdrop_untrack_object(image, &backdrop_image);
        backdrop_image = NULL;
        lv_obj_delete(image);
    }
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
    backdrop_static = false;
    backdrop_static_bound = false;
    backdrop_tearing_down = false;
}

void player_lyrics_backdrop_teardown(void) {
    backdrop_teardown_internal(NULL);
}

static bool backdrop_snapshot_pair(void) {
    /* A warm capture can run during an ancestor's enter/track fade. Such
     * opacity would be baked into the pixels and applied again at playback. */
    if (!backdrop_wrapper || !backdrop_ancestry_is_opaque_and_visible(backdrop_wrapper)) return false;
    lv_obj_update_layout(backdrop_wrapper);
    int32_t width = lv_obj_get_width(backdrop_wrapper);
    int32_t height = lv_obj_get_height(backdrop_wrapper);
    if (width <= 0 || height <= 0) return false;
    if (!backdrop_static &&
        backdrop_open_dim_transition.start_value != backdrop_open_dim_transition.end_value &&
        backdrop_open_area_transition.start_value != backdrop_open_area_transition.end_value) {
        /* Two overlapping opacity layers compose nonlinearly. A linear blend
         * between two snapshots cannot preserve their intermediate pixels. */
        lv_area_t dim_bounds, area_bounds, overlap;
        lv_obj_get_coords(backdrop_dim, &dim_bounds);
        lv_obj_get_coords(backdrop_area, &area_bounds);
        lv_area_increase(&dim_bounds, lv_obj_get_ext_draw_size(backdrop_dim),
                         lv_obj_get_ext_draw_size(backdrop_dim));
        lv_area_increase(&area_bounds, lv_obj_get_ext_draw_size(backdrop_area),
                         lv_obj_get_ext_draw_size(backdrop_area));
        lv_obj_get_transformed_area(backdrop_dim, &dim_bounds, LV_OBJ_POINT_TRANSFORM_FLAG_RECURSIVE);
        lv_obj_get_transformed_area(backdrop_area, &area_bounds, LV_OBJ_POINT_TRANSFORM_FLAG_RECURSIVE);
        if (lv_area_intersect(&overlap, &dim_bounds, &area_bounds)) return false;
    }
    if (backdrop_static) {
        bool has_content = false;
        uint32_t children = lv_obj_get_child_count(backdrop_wrapper);
        for (uint32_t i = 0; i < children && !has_content; ++i) {
            lv_obj_t * child = lv_obj_get_child(backdrop_wrapper, i);
            has_content = child != backdrop_image && !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN);
        }
        /* A solid fill is cheaper than a cached full-screen image. */
        if (!has_content) return false;
        backdrop_static_cache_key_t key;
        if (backdrop_static_cache_key_read(&key)) {
            /* An unmodified native image already costs only a direct copy per
             * refresh. Retain the rendered form when recoloring is enabled,
             * since that work is repeated for every source pixel otherwise. */
            if (key.recolor_opa == LV_OPA_TRANSP) return false;
            if (backdrop_static_retained && backdrop_static_cache_key_matches()) return true;
            if (backdrop_static_snapshot_retained()) return true;
        }
    }
    bool opaque = lv_obj_get_style_bg_opa(backdrop_wrapper, LV_PART_MAIN) == LV_OPA_COVER;
    lv_color_format_t format = opaque ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_ARGB8888;
    uint32_t pixel_bytes = lv_color_format_get_size(format);
    uint64_t max_data = (uint64_t) width * (uint64_t) height * pixel_bytes;
    if (max_data * (backdrop_static ? 1U : 3U) > BACKDROP_MAX_BYTES) return false;
    if (backdrop_static) {
        backdrop_closed = lv_snapshot_take(backdrop_wrapper, format);
        if (!backdrop_closed || backdrop_closed->header.cf != format ||
            backdrop_closed->header.w != (uint32_t) width ||
            backdrop_closed->header.h != (uint32_t) height ||
            backdrop_closed->header.stride < (uint32_t) width * pixel_bytes ||
            (uint64_t) backdrop_closed->header.stride * backdrop_closed->header.h > backdrop_closed->data_size ||
            backdrop_closed->data_size > BACKDROP_MAX_BYTES) {
            backdrop_free_frames();
            return false;
        }
        if (format == LV_COLOR_FORMAT_ARGB8888) {
            /* Transparent containers can hold an opaque image. Inspect the
             * rendered alpha, then reuse this allocation as a faster RGB565
             * image only when every pixel is proven opaque. */
            opaque = true;
            for (uint32_t y = 0; y < backdrop_closed->header.h && opaque; ++y) {
                const uint8_t * row = backdrop_closed->data + (size_t)y * backdrop_closed->header.stride;
                for (uint32_t x = 0; x < backdrop_closed->header.w; ++x) {
                    if (row[(size_t)x * 4U + 3U] != LV_OPA_COVER) {
                        opaque = false;
                        break;
                    }
                }
            }
            if (opaque) {
                uint32_t source_stride = backdrop_closed->header.stride;
                uint32_t stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);
                /* Forward conversion is safe in place: each output row is
                 * shorter than its input row, and reads precede writes. */
                transition_compositor_blend_argb8888_over_rgb565(backdrop_closed->data, stride,
                    backdrop_closed->data, source_stride, width, height);
                lv_draw_buf_reshape(backdrop_closed, LV_COLOR_FORMAT_RGB565, width, height, stride);
                lv_draw_buf_clear_flag(backdrop_closed, LV_IMAGE_FLAGS_PREMULTIPLIED);
            }
        }
        lv_image_set_src(backdrop_image, backdrop_closed);
        return true;
    }

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
    if (!backdrop_wrapper || (!backdrop_static && (!backdrop_dim || !backdrop_area)) || !backdrop_image ||
        backdrop_warm_timer || backdrop_closed || backdrop_busy) return;
    backdrop_warm_timer = lv_timer_create(backdrop_warm_timer_cb, BACKDROP_WARM_DELAY_MS, NULL);
    if (backdrop_warm_timer) lv_timer_set_repeat_count(backdrop_warm_timer, 1);
}

void player_lyrics_backdrop_bind(lv_obj_t * wrapper, lv_obj_t * dim, lv_obj_t * area,
                                 lv_anim_timeline_t * open_timeline,
                                 lv_anim_timeline_t * close_timeline) {
    bool static_mode = !dim && !area;

    /* Built-in lyric timelines are rebuilt when the direction changes. Keep
     * the static snapshot when the same wrapper and its source/style key are
     * still valid, while dropping every reference to the old timelines before
     * their owner deletes them. The NULL pair is a synchronous detach point;
     * no warm capture is started until a replacement pair is accepted. */
    if (wrapper && wrapper == backdrop_wrapper && backdrop_static && static_mode) {
        bool detached = !open_timeline && !close_timeline;
        bool preserve_cache = backdrop_static_retained && backdrop_static_cache_key_matches();

        player_lyrics_backdrop_end(preserve_cache);
        backdrop_cancel_warm();
        if (!preserve_cache) backdrop_free_frames();

        /* Clear first so even a rejected replacement cannot leave stale
         * pointers to timelines that the caller is about to destroy. */
        backdrop_open_timeline = NULL;
        backdrop_close_timeline = NULL;
        if (detached) return;

        if (!open_timeline || !close_timeline) {
            player_lyrics_backdrop_teardown();
            return;
        }
        backdrop_open_timeline = open_timeline;
        backdrop_close_timeline = close_timeline;
        if (!backdrop_load_transitions(open_timeline, close_timeline)) {
            player_lyrics_backdrop_teardown();
            return;
        }
        backdrop_schedule_warm();
        return;
    }

    player_lyrics_backdrop_teardown();
    if (!wrapper || ((!dim || !area) && !static_mode) ||
        (!static_mode && lv_obj_get_style_bg_opa(wrapper, LV_PART_MAIN) != LV_OPA_COVER) ||
        lv_obj_get_style_opa(wrapper, LV_PART_MAIN) != LV_OPA_COVER) return;
    backdrop_wrapper = wrapper;
    backdrop_dim = dim;
    backdrop_area = area;
    backdrop_static = static_mode;
    if (!backdrop_load_transitions(open_timeline, close_timeline)) {
        backdrop_wrapper = NULL;
        backdrop_dim = NULL;
        backdrop_area = NULL;
        return;
    }
    if (!backdrop_track_object(backdrop_wrapper, &backdrop_wrapper) ||
        (backdrop_dim && !backdrop_track_object(backdrop_dim, &backdrop_dim)) ||
        (backdrop_area && !backdrop_track_object(backdrop_area, &backdrop_area))) {
        player_lyrics_backdrop_teardown();
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
    if (!backdrop_track_object(backdrop_image, &backdrop_image)) {
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
    player_lyrics_backdrop_end(false);
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
    /* This also covers delayed starts without early_apply: the authored
     * value must match the pixels represented by the first cached frame. */
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

static void backdrop_frame_at_time(uint32_t timeline_elapsed) {
    if (!backdrop_active || !backdrop_timeline || !backdrop_frame) return;
    const player_layout_style_transition_t * dim_transition = backdrop_opening ?
        &backdrop_open_dim_transition : &backdrop_close_dim_transition;
    const player_layout_style_transition_t * area_transition = backdrop_opening ?
        &backdrop_open_area_transition : &backdrop_close_area_transition;
    const player_layout_style_transition_t * transition =
        dim_transition->start_value != dim_transition->end_value ? dim_transition : area_transition;
    int32_t progress = backdrop_transition_value(transition, timeline_elapsed);
    int32_t mix = (int32_t)(((int64_t)progress * 256) / 1024);
    if (mix < 0) mix = 0;
    if (mix > 256) mix = 256;
    if (!backdrop_opening) mix = 256 - mix;
    backdrop_render_mix((uint32_t)mix);
}

static void backdrop_frame_timer_cb(lv_timer_t * timer) {
    (void) timer;
    if (backdrop_timeline) backdrop_frame_at_time(backdrop_timeline_elapsed(backdrop_timeline));
}

static void backdrop_timeline_observer_cb(lv_anim_timeline_t * timeline, uint32_t act_time, void * user_data) {
    (void) user_data;
    if (timeline == backdrop_timeline) backdrop_frame_at_time(act_time);
}

bool player_lyrics_backdrop_begin(bool opening) {
    if (!backdrop_wrapper || !backdrop_image || !backdrop_closed ||
        (!backdrop_static && (!backdrop_open || !backdrop_frame)) ||
        backdrop_active || backdrop_busy) return false;
    if (backdrop_static) {
        if (!backdrop_load_transitions(backdrop_open_timeline, backdrop_close_timeline)) return false;
        if (backdrop_static_retained && !backdrop_static_cache_key_matches()) {
            player_lyrics_backdrop_invalidate();
            return false;
        }
        if (backdrop_static_bound) {
            backdrop_active = true;
            backdrop_busy = true;
            backdrop_cache_observer_update();
            return true;
        }
        uint32_t child_count = lv_obj_get_child_count(backdrop_wrapper);
        if (child_count > backdrop_child_capacity ||
            backdrop_closed->header.w != (uint32_t) lv_obj_get_width(backdrop_wrapper) ||
            backdrop_closed->header.h != (uint32_t) lv_obj_get_height(backdrop_wrapper)) return false;
        if (!backdrop_capture_children(child_count)) return false;
        backdrop_active = true;
        backdrop_busy = true;
        backdrop_static_bound = true;
        backdrop_cache_observer_update();
        lv_obj_remove_flag(backdrop_image, LV_OBJ_FLAG_HIDDEN);
        for (uint32_t i = 0; i < backdrop_child_count; ++i)
            lv_obj_add_flag(backdrop_children[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(backdrop_wrapper);
        return true;
    }
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
    lv_anim_timeline_t * timeline = opening ? backdrop_open_timeline : backdrop_close_timeline;

    if (!backdrop_capture_children(child_count)) return false;
    backdrop_opening = opening;
    backdrop_timeline = timeline;
    backdrop_active = true;
    backdrop_busy = true;
    backdrop_observer_attached = player_layouts_timeline_observer_attach(
        timeline, backdrop_timeline_observer_cb, NULL);
    if (!backdrop_observer_attached) {
        backdrop_frame_timer = lv_timer_create(backdrop_frame_timer_cb, BACKDROP_FRAME_PERIOD_MS, NULL);
        if (!backdrop_frame_timer) {
            backdrop_active = false;
            backdrop_busy = false;
            backdrop_timeline = NULL;
            backdrop_untrack_saved_children();
            backdrop_child_count = 0;
            return false;
        }
    }
    backdrop_render_mix(opening ? 0U : 256U);
    for (uint32_t i = 0; i < backdrop_child_count; ++i)
        lv_obj_add_flag(backdrop_children[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(backdrop_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(backdrop_wrapper);
    return true;
}
