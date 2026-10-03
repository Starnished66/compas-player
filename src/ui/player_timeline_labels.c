/* Raster proxies for XML-animated labels. The underlying labels stay alive
 * and keep their marquee state; only timeline-owned style writes are held
 * while the cached glyphs move inside a clipped image. */
#include "player_timeline_labels.h"
#include "player_layouts.h"

#include "lvgl/src/draw/snapshot/lv_snapshot.h"
#include "lvgl/src/draw/lv_draw_buf.h"
#include "lvgl/src/core/lv_obj_tree.h"
#include "lvgl/src/core/lv_obj_draw.h"
#include "lvgl/src/widgets/label/lv_label.h"
#include "lvgl/src/misc/lv_text.h"
#include "lvgl/src/misc/lv_event_private.h"

#include <stdlib.h>
#include <string.h>

#define LABEL_PROXY_MAX 8
#define LABEL_ANIMATION_MAX 6
#define LABEL_TARGETS_MAX 64
#define LABEL_SNAPSHOT_BYTES_MAX (512U * 1024U)
#define LABEL_PROXY_TIMER_MS 16

typedef struct {
    player_layout_style_transition_t transition;
    bool suppressed;
} label_animation_t;

typedef struct {
    lv_obj_t * label;
    lv_obj_t * clip;
    lv_obj_t * image;
    lv_draw_buf_t * frame;
    label_animation_t animations[LABEL_ANIMATION_MAX];
    uint8_t animation_count;
    lv_label_long_mode_t long_mode;
    int32_t x, y, width, height;
    int32_t capture_width, capture_height;
    int32_t frame_inset_x, frame_inset_y;
    int32_t text_width;
    int32_t translate_x, translate_y;
    int32_t pad_left, pad_right;
    lv_text_align_t captured_align;
    bool was_hidden;
    bool active;
} label_proxy_t;

static label_proxy_t proxies[LABEL_PROXY_MAX];
static uint8_t proxy_count;
static size_t snapshot_bytes;
static lv_anim_timeline_t * active_timeline;
static lv_timer_t * proxy_timer;

static bool prop_is_supported(lv_style_prop_t prop) {
    return prop == LV_STYLE_WIDTH || prop == LV_STYLE_HEIGHT ||
           prop == LV_STYLE_TRANSLATE_X || prop == LV_STYLE_TRANSLATE_Y ||
           prop == LV_STYLE_TEXT_ALIGN;
}

static label_animation_t * find_animation(label_proxy_t * proxy, lv_style_prop_t prop) {
    for (uint8_t i = 0; i < proxy->animation_count; ++i)
        if (proxy->animations[i].transition.prop == prop) return &proxy->animations[i];
    return NULL;
}

static bool alignment_valid(int32_t align) {
    return align >= LV_TEXT_ALIGN_LEFT && align <= LV_TEXT_ALIGN_RIGHT;
}

static int32_t alignment_offset(int32_t align, int32_t width, int32_t ink_width,
                                int32_t pad_left, int32_t pad_right) {
    int32_t available = width - pad_left - pad_right - ink_width;
    if (available < 0) available = 0;
    if (align == LV_TEXT_ALIGN_CENTER) return available / 2;
    if (align == LV_TEXT_ALIGN_RIGHT) return available;
    return 0;
}

static bool get_current_value(label_animation_t * animation, int32_t * value) {
    player_layout_style_transition_t current;
    /* Re-fetch this animation by its identifying tuple so current_value comes
     * from the prepared timeline's sampled path and not a stale snapshot. */
    if (!player_layouts_timeline_style_transition(active_timeline,
            animation->transition.target, animation->transition.prop,
            animation->transition.selector, &current) || !current.has_current_value) return false;
    *value = current.current_value;
    animation->transition.animation.act_time = current.animation.act_time;
    return true;
}

static int32_t captured_property_value(label_proxy_t * proxy, lv_style_prop_t prop) {
    switch (prop) {
        case LV_STYLE_WIDTH: return proxy->width;
        case LV_STYLE_HEIGHT: return proxy->height;
        case LV_STYLE_TRANSLATE_X: return proxy->translate_x;
        case LV_STYLE_TRANSLATE_Y: return proxy->translate_y;
        case LV_STYLE_TEXT_ALIGN: return proxy->captured_align;
        default: return 0;
    }
}

