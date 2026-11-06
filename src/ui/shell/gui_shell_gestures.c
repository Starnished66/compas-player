#include "gui_shell_internal.h"

#define QUICK_DRAWER_TRIGGER_ZONE BOARD_SCALE_PX(140)
static void gui_shell_resume_fast_timers(void);


/* Handle stashed by gui_init() at creation time -- see poll_quick_drawer_
 * drag()'s own comment on why this timer runs at LV_DEF_REFR_PERIOD (~60fps)
 * instead of update_timer_cb's shared 500ms one. Paused by poll_quick_
 * drawer_drag() itself the instant nothing's pressed (so a ~60fps timer
 * doesn't sit registered forever, capping how long main()'s own idle
 * usleep() between lv_timer_handler() calls can ever be -- real cost even
 * though each individual idle tick barely does anything) and resumed by
 * resume_fast_gesture_timers_cb() (registered on the pointer indev, next to
 * this timer's own creation in gui_init()) the instant a new press begins
 * anywhere -- LV_EVENT_PRESSED is the one indev event LVGL dispatches
 * regardless of hit target (see poll_quick_drawer_drag()'s own doc comment
 * on why that's reliable here but LV_EVENT_PRESSING isn't). */
static lv_timer_t * quick_drawer_drag_timer = NULL;
bool quick_drawer_drag_tracking = false;
static bool quick_drawer_drag_claimed = false;
static bool quick_drawer_expansion_dragging = false;
bool quick_drawer_was_pressed = false;
static bool drag_adjust_press_owned = false;
static int32_t quick_drawer_drag_touch_start_y = 0;
static int32_t quick_drawer_drag_panel_start_y = 0;
static int32_t quick_drawer_last_velocity = 0;
#define QUICK_DRAWER_DRAG_DEADZONE 10 /* matches LVGL's own LV_INDEV_DEF_SCROLL_LIMIT -- see poll_quick_drawer_drag()'s comment */

/* Drives the quick drawer's open/close by following the finger's raw Y position
 * every tick, snapping to fully open or closed when the finger lifts.
 *
 * Polled from its own dedicated ~60fps lv_timer (see gui_init()). Reading
 * raw indev coordinates directly avoids widget hit-test interception
 * (e.g. over the brightness slider), and the ~60fps polling rate provides
 * responsive tracking throughout quick swipes. */
/* Not just lv_indev_get_next(NULL) -- the target build only ever registers
 * the one touchscreen indev, but the host simulator also registers a
 * keyboard indev (see main.c's lv_sdl_keyboard_create()), and there's no
 * guarantee which one comes back first. Explicitly finding the
 * pointer-type one is correct on both. Shared by every raw-touch-polling
 * timer in this file (poll_quick_drawer_drag(), poll_az_index_drag()) --
 * see poll_quick_drawer_drag()'s own doc comment for why polling raw indev
 * state is necessary here at all instead of LVGL's own touch events. */
lv_indev_t * find_pointer_indev(void) {
    for (lv_indev_t * candidate = lv_indev_get_next(NULL); candidate; candidate = lv_indev_get_next(candidate)) {
        if (lv_indev_get_type(candidate) == LV_INDEV_TYPE_POINTER) return candidate;
    }
    return NULL;
}
static void prefetch_press_targets(lv_point_t point);
static lv_indev_read_cb_t s_orig_pointer_read_cb = NULL;
static lv_indev_t * s_hooked_indev = NULL;
static lv_indev_state_t s_last_raw_pointer_state = LV_INDEV_STATE_RELEASED;
static bool s_require_release_after_wake = true;
static lv_point_t s_last_raw_pointer_point;
static bool s_last_raw_pointer_point_valid;

/* The read callback precedes LVGL's state publication. Only mark the existing
 * timer ready here: its callback cannot run until this read has returned to
 * the handler. Never recognize gestures or refresh a display from this path.
 * Once tracking owns the press, leave presentation on its normal 16 ms tick. */
static void gesture_note_pointer_sample(const lv_indev_data_t * data) {
    bool pressed = data->state == LV_INDEV_STATE_PRESSED;
    bool was_pressed = s_last_raw_pointer_state == LV_INDEV_STATE_PRESSED;
    bool changed = pressed != was_pressed || !s_last_raw_pointer_point_valid ||
                   data->point.x != s_last_raw_pointer_point.x ||
                   data->point.y != s_last_raw_pointer_point.y;
    s_last_raw_pointer_point = data->point;
    s_last_raw_pointer_point_valid = true;
    if (!changed || (!pressed && !was_pressed)) return;
#ifdef UI_PERF_TRACE
    ui_frame_trace_event(UI_FRAME_TRACE_EVENT_POINTER_SAMPLE,
                         ((uint32_t)(uint16_t)data->point.x << 16) |
                         (uint16_t)data->point.y, pressed ? 1u : 0u);
#endif
#ifndef UI_SWIPE_SAMPLE_READY_DISABLE
    bool tracking = home_swipe.tracking || player_swipe.tracking || back_swipe.tracking ||
                    quick_drawer_drag_claimed || quick_drawer_expansion_dragging;
    bool candidate = home_swipe.candidate || player_swipe.candidate || back_swipe.candidate ||
                     quick_drawer_drag_tracking;
    if (quick_drawer_drag_timer && !lv_timer_get_paused(quick_drawer_drag_timer) &&
        (!pressed || (!tracking && candidate)))
        lv_timer_ready(quick_drawer_drag_timer);
#endif
}

