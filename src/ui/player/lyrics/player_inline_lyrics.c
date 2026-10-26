#include "player_inline_lyrics.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../core/system/runtime/board_config.h"
#include "../screen/gui_lyrics.h"
#include "../../shell/gui_navigation.h"
#include "../../appearance/gui_theme.h"
#include "../artwork/player_cover_fade.h"
#include "../layout/player_layout_internal.h"
#include "../layout/player_layouts.h"
#include "../artwork/player_lyrics_backdrop.h"
#include "player_timeline_labels.h"
#include "../../diagnostics/ui_frame_trace.h"
#include "../../effects/transition_compositor.h"
#include "lvgl/src/core/lv_obj_draw_private.h"
#include "lvgl/src/draw/sw/lv_draw_sw_private.h"

#define PLAYER_MAX_HIT_TARGETS 8
#define PLAYER_MAX_LYRICS_CONTROLS 16
#define PLAYER_LYRICS_GEOMETRY_OBJECT_COUNT 4
#define PLAYER_TIMELINE_COVER_LIVE_DECORATION_MAX 8

static player_inline_lyrics_view_t player_inline_view;
static lv_obj_t * player_inline_hit_targets[PLAYER_MAX_HIT_TARGETS];
static unsigned player_inline_hit_target_count;
static lv_obj_t * player_inline_controls[PLAYER_MAX_LYRICS_CONTROLS];
static unsigned player_inline_control_count;
static lv_draw_buf_t * player_timeline_cover_frame;
static const void * player_timeline_cover_source;
static bool player_timeline_cover_source_owned;
static lv_draw_buf_t * player_timeline_cover_cache;
static uint64_t player_timeline_cover_cache_generation;
static uint64_t player_timeline_cover_style_epoch;
static uint64_t player_timeline_cover_cache_epoch;
static bool player_timeline_cover_cache_antialias;
static bool player_timeline_cover_internal;
static lv_display_t * player_timeline_cover_cache_display;

/* The live parent screen is intentionally queried: during screen construction
 * and deletion the Player global changes only at the original assignment seam. */
#define player_screen (player_inline_view.current_screen ? player_inline_view.current_screen() : NULL)
#define cover_card (player_inline_view.cover_card_obj)
#define cover_img (player_inline_view.cover_image)
#define player_cover_thumbnail (player_inline_view.cover_thumbnail)
#define player_overlay_panel (player_inline_view.overlay)
#define song_title_label (player_inline_view.title)
#define artist_label (player_inline_view.artist)
#define album_label (player_inline_view.album)
#define favorite_circle (player_inline_view.favorite_circle_obj)
#define player_lyrics_area (player_inline_view.lyrics_area)
#define player_layout_builtin (player_inline_view.builtin_layout)
#define player_artist_standin (player_inline_view.artist_is_standin)
#define player_album_standin (player_inline_view.album_is_standin)
#define player_timeline_cover_fade (player_inline_view.cover_fade)
#define player_timeline_backdrop_covers_cover (player_inline_view.backdrop_contains_cover)
#define player_timeline_lyrics_open (player_inline_view.lyrics_open)
#define player_timeline_lyrics_close (player_inline_view.lyrics_close)
#define player_hit_targets player_inline_hit_targets
#define player_hit_target_count player_inline_hit_target_count
#define player_lyrics_controls player_inline_controls
#define player_lyrics_control_count player_inline_control_count

static player_inline_cover_info_t player_inline_cover_info(void) {
    player_inline_cover_info_t info = {0};
    (void) player_inline_view.query_cover(&info);
    return info;
}


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