static uint32_t transition_fraction(label_animation_t * animation) {
    const player_layout_style_transition_t * t = &animation->transition;
    if (t->duration == 0) return 0;
    if (t->start_value == t->end_value) {
        int32_t sampled_value;
        if (!get_current_value(animation, &sampled_value)) return 0;
        (void) sampled_value;
        lv_anim_t sample = t->animation;
        sample.start_value = 0;
        sample.end_value = 1024;
        int32_t act_time = sample.act_time;
        if (act_time < 0) return 0;
        if (act_time > sample.duration) act_time = sample.duration;
        sample.act_time = act_time;
        int32_t fraction = sample.path_cb ? sample.path_cb(&sample) : 0;
        if (fraction < 0) return 0;
        if (fraction > 1024) return 1024;
        return (uint32_t) fraction;
    }
    int32_t value;
    if (!get_current_value(animation, &value)) value = t->start_value;
    int64_t numerator = (int64_t) (value - t->start_value) * 1024;
    int64_t denominator = (int64_t) t->end_value - t->start_value;
    int64_t fraction = numerator / denominator;
    if (fraction < 0) return 0;
    if (fraction > 1024) return 1024;
    return (uint32_t) fraction;
}

static int32_t sampled_proxy_value(label_proxy_t * proxy, label_animation_t * animation) {
    const player_layout_style_transition_t * t = &animation->transition;
    int32_t value;
    return get_current_value(animation, &value) ? value : captured_property_value(proxy, t->prop);
}

static void update_proxy(label_proxy_t * proxy) {
    label_animation_t * width_anim = find_animation(proxy, LV_STYLE_WIDTH);
    label_animation_t * height_anim = find_animation(proxy, LV_STYLE_HEIGHT);
    label_animation_t * tx_anim = find_animation(proxy, LV_STYLE_TRANSLATE_X);
    label_animation_t * ty_anim = find_animation(proxy, LV_STYLE_TRANSLATE_Y);
    label_animation_t * align_anim = find_animation(proxy, LV_STYLE_TEXT_ALIGN);
    int32_t width = proxy->width;
    int32_t height = proxy->height;
    int32_t x = proxy->x;
    int32_t y = proxy->y;
    if (width_anim) width = sampled_proxy_value(proxy, width_anim);
    if (height_anim) height = sampled_proxy_value(proxy, height_anim);
    if (tx_anim) x += sampled_proxy_value(proxy, tx_anim) - proxy->translate_x;
    if (ty_anim) y += sampled_proxy_value(proxy, ty_anim) - proxy->translate_y;
    if (width < 0) width = 0;
    if (height < 0) height = 0;

    int32_t align_offset = 0;
    if (align_anim) {
        const player_layout_style_transition_t * t = &align_anim->transition;
        label_animation_t * motion = width_anim ? width_anim : (tx_anim ? tx_anim : (ty_anim ? ty_anim : align_anim));
        uint32_t fraction = transition_fraction(motion);
        int32_t from = alignment_offset(proxy->captured_align, width, proxy->text_width,
                                       proxy->pad_left, proxy->pad_right);
        int32_t to = alignment_offset(t->end_value, width, proxy->text_width,
                                     proxy->pad_left, proxy->pad_right);
        align_offset = from + (int32_t) (((int64_t) (to - from) * fraction) / 1024);
    } else {
        align_offset = alignment_offset(proxy->captured_align, width, proxy->text_width,
                                        proxy->pad_left, proxy->pad_right);
    }
    int32_t inset_x = proxy->frame_inset_x;
    int32_t inset_y = proxy->frame_inset_y;
    lv_obj_set_pos(proxy->clip, x, y);
    lv_obj_set_size(proxy->clip, width, height);
    lv_obj_set_pos(proxy->image, -inset_x + align_offset, -inset_y);
}

static void proxy_timer_cb(lv_timer_t * timer) {
    (void) timer;
    for (uint8_t i = 0; i < proxy_count; ++i) update_proxy(&proxies[i]);
}