/* Wake gate: true when this raw sample must be swallowed (screen off, or a
 * finger held down across the wake transition until it is released). */
static bool pointer_sample_suppressed(lv_indev_data_t * data) {
    if (!backlight_screen_is_on()) {
        data->state = LV_INDEV_STATE_RELEASED;
        s_last_raw_pointer_state = LV_INDEV_STATE_RELEASED;
        s_require_release_after_wake = true;
        s_last_raw_pointer_point_valid = false;
        return true;
    }

    if (s_require_release_after_wake) {
        if (data->state == LV_INDEV_STATE_PRESSED) {
            /* Finger was held down across the wake transition; suppress until released */
#ifdef UI_GESTURE_TRACE
            if (s_last_raw_pointer_state != LV_INDEV_STATE_PRESSED) {
                printf("[GESTURE_TRACE] raw pointer: suppressing held touch across wake at (%d, %d)\n",
                       (int)data->point.x, (int)data->point.y);
            }
#endif
            data->state = LV_INDEV_STATE_RELEASED;
            s_last_raw_pointer_state = LV_INDEV_STATE_RELEASED;
            return true;
        } else {
            /* Physical release observed: establish clean baseline and arm subsequent presses */
            s_require_release_after_wake = false;
#ifdef UI_GESTURE_TRACE
            printf("[GESTURE_TRACE] raw pointer: release baseline established after wake\n");
#endif
        }
    }
    return false;
}

/* Unused on the host, see gui_shell_install_indev_hooks(). */
static void __attribute__((unused)) wrapped_pointer_read_cb(lv_indev_t * indev, lv_indev_data_t * data) {
    if (s_orig_pointer_read_cb) {
        s_orig_pointer_read_cb(indev, data);
    }

    if (pointer_sample_suppressed(data)) return;

    gesture_note_pointer_sample(data);

    if (data->state == LV_INDEV_STATE_PRESSED &&
        s_last_raw_pointer_state != LV_INDEV_STATE_PRESSED) {
#ifdef UI_GESTURE_TRACE
        printf("[GESTURE_TRACE] raw pointer press edge detected at (%d, %d)\n",
               (int)data->point.x, (int)data->point.y);
#endif
        prefetch_press_targets(data->point);
        gui_shell_resume_fast_timers();
        gui_library_resume_fast_timers();
    }
    s_last_raw_pointer_state = data->state;
}
static void indev_pressed_event_cb(lv_event_t * e) {
#ifdef UI_GESTURE_TRACE
    printf("[GESTURE_TRACE] indev LV_EVENT_PRESSED callback fired\n");
#endif
    /* A click-driven back slide is running: swallow the press so it neither
     * pops again nor lands on the hidden source screen's widgets. */
    if (gui_navigation_fixed_transition_running()) {
        lv_indev_t * pressed_indev = lv_event_get_indev(e);
        lv_indev_wait_release(pressed_indev);
        lv_indev_stop_processing(pressed_indev); /* hidden source widget must not see PRESSED (sliders act on it) */
        return;
    }
    gui_shell_resume_fast_timers();
    gui_library_resume_fast_timers();
}
void gui_shell_install_indev_hooks(lv_indev_t * indev) {
    if (!indev) indev = find_pointer_indev();
    if (!indev) {
#ifdef UI_GESTURE_TRACE
        printf("[GESTURE_TRACE] indev hook: pointer indev not found\n");
#endif
        return;
    }
    if (s_hooked_indev == indev) {
        return;
    }

#ifndef HOST_BUILD
    /* Not on the host: LVGL's SDL driver finds its mouse by this read_cb, so
     * wrapping it would drop every mouse event. */
    lv_indev_read_cb_t cur_read_cb = lv_indev_get_read_cb(indev);
    if (cur_read_cb && cur_read_cb != wrapped_pointer_read_cb) {
        s_orig_pointer_read_cb = cur_read_cb;
        lv_indev_set_read_cb(indev, wrapped_pointer_read_cb);
#ifdef UI_GESTURE_TRACE
        printf("[GESTURE_TRACE] indev hook: wrapped pointer read_cb for indev %p\n", (void*)indev);
#endif
    }
#endif
    lv_indev_add_event_cb(indev, indev_pressed_event_cb, LV_EVENT_PRESSED, NULL);
    s_hooked_indev = indev;
}