static player_lyrics_geometry_t player_lyrics_geometry[PLAYER_LYRICS_GEOMETRY_OBJECT_COUNT];
static unsigned player_lyrics_object_count;
static bool player_lyrics_generated_timelines;

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
    if (refresh && requested && player_inline_view.display_refresh_allowed()) {
        lv_timer_resume(refresh);
        lv_timer_ready(refresh);
    }
}
static bool player_timeline_cover_antialias_saved;
static bool player_timeline_cover_antialias;
static bool player_timeline_cover_fade_was_hidden;
static int32_t player_timeline_cover_w;
static int32_t player_timeline_cover_h;
static bool player_timeline_cover_clip_corner;
static void player_lyrics_update_thumbnail_clickable(void) {
    player_inline_lyrics_update_thumbnail_clickable(player_cover_thumbnail);
}

void player_inline_lyrics_update_thumbnail_clickable(lv_obj_t *thumbnail) {
    if (!thumbnail) return;
    if (player_lyrics_open && !player_lyrics_animating)
        lv_obj_add_flag(thumbnail, LV_OBJ_FLAG_CLICKABLE);
    else
        lv_obj_remove_flag(thumbnail, LV_OBJ_FLAG_CLICKABLE);
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
    player_inline_view.fit_cover_img_to_card();
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

void player_inline_lyrics_invalidate_cover_source(void) {
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
    player_inline_lyrics_invalidate_cover_source();
}

/* Retention is deliberately narrower than the one-shot transition cache:
 * only a plain opaque bitmap can be reused at rest without baking live
 * decorations, inherited recolor, or translucent child blend rounding. */
void player_inline_lyrics_cover_tap_cb(lv_event_t * e);

static bool player_timeline_cover_has_only_owned_events(lv_obj_t * object) {
    for (uint32_t i = 0; i < lv_obj_get_event_count(object); ++i) {
        lv_event_dsc_t * event = lv_obj_get_event_dsc(object, i);
        lv_event_cb_t callback = lv_event_dsc_get_cb(event);
        if (callback != player_inline_lyrics_cover_tap_cb && callback != player_inline_view.cover_size_changed_cb &&
            callback != player_timeline_cover_style_changed_cb) return false;
    }
    return true;
}

static bool player_timeline_cover_can_retain(void) {
    if (!cover_card || !cover_img || !player_inline_cover_info().has_pixels ||
        player_inline_cover_info().descriptor->header.cf != LV_COLOR_FORMAT_RGB565 ||
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
        if (player_lyrics_animating && !player_lyrics_generated_timelines &&
            player_timeline_lyrics_open && player_timeline_lyrics_close &&
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
        player_timeline_cover_cache_generation != player_inline_cover_info().generation ||
        player_timeline_cover_cache_epoch != player_timeline_cover_style_epoch ||
        lv_image_get_src(cover_img) != player_timeline_cover_frame ||
        lv_obj_get_width(cover_card) != (int32_t) player_timeline_cover_cache->header.w ||
        lv_obj_get_height(cover_card) != (int32_t) player_timeline_cover_cache->header.h ||
        lv_image_get_antialias(cover_img)) {
        /* A caller explicitly enabling antialias on the kept image should
         * retain that setting when the original source is rebound. */
        if (cover_img && lv_image_get_antialias(cover_img)) player_timeline_cover_antialias = true;
        player_inline_lyrics_invalidate_cover_source();
    }
}

/* The native circular cover can retain a one-shot artwork snapshot while its
 * static circular border decorations stay live. Rounded rectangles and
 * authored timelines keep their existing live-rendering fallback. */
static bool player_timeline_cover_circle_snapshot(lv_anim_timeline_t * timeline) {
    if (!timeline || !player_lyrics_generated_timelines || !cover_card ||
        lv_obj_get_style_radius(cover_card, LV_PART_MAIN) != LV_RADIUS_CIRCLE ||
        player_lyrics_object_count == 0 || player_lyrics_geometry[0].obj != cover_card) return false;
    const player_lyrics_geometry_t * geometry = &player_lyrics_geometry[0];
    if (geometry->w <= 0 || geometry->h != geometry->w || geometry->compact_w <= 0 ||
        geometry->compact_h != geometry->compact_w) return false;

    bool has_circle_radius = false;
    uint32_t count = player_layouts_timeline_style_animation_count(timeline);
    for (uint32_t i = 0; i < count; ++i) {
        player_layout_style_transition_t animation;
        if (!player_layouts_timeline_get_style_animation(timeline, i, &animation)) return false;
        if (animation.target == cover_card && animation.prop == LV_STYLE_RADIUS) {
            if (animation.selector != LV_PART_MAIN || animation.start_value != LV_RADIUS_CIRCLE ||
                animation.end_value != LV_RADIUS_CIRCLE) return false;
            has_circle_radius = true;
        }
    }
    return has_circle_radius;
}

/* Only plain, static circular border objects wholly inside the circular card
 * may remain live after the card's rounded clipping is replaced by a snapshot. */
static bool player_timeline_cover_live_circle_decoration(lv_obj_t * child) {
    int32_t width_style = child ? lv_obj_get_style_width(child, LV_PART_MAIN) : 0;
    int32_t height_style = child ? lv_obj_get_style_height(child, LV_PART_MAIN) : 0;
    if (!child || !LV_COORD_IS_PCT(width_style) || !LV_COORD_IS_PCT(height_style) ||
        LV_COORD_GET_PCT(width_style) <= 0 || LV_COORD_GET_PCT(width_style) > 100 ||
        LV_COORD_GET_PCT(height_style) <= 0 || LV_COORD_GET_PCT(height_style) > 100 ||
        LV_COORD_GET_PCT(width_style) != LV_COORD_GET_PCT(height_style) ||
        lv_obj_get_style_min_width(child, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_min_height(child, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_max_width(child, LV_PART_MAIN) != LV_COORD_MAX ||
        lv_obj_get_style_max_height(child, LV_PART_MAIN) != LV_COORD_MAX ||
        lv_obj_get_style_align(child, LV_PART_MAIN) != LV_ALIGN_CENTER ||
        lv_obj_get_style_x(child, LV_PART_MAIN) != 0 || lv_obj_get_style_y(child, LV_PART_MAIN) != 0 ||
        lv_obj_get_style_pad_left(child, LV_PART_MAIN) || lv_obj_get_style_pad_right(child, LV_PART_MAIN) ||
        lv_obj_get_style_pad_top(child, LV_PART_MAIN) || lv_obj_get_style_pad_bottom(child, LV_PART_MAIN)) return false;
    if (lv_obj_get_class(child) != &lv_obj_class ||
        lv_obj_get_child_count(child) != 0 || lv_obj_get_ext_draw_size(child) != 0 ||
        lv_obj_get_event_count(child) != 0 ||
        lv_obj_get_layer_type(child) != LV_LAYER_TYPE_NONE ||
        lv_obj_get_style_opa_recursive(child, LV_PART_MAIN) != LV_OPA_COVER ||
        lv_obj_get_style_bg_opa(child, LV_PART_MAIN) != LV_OPA_TRANSP ||
        lv_obj_get_style_bg_image_src(child, LV_PART_MAIN) ||
        lv_obj_get_style_border_width(child, LV_PART_MAIN) <= 0 ||
        lv_obj_get_style_border_opa(child, LV_PART_MAIN) != LV_OPA_COVER ||
        lv_obj_get_style_radius(child, LV_PART_MAIN) != LV_RADIUS_CIRCLE ||
        lv_obj_get_style_shadow_width(child, LV_PART_MAIN) ||
        lv_obj_get_style_outline_width(child, LV_PART_MAIN) ||
        lv_obj_get_style_transform_width(child, LV_PART_MAIN) ||
        lv_obj_get_style_transform_height(child, LV_PART_MAIN) ||
        lv_obj_get_style_transform_rotation(child, LV_PART_MAIN) ||
        lv_obj_get_style_transform_scale_x(child, LV_PART_MAIN) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_scale_y(child, LV_PART_MAIN) != LV_SCALE_NONE ||
        lv_obj_get_style_transform_skew_x(child, LV_PART_MAIN) ||
        lv_obj_get_style_transform_skew_y(child, LV_PART_MAIN) ||
        lv_obj_get_style_translate_x(child, LV_PART_MAIN) ||
        lv_obj_get_style_translate_y(child, LV_PART_MAIN) ||
        lv_obj_get_style_color_filter_dsc(child, LV_PART_MAIN) ||
        lv_obj_get_style_color_filter_opa(child, LV_PART_MAIN) != LV_OPA_TRANSP ||
        lv_anim_get(child, NULL)) return false;

    int32_t x = lv_obj_get_x(child), y = lv_obj_get_y(child);
    int32_t width = lv_obj_get_width(child), height = lv_obj_get_height(child);
    int32_t card_width = lv_obj_get_width(cover_card), card_height = lv_obj_get_height(cover_card);
    const player_lyrics_geometry_t * geometry = &player_lyrics_geometry[0];
    int32_t expected_width = card_width * LV_COORD_GET_PCT(width_style) / 100;
    int32_t expected_height = card_height * LV_COORD_GET_PCT(height_style) / 100;
    int32_t compact_width = geometry->compact_w * LV_COORD_GET_PCT(width_style) / 100;
    int32_t compact_height = geometry->compact_h * LV_COORD_GET_PCT(height_style) / 100;
    int32_t compact_x = geometry->compact_w / 2 - compact_width / 2;
    int32_t compact_y = geometry->compact_h / 2 - compact_height / 2;
    int32_t border_width = lv_obj_get_style_border_width(child, LV_PART_MAIN);
    return width > 0 && height > 0 && width == height &&
           LV_ABS(width - expected_width) <= 1 && LV_ABS(height - expected_height) <= 1 && x >= 0 && y >= 0 &&
           x + width <= card_width && y + height <= card_height &&
           LV_ABS(2 * x + width - card_width) <= 1 && LV_ABS(2 * y + height - card_height) <= 1 &&
           compact_width > 0 && compact_height > 0 && LV_ABS(compact_width - compact_height) <= 1 &&
           compact_x >= 0 && compact_y >= 0 && compact_x + compact_width <= geometry->compact_w &&
           compact_y + compact_height <= geometry->compact_h &&
           LV_ABS(2 * compact_x + compact_width - geometry->compact_w) <= 1 &&
           LV_ABS(2 * compact_y + compact_height - geometry->compact_h) <= 1 &&
           border_width > 0 && border_width <= compact_width;
}

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
    bool circle_snapshot = player_timeline_cover_circle_snapshot(timeline);
    uint32_t children = lv_obj_get_child_count(cover_card);
    uint32_t live_decoration_count = 0;
    for (uint32_t i = 0; i < children; ++i) {
        lv_obj_t * child = lv_obj_get_child(cover_card, i);
        if (child != cover_img && child != player_timeline_cover_fade &&
            lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN) &&
            !player_layouts_timeline_has_animation_for_obj(timeline, child)) continue;
        if (child != cover_img && child != player_timeline_cover_fade) {
            if (circle_snapshot && !player_layouts_timeline_has_animation_for_obj(timeline, child) &&
                player_timeline_cover_live_circle_decoration(child)) {
                if (++live_decoration_count > PLAYER_TIMELINE_COVER_LIVE_DECORATION_MAX) return false;
                continue;
            }
            return false;
        }
        if ((lv_obj_get_child_count(child) != 0 &&
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
             animation.prop != LV_STYLE_OPA &&
             !(animation.prop == LV_STYLE_RADIUS && circle_snapshot &&
               animation.start_value == LV_RADIUS_CIRCLE && animation.end_value == LV_RADIUS_CIRCLE))) return false;
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
    int32_t current_w = lv_obj_get_width(cover_card);
    int32_t current_h = lv_obj_get_height(cover_card);
    int32_t snapshot_w = player_timeline_cover_w > 0 ? player_timeline_cover_w : current_w;
    int32_t snapshot_h = player_timeline_cover_h > 0 ? player_timeline_cover_h : current_h;
    lv_obj_t * live_decorations[PLAYER_TIMELINE_COVER_LIVE_DECORATION_MAX];
    unsigned live_decoration_count = 0;
    if (player_timeline_cover_circle_snapshot(timeline)) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(cover_card); ++i) {
            lv_obj_t * child = lv_obj_get_child(cover_card, i);
            if (child == cover_img || child == player_timeline_cover_fade ||
                lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
            if (player_layouts_timeline_has_animation_for_obj(timeline, child) ||
                !player_timeline_cover_live_circle_decoration(child) ||
                live_decoration_count >= PLAYER_TIMELINE_COVER_LIVE_DECORATION_MAX) return;
            live_decorations[live_decoration_count++] = child;
        }
    }
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
    bool retain = source == player_inline_cover_info().descriptor && player_timeline_cover_can_retain() &&
        snapshot_w > 0 && snapshot_h > 0 &&
        (uint64_t) lv_draw_buf_width_to_stride(snapshot_w, LV_COLOR_FORMAT_ARGB8888) * snapshot_h +
            LV_DRAW_BUF_ALIGN - 1U + sizeof(lv_draw_buf_t) <= 1024U * 1024U;
    bool hit = retain && player_timeline_cover_cache &&
        player_timeline_cover_cache_generation == player_inline_cover_info().generation &&
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
        player_inline_view.fit_cover_img_to_card();
        if (retain && !player_timeline_cover_can_retain()) {
            retain = false;
            format = LV_COLOR_FORMAT_ARGB8888;
        }
        if (retain) {
            /* An undecorated identity RGB565 image fills every card pixel.
             * Capture it natively instead of allocating ARGB, checking every
             * alpha byte and converting the same opaque pixels back to 565. */
            player_inline_cover_info_t cover_info = player_inline_cover_info();
            bool native_opaque = cover_info.descriptor &&
                snapshot_w == (int32_t)cover_info.descriptor->header.w &&
                snapshot_h == (int32_t)cover_info.descriptor->header.h &&
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
        if (!player_timeline_cover_frame) {
            for (unsigned i = 0; i < live_decoration_count; ++i)
                lv_obj_add_flag(live_decorations[i], LV_OBJ_FLAG_HIDDEN);
            player_timeline_cover_frame = lv_snapshot_take(cover_card, format);
            for (unsigned i = 0; i < live_decoration_count; ++i)
                lv_obj_remove_flag(live_decorations[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_size(cover_card, current_w, current_h);
        lv_obj_update_layout(cover_card);
        if (retain && player_timeline_cover_frame) {
            if (player_timeline_cover_cache) {
                lv_image_cache_drop(player_timeline_cover_cache);
                lv_draw_buf_destroy(player_timeline_cover_cache);
            }
            player_timeline_cover_cache = player_timeline_cover_frame;
            player_timeline_cover_cache_generation = player_inline_cover_info().generation;
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
        player_inline_view.fit_cover_img_to_card();
        player_timeline_cover_internal = was_internal;
        return;
    }

    if (player_timeline_cover_frame == player_timeline_cover_cache &&
        player_timeline_cover_frame->header.cf == LV_COLOR_FORMAT_RGB565)
        lv_draw_buf_set_flag(player_timeline_cover_frame, LV_IMAGE_FLAGS_USER3);
    lv_image_set_src(cover_img, player_timeline_cover_frame);
    player_inline_view.fit_cover_img_to_card();
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
/* Cover first, then the metadata labels the layout has; the title is required
 * but artist and album are optional, so this is 2..4. */
static lv_obj_t * player_lyrics_hidden[32];
static unsigned player_lyrics_hidden_count;
static int32_t player_lyrics_content_top;

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

void player_inline_lyrics_register_perf_display_events(void) {
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

void player_inline_lyrics_set_open(bool open, bool animate);

void player_inline_lyrics_area_empty_tap_cb(void) {
    player_inline_lyrics_set_open(false, true);
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
    player_inline_view.mark_transition_dirty();
}

/* A layout that ships both `lyrics_open` and `lyrics_close` timelines animates
 * the transition itself; native layouts supply equivalent measured properties. The timelines
 * own the geometry, so all this does is sequence them with what must stay
 * native: disabling the touch targets, hiding the controls once they have
 * animated away, and embedding the lyrics where the cover and metadata ended
 * up. The shared runner completes when its native timeline animation finishes. */
static lv_timer_t * player_lyrics_timeline_timer;

void player_inline_lyrics_cancel_timer(void) {
    player_lyrics_release_start_refresh();
    if (player_inline_view.pause_artwork_cache_timer)
        player_inline_view.pause_artwork_cache_timer();
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
        player_timeline_cover_cache_generation == player_inline_cover_info().generation &&
        player_timeline_cover_cache_epoch == player_timeline_cover_style_epoch &&
        player_timeline_cover_source == player_inline_cover_info().descriptor &&
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
    player_inline_view.fit_cover_img_to_card();
    player_inline_view.mark_transition_dirty();
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
    uint32_t playtime = player_inline_view.timeline_play(timeline, player_lyrics_timeline_completed_cb,
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
    player_inline_lyrics_cancel_timer();
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
        if (source == player_inline_cover_info().descriptor ||
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
        !player_timeline_display_refreshing && player_inline_view.display_refresh_allowed() &&
        lv_display_get_screen_active(display) == player_screen) {
        player_lyrics_start_display = display;
        player_lyrics_start_refresh_requested = !lv_timer_get_paused(refresh);
        lv_timer_pause(refresh);
    }
    player_timeline_cover_internal = was_internal;
}

/* Native layouts supply measured geometry; playback, raster proxies and
 * lifecycle are shared with authored property timelines. */
void player_inline_lyrics_generated_release(bool keep_backdrop) {
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
    player_inline_lyrics_generated_release(true);
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
        /* Keep fixed-radius native cards live as their geometry changes.
         * A constant circle radius also permits the bounded artwork snapshot
         * while static circular borders remain live. */
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

void player_inline_lyrics_set_open(bool open, bool animate) {
    if (open && !player_layouts_get_interactions()->lyrics_enabled) return;
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
        player_inline_lyrics_cancel_timer();
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

void player_inline_lyrics_cover_tap_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (lv_obj_get_screen(lv_event_get_target(e)) != player_screen) return;
    if (player_lyrics_open)
        player_inline_lyrics_set_open(false, true);
    else if (player_inline_view.lyrics_enabled())
        player_inline_lyrics_set_open(true, true);
}

void player_inline_lyrics_screen_unloaded_cb(lv_event_t * e) {
    (void) e;
    player_inline_lyrics_set_open(false, false);
}



void player_inline_lyrics_bind_cover(const player_inline_lyrics_view_t *view) {
    if (view) player_inline_view = *view;
    player_timeline_cover_fade = player_inline_view.cover_fade &&
        lv_obj_get_parent(player_inline_view.cover_fade) == player_inline_view.cover_card_obj
        ? player_inline_view.cover_fade : NULL;
    if (player_inline_view.cover_card_obj) {
        player_timeline_cover_w = lv_obj_get_width(player_inline_view.cover_card_obj);
        player_timeline_cover_h = lv_obj_get_height(player_inline_view.cover_card_obj);
    }

    lv_obj_t * cached_cover_objects[] = { player_inline_view.cover_card_obj,
                                          player_inline_view.cover_image };
    for (unsigned i = 0; i < sizeof(cached_cover_objects) / sizeof(cached_cover_objects[0]); ++i) {
        if (!cached_cover_objects[i]) continue;
        lv_obj_add_event_cb(cached_cover_objects[i], player_timeline_cover_style_changed_cb,
                            LV_EVENT_STYLE_CHANGED, NULL);
        lv_obj_add_event_cb(cached_cover_objects[i], player_timeline_cover_style_changed_cb,
                            LV_EVENT_SIZE_CHANGED, NULL);
    }
    lv_display_t * cache_display = lv_obj_get_display(player_inline_view.screen);
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
}

void player_inline_lyrics_bind_pane(const player_inline_lyrics_view_t *view) {
    if (!view) return;
    player_inline_view = *view;
    player_inline_view.cover_fade = view->cover_fade &&
        lv_obj_get_parent(view->cover_fade) == view->cover_card_obj ? view->cover_fade : NULL;
}

void player_inline_lyrics_reset_hit_targets(void) { player_inline_hit_target_count = 0; }
void player_inline_lyrics_reset_controls(void) { player_inline_control_count = 0; }

void player_inline_lyrics_add_hit_target(lv_obj_t *obj) {
    if (player_inline_hit_target_count < PLAYER_MAX_HIT_TARGETS)
        player_inline_hit_targets[player_inline_hit_target_count++] = obj;
}

void player_inline_lyrics_add_control(lv_obj_t *obj) {
    if (obj && player_inline_control_count < PLAYER_MAX_LYRICS_CONTROLS)
        player_inline_controls[player_inline_control_count++] = obj;
}

bool player_inline_lyrics_is_open(void) { return player_lyrics_open; }
bool player_inline_lyrics_area_is_open(void) { return player_lyrics_area && player_lyrics_open; }
bool player_inline_lyrics_is_animating(void) { return player_lyrics_animating; }

const void *player_inline_lyrics_source_for_scaling(const void *image_source) {
    return player_timeline_cover_frame && image_source == player_timeline_cover_frame
        ? player_timeline_cover_source : image_source;
}

void player_inline_lyrics_release_cache_memory(void) {
    lv_draw_sw_image_cache_set_transition_active(false);
    lv_draw_sw_image_cache_set_intermediate_source(NULL);
    player_inline_lyrics_invalidate_cover_source();
}

void player_inline_lyrics_reset(void) {
#ifdef UI_PERF_TRACE
    player_lyrics_perf_abort("screen_unloaded");
#endif
    player_inline_lyrics_invalidate_cover_source();
    player_lyrics_open = false;
    player_lyrics_animating = false;
    player_timeline_cover_antialias_saved = false;
    player_inline_view.cover_fade = NULL;
    player_timeline_cover_w = 0;
    player_timeline_cover_h = 0;
    player_inline_view.backdrop_contains_cover = false;
    player_lyrics_object_count = 0;
    player_lyrics_hidden_count = 0;
    player_inline_lyrics_reset_hit_targets();
    player_inline_lyrics_reset_controls();
    player_inline_view.screen = NULL;
    player_inline_view.cover_card_obj = NULL;
    player_inline_view.cover_image = NULL;
    player_inline_view.cover_thumbnail = NULL;
    player_inline_view.overlay = NULL;
    player_inline_view.title = NULL;
    player_inline_view.artist = NULL;
    player_inline_view.album = NULL;
    player_inline_view.favorite_circle_obj = NULL;
    player_inline_view.lyrics_area = NULL;
    player_inline_view.lyrics_open = NULL;
    player_inline_view.lyrics_close = NULL;
    player_inline_view.builtin_layout = false;
    player_inline_view.artist_is_standin = false;
    player_inline_view.album_is_standin = false;
}

/* Keep the compatibility aliases private to this implementation file. */
#undef player_screen
#undef cover_card
#undef cover_img
#undef player_cover_thumbnail
#undef player_overlay_panel
#undef song_title_label
#undef artist_label
#undef album_label
#undef favorite_circle
#undef player_lyrics_area
#undef player_layout_builtin
#undef player_artist_standin
#undef player_album_standin
#undef player_timeline_cover_fade
#undef player_timeline_backdrop_covers_cover
#undef player_timeline_lyrics_open
#undef player_timeline_lyrics_close
#undef player_hit_targets
#undef player_hit_target_count
#undef player_lyrics_controls
#undef player_lyrics_control_count