static bool label_has_unsupported_style(label_proxy_t * proxy, uint32_t style_count) {
    (void) style_count;
    for (uint32_t i = 0; i < style_count; ++i) {
        player_layout_style_transition_t t;
        if (!player_layouts_timeline_get_style_animation(active_timeline, i, &t)) return true;
        if (t.target != proxy->label) continue;
        if (!prop_is_supported(t.prop) || t.selector != LV_PART_MAIN || proxy->animation_count >= LABEL_ANIMATION_MAX)
            return true;
        for (uint8_t j = 0; j < proxy->animation_count; ++j)
            if (proxy->animations[j].transition.prop == t.prop) return true;
        proxy->animations[proxy->animation_count++].transition = t;
    }
    if (proxy->animation_count == 0) return true;
    if (player_layouts_timeline_animation_count_for_obj(active_timeline, proxy->label) != proxy->animation_count)
        return true;
    label_animation_t * align = find_animation(proxy, LV_STYLE_TEXT_ALIGN);
    if (align && (!alignment_valid(align->transition.start_value) || !alignment_valid(align->transition.end_value)))
        return true;
    if (lv_label_get_long_mode(proxy->label) == LV_LABEL_LONG_MODE_WRAP &&
        find_animation(proxy, LV_STYLE_WIDTH)) return true;
    return false;
}

/* Click callbacks do not affect the captured pixels. Custom drawing does. */
static bool label_has_custom_drawing(lv_obj_t * label) {
    for (uint32_t i = 0; i < lv_obj_get_event_count(label); ++i) {
        lv_event_dsc_t * event = lv_obj_get_event_dsc(label, i);
        if (!event || (event->filter & LV_EVENT_MARKED_DELETING)) continue;
        uint32_t filter = event->filter & ~LV_EVENT_PREPROCESS;
        if (filter == LV_EVENT_ALL ||
            (filter >= LV_EVENT_COVER_CHECK && filter <= LV_EVENT_DRAW_TASK_ADDED)) return true;
    }
    return false;
}

/* Snapshot a private label so capture never resets the live marquee phase.
 * One owned style avoids refreshing the clone once per copied property. */