/* Checks whether the active pressed object or any of its parents is an
 * interactive drag-adjust widget (slider, switch, dropdown, roller) so that
 * horizontal drag adjustments are not intercepted by the swipe-to-player
 * detector. The press owner is latched to prevent fast drags from slipping
 * outside widget bounds. */
static bool active_object_is_drag_adjust_widget(void) {
    lv_obj_t * act = lv_indev_get_active_obj();
    while (act) {
        if (lv_obj_check_type(act, &lv_slider_class) ||
            lv_obj_check_type(act, &lv_arc_class) ||
            lv_obj_check_type(act, &lv_switch_class) ||
            lv_obj_check_type(act, &lv_dropdown_class) ||
            lv_obj_check_type(act, &lv_roller_class)) {
            return true;
        }
        act = lv_obj_get_parent(act);
    }
    return false;
}
bool active_press_is_over_drag_adjust_widget(void) {
    return drag_adjust_press_owned || active_object_is_drag_adjust_widget();
}

/* Checks point coordinates against registered dead zones. Prevents presses
 * starting on non-clickable card containers near sliders from triggering
 * swipe transitions. Capacity accommodates native sliders and dynamic
 * plugin settings list sliders. */
/* 13 native slider cards, up to 20 plugin settings sliders
 * (PLUGIN_SETTINGS_LIST_SCREEN_POOL_SIZE x PLUGIN_SETTINGS_LIST_MAX_SLIDERS),
 * the accent color picker, ten Quick Drawer reorder handles and up to 24
 * plugin item editor handles, plus one bounded 200-row playlist edit page.
 * Reserve 320 slots for these 268 controls and future native controls. */
#define SWIPE_DEAD_ZONE_MAX 320
static lv_obj_t * swipe_dead_zones[SWIPE_DEAD_ZONE_MAX];
static int swipe_dead_zone_count = 0;
void register_swipe_dead_zone(lv_obj_t * obj) {
    if (swipe_dead_zone_count < SWIPE_DEAD_ZONE_MAX) swipe_dead_zones[swipe_dead_zone_count++] = obj;
}

/* Compact-remove by pointer identity -- pairs with register_swipe_dead_zone()
 * above for objects that DON'T live forever (unlike every native slider
 * card, which registers once at startup and never needs to unregister). A
 * plugin.show_settings_list() pool slot's slider cards are deleted and
 * recreated on every call that reuses that slot (lv_obj_clean(), see
 * gui_plugin_show_settings_list()) -- calling this for each of a slot's own
 * previously-registered cards BEFORE that lv_obj_clean() runs is required,
 * not just tidy: point_in_swipe_dead_zone()'s own lv_obj_get_screen(obj) !=
 * lv_screen_active() guard still needs `obj` to be a live pointer to
 * dereference, so leaving a freed card's pointer in this array would be a
 * use-after-free on the next swipe check, not a graceful skip. No-op if obj
 * isn't currently registered. */
void unregister_swipe_dead_zone(lv_obj_t * obj) {
    for (int i = 0; i < swipe_dead_zone_count; i++) {
        if (swipe_dead_zones[i] == obj) {
            swipe_dead_zones[i] = swipe_dead_zones[swipe_dead_zone_count - 1];
            swipe_dead_zone_count--;
            return;
        }
    }
}

/* For gui_reload.c's in-process UI reload -- every native slider card
 * registers itself here once at startup and, per register_swipe_dead_zone()'s
 * own comment, is never expected to unregister because it "lives forever."
 * A reload breaks that assumption: it deletes every native slider and
 * builds fresh ones, but without this, the OLD (now-freed) pointers stay in
 * the array forever -- point_in_swipe_dead_zone() would dereference freed
 * memory on the very next swipe check, and every reload would also append
 * the NEW cards on top without ever clearing the old slots, filling
 * SWIPE_DEAD_ZONE_MAX permanently after just a few reloads. Only clears the
 * array (these are borrowed pointers, not owned -- nothing here to free);
 * every screen rebuilt after this call re-registers its own sliders fresh. */
void reset_swipe_dead_zones(void) {
    swipe_dead_zone_count = 0;
}
bool point_in_swipe_dead_zone(lv_point_t p) {
    for (int i = 0; i < swipe_dead_zone_count; i++) {
        lv_obj_t * obj = swipe_dead_zones[i];
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_get_screen(obj) != lv_screen_active()) continue;
        /* A seek ring reserves only its annulus, not the cover centre. */
        if (lv_obj_check_type(obj, &lv_arc_class)) {
            if (lv_obj_hit_test(obj, &p)) return true;
            continue;
        }
        lv_area_t area;
        /* lv_obj_get_click_area(), not lv_obj_get_coords() -- a native
         * slider's raw box can be a few px tall (e.g. the player's progress
         * bar) while lv_obj_set_ext_click_area() widens what LVGL itself
         * treats as a touch on it. Using the raw box here left a real,
         * visually-on-the-slider press just outside the registered dead
         * zone free to start a back-swipe instead. Falls back to the raw
         * box automatically when an object has no ext click area set. */
        lv_obj_get_click_area(obj, &area);
        if (p.x >= area.x1 && p.x <= area.x2 && p.y >= area.y1 && p.y <= area.y2) return true;
    }
    return false;
}
bool player_swipe_press_excluded(lv_point_t p) {
    return active_press_is_over_drag_adjust_widget() || point_in_swipe_dead_zone(p);
}


