#include "gui_shell_internal.h"

static lv_obj_t * quick_drawer_motion_image = NULL;
lv_draw_buf_t * quick_drawer_motion_buf = NULL;
bool quick_drawer_bitmap_motion = false;
bool quick_drawer_direct_motion = false;
static int32_t quick_drawer_direct_y = 0;
bool quick_drawer_snapshot_dirty = true;
bool quick_drawer_open = false;
static lv_timer_t * quick_drawer_snapshot_retry_timer = NULL;
static bool quick_drawer_snapshot_wait_for_wake = false;
int32_t quick_drawer_expansion_full = 0; /* height when fully open */
int32_t quick_drawer_expansion_y = 0;    /* current, 0..full */
bool quick_drawer_expanded = false;
/* Snapshots must describe a settled drawer, not a partially revealed one. */
bool quick_drawer_expansion_motion = false;
static bool quick_drawer_expansion_pending_target = false;
lv_timer_t * quick_drawer_expansion_start_timer = NULL;

/* Everything that slides down by the revealed height, captured at build time
 * with its own collapsed y so applying a shift is one lv_obj_set_y() per
 * entry and never re-derives geometry. lv_obj_set_y() is correct for the
 * lv_obj_align()-positioned entries too: align mode is stored separately and
 * stays put, so this just replaces the y offset it aligns by. */
#define QUICK_DRAWER_SHIFT_MAX 12
static struct {
    lv_obj_t * obj;
    int32_t base_y;
} quick_drawer_shift_objs[QUICK_DRAWER_SHIFT_MAX];
int quick_drawer_shift_count = 0;

/* Two opaque views share one owned, settled drawer frame. The row view is
 * clipped by the existing expansion box; the controls view slides beneath it.
 * RGB565 avoids blending a large transparent layer on every animation tick. */
static lv_draw_buf_t * quick_drawer_expansion_frame;
static lv_draw_buf_t * quick_drawer_expansion_base;
static bool quick_drawer_expansion_direct;
static lv_image_dsc_t quick_drawer_rows_view, quick_drawer_controls_view;
static lv_obj_t * quick_drawer_rows_image, * quick_drawer_controls_image;
static int32_t quick_drawer_controls_base_y;
#define QUICK_DRAWER_CACHE_OBJECT_MAX (40 + QUICK_DRAWER_SHIFT_MAX)
static struct {
    lv_obj_t * obj;
    lv_opa_t opa;
} quick_drawer_cached_objects[QUICK_DRAWER_CACHE_OBJECT_MAX];
static unsigned quick_drawer_cached_object_count;
static bool quick_drawer_motion_cache_prepared;
static bool quick_drawer_expansion_art_dirty = true;
static bool quick_drawer_expansion_cache_warming;
static void quick_drawer_sync_cached_eq(void);
static void quick_drawer_mark_geometry_dirty(void);
static void quick_drawer_discard_expansion_art(void);
void quick_drawer_register_shift_obj(lv_obj_t * obj, int32_t base_y) {
    if (!obj || quick_drawer_shift_count >= QUICK_DRAWER_SHIFT_MAX) return;
    quick_drawer_shift_objs[quick_drawer_shift_count].obj = obj;
    quick_drawer_shift_objs[quick_drawer_shift_count].base_y = base_y;
    quick_drawer_shift_count++;
}
void quick_drawer_apply_expansion(int32_t y) {
    if (y < 0) y = 0;
    if (y > quick_drawer_expansion_full) y = quick_drawer_expansion_full;
    if (y == quick_drawer_expansion_y) return;
#if defined(BOARD_TEMPOTEC_V1)
    bool was_expanded = quick_drawer_expansion_y > 0;
#endif
    quick_drawer_expansion_y = y;
    if (quick_drawer_expansion_box) lv_obj_set_height(quick_drawer_expansion_box, y);
    for (int i = 0; i < quick_drawer_shift_count; i++) {
        lv_obj_set_y(quick_drawer_shift_objs[i].obj, quick_drawer_shift_objs[i].base_y + y);
    }
    if (quick_drawer_controls_image)
        lv_obj_set_y(quick_drawer_controls_image, quick_drawer_controls_base_y + y);
    quick_drawer_sync_cached_eq();
    /* Hidden the moment the expansion is off zero -- even a partly-shifted
     * volume rail already overlaps the card's top edge. */
    if (quick_drawer_card) {
        bool hidden = lv_obj_has_flag(quick_drawer_card, LV_OBJ_FLAG_HIDDEN);
        if (y > 0 && !hidden) lv_obj_add_flag(quick_drawer_card, LV_OBJ_FLAG_HIDDEN);
        else if (y == 0 && hidden) lv_obj_remove_flag(quick_drawer_card, LV_OBJ_FLAG_HIDDEN);
    }
#if defined(BOARD_TEMPOTEC_V1)
    if (was_expanded != (y > 0)) gui_shell_refresh_quick_drawer_volume_visibility();
#endif
    if (quick_drawer_expansion_direct) {
        if (y == 0) {
            transition_compositor_end();
            quick_drawer_expansion_direct = false;
        } else if (!transition_compositor_vertical_reveal_frame(y)) {
            /* The compositor restores LVGL on a presentation failure. The
             * two cached images already have this same geometry. */
            quick_drawer_expansion_direct = false;
            lv_obj_invalidate(quick_drawer);
        }
    }
    if (!quick_drawer_expansion_cache_warming) quick_drawer_mark_geometry_dirty();
}
void quick_drawer_release_motion_cache(void) {
    if (quick_drawer_expansion_direct) transition_compositor_end();
    quick_drawer_expansion_direct = false;
    lv_obj_t ** images[] = { &quick_drawer_rows_image, &quick_drawer_controls_image };
    for (unsigned i = 0; i < 2; ++i) {
        if (!*images[i]) continue;
        lv_image_set_src(*images[i], NULL);
        lv_obj_delete(*images[i]);
        *images[i] = NULL;
    }
    for (unsigned i = 0; i < quick_drawer_cached_object_count; ++i)
        lv_obj_set_style_opa(quick_drawer_cached_objects[i].obj,
                             quick_drawer_cached_objects[i].opa, LV_PART_MAIN);
    quick_drawer_cached_object_count = 0;
    if (quick_drawer_eq_button) lv_obj_move_foreground(quick_drawer_eq_button);
    lv_image_cache_drop(&quick_drawer_rows_view);
    lv_image_cache_drop(&quick_drawer_controls_view);
    quick_drawer_motion_cache_prepared = false;
    if (!quick_drawer_open || quick_drawer_expansion_art_dirty)
        quick_drawer_discard_expansion_art();
}