static lv_draw_buf_t * snapshot_private_label(lv_obj_t * source, int32_t width, int32_t height) {
    static const lv_style_prop_t props[] = {
        LV_STYLE_WIDTH, LV_STYLE_HEIGHT,
        LV_STYLE_PAD_TOP, LV_STYLE_PAD_BOTTOM, LV_STYLE_PAD_LEFT, LV_STYLE_PAD_RIGHT,
        LV_STYLE_BG_OPA, LV_STYLE_BG_COLOR, LV_STYLE_RADIUS,
        LV_STYLE_BG_GRAD, LV_STYLE_BG_GRAD_DIR, LV_STYLE_BG_GRAD_OPA,
        LV_STYLE_BG_GRAD_COLOR, LV_STYLE_BG_MAIN_STOP, LV_STYLE_BG_GRAD_STOP,
        LV_STYLE_BG_IMAGE_SRC, LV_STYLE_BG_IMAGE_OPA, LV_STYLE_BG_IMAGE_RECOLOR_OPA,
        LV_STYLE_BG_IMAGE_TILED, LV_STYLE_BG_IMAGE_RECOLOR,
        LV_STYLE_BORDER_WIDTH, LV_STYLE_BORDER_OPA, LV_STYLE_BORDER_COLOR, LV_STYLE_BORDER_SIDE,
        LV_STYLE_OUTLINE_WIDTH, LV_STYLE_OUTLINE_OPA, LV_STYLE_OUTLINE_COLOR, LV_STYLE_OUTLINE_PAD,
        LV_STYLE_SHADOW_WIDTH, LV_STYLE_SHADOW_OPA, LV_STYLE_SHADOW_COLOR,
        LV_STYLE_SHADOW_OFFSET_X, LV_STYLE_SHADOW_OFFSET_Y, LV_STYLE_SHADOW_SPREAD,
        LV_STYLE_TEXT_FONT, LV_STYLE_TEXT_OPA, LV_STYLE_TEXT_COLOR, LV_STYLE_TEXT_ALIGN,
        LV_STYLE_TEXT_LETTER_SPACE, LV_STYLE_TEXT_LINE_SPACE,
        LV_STYLE_TEXT_OUTLINE_STROKE_WIDTH, LV_STYLE_TEXT_OUTLINE_STROKE_OPA,
        LV_STYLE_TEXT_OUTLINE_STROKE_COLOR, LV_STYLE_TEXT_DECOR,
        LV_STYLE_OPA, LV_STYLE_OPA_LAYERED, LV_STYLE_BLEND_MODE,
        LV_STYLE_COLOR_FILTER_DSC, LV_STYLE_COLOR_FILTER_OPA
    };
    lv_style_t style;
    lv_style_init(&style);
    for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); ++i) {
        lv_style_value_t value = lv_obj_get_style_prop(source, LV_PART_MAIN, props[i]);
        lv_style_set_prop(&style, props[i], value);
    }
    lv_style_value_t value = {.num = width};
    lv_style_set_prop(&style, LV_STYLE_WIDTH, value);
    value.num = height;
    lv_style_set_prop(&style, LV_STYLE_HEIGHT, value);
    value.num = LV_TEXT_ALIGN_LEFT;
    lv_style_set_prop(&style, LV_STYLE_TEXT_ALIGN, value);
    lv_obj_t * clone = lv_label_create(lv_obj_get_parent(source));
    if (!clone) {
        lv_style_reset(&style);
        return NULL;
    }
    lv_obj_remove_style_all(clone);
    lv_obj_add_flag(clone, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(clone, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_state(clone, lv_obj_get_state(source));
    lv_obj_add_style(clone, &style, LV_PART_MAIN | lv_obj_get_state(source));
    lv_label_set_long_mode(clone, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(clone, lv_label_get_text(source));
    lv_obj_set_pos(clone, lv_obj_get_x(source), lv_obj_get_y(source));
    lv_obj_update_layout(clone);
    lv_draw_buf_t * frame = lv_snapshot_take(clone, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_delete(clone);
    lv_style_reset(&style);
    return frame;
}

static bool make_proxy(label_proxy_t * proxy) {
    lv_obj_t * label = proxy->label;
    if (lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN) || !lv_label_get_text(label) || !lv_label_get_text(label)[0]) return false;
    lv_obj_t * parent = lv_obj_get_parent(label);
    if (!parent) return false;
    lv_align_t alignment = lv_obj_get_style_align(label, LV_PART_MAIN);
    if (alignment != LV_ALIGN_DEFAULT && alignment != LV_ALIGN_TOP_LEFT) return false;
    if (lv_obj_get_style_layout(parent, LV_PART_MAIN) != LV_LAYOUT_NONE &&
        !lv_obj_has_flag(label, LV_OBJ_FLAG_IGNORE_LAYOUT)) return false;
    /* A moving parent can reflow a child while the original label is hidden. */
    for (lv_obj_t * ancestor = parent; ancestor; ancestor = lv_obj_get_parent(ancestor))
        if (player_layouts_timeline_animation_count_for_obj(active_timeline, ancestor) != 0) return false;
    if (lv_obj_get_style_bg_opa(label, LV_PART_MAIN) != LV_OPA_TRANSP ||
        lv_obj_get_style_border_width(label, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_outline_width(label, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_shadow_width(label, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_base_dir(label, LV_PART_MAIN) != LV_BASE_DIR_LTR ||
        lv_obj_get_style_transform_rotation(label, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_transform_scale_x(label, LV_PART_MAIN) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_scale_y(label, LV_PART_MAIN) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_skew_x(label, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_transform_skew_y(label, LV_PART_MAIN) != 0 ||
        label_has_custom_drawing(label)) return false;
    for (lv_obj_t * ancestor = label; ancestor; ancestor = lv_obj_get_parent(ancestor))
        if (lv_obj_get_style_opa(ancestor, LV_PART_MAIN) != LV_OPA_COVER) return false;

    proxy->long_mode = lv_label_get_long_mode(label);
    proxy->x = lv_obj_get_x(label);
    proxy->y = lv_obj_get_y(label);
    proxy->width = lv_obj_get_width(label);
    proxy->height = lv_obj_get_height(label);
    if (proxy->width <= 0 || proxy->height <= 0) return false;
    int32_t authored_width = lv_obj_get_style_width(label, LV_PART_MAIN);
    int32_t authored_height = lv_obj_get_style_height(label, LV_PART_MAIN);
    if (authored_width <= 0 || authored_height <= 0 ||
        LV_COORD_IS_PCT(authored_width) || LV_COORD_IS_PCT(authored_height)) return false;
    proxy->capture_width = proxy->width;
    proxy->capture_height = proxy->height;
    label_animation_t * width_anim = find_animation(proxy, LV_STYLE_WIDTH);
    if (width_anim) {
        if (width_anim->transition.start_value <= 0 || width_anim->transition.end_value <= 0) return false;
        if (width_anim->transition.start_value > proxy->capture_width)
            proxy->capture_width = width_anim->transition.start_value;
        if (width_anim->transition.end_value > proxy->capture_width)
            proxy->capture_width = width_anim->transition.end_value;
        if ((int64_t) proxy->capture_width > (int64_t) proxy->width * 4) return false;
    }
    label_animation_t * height_anim = find_animation(proxy, LV_STYLE_HEIGHT);
    if (height_anim) {
        if (height_anim->transition.start_value <= 0 || height_anim->transition.end_value <= 0) return false;
        if (height_anim->transition.start_value > proxy->capture_height)
            proxy->capture_height = height_anim->transition.start_value;
        if (height_anim->transition.end_value > proxy->capture_height)
            proxy->capture_height = height_anim->transition.end_value;
        if ((int64_t) proxy->capture_height > (int64_t) proxy->height * 4) return false;
    }
    int32_t ext = lv_obj_calculate_ext_draw_size(label, LV_PART_MAIN);
    if (ext < 0) ext = 0;
    uint64_t estimated_width = (uint64_t) proxy->capture_width + (uint64_t) ext * 2U;
    uint64_t estimated_height = (uint64_t) proxy->capture_height + (uint64_t) ext * 2U;
    if (estimated_width > UINT32_MAX || estimated_height > UINT32_MAX ||
        estimated_width > UINT64_MAX / estimated_height / 4U) return false;
    uint64_t estimated = estimated_width * estimated_height * 4U;
    if (estimated > LABEL_SNAPSHOT_BYTES_MAX - snapshot_bytes) return false;
    proxy->captured_align = (lv_text_align_t) lv_obj_get_style_text_align(label, LV_PART_MAIN);
    if (!alignment_valid(proxy->captured_align)) return false;
    proxy->translate_x = lv_obj_get_style_translate_x(label, LV_PART_MAIN);
    proxy->translate_y = lv_obj_get_style_translate_y(label, LV_PART_MAIN);
    if (LV_COORD_IS_PCT(proxy->translate_x) || LV_COORD_IS_PCT(proxy->translate_y)) return false;
    proxy->pad_left = lv_obj_get_style_pad_left(label, LV_PART_MAIN);
    proxy->pad_right = lv_obj_get_style_pad_right(label, LV_PART_MAIN);
    proxy->frame_inset_x = 0;
    proxy->frame_inset_y = 0;
    lv_point_t text_size;
    lv_text_get_size(&text_size, lv_label_get_text(label),
                     lv_obj_get_style_text_font(label, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(label, LV_PART_MAIN),
                     lv_obj_get_style_text_line_space(label, LV_PART_MAIN),
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    proxy->text_width = text_size.x;
    if (proxy->text_width <= 0 || strchr(lv_label_get_text(label), '\n') ||
        strchr(lv_label_get_text(label), '\r')) return false;
    int32_t live_text_width = proxy->width - proxy->pad_left - proxy->pad_right;
    int32_t narrowest_width = live_text_width;
    if (width_anim) {
        int32_t start_width = width_anim->transition.start_value - proxy->pad_left - proxy->pad_right;
        int32_t end_width = width_anim->transition.end_value - proxy->pad_left - proxy->pad_right;
        if (start_width < narrowest_width) narrowest_width = start_width;
        if (end_width < narrowest_width) narrowest_width = end_width;
    }
    if ((proxy->long_mode == LV_LABEL_LONG_MODE_WRAP ||
         proxy->long_mode == LV_LABEL_LONG_MODE_SCROLL ||
         proxy->long_mode == LV_LABEL_LONG_MODE_SCROLL_CIRCULAR ||
         proxy->long_mode == LV_LABEL_LONG_MODE_DOTS) && proxy->text_width > narrowest_width) return false;
    proxy->frame = snapshot_private_label(label, proxy->capture_width, proxy->capture_height);
    if (!proxy->frame) return false;
    if (proxy->frame->data_size > LABEL_SNAPSHOT_BYTES_MAX - snapshot_bytes) {
        lv_draw_buf_destroy(proxy->frame);
        proxy->frame = NULL;
        return false;
    }
    snapshot_bytes += proxy->frame->data_size;
    proxy->frame_inset_x = ((int32_t) proxy->frame->header.w - proxy->capture_width) / 2;
    proxy->frame_inset_y = ((int32_t) proxy->frame->header.h - proxy->capture_height) / 2;
    proxy->clip = lv_obj_create(parent);
    if (!proxy->clip) return false;
    lv_obj_remove_style_all(proxy->clip);
    lv_obj_add_flag(proxy->clip, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(proxy->clip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    proxy->image = lv_image_create(proxy->clip);
    if (!proxy->image) return false;
    lv_obj_remove_style_all(proxy->image);
    lv_image_set_src(proxy->image, proxy->frame);
    int32_t index = lv_obj_get_index(label);
    if (index >= 0) lv_obj_move_to_index(proxy->clip, index + 1);
    for (uint8_t i = 0; i < proxy->animation_count; ++i) {
        player_layout_style_transition_t * t = &proxy->animations[i].transition;
        if (!player_layouts_timeline_suppress_style(active_timeline, label, t->prop, t->selector, true))
            return false;
        proxy->animations[i].suppressed = true;
    }
    proxy->was_hidden = lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    proxy->active = true;
    update_proxy(proxy);
    return true;
}

static void release_proxy(label_proxy_t * proxy, bool commit_endpoints) {
    for (uint8_t i = 0; i < proxy->animation_count; ++i) {
        label_animation_t * a = &proxy->animations[i];
        if (a->suppressed) {
            player_layouts_timeline_suppress_style(active_timeline, proxy->label,
                a->transition.prop, a->transition.selector, false);
            a->suppressed = false;
        }
        int32_t value = a->transition.end_value;
        bool commit = proxy->active && commit_endpoints;
        if (proxy->active && !commit_endpoints) {
            commit = get_current_value(a, &value);
        }
        if (commit) {
            lv_style_value_t style;
            style.num = value;
            lv_obj_set_local_style_prop(proxy->label, a->transition.prop, style,
                                        a->transition.selector);
        }
    }
    if (proxy->clip) lv_obj_delete(proxy->clip);
    if (proxy->frame) {
        if (snapshot_bytes >= proxy->frame->data_size) snapshot_bytes -= proxy->frame->data_size;
        lv_draw_buf_destroy(proxy->frame);
    }
    if (proxy->active && proxy->label && !proxy->was_hidden)
        lv_obj_remove_flag(proxy->label, LV_OBJ_FLAG_HIDDEN);
    memset(proxy, 0, sizeof(*proxy));
}

void player_timeline_labels_teardown(void) {
    if (proxy_timer) {
        lv_timer_delete(proxy_timer);
        proxy_timer = NULL;
    }
    while (proxy_count > 0) release_proxy(&proxies[--proxy_count], false);
    snapshot_bytes = 0;
    active_timeline = NULL;
}

void player_timeline_labels_end(void) {
    if (proxy_timer) {
        lv_timer_delete(proxy_timer);
        proxy_timer = NULL;
    }
    while (proxy_count > 0) release_proxy(&proxies[--proxy_count], true);
    snapshot_bytes = 0;
    active_timeline = NULL;
}

bool player_timeline_labels_begin(lv_anim_timeline_t * timeline) {
    player_timeline_labels_teardown();
    if (!timeline || !player_layouts_timeline_refresh_timing(timeline)) return false;
    active_timeline = timeline;
    uint32_t style_count = player_layouts_timeline_style_animation_count(timeline);
    lv_obj_t * attempted[LABEL_TARGETS_MAX];
    uint8_t attempted_count = 0;
    for (uint32_t i = 0; i < style_count && proxy_count < LABEL_PROXY_MAX; ++i) {
        player_layout_style_transition_t t;
        if (!player_layouts_timeline_get_style_animation(timeline, i, &t) || !t.target ||
            !lv_obj_check_type(t.target, &lv_label_class)) continue;
        bool already_attempted = false;
        for (uint8_t j = 0; j < attempted_count; ++j)
            if (attempted[j] == t.target) already_attempted = true;
        if (already_attempted) continue;
        if (attempted_count >= LABEL_TARGETS_MAX) continue;
        attempted[attempted_count++] = t.target;
        label_proxy_t candidate;
        memset(&candidate, 0, sizeof(candidate));
        candidate.label = t.target;
        if (label_has_unsupported_style(&candidate, style_count)) continue;
        if (!make_proxy(&candidate)) {
            release_proxy(&candidate, false);
            continue;
        }
        proxies[proxy_count++] = candidate;
    }
    if (!proxy_count) {
        active_timeline = NULL;
        return false;
    }
    /* A timeline returned by XML can retain an old end progress. Captures use
     * the current rendered alignment above; now reset under suppression so
     * the first proxy frame reflects the timeline's true starting geometry. */
    lv_anim_timeline_set_progress(timeline, 0);
    for (uint8_t i = 0; i < proxy_count; ++i) update_proxy(&proxies[i]);
    proxy_timer = lv_timer_create(proxy_timer_cb, LABEL_PROXY_TIMER_MS, NULL);
    if (!proxy_timer) {
        player_timeline_labels_teardown();
        return false;
    }
    proxy_timer_cb(proxy_timer);
    return true;
}