/* Only hints for gestures eligible at this touch origin. No cache creation,
 * layout, locks that wait for the worker, or image copies on this path. */
static void prefetch_press_targets(lv_point_t point) {
    lv_obj_t *screen = lv_screen_active();
    if (gui_navigation_transition_in_progress() || gui_library_navigation_blocked() ||
        shell_screen_blocks_swipe(screen)) return;
    bool drawer_origin = quick_drawer_open ||
                         (point.y <= QUICK_DRAWER_TRIGGER_ZONE &&
                          quick_drawer_customization_enabled());
    lv_draw_buf_t *drawer = drawer_origin && !quick_drawer_snapshot_dirty
                               ? quick_drawer_motion_buf : NULL;
    if (quick_drawer_open) {
        gui_navigation_prefetch_targets(false, false, false, drawer);
        return;
    }
    /* Raw read precedes LVGL hit-testing; use this press's coordinates,
     * not the active object retained from the previous press. */
    if (point_in_swipe_dead_zone(point)) return;
    int32_t height = lv_display_get_vertical_resolution(lv_display_get_default());
    bool home = current_settings.swipe_up_home_enabled &&
                screen != gui_shell_get_home_screen() && screen != gui_lyrics_get_screen() &&
                point.y >= height - HOME_INDICATOR_BAND_HEIGHT - HOME_SWIPE_HIT_EXTRA_PX;
    gui_navigation_prefetch_targets(screen != gui_player_get_screen() &&
                                   screen != gui_lyrics_get_screen() &&
                                   screen != gui_track_info_get_screen(),
                                   gui_navigation_get_depth() > 1, home, drawer);
}

/* Whether `point` falls in a slider's click area (the V1 drawer also hides
 * its sliders while the expansion is dragged open). */
static bool quick_drawer_slider_hit(lv_obj_t * track, lv_point_t point) {
#if defined(BOARD_TEMPOTEC_V1)
    if (quick_drawer_expansion_y > 0 ||
        lv_obj_has_flag(track, LV_OBJ_FLAG_HIDDEN)) return false;
#endif
    lv_area_t area;
    lv_obj_get_click_area(track, &area);
    return point.x >= area.x1 && point.x <= area.x2 &&
           point.y >= area.y1 && point.y <= area.y2;
}
static bool quick_drawer_brightness_hit_test(lv_point_t point) {
    if (!quick_drawer_open || !quick_drawer_brightness_track) return false;
    return quick_drawer_slider_hit(quick_drawer_brightness_track, point);
}

/* quick_drawer_brightness_hit_test()'s own twin for the volume slider --
 * this one was missing entirely, which was the real cause of "dragging the
 * volume slider sometimes stops tracking / triggers a swipe": without it,
 * drag_adjust_press_owned below fell back to active_object_is_drag_adjust_
 * widget() alone, which reads lv_indev_get_active_obj() -- state LVGL's own
 * indev processing updates independently of this timer's own 16ms poll, so
 * on some press-downs (this timer racing ahead of that update, more likely
 * under a fast repeated drag) it read stale state and saw no slider owning
 * the press. Ownership then fell through to quick_drawer_drag_tracking /
 * the swipe detectors instead, at which point the slider stopped
 * following the finger and the drawer itself (or a swipe-up) took over.
 * A direct, coordinate-based check has no such race. */
static bool quick_drawer_volume_hit_test(lv_point_t point) {
    if (!quick_drawer_open || !ui_customization_drawer_volume_visible() ||
        !quick_drawer_volume_track) return false;
    return quick_drawer_slider_hit(quick_drawer_volume_track, point);
}

/* The expansion drag owns everything above the now-playing card, not just
 * the handle, so the target is the whole toggle area rather than an 8px
 * pill. Must be in the same press-down ownership chain: without it a
 * downward drag here reads as an ordinary drawer drag and closes the
 * drawer instead of expanding it -- the exact failure the volume rail
 * already hit once. The two rails are excluded because their own hit areas
 * overlap this region and a press inside them is a slider drag; dragging
 * from the card downwards still closes the whole drawer. */