/* Keep at most two RGB565 frames while the drawer is open. Live changes
 * invalidate them; closing/reloading releases the retained allocation. */
static void quick_drawer_discard_expansion_art(void) {
    lv_image_cache_drop(&quick_drawer_rows_view);
    lv_image_cache_drop(&quick_drawer_controls_view);
    if (quick_drawer_expansion_frame) lv_draw_buf_destroy(quick_drawer_expansion_frame);
    if (quick_drawer_expansion_base) lv_draw_buf_destroy(quick_drawer_expansion_base);
    quick_drawer_expansion_frame = NULL;
    quick_drawer_expansion_base = NULL;
    memset(&quick_drawer_rows_view, 0, sizeof(quick_drawer_rows_view));
    memset(&quick_drawer_controls_view, 0, sizeof(quick_drawer_controls_view));
    quick_drawer_expansion_art_dirty = true;
}
static void quick_drawer_cache_object(lv_obj_t * obj) {
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) ||
        quick_drawer_cached_object_count >= QUICK_DRAWER_CACHE_OBJECT_MAX) return;
    quick_drawer_cached_objects[quick_drawer_cached_object_count].obj = obj;
    quick_drawer_cached_objects[quick_drawer_cached_object_count++].opa = lv_obj_get_style_opa(obj, LV_PART_MAIN);
    /* Keep the live input objects and their geometry while substituting art. */
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
}
static void quick_drawer_sync_cached_eq(void) {
    if (!quick_drawer_eq_button || !quick_drawer_motion_cache_prepared) return;
    for (unsigned i = 0; i < quick_drawer_cached_object_count; ++i) {
        if (quick_drawer_cached_objects[i].obj != quick_drawer_eq_button) continue;
        /* At zero reveal the card covers the moving strip. Its shortcut
         * must retain the native layer above the card at that endpoint. */
        lv_obj_set_style_opa(quick_drawer_eq_button, quick_drawer_expansion_y == 0
                            ? quick_drawer_cached_objects[i].opa : LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_move_foreground(quick_drawer_eq_button);
        break;
    }
}
void quick_drawer_prepare_motion_cache(void) {
    if (quick_drawer_motion_cache_prepared || !quick_drawer || !quick_drawer_expansion_box ||
        quick_drawer_expansion_full <= 0 || quick_drawer_shift_count <= 0) return;
#if defined(BOARD_TEMPOTEC_V1)
    /* The expanded rows are a genuinely scrollable V1 viewport. Leave this
     * subtree live so LVGL can move its children; the drawer's separate
     * open/close compositor remains unchanged. */
    return;
#endif
    int32_t saved_y = quick_drawer_expansion_y;
    bool card_hidden = quick_drawer_card && lv_obj_has_flag(quick_drawer_card, LV_OBJ_FLAG_HIDDEN);
    if (quick_drawer_expansion_frame && !quick_drawer_expansion_art_dirty)
        goto install_images;
    quick_drawer_discard_expansion_art();
    quick_drawer_apply_expansion(quick_drawer_expansion_full);
    lv_obj_update_layout(quick_drawer);
    int32_t first = INT32_MAX, last = INT32_MIN;
    for (int i = 0; i < quick_drawer_shift_count; ++i) {
        lv_obj_t * obj = quick_drawer_shift_objs[i].obj;
        if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) continue;
        int32_t ext = lv_obj_get_ext_draw_size(obj);
        int32_t top = lv_obj_get_y(obj) - ext;
        int32_t bottom = lv_obj_get_y(obj) + lv_obj_get_height(obj) + ext;
        if (top < first) first = top;
        if (bottom > last) last = bottom;
    }
    int32_t rows_top = lv_obj_get_y(quick_drawer_expansion_box);
    lv_draw_buf_t * frame = lv_snapshot_take(quick_drawer, LV_COLOR_FORMAT_RGB565);
    quick_drawer_apply_expansion(saved_y);
    if (quick_drawer_card) {
        lv_obj_set_flag(quick_drawer_card, LV_OBJ_FLAG_HIDDEN, card_hidden);
    }
    lv_obj_update_layout(quick_drawer);
    /* Native drawer has no extended drawing margin. A layout with different
     * bounds safely uses live rendering rather than an invalid crop. */
    if (!frame) return;
    if (frame->header.w != (uint32_t)lv_obj_get_width(quick_drawer) ||
        frame->header.h != (uint32_t)lv_obj_get_height(quick_drawer) || rows_top < 0 ||
        rows_top + quick_drawer_expansion_full > (int32_t)frame->header.h ||
        first < rows_top + quick_drawer_expansion_full || last <= first ||
        last > (int32_t)frame->header.h) {
        lv_draw_buf_destroy(frame);
        return;
    }
    quick_drawer_expansion_frame = frame;
    quick_drawer_rows_view.header = frame->header;
    quick_drawer_rows_view.header.flags = 0; /* Borrowed image view, not an owned draw buffer. */
    quick_drawer_rows_view.header.h = quick_drawer_expansion_full;
    quick_drawer_rows_view.data = frame->data + rows_top * frame->header.stride;
    quick_drawer_rows_view.data_size = quick_drawer_expansion_full * frame->header.stride;
    quick_drawer_controls_view.header = frame->header;
    quick_drawer_controls_view.header.flags = 0;
    quick_drawer_controls_view.header.h = last - first;
    quick_drawer_controls_view.data = frame->data + first * frame->header.stride;
    quick_drawer_controls_view.data_size = (last - first) * frame->header.stride;
    quick_drawer_controls_base_y = first - quick_drawer_expansion_full;
install_images:
    quick_drawer_expansion_art_dirty = false;
    quick_drawer_rows_image = lv_image_create(quick_drawer_expansion_box);
    quick_drawer_controls_image = lv_image_create(quick_drawer);
    if (!quick_drawer_rows_image || !quick_drawer_controls_image) {
        quick_drawer_release_motion_cache();
        return;
    }
    lv_image_set_src(quick_drawer_rows_image, &quick_drawer_rows_view);
    lv_image_set_src(quick_drawer_controls_image, &quick_drawer_controls_view);
    lv_obj_set_pos(quick_drawer_rows_image, 0, 0);
    lv_obj_set_pos(quick_drawer_controls_image, 0, quick_drawer_controls_base_y + saved_y);
    /* Extended control bounds can overlap the card at zero reveal. Preserve
     * the native stacking order until the first moving frame hides it. */
    if (quick_drawer_card) lv_obj_move_foreground(quick_drawer_card);
    lv_obj_t * images[] = { quick_drawer_rows_image, quick_drawer_controls_image };
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_remove_flag(images[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(images[i], LV_OBJ_FLAG_IGNORE_LAYOUT);
    }
    uint32_t children = lv_obj_get_child_count(quick_drawer_expansion_box);
    for (uint32_t i = 0; i < children; ++i) {
        lv_obj_t * child = lv_obj_get_child(quick_drawer_expansion_box, i);
        if (child != quick_drawer_rows_image) quick_drawer_cache_object(child);
    }
    for (int i = 0; i < quick_drawer_shift_count; ++i)
        quick_drawer_cache_object(quick_drawer_shift_objs[i].obj);
    quick_drawer_motion_cache_prepared = true;
    if (transition_compositor_available() && !quick_drawer_expansion_base) {
        /* Static frame for direct row copies. Keep the existing open/close
         * underlay untouched: it belongs to the screen behind the drawer. */
        lv_obj_add_flag(quick_drawer_rows_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(quick_drawer_controls_image, LV_OBJ_FLAG_HIDDEN);
        if (quick_drawer_card) lv_obj_add_flag(quick_drawer_card, LV_OBJ_FLAG_HIDDEN);
        quick_drawer_expansion_base = lv_snapshot_take(quick_drawer, LV_COLOR_FORMAT_RGB565);
        lv_obj_remove_flag(quick_drawer_rows_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(quick_drawer_controls_image, LV_OBJ_FLAG_HIDDEN);
        if (quick_drawer_card && !card_hidden) lv_obj_remove_flag(quick_drawer_card, LV_OBJ_FLAG_HIDDEN);
    }
    quick_drawer_sync_cached_eq();
}
void quick_drawer_begin_expansion_composition(void) {
    if (quick_drawer_expansion_direct || !quick_drawer_expansion_base ||
        !quick_drawer_expansion_frame) return;
    int32_t fixed_top = status_bar_band && !lv_obj_has_flag(status_bar_band, LV_OBJ_FLAG_HIDDEN)
                      ? lv_obj_get_height(status_bar_band) : 0;
    quick_drawer_expansion_direct = transition_compositor_begin_vertical_reveal(
        quick_drawer_expansion_base, quick_drawer_expansion_frame,
        lv_obj_get_y(quick_drawer_expansion_box), quick_drawer_expansion_full,
        quick_drawer_controls_base_y + quick_drawer_expansion_full,
        quick_drawer_controls_view.header.h, fixed_top);
}
#ifdef UI_PERF_TRACE
static uint64_t drawer_expansion_render_start, drawer_expansion_render_total;
static uint64_t drawer_expansion_render_max, drawer_expansion_last_frame, drawer_expansion_max_gap;
static unsigned drawer_expansion_frames;
static bool drawer_expansion_perf_active;
static void drawer_expansion_perf_event(lv_event_t * e) {
    if (!drawer_expansion_perf_active) return;
    uint64_t now = ui_perf_now_us();
    if (lv_event_get_code(e) == LV_EVENT_RENDER_START) {
        if (drawer_expansion_last_frame && now - drawer_expansion_last_frame > drawer_expansion_max_gap)
            drawer_expansion_max_gap = now - drawer_expansion_last_frame;
        drawer_expansion_last_frame = now;
        drawer_expansion_render_start = now;
    } else {
        uint64_t elapsed = now - drawer_expansion_render_start;
        drawer_expansion_render_total += elapsed;
        if (elapsed > drawer_expansion_render_max) drawer_expansion_render_max = elapsed;
        drawer_expansion_frames++;
    }
}
static void drawer_expansion_perf_begin(void) {
    static bool registered;
    if (!registered) {
        lv_display_add_event_cb(lv_display_get_default(), drawer_expansion_perf_event, LV_EVENT_RENDER_START, NULL);
        lv_display_add_event_cb(lv_display_get_default(), drawer_expansion_perf_event, LV_EVENT_RENDER_READY, NULL);
        registered = true;
    }
    drawer_expansion_render_start = drawer_expansion_render_total = drawer_expansion_render_max = 0;
    drawer_expansion_last_frame = drawer_expansion_max_gap = 0;
    drawer_expansion_frames = 0;
    drawer_expansion_perf_active = true;
}
#endif
void quick_drawer_expansion_anim_cb(void * var, int32_t v) {
    (void) var;
    quick_drawer_apply_expansion(v);
}
static void quick_drawer_expansion_anim_done_cb(lv_anim_t * a) {
    (void) a;
#ifdef UI_PERF_TRACE
    printf("PERF drawer_expand direction=%s frames=%u render_avg_us=%llu render_max_us=%llu maxgap_us=%llu\n",
           quick_drawer_expanded ? "open" : "close", drawer_expansion_frames,
           (unsigned long long)(drawer_expansion_frames ? drawer_expansion_render_total / drawer_expansion_frames : 0),
           (unsigned long long)drawer_expansion_render_max, (unsigned long long)drawer_expansion_max_gap);
    drawer_expansion_perf_active = false;
#endif
    quick_drawer_expansion_motion = false;
    quick_drawer_release_motion_cache();
    quick_drawer_mark_geometry_dirty();
}
void quick_drawer_cancel_expansion_start(void) {
    if (!quick_drawer_expansion_start_timer) return;
    lv_timer_delete(quick_drawer_expansion_start_timer);
    quick_drawer_expansion_start_timer = NULL;
}
static void quick_drawer_expansion_start_timer_cb(lv_timer_t * timer) {
    if (quick_drawer_expansion_start_timer == timer)
        quick_drawer_expansion_start_timer = NULL;
    lv_timer_delete(timer);
    if (!quick_drawer_expansion_box || quick_drawer_expansion_full <= 0) {
        quick_drawer_expansion_motion = false;
        quick_drawer_release_motion_cache();
        quick_drawer_mark_snapshot_dirty();
        return;
    }
    quick_drawer_begin_expansion_composition();
    lv_anim_t a;
    lv_anim_init(&a);
#ifdef UI_PERF_TRACE
    drawer_expansion_perf_begin();
#endif
    lv_anim_set_var(&a, quick_drawer_expansion_box);
    lv_anim_set_values(&a, quick_drawer_expansion_y,
                       quick_drawer_expansion_pending_target ? quick_drawer_expansion_full : 0);
    lv_anim_set_duration(&a, gui_anim_ms(QUICK_DRAWER_ANIM_MS));
    lv_anim_set_exec_cb(&a, quick_drawer_expansion_anim_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, quick_drawer_expansion_anim_done_cb);
    lv_anim_start(&a);
}
void quick_drawer_animate_expansion(bool expand) {
    if (!quick_drawer_expansion_box || quick_drawer_expansion_full <= 0) return;
    quick_drawer_cancel_expansion_start();
    quick_drawer_expansion_motion = true;
    /* Re-read on the way open rather than when the drawer opens -- see
     * open_quick_drawer()'s own comment on why this must not run there. */
    if (expand) gui_shell_refresh_quick_drawer_expansion_toggles();
    quick_drawer_expanded = expand;
    int32_t target = expand ? quick_drawer_expansion_full : 0;
#if defined(BOARD_TEMPOTEC_V1)
    if (expand && quick_drawer_expansion_y == 0)
        lv_obj_scroll_to_y(quick_drawer_expansion_box, 0, LV_ANIM_OFF);
#endif
    lv_anim_delete(quick_drawer_expansion_box, quick_drawer_expansion_anim_cb);
    if (quick_drawer_expansion_y == target) {
        quick_drawer_expansion_motion = false;
        quick_drawer_release_motion_cache();
        quick_drawer_mark_snapshot_dirty();
        return;
    }
    if (gui_anims_off()) {
        quick_drawer_expansion_motion = false;
        quick_drawer_release_motion_cache();
        quick_drawer_apply_expansion(target);
        return;
    }
    quick_drawer_expansion_motion = true;
    quick_drawer_expansion_pending_target = expand;
    quick_drawer_prepare_motion_cache();
    /* Let refreshed labels/icons settle before the first animated frame. */
    quick_drawer_expansion_start_timer = lv_timer_create(quick_drawer_expansion_start_timer_cb, 1, NULL);
    if (!quick_drawer_expansion_start_timer) {
        quick_drawer_expansion_motion = false;
        quick_drawer_release_motion_cache();
        quick_drawer_apply_expansion(target);
    }
}

/* Back to collapsed with no animation -- the drawer always opens in its
 * unexpanded state, so this runs on close rather than leaving the expansion
 * to reappear mid-slide the next time it is pulled down. */
void quick_drawer_reset_expansion(void) {
    quick_drawer_cancel_expansion_start();
    if (quick_drawer_expansion_box)
        lv_anim_delete(quick_drawer_expansion_box, quick_drawer_expansion_anim_cb);
    quick_drawer_expanded = false;
    quick_drawer_expansion_motion = false;
    quick_drawer_release_motion_cache();
    quick_drawer_discard_expansion_art();
#if defined(BOARD_TEMPOTEC_V1)
    if (quick_drawer_expansion_box) {
        lv_obj_stop_scroll_anim(quick_drawer_expansion_box);
        lv_obj_scroll_to_y(quick_drawer_expansion_box, 0, LV_ANIM_OFF);
    }
#endif
    quick_drawer_apply_expansion(0);
}

/* Restarts the now-playing marquees from offset 0, with their 2s wait
 * counted from now.
 *
 * lv_label_set_long_mode() deletes the running offset animations and zeroes
 * the offset even when the mode is unchanged, so re-setting it is the
 * restart. That matters because the scroll animation is otherwise created
 * once, when the track's metadata is set -- and the 2s delay rides on that
 * animation's act_time (lv_anim_set_delay() stores it as a negative
 * act_time, which lv_label.c's overwrite_anim_property() only copies while
 * act_time <= 0, i.e. only at creation). Without this the wait elapses
 * while the drawer is still closed and the title is already scrolling by
 * the time it is pulled down. */
static void quick_drawer_restart_marquee(void) {
    if (quick_drawer_title_label)
        lv_label_set_long_mode(quick_drawer_title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    if (quick_drawer_artist_label)
        lv_label_set_long_mode(quick_drawer_artist_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
}
void quick_drawer_expansion_handle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* A drag that already moved the expansion consumes its own release in
     * poll_quick_drawer_drag(); only a genuine tap reaches here. */
    quick_drawer_animate_expansion(!quick_drawer_expanded);
}
void quick_drawer_anim_y_cb(void * var, int32_t v) {
    (void) var;
    if (quick_drawer_direct_motion) {
        quick_drawer_direct_y = v;
        if (transition_compositor_vertical_overlay_frame(v)) return;

        /* A failed framebuffer present tears the compositor session down
         * itself. Continue the same gesture through the already-built LVGL
         * bitmap rather than dropping or snapping the drawer. */
        quick_drawer_direct_motion = false;
        lv_obj_set_y(quick_drawer_motion_image, v);
        lv_obj_add_flag(quick_drawer_motion_image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(quick_drawer_motion_image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(quick_drawer_motion_image);
        lv_obj_move_foreground(status_bar_band);
        return;
    }
    lv_obj_set_y(quick_drawer_bitmap_motion ? quick_drawer_motion_image : quick_drawer, v);
}
int32_t quick_drawer_motion_y(void) {
    if (quick_drawer_direct_motion) return quick_drawer_direct_y;
    return lv_obj_get_y(quick_drawer_bitmap_motion ? quick_drawer_motion_image : quick_drawer);
}
static void quick_drawer_schedule_snapshot_retry(void);
static void quick_drawer_free_motion_buf(void) {
    if (!quick_drawer_motion_buf) return;
    ui_snapshot_prefetch_forget(quick_drawer_motion_buf);
    lv_draw_buf_destroy(quick_drawer_motion_buf);
    quick_drawer_motion_buf = NULL;
}
void quick_drawer_rebuild_snapshot(void) {
    if (!quick_drawer || quick_drawer_bitmap_motion || quick_drawer_expansion_motion) return;
    if (!quick_drawer_customization_enabled()) {
        if (quick_drawer_motion_image) {
            lv_image_set_src(quick_drawer_motion_image, NULL);
            lv_obj_add_flag(quick_drawer_motion_image, LV_OBJ_FLAG_HIDDEN);
        }
        quick_drawer_free_motion_buf();
        quick_drawer_snapshot_dirty = true;
        quick_drawer_snapshot_wait_for_wake = false;
        if (quick_drawer_snapshot_retry_timer) lv_timer_pause(quick_drawer_snapshot_retry_timer);
        return;
    }
    UI_TRACE_SPAN_BEGIN(perf_span);
    if (quick_drawer_open && quick_drawer_expansion_art_dirty && !gui_anims_off()) {
        quick_drawer_expansion_cache_warming = true;
        gui_shell_refresh_quick_drawer_expansion_toggles();
        quick_drawer_prepare_motion_cache();
        quick_drawer_release_motion_cache();
        quick_drawer_expansion_cache_warming = false;
        /* The warm-up and the drawer snapshot are each tens of ms: take the
         * snapshot in a later pass rather than both in one. */
        if (quick_drawer_snapshot_dirty && !quick_drawer_expansion_art_dirty) {
            UI_TRACE_SPAN_END(UI_TRACE_SPAN_DRAWER_SNAPSHOT, perf_span);
            quick_drawer_schedule_snapshot_retry();
            return;
        }
    }
    /* A warm-only pass keeps the existing drawer snapshot. */
    if (!quick_drawer_snapshot_dirty && quick_drawer_motion_buf) {
        UI_TRACE_SPAN_END(UI_TRACE_SPAN_DRAWER_SNAPSHOT, perf_span);
        return;
    }
    lv_draw_buf_t * fresh = lv_snapshot_take(quick_drawer, LV_COLOR_FORMAT_RGB565);
    if (!fresh) {
        UI_TRACE_SPAN_END(UI_TRACE_SPAN_DRAWER_SNAPSHOT, perf_span);
        return;
    }
    if (!quick_drawer_motion_image) {
        quick_drawer_motion_image = lv_image_create(lv_layer_top());
        lv_obj_add_flag(quick_drawer_motion_image, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(quick_drawer_motion_image, NULL);
    }
    quick_drawer_free_motion_buf();
    quick_drawer_motion_buf = fresh;
    lv_image_set_src(quick_drawer_motion_image, quick_drawer_motion_buf);
    UI_TRACE_SPAN_END(UI_TRACE_SPAN_DRAWER_SNAPSHOT, perf_span);
    quick_drawer_snapshot_dirty = false;
    if (quick_drawer_snapshot_retry_timer) lv_timer_pause(quick_drawer_snapshot_retry_timer);
}
static void quick_drawer_snapshot_retry_cb(lv_timer_t * timer);

/* Closing discards the expansion art, so each open has to warm it again;
 * otherwise the first expand tap takes both of its snapshots synchronously. */
static bool quick_drawer_expansion_warm_pending(void) {
    return quick_drawer_open && quick_drawer_expansion_art_dirty && !gui_anims_off();
}
static bool quick_drawer_snapshot_work_pending(void) {
    return quick_drawer_snapshot_dirty || quick_drawer_expansion_warm_pending();
}
static bool quick_drawer_pointer_pressed(void) {
    for (lv_indev_t * indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
            lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) return true;
    }
    return false;
}
static void quick_drawer_schedule_snapshot_retry(void) {
    if (!quick_drawer || !quick_drawer_snapshot_work_pending() || !backlight_screen_is_on()) return;
    if (!quick_drawer_snapshot_retry_timer) {
        quick_drawer_snapshot_retry_timer = lv_timer_create(quick_drawer_snapshot_retry_cb, 16, NULL);
        if (!quick_drawer_snapshot_retry_timer) return;
        lv_timer_pause(quick_drawer_snapshot_retry_timer);
    }
    if (lv_timer_get_paused(quick_drawer_snapshot_retry_timer))
        lv_timer_resume(quick_drawer_snapshot_retry_timer);
}
static uint32_t quick_drawer_snapshot_waited_ms;
static void quick_drawer_snapshot_async_cb(void * unused) {
    (void) unused;
    if (!quick_drawer || !quick_drawer_snapshot_work_pending()) {
        quick_drawer_snapshot_waited_ms = 0;
        return;
    }
    if (!backlight_screen_is_on()) {
        quick_drawer_snapshot_wait_for_wake = true;
        return;
    }
    quick_drawer_snapshot_wait_for_wake = false;
    if (quick_drawer_bitmap_motion || quick_drawer_expansion_motion) return;
    /* A finger on the glass may be starting a drag; a full snapshot now
     * would land on its first frames. */
    if (gui_navigation_transition_in_progress() || gui_player_lyrics_animation_in_progress() ||
        quick_drawer_pointer_pressed()) {
        quick_drawer_snapshot_waited_ms = 0; /* the patience waiver counts only otherwise-idle time */
        quick_drawer_schedule_snapshot_retry();
        return;
    }
    if (!gui_navigation_capture_idle(quick_drawer_snapshot_waited_ms)) {
        quick_drawer_snapshot_waited_ms += 16;
        quick_drawer_schedule_snapshot_retry();
        return;
    }
    quick_drawer_snapshot_waited_ms = 0;
    quick_drawer_rebuild_snapshot();
}
static void quick_drawer_snapshot_retry_cb(lv_timer_t * timer) {
    lv_timer_pause(timer);
    quick_drawer_snapshot_async_cb(NULL);
}

/* Delayed past the open animation's last frames, which would otherwise
 * absorb the two expansion snapshots. */
#define QUICK_DRAWER_EXPANSION_WARM_DELAY_MS 250
static lv_timer_t * quick_drawer_expansion_warm_timer;
static void quick_drawer_expansion_warm_cb(lv_timer_t * timer) {
    lv_timer_delete(timer);
    quick_drawer_expansion_warm_timer = NULL;
    quick_drawer_snapshot_async_cb(NULL);
}
static void quick_drawer_schedule_expansion_warm(void) {
    if (quick_drawer_expansion_warm_timer) {
        lv_timer_reset(quick_drawer_expansion_warm_timer);
        return;
    }
    quick_drawer_expansion_warm_timer = lv_timer_create(quick_drawer_expansion_warm_cb,
                                                        QUICK_DRAWER_EXPANSION_WARM_DELAY_MS, NULL);
}
static void quick_drawer_mark_geometry_dirty(void) {
    quick_drawer_snapshot_dirty = true;
    if (quick_drawer && !quick_drawer_bitmap_motion && !quick_drawer_expansion_motion &&
        !quick_drawer_expansion_cache_warming)
        lv_async_call(quick_drawer_snapshot_async_cb, NULL);
}
void quick_drawer_mark_snapshot_dirty(void) {
    quick_drawer_expansion_art_dirty = true;
    quick_drawer_mark_geometry_dirty();
}

/* Frees the drawer's full-screen motion snapshot and expansion frames for an
 * external session. Skipped while the drawer is open or moving, when they are
 * in use. quick_drawer_mark_snapshot_dirty() rebuilds them afterwards, the
 * same way any drawer change does; until then drags follow the live panel. */
void gui_shell_release_memory_for_external_session(void) {
    if (!quick_drawer || quick_drawer_open || quick_drawer_bitmap_motion || quick_drawer_expansion_motion) return;
    if (quick_drawer_motion_image) {
        lv_image_set_src(quick_drawer_motion_image, NULL);
        lv_obj_add_flag(quick_drawer_motion_image, LV_OBJ_FLAG_HIDDEN);
    }
    if (quick_drawer_motion_buf) lv_image_cache_drop(quick_drawer_motion_buf);
    quick_drawer_free_motion_buf();
    quick_drawer_discard_expansion_art();
    quick_drawer_snapshot_dirty = true;
}
bool quick_drawer_begin_bitmap_motion(void) {
    if (quick_drawer_bitmap_motion) return true;
    /* Never lv_snapshot_take() on the drag/animation tick: a full-panel
     * RGB565 snapshot is a multi-millisecond hitch on this SoC and was
     * the "dragging the drawer feels slow" report. Use a buffer already
     * built while idle, or follow the live panel. */
    if (quick_drawer_snapshot_dirty || !quick_drawer_motion_buf || !quick_drawer_motion_image)
        return false;
    int32_t initial_y = lv_obj_get_y(quick_drawer);
    quick_drawer_direct_y = initial_y;
    int32_t fixed_top = (status_bar_band && !lv_obj_has_flag(status_bar_band, LV_OBJ_FLAG_HIDDEN))
                        ? lv_obj_get_height(status_bar_band)
                        : 0;
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());
    bool reuse_underlay = initial_y > -h;
#if defined(UI_PERF_TRACE) || defined(UI_GESTURE_TRACE)
    printf("[DRAWER_TRACE] begin_bitmap_motion: initial_y=%d, status_bar_band=%p, hidden=%d, fixed_top=%d, reuse_underlay=%d\n",
           (int)initial_y, (void*)status_bar_band,
           status_bar_band ? lv_obj_has_flag(status_bar_band, LV_OBJ_FLAG_HIDDEN) : -1,
           (int)fixed_top, (int)reuse_underlay);
#endif
    if (transition_compositor_begin_vertical_overlay(quick_drawer_motion_buf, fixed_top,
                                                     reuse_underlay)) {
        lv_obj_add_flag(quick_drawer, LV_OBJ_FLAG_HIDDEN);
        quick_drawer_bitmap_motion = true;
        quick_drawer_direct_motion = true;
        if (transition_compositor_vertical_overlay_frame(initial_y)) return true;
        /* The begin succeeded but the first present did not. Its failure
         * path has already restored LVGL; fall through to bitmap motion. */
        quick_drawer_direct_motion = false;
    }
    lv_obj_set_y(quick_drawer_motion_image, initial_y);
    lv_obj_add_flag(quick_drawer_motion_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(quick_drawer_motion_image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(quick_drawer_motion_image);
    lv_obj_move_foreground(status_bar_band);
    lv_obj_add_flag(quick_drawer, LV_OBJ_FLAG_HIDDEN);
    quick_drawer_bitmap_motion = true;
    return true;
}
void quick_drawer_finish_bitmap_motion(void) {
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());
    if (quick_drawer_direct_motion) transition_compositor_end();
    quick_drawer_direct_motion = false;
    lv_obj_set_y(quick_drawer, quick_drawer_open ? 0 : -h);
    lv_obj_remove_flag(quick_drawer, LV_OBJ_FLAG_HIDDEN);
    if (quick_drawer_motion_image) {
        lv_obj_remove_flag(quick_drawer_motion_image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(quick_drawer_motion_image, LV_OBJ_FLAG_HIDDEN);
    }
    quick_drawer_bitmap_motion = false;
    if (!quick_drawer_open) {
        transition_compositor_discard_vertical_base();
        /* Collapse here, off-screen, rather than on the next open: the
         * snapshot was last rebuilt while the drawer was still expanded, so
         * collapsing at open time would leave that stale expanded image to
         * animate in and visibly pop to collapsed once motion finished.
         * Doing it now lets the rebuild below produce a clean collapsed
         * snapshot while nothing is visible. */
        quick_drawer_reset_expansion();
        /* Park the marquees at offset 0 while off-screen so the snapshot
         * rebuilt below captures them unscrolled -- otherwise it freezes
         * whatever mid-scroll position they happened to be at, and the next
         * open animates that stale image before the live labels take over. */
        quick_drawer_restart_marquee();
        if (quick_drawer_customization_reflow_pending) {
            quick_drawer_customization_reflow_pending = false;
            quick_drawer_reflow_customization();
        }
    }
    if (quick_drawer_snapshot_dirty) lv_async_call(quick_drawer_snapshot_async_cb, NULL);
    else if (quick_drawer_expansion_warm_pending()) quick_drawer_schedule_expansion_warm();
}
static void quick_drawer_anim_done_cb(lv_anim_t * a) {
    (void) a;
    quick_drawer_finish_bitmap_motion();
}
void open_quick_drawer(void) {
    if (!quick_drawer) return;
    if (!quick_drawer_customization_enabled()) {
        /* The release-snap path may already have moved the panel partway
         * onscreen and forced quick_drawer_open false before reaching here.
         * Finish that motion as closed so neither a live panel nor its bitmap
         * overlay can be stranded when customization disables the drawer. */
        quick_drawer_open = false;
        lv_anim_delete(quick_drawer, quick_drawer_anim_y_cb);
        quick_drawer_finish_bitmap_motion();
        quick_drawer_rebuild_snapshot();
        return;
    }
    if (quick_drawer_open) return;
    quick_drawer_open = true;
    refresh_quick_drawer_brightness(); /* see its own comment -- keeps the slider from showing a stale pre-screen-off value */
    quick_drawer_refresh_volume();
    /* Customization can move expanded controls into the collapsed row.
     * Refresh their authoritative values on open too; unchanged captions
     * and image sources leave the cached drawer snapshot clean. */
    gui_shell_refresh_quick_drawer_expansion_toggles();
    quick_drawer_reset_expansion();
    /* Both labels are already parked at offset 0 by the close path below,
     * so this adds no visible jump -- it just starts the 2s wait now, on
     * open, which is where the wait is meant to be measured from. */
    quick_drawer_restart_marquee();
    quick_drawer_begin_bitmap_motion();
    lv_obj_move_foreground(quick_drawer); /* above regular screens/volume popup while showing */
    /* ...but the status bar (clock/battery/wifi/bt) stays above THAT --
     * real-hardware feedback wanted it to stay visible/readable the whole
     * time the drawer is open, not get covered by it. quick_drawer's own
     * pull_down/bg.png is opaque black for the first ~59px anyway (measured
     * directly off the asset), so the status bar ends up sitting on that as
     * a backdrop rather than on anything from the screen underneath. */
    lv_obj_move_foreground(status_bar_band);
    /* Moving it forward is not enough on an immersive Player, where it is
     * flagged hidden; re-syncing now that quick_drawer_open is set reveals
     * it for as long as the drawer is down. */
    sync_player_topbar_visibility(lv_screen_active());
    /* Cancel any prior animation on this exact (var, exec_cb) pair before
     * starting a new one to prevent concurrent animations from fighting. */
    lv_anim_delete(quick_drawer, quick_drawer_anim_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, quick_drawer);
    lv_anim_set_values(&a, quick_drawer_motion_y(), 0);
    lv_anim_set_duration(&a, gui_anim_ms(QUICK_DRAWER_ANIM_MS));
    lv_anim_set_exec_cb(&a, quick_drawer_anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, quick_drawer_anim_done_cb);
    lv_anim_start(&a);
}
void close_quick_drawer(void) {
    if (!quick_drawer_open) return;
    quick_drawer_open = false;
    quick_drawer_cancel_expansion_start();
    if (quick_drawer_expansion_box)
        lv_anim_delete(quick_drawer_expansion_box, quick_drawer_expansion_anim_cb);
    quick_drawer_expansion_motion = false;
    quick_drawer_release_motion_cache();
    /* Hidden again on the way out, so it leaves with the drawer rather than
     * popping out once the slide finishes. */
    sync_player_topbar_visibility(lv_screen_active());
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());
    lv_anim_delete(quick_drawer, quick_drawer_anim_y_cb); /* see open_quick_drawer()'s own comment on why */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, quick_drawer);
    quick_drawer_begin_bitmap_motion();
    lv_anim_set_values(&a, quick_drawer_motion_y(), -h);
    lv_anim_set_duration(&a, gui_anim_ms(QUICK_DRAWER_ANIM_MS));
    lv_anim_set_exec_cb(&a, quick_drawer_anim_y_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, quick_drawer_anim_done_cb);
    lv_anim_start(&a);
}
#if defined(BOARD_TEMPOTEC_V1)
void quick_drawer_expansion_scroll_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCROLL_BEGIN || code == LV_EVENT_SCROLL) {
        /* Closing can begin before inertia ends. Force the open/close path to
         * follow the live tree until the settled scrolled snapshot is ready. */
        quick_drawer_snapshot_dirty = true;
        quick_drawer_expansion_art_dirty = true;
    } else if (code == LV_EVENT_SCROLL_END) {
        quick_drawer_mark_snapshot_dirty();
    }
}
#endif
void quick_drawer_motion_teardown_cancel(void) {
    quick_drawer_cancel_expansion_start();
    lv_async_call_cancel(quick_drawer_snapshot_async_cb, NULL);
    if (quick_drawer_snapshot_retry_timer) {
        lv_timer_delete(quick_drawer_snapshot_retry_timer);
        quick_drawer_snapshot_retry_timer = NULL;
    }
    quick_drawer_snapshot_wait_for_wake = false;
    if (quick_drawer_expansion_box)
        lv_anim_delete(quick_drawer_expansion_box, quick_drawer_expansion_anim_cb);
    quick_drawer_expansion_motion = false;
    quick_drawer_release_motion_cache();
    quick_drawer_discard_expansion_art();
}
void quick_drawer_motion_teardown_image(void) {
    if (quick_drawer_motion_image) {
        lv_obj_delete(quick_drawer_motion_image);
        quick_drawer_motion_image = NULL;
    }
    quick_drawer_free_motion_buf();
    quick_drawer_bitmap_motion = false;
}
void quick_drawer_motion_teardown_state(void) {
    quick_drawer_shift_count = 0;
    quick_drawer_expansion_full = 0;
    quick_drawer_expansion_y = 0;
    quick_drawer_expanded = false;
    quick_drawer_expansion_pending_target = false;
}
void quick_drawer_poll_snapshot_wake(void) {
    if (quick_drawer_snapshot_wait_for_wake && quick_drawer && quick_drawer_snapshot_dirty &&
        backlight_screen_is_on()) {
        quick_drawer_snapshot_wait_for_wake = false;
        quick_drawer_schedule_snapshot_retry();
    }
}