static bool quick_drawer_expansion_region_hit_test(lv_point_t point) {
    if (!quick_drawer_open || !quick_drawer_expansion_handle) return false;
    if (quick_drawer_expansion_full <= 0) return false;
    if (quick_drawer_brightness_hit_test(point)) return false;
    if (quick_drawer_volume_hit_test(point)) return false;
    lv_area_t handle;
    lv_obj_get_coords(quick_drawer_expansion_handle, &handle);
#if defined(BOARD_TEMPOTEC_V1)
    int32_t pad_x = BOARD_SCALE_PX(40);
    int32_t pad_y = BOARD_SCALE_PY(4);
    return point.x >= handle.x1 - pad_x && point.x <= handle.x2 + pad_x &&
           point.y >= handle.y1 - pad_y && point.y <= handle.y2 + pad_y;
#else
    return point.y <= handle.y2;
#endif
}
static bool quick_drawer_expansion_scroll_hit_test(lv_point_t point) {
#if defined(BOARD_TEMPOTEC_V1)
    if (!quick_drawer_open || !quick_drawer_expansion_box || quick_drawer_expansion_y <= 0)
        return false;
    lv_area_t viewport;
    lv_obj_get_coords(quick_drawer_expansion_box, &viewport);
    return point.x >= viewport.x1 && point.x <= viewport.x2 &&
           point.y >= viewport.y1 && point.y <= viewport.y2;
#else
    (void) point;
    return false;
#endif
}
static bool quick_drawer_expansion_moved = false;
static int32_t quick_drawer_expansion_drag_start_y = 0;
static int32_t quick_drawer_expansion_drag_start_value = 0;

/* Press-down stage: decide once who owns this press (expansion drag, slider,
 * dead zone, ...). Order matters: the expansion claim is evaluated first. */
static void quick_drawer_poll_press_ownership(lv_point_t p) {
    /* Gesture ownership is decided once at press-down. A fast slider
     * drag may leave its bounds, but it remains a slider drag until lift. */
    quick_drawer_expansion_dragging = quick_drawer_expansion_region_hit_test(p);
    quick_drawer_expansion_moved = false;
    if (quick_drawer_expansion_dragging) {
        quick_drawer_cancel_expansion_start();
        lv_anim_delete(quick_drawer_expansion_box, quick_drawer_expansion_anim_cb);
        quick_drawer_expansion_motion = true;
        quick_drawer_expansion_drag_start_y = p.y;
        quick_drawer_expansion_drag_start_value = quick_drawer_expansion_y;
        /* Same read-on-the-way-open as the animated path, for a drag
         * that reveals the rows without going through it. */
        if (quick_drawer_expansion_y == 0) {
#if defined(BOARD_TEMPOTEC_V1)
            lv_obj_scroll_to_y(quick_drawer_expansion_box, 0, LV_ANIM_OFF);
#endif
            gui_shell_refresh_quick_drawer_expansion_toggles();
        }
    }

    /* The open drawer covers the screen, so that screen's dead zones
     * (progress bar, slider cards) must not block a drawer drag. */
    drag_adjust_press_owned = active_object_is_drag_adjust_widget() ||
                              (!quick_drawer_open && point_in_swipe_dead_zone(p)) ||
                              quick_drawer_brightness_hit_test(p) ||
                              quick_drawer_volume_hit_test(p) ||
                              quick_drawer_expansion_dragging ||
                              quick_drawer_expansion_scroll_hit_test(p) ||
                              gui_player_volume_control_hit_test(p);
}

/* Press-down stage: drawer drag candidate (open-drag from the top zone or
 * close-drag while open). */
static void quick_drawer_poll_press_drawer_candidate(lv_point_t p) {
    /* Cancel any release-snap animation still in flight to prevent it
     * from fighting a newly started drag. */
    lv_anim_delete(quick_drawer, quick_drawer_anim_y_cb);

    /* Use the drawer's current Y so interrupted animations continue
     * naturally. Adjustment widgets keep ownership of their drags. */
    if (drag_adjust_press_owned) {
        quick_drawer_drag_tracking = false;
    } else if (quick_drawer_open) {
        quick_drawer_drag_tracking = true;
        quick_drawer_drag_panel_start_y = quick_drawer_motion_y();
        quick_drawer_last_velocity = 0; /* fresh drag, no leftover direction from a previous completed one -- same reset as home/player/back-swipe's own tracking-start */
    } else if (quick_drawer_customization_enabled() &&
               p.y <= QUICK_DRAWER_TRIGGER_ZONE && !gui_library_navigation_blocked() &&
               !gui_navigation_fixed_transition_running() &&
               lv_screen_active() != gui_lock_screen_get_screen()) {
        /* gui_library_navigation_blocked() only covers modal library
         * operations that own the active screen. Optional artwork/search
         * workers must not disable normal navigation. This only blocks a NEW open-drag;
         * if the drawer somehow got dragged open right as a rescan
         * started, the quick_drawer_open branch above still lets it be
         * dragged closed again. */
        quick_drawer_drag_tracking = true;
        quick_drawer_drag_panel_start_y = quick_drawer_motion_y();
        quick_drawer_last_velocity = 0; /* fresh drag -- see the other branch's own comment */
        /* This is only a candidate: a header Back tap also starts here.
         * Reordering siblings invalidates the full top layer twice;
         * defer z-order changes until a real drawer drag is claimed. */
    } else {
        quick_drawer_drag_tracking = false;
    }
    quick_drawer_drag_claimed = false;
    quick_drawer_drag_touch_start_y = p.y;
}

/* Tracking stage: expansion drag follows the finger. */
static void quick_drawer_poll_expansion_tracking(lv_indev_t * indev, lv_point_t p, bool pressed) {
    /* Expansion drag: tracks the finger 1:1 between collapsed and fully
     * open. Runs before the drawer's own drag block below, which cannot be
     * active at the same time -- drag_adjust_press_owned took this press. */
    if (pressed && quick_drawer_expansion_dragging) {
        int32_t raw_delta = p.y - quick_drawer_expansion_drag_start_y;
        if (!quick_drawer_expansion_moved &&
            (raw_delta > QUICK_DRAWER_DRAG_DEADZONE || raw_delta < -QUICK_DRAWER_DRAG_DEADZONE)) {
            /* Past the deadzone this is a drag, not a tap on the handle --
             * suppress the release so quick_drawer_expansion_handle_cb()
             * cannot also fire and immediately undo the snap below. */
            quick_drawer_expansion_moved = true;
            quick_drawer_prepare_motion_cache();
            quick_drawer_begin_expansion_composition();
            lv_indev_wait_release(indev);
        }
        if (quick_drawer_expansion_moved) {
            int32_t adjusted = raw_delta > 0 ? raw_delta - QUICK_DRAWER_DRAG_DEADZONE
                                             : raw_delta + QUICK_DRAWER_DRAG_DEADZONE;
            quick_drawer_apply_expansion(quick_drawer_expansion_drag_start_value + adjusted);
        }
    }
}

/* Release stage: expansion drag snap, close, or hand-off to the handle tap. */
static void quick_drawer_poll_expansion_release(lv_point_t p, bool pressed) {
    if (!pressed && quick_drawer_was_pressed && quick_drawer_expansion_dragging) {
        quick_drawer_expansion_dragging = false;
        if (quick_drawer_expansion_moved) {
            if (quick_drawer_expansion_drag_start_value == 0 &&
                p.y < quick_drawer_expansion_drag_start_y) {
                /* Nothing was expanded to collapse, so an upward drag here is
                 * the drawer's own close gesture rather than a row drag. */
                close_quick_drawer();
            } else {
                /* Snap to whichever end the drag ended nearer. A tap that never
                 * left the deadzone falls through to the handle's own CLICKED
                 * handler instead. */
                quick_drawer_animate_expansion(quick_drawer_expansion_y * 2 >= quick_drawer_expansion_full);
            }
            quick_drawer_expansion_moved = false;
        } else {
            /* A press that never became a drag is handed to LVGL's click
             * callback; release the snapshot gate until that callback runs. */
            if (!quick_drawer_expansion_start_timer &&
                !lv_anim_get(quick_drawer_expansion_box, quick_drawer_expansion_anim_cb)) {
                if (quick_drawer_expansion_y > 0 &&
                    quick_drawer_expansion_y < quick_drawer_expansion_full) {
                    quick_drawer_animate_expansion(quick_drawer_expanded);
                } else {
                    quick_drawer_expansion_motion = false;
                    quick_drawer_release_motion_cache();
                    quick_drawer_mark_snapshot_dirty();
                }
            }
        }
    }
}

/* Panel Y for a drag past the deadzone: the deadzone is subtracted so drag
 * motion starts smoothly from zero, clamped between fully open and off. */
static int32_t quick_drawer_drag_target_y(int32_t raw_delta, int32_t h) {
    int32_t adjusted_delta = raw_delta > 0 ? raw_delta - QUICK_DRAWER_DRAG_DEADZONE
                                            : raw_delta + QUICK_DRAWER_DRAG_DEADZONE;
    int32_t new_y = quick_drawer_drag_panel_start_y + adjusted_delta;
    if (new_y > 0) new_y = 0;
    if (new_y < -h) new_y = -h;
    return new_y;
}

/* Past the deadzone this is a drag, not a tap. The live drawer does not cover
 * the list while opening (it starts off-screen), and bitmap motion hides the
 * real panel behind a snapshot -- without wait_release(), LVGL re-hit-tests
 * the still-down finger onto whatever row is now underneath and fires CLICKED
 * on release. Same tool as the player-swipe path. */
static void quick_drawer_claim_drag(lv_indev_t * indev) {
#ifdef UI_PERF_TRACE
    ui_frame_trace_event(UI_FRAME_TRACE_EVENT_DRAWER_CLAIM, 0, 0);
#endif
    quick_drawer_drag_claimed = true;
    lv_obj_move_foreground(quick_drawer);
    lv_obj_move_foreground(status_bar_band);
    lv_indev_wait_release(indev);
}

/* Deadzone and tracking stage: drawer drag follows the finger. */
static void quick_drawer_poll_drawer_tracking(lv_indev_t * indev, lv_point_t p, int32_t h, bool pressed) {
    if (pressed && quick_drawer_drag_tracking) {
        /* Deadzone before moving panel: suppresses minor jitter during taps
         * and long-presses so child widgets (icons) do not receive PRESS_LOST.
         * Beyond QUICK_DRAWER_DRAG_DEADZONE (10px), the deadzone is subtracted
         * so drag motion starts smoothly from zero. */
        int32_t raw_delta = p.y - quick_drawer_drag_touch_start_y;
        if (raw_delta > QUICK_DRAWER_DRAG_DEADZONE || raw_delta < -QUICK_DRAWER_DRAG_DEADZONE) {
            int32_t new_y = quick_drawer_drag_target_y(raw_delta, h);
            if (!quick_drawer_drag_claimed) quick_drawer_claim_drag(indev);
            /* Per-tick velocity, in case the finger lifts mid-flick (see the
             * release stage) -- a plain position delta rather than
             * lv_indev_get_vect() so it's driven by the exact same samples
             * this function already reads, not a second/possibly-
             * differently-timed source. */
            if (!quick_drawer_bitmap_motion) quick_drawer_begin_bitmap_motion();
            /* Sticky velocity -- see swipe_track_update()'s own comment on this
             * same pattern. A zero-delta tick (finger momentarily still, or
             * settled back within the deadzone) must not clobber the last
             * real direction, since a deliberate slow drag naturally
             * decelerates to near-zero right before it lifts. */
            int32_t quick_drawer_delta = new_y - quick_drawer_motion_y();
            if (quick_drawer_delta != 0) quick_drawer_last_velocity = quick_drawer_delta;
            quick_drawer_anim_y_cb(quick_drawer, new_y);
        }
    }
}

/* Release stage: drawer settles open or closed. */
static void quick_drawer_poll_drawer_release(int32_t h, bool pressed) {
    if (!pressed && quick_drawer_was_pressed && quick_drawer_drag_tracking) {
        quick_drawer_drag_tracking = false;
        /* Unclaimed taps need no settle animation. An interrupted bitmap
         * motion still needs settling even if this press never became a drag. */
        if (quick_drawer_drag_claimed || quick_drawer_bitmap_motion) {
            /* Settle decision on release: any registered direction at the exact
             * moment of release wins, however slow; only a perfectly still
             * release (zero exit velocity) falls back to whichever side of the
             * halfway mark the panel was on. */
            bool snap_open;
            if (quick_drawer_last_velocity > 0) {
                snap_open = true; /* still moving down (toward open) at release, however slowly */
            } else if (quick_drawer_last_velocity < 0) {
                snap_open = false; /* still moving up (toward closed) at release, however slowly */
            } else {
                snap_open = quick_drawer_motion_y() > -h / 2; /* perfectly still at release -- fall back to position */
            }
            /* open_quick_drawer()/close_quick_drawer() animate from the
             * drawer's CURRENT (mid-drag) position, so forcing quick_drawer_open
             * to the opposite state first just defeats their own early-return
             * guard rather than fighting the animation. */
            if (snap_open) {
                quick_drawer_open = false;
                open_quick_drawer();
            } else {
                quick_drawer_open = true;
                close_quick_drawer();
            }
        }
    }
}

/* Press-down stages: who owns this press, then the drawer and swipe candidates. */
static void quick_drawer_poll_press_down(lv_point_t p, int32_t h) {
    quick_drawer_poll_press_ownership(p);
    quick_drawer_poll_press_drawer_candidate(p);
    gui_shell_swipes_poll_press(p, h);
}

/* Per-tick tracking and release stages. Order matters: swipe deadzones and
 * tracking, expansion, then the drawer, and the swipe releases last. */
static void quick_drawer_poll_stages(lv_indev_t * indev, lv_point_t p, int32_t h, bool pressed) {
    gui_shell_swipes_poll_deadzones(indev, p, pressed);
    gui_shell_swipes_poll_tracking(p, h, pressed);
    quick_drawer_poll_expansion_tracking(indev, p, pressed);
    quick_drawer_poll_expansion_release(p, pressed);
    quick_drawer_poll_drawer_tracking(indev, p, h, pressed);
    quick_drawer_poll_drawer_release(h, pressed);
    gui_shell_swipes_poll_release(h, pressed);
}

static void poll_quick_drawer_drag(lv_timer_t * timer) {
    if (!current_settings.setup_complete) return;
    lv_indev_t * indev = find_pointer_indev();
    if (!indev) return;

    bool pressed = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());

    if (pressed && !quick_drawer_was_pressed) quick_drawer_poll_press_down(p, h);
    quick_drawer_poll_stages(indev, p, h, pressed);

    if (!pressed && quick_drawer_was_pressed) {
#ifdef UI_GESTURE_TRACE
        printf("[GESTURE_TRACE] poll: release observed (home_tracking=%d, drawer_tracking=%d, player_tracking=%d, back_tracking=%d)\n",
               home_swipe.tracking, quick_drawer_drag_tracking, player_swipe.tracking, back_swipe.tracking);
#endif
        gui_shell_swipes_clear_candidates();
    }

    quick_drawer_was_pressed = pressed;

    /* Every release-handling branch above (drawer snap, player-swipe
     * settle) has already run by this point in the same call that observed
     * the release -- nothing left to track until resume_fast_gesture_
     * timers_cb() wakes this again on the next press-down. See this
     * timer's own handle comment for why pausing (not just letting the
     * ~60fps tick keep firing and no-op) is what actually matters here. */
    if (!pressed) {
        drag_adjust_press_owned = false;
#ifdef UI_GESTURE_TRACE
        printf("[GESTURE_TRACE] poll: timer self-paused\n");
#endif
        lv_timer_pause(timer);
    }
}
static void gui_shell_resume_fast_timers(void) {
    if (quick_drawer_drag_timer && lv_timer_get_paused(quick_drawer_drag_timer)) {
#ifdef UI_GESTURE_TRACE
        printf("[GESTURE_TRACE] resume_fast_timers: quick_drawer_drag_timer\n");
#endif
        lv_timer_resume(quick_drawer_drag_timer);
        lv_timer_ready(quick_drawer_drag_timer);
    }
}
/* Reset, expansion part: stop any expansion drag or settle and commit to a
 * stable endpoint. */
static void quick_drawer_reset_expansion_drag(void) {
    bool expansion_was_dragging = quick_drawer_expansion_dragging;
    quick_drawer_expansion_dragging = false;
    quick_drawer_expansion_moved = false;
    quick_drawer_cancel_expansion_start();
    if (quick_drawer_expansion_box)
        lv_anim_delete(quick_drawer_expansion_box, quick_drawer_expansion_anim_cb);
    if (quick_drawer_expansion_motion) {
        /* Screen-off/reload can interrupt either a finger drag or a settle.
         * Commit to a stable endpoint so no partial drawer snapshot survives. */
        if (expansion_was_dragging && quick_drawer_expansion_full > 0)
            quick_drawer_expanded = quick_drawer_expansion_y * 2 >= quick_drawer_expansion_full;
        quick_drawer_expansion_motion = false;
        quick_drawer_apply_expansion(quick_drawer_expanded ? quick_drawer_expansion_full : 0);
        quick_drawer_release_motion_cache();
        quick_drawer_mark_snapshot_dirty();
    }
}

/* Cancel any active drawer animation or motion and restore deterministic closed state */
static void quick_drawer_reset_motion(void) {
    lv_anim_delete(quick_drawer, quick_drawer_anim_y_cb);
    if (quick_drawer_bitmap_motion || quick_drawer_direct_motion) {
        quick_drawer_open = false;
        quick_drawer_finish_bitmap_motion();
    }
}

void gui_shell_reset_drag_state(void) {
#ifdef UI_GESTURE_TRACE
    printf("[GESTURE_TRACE] reset_drag_state called (was_pressed=%d, home_tracking=%d, drawer_tracking=%d, player_tracking=%d)\n",
           quick_drawer_was_pressed, home_swipe.tracking, quick_drawer_drag_tracking, player_swipe.tracking);
#endif
    quick_drawer_was_pressed = false;
    quick_drawer_drag_tracking = false;
    drag_adjust_press_owned = false;
    quick_drawer_drag_claimed = false;
    quick_drawer_last_velocity = 0;
    quick_drawer_reset_expansion_drag();

    gui_shell_swipes_reset_home();
    quick_drawer_reset_motion();
    gui_shell_swipes_reset_player_back();
    s_last_raw_pointer_state = LV_INDEV_STATE_RELEASED;
    s_require_release_after_wake = true;
    s_last_raw_pointer_point_valid = false;
    if (quick_drawer_drag_timer) {
        lv_timer_pause(quick_drawer_drag_timer);
    }
}

void gui_shell_set_external_touch_held(bool held) {
    s_require_release_after_wake = held;
}
void gui_shell_gestures_start(void) {
    gui_shell_reset_drag_state();
    if (!quick_drawer_drag_timer) {
        quick_drawer_drag_timer = lv_timer_create(poll_quick_drawer_drag, LV_DEF_REFR_PERIOD, NULL);
    }
    gui_shell_install_indev_hooks(NULL);
}
void gui_shell_gestures_reset_expansion_flags(void) {
    quick_drawer_expansion_dragging = false;
    quick_drawer_expansion_moved = false;
}
bool gui_shell_boot_prompt_blocked(void) {
    if (quick_drawer_open || quick_drawer_drag_tracking || quick_drawer_drag_claimed ||
        quick_drawer_expansion_dragging || quick_drawer_bitmap_motion || quick_drawer_direct_motion) return true;
    if (gui_shell_swipes_busy()) return true;
    if (!quick_drawer) return false;
    int32_t height = lv_display_get_vertical_resolution(lv_display_get_default());
    return height > 0 && quick_drawer_motion_y() > -height;
}
