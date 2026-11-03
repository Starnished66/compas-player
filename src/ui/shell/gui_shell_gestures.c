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
static bool quick_drawer_drag_tracking = false;
static bool quick_drawer_drag_claimed = false;
static bool quick_drawer_expansion_dragging = false;
static bool quick_drawer_was_pressed = false;
static bool drag_adjust_press_owned = false;
static int32_t quick_drawer_drag_touch_start_y = 0;
static int32_t quick_drawer_drag_panel_start_y = 0;
static int32_t quick_drawer_last_velocity = 0;
#define QUICK_DRAWER_DRAG_DEADZONE 10 /* matches LVGL's own LV_INDEV_DEF_SCROLL_LIMIT -- see poll_quick_drawer_drag()'s comment */

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
static swipe_state_t home_swipe, player_swipe, back_swipe;
static void swipe_clear_flags(swipe_state_t * s) {
    s->candidate = false;
    s->tracking = false;
    s->just_confirmed = false;
}

/* Dx/dy from the press origin; true once either axis passes the deadzone. */
static bool swipe_deadzone_exceeded(const swipe_state_t * s, lv_point_t p,
                                    int32_t * dx, int32_t * dy, int32_t * adx, int32_t * ady) {
    *dx = p.x - s->start_x;
    *dy = p.y - s->start_y;
    *adx = *dx < 0 ? -*dx : *dx;
    *ady = *dy < 0 ? -*dy : *dy;
    return *adx >= NAV_SWIPE_DEADZONE_PX || *ady >= NAV_SWIPE_DEADZONE_PX;
}

/* Per-tick tracking update for an already-clamped offset v. Velocity is
 * sticky: only overwritten on an actual per-tick move. A real finger
 * decelerates to a near-zero last-tick delta right before it lifts, so
 * clobbering the last direction with that final zero tick would make the
 * halfway-position fallback fire on most slow drags. last_v always stays
 * current; only the frame PRESENT is skipped on the tick that began
 * tracking (see just_confirmed), since begin_slide_transition_ex() and the
 * first present would otherwise land in the same tick. */
static void swipe_track_update(swipe_state_t * s, int32_t v) {
    int32_t delta = v - s->last_v;
    if (delta != 0) s->last_velocity = delta;
    s->last_v = v;
    if (s->just_confirmed) {
        s->just_confirmed = false;
    } else {
        slide_transition_anim_x_cb(s->ctx, v);
    }
}

/* Release decision: any registered direction at release wins, however slow;
 * a perfectly still release falls back to which side of halfway it is on.
 * sign is +1 when committing means a positive offset, -1 for negative. */
static bool swipe_release_commit(const swipe_state_t * s, int32_t sign, int32_t extent) {
    if (sign * s->last_velocity > 0) return true;
    if (sign * s->last_velocity < 0) return false;
    return sign * s->last_v > extent / 2;
}

/* Settles the live overlay to `to` with the shared short ease-out. */
static void swipe_settle(slide_transition_ctx_t * ctx, int32_t from, int32_t to) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ctx);
    lv_anim_set_user_data(&a, ctx);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, gui_anim_ms(QUICK_DRAWER_ANIM_MS));
    lv_anim_set_exec_cb(&a, slide_transition_anim_x_cb);
    lv_anim_set_completed_cb(&a, slide_transition_done_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* Swipe-left-to-player tracking -- same "raw indev polling, own dedicated
 * fast timer" reasoning as poll_quick_drawer_drag()'s own doc comment,
 * replacing the old LV_EVENT_GESTURE-based instant cut (see
 * screen_gesture_event_cb()'s own comment on why that couldn't just be
 * left running alongside this). Unlike the drawer's drag (claimed
 * instantly, by which zone the press started in) or the home-swipe
 * (claimed instantly, by starting inside a fixed band), this can start
 * ANYWHERE on screen -- matching the gesture it replaces -- so which
 * press this is can't be decided at press-down; it's provisional
 * (player_swipe.candidate) until enough movement accumulates to judge
 * direction, then either confirmed (player_swipe.tracking, the overlay
 * gets built and starts following the finger) or abandoned, letting the
 * press fall through as whatever else it actually was (a tap, a vertical
 * scroll, or a rightward back-swipe -- that one has its own live-tracking
 * state machine below. screen_gesture_event_cb()'s event-based right-swipe
 * remains the fallback for presses that state machine rejects). */

/* Swipe-right-to-go-back -- same live per-tick overlay as player_swipe_*
 * above, mirrored in sign. Provisional until NAV_SWIPE_DEADZONE_PX so a
 * leftward player-swipe or a vertical scroll can still claim the press.
 * screen_gesture_event_cb()'s LV_DIR_RIGHT -> nav_pop() path stays as
 * the fallback for presses this candidate rejects (excluded screens,
 * dead zones, depth == 1). Unlike player-swipe's own left-swipe (which has
 * no competing consumer -- screen_gesture_event_cb only ever acts on
 * LV_DIR_RIGHT), this candidate genuinely races LVGL's own native gesture
 * recognition for the exact same direction: on real hardware, a fast swipe
 * can cross LVGL's own internal gesture threshold and dispatch
 * LV_EVENT_GESTURE before this poll-based NAV_SWIPE_DEADZONE_PX confirms on
 * its own next tick, so wait_release() alone does not reliably win that
 * race (confirmed via on-device logging -- both fired for the same
 * continued drag, each independently acting on directory depth). back_swipe_
 * owns_press below is the actual mutual-exclusion mechanism: latched true
 * at press-down whenever this press is eligible at all (regardless of
 * whether the deadzone ever confirms a direction), and checked by
 * gui_shell_back_swipe_owns_press() so screen_gesture_event_cb can
 * unconditionally stand down for the whole press rather than trust timing. */
static bool back_swipe_owns_press = false;
static lv_obj_t * back_swipe_target_scr = NULL;
bool gui_shell_back_swipe_owns_press(void) {
    return back_swipe_owns_press;
}

/* Forward declarations -- both fully built in gui_network.c, needed here
 * so poll_quick_drawer_drag() below can exclude the home-swipe gesture
 * while either DAC overlay is active (see its own comment on why). */

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

/* Unused on the host, see gui_shell_install_indev_hooks(). */
static void __attribute__((unused)) wrapped_pointer_read_cb(lv_indev_t * indev, lv_indev_data_t * data) {
    if (s_orig_pointer_read_cb) {
        s_orig_pointer_read_cb(indev, data);
    }

    if (!backlight_screen_is_on()) {
        data->state = LV_INDEV_STATE_RELEASED;
        s_last_raw_pointer_state = LV_INDEV_STATE_RELEASED;
        s_require_release_after_wake = true;
        s_last_raw_pointer_point_valid = false;
        return;
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
            return;
        } else {
            /* Physical release observed: establish clean baseline and arm subsequent presses */
            s_require_release_after_wake = false;
#ifdef UI_GESTURE_TRACE
            printf("[GESTURE_TRACE] raw pointer: release baseline established after wake\n");
#endif
        }
    }

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
static bool player_swipe_press_excluded(lv_point_t p) {
    return active_press_is_over_drag_adjust_widget() || point_in_swipe_dead_zone(p);
}

/* Screens that own their own exit (lock, text entry/view, busy, both DAC
 * overlays): no swipe-back and no navigation prefetch from them. */
static bool shell_screen_blocks_swipe(lv_obj_t * screen) {
    return screen == gui_lock_screen_get_screen() || screen == gui_text_input_get_screen() ||
           screen == gui_text_view_get_screen() || screen == gui_busy_get_screen() ||
           screen == gui_network_get_usb_dac_overlay() ||
           screen == gui_network_get_bt_dac_overlay();
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
static void poll_quick_drawer_drag(lv_timer_t * timer) {
    if (!current_settings.setup_complete) return;
    lv_indev_t * indev = find_pointer_indev();
    if (!indev) return;

    bool pressed = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());

    if (pressed && !quick_drawer_was_pressed) {
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

        drag_adjust_press_owned = active_object_is_drag_adjust_widget() ||
                                  point_in_swipe_dead_zone(p) ||
                                  quick_drawer_brightness_hit_test(p) ||
                                  quick_drawer_volume_hit_test(p) ||
                                  quick_drawer_expansion_dragging ||
                                  quick_drawer_expansion_scroll_hit_test(p) ||
                                  gui_player_volume_control_hit_test(p);
    }

    if (pressed && !quick_drawer_was_pressed) {
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

        gesture_home_config_t home_cfg;
        home_cfg.swipe_up_home_enabled = current_settings.swipe_up_home_enabled;
        home_cfg.quick_drawer_open = quick_drawer_open;
        home_cfg.is_bt_dac_overlay = (lv_screen_active() == gui_network_get_bt_dac_overlay());
        home_cfg.is_usb_dac_overlay = (lv_screen_active() == gui_network_get_usb_dac_overlay());
        home_cfg.is_lyrics_screen = (lv_screen_active() == gui_lyrics_get_screen());
        home_cfg.is_lock_screen = (lv_screen_active() == gui_lock_screen_get_screen());
        home_cfg.has_background_work = gui_library_navigation_blocked();
        home_cfg.screen_height = h;
        /* Slightly expand only the raw press-down target. The overlay band and
         * its visible pill retain their existing dimensions. */
        home_cfg.band_height = HOME_INDICATOR_BAND_HEIGHT + HOME_SWIPE_HIT_EXTRA_PX;

        /* gesture_home_config_t's fields are shared with gui_lock_screen.c's
         * own independent use of gesture_home_state_is_eligible(), which
         * doesn't need these -- same reasoning as back-swipe's own
         * exclusions just above: text-entry and the busy overlay
         * skip finalize_screen_navigation() because leaving
         * them needs teardown (in-progress input, modal ownership) that a stack-only reset to Home would bypass.
         *
         * Deliberately NOT gated on drag_adjust_press_owned (unlike player-
         * swipe/back-swipe below): home_indicator_band already sits on
         * lv_layer_top(), CLICKABLE, sized to this exact band, so nothing on
         * the active screen can genuinely receive a press inside this strip.
         * point_in_swipe_dead_zone() is purely coordinate-based with no
         * z-order awareness, so it was vetoing this gesture whenever some
         * OTHER, actually-covered widget's registered dead zone happened to
         * overlap this band's coordinates -- e.g. a bottom-aligned EQ screen
         * or a plugin slider card extending into the last ~31px of the
         * screen. Keep this ungated so the indicator band's own hitbox keeps
         * real priority over whatever is positioned underneath it. */
        home_swipe.candidate = gesture_home_state_is_eligible(&home_cfg, p.y) &&
                                lv_screen_active() != gui_shell_get_home_screen() &&
                                lv_screen_active() != gui_text_input_get_screen() &&
                                lv_screen_active() != gui_busy_get_screen();
        home_swipe.start_x = p.x;
        home_swipe.start_y = p.y;
        home_swipe.tracking = false;

#ifdef UI_GESTURE_TRACE
        printf("[GESTURE_TRACE] poll: press-down at (%d, %d), res_h=%d\n", (int)p.x, (int)p.y, (int)h);
        printf("[GESTURE_TRACE] poll: home_swipe eval: enabled=%d, tracking=%d\n",
               current_settings.swipe_up_home_enabled, home_swipe.candidate);
#endif

        /* Player-swipe: eligible unless claimed by the drawer drag, the drawer
         * is open, the player screen is already active, or the press started on
         * a drag-adjust widget/dead-zone. Excluded on lyrics, track info,
         * lock screen, both DAC mode overlays (leaving them is only by
         * closing DAC mode), and while library navigation is blocked.
         * Candidate only until sufficient displacement accumulates to
         * determine gesture direction. */
        player_swipe.candidate = !quick_drawer_drag_tracking && !quick_drawer_open &&
                                  lv_screen_active() != gui_player_get_screen() &&
                                  lv_screen_active() != gui_lyrics_get_screen() &&
                                  lv_screen_active() != gui_track_info_get_screen() &&
                                  lv_screen_active() != gui_lock_screen_get_screen() &&
                                  lv_screen_active() != gui_text_view_get_screen() &&
                                  lv_screen_active() != gui_network_get_usb_dac_overlay() &&
                                  lv_screen_active() != gui_network_get_bt_dac_overlay() &&
                                  !gui_library_navigation_blocked() &&
                                  !player_swipe_press_excluded(p);
        player_swipe.start_x = p.x;
        player_swipe.start_y = p.y;
        player_swipe.tracking = false;

        /* Depth > 1 is the stack-pop precondition. Lyrics owns its own
         * right-swipe (lyrics_gesture_event_cb -> close_lyrics_screen).
         * Lock, text-entry, both DAC overlays, and the
         * busy overlay skip finalize_screen_navigation() -- a live pop
         * here would bypass their leave-confirmation, teardown, or
         * modal-ownership. Same drawer/dead-zone/drag-adjust exclusions
         * as player-swipe: a slider drag must never become a back-swipe.
         * Lyrics is NOT excluded -- it now uses this same live-tracking
         * swipe-back (reveal-style, exiting to Player) instead of its own
         * lyrics_gesture_event_cb()'s plain nav_pop(); that handler still
         * exists for the auto-close-on-track-change-with-no-lyrics path,
         * but stands down for a user swipe once this candidate owns the
         * press (see gui_shell_back_swipe_owns_press()). */
        back_swipe.candidate = !quick_drawer_drag_tracking && !quick_drawer_open &&
                                gui_navigation_get_depth() > 1 &&
                                !shell_screen_blocks_swipe(lv_screen_active()) &&
                                !gui_library_navigation_blocked() &&
                                !player_swipe_press_excluded(p);
        back_swipe.start_x = p.x;
        back_swipe.start_y = p.y;
        back_swipe.tracking = false;
        back_swipe_owns_press = back_swipe.candidate;
        DB_LOG("GESTURE", "back_swipe.candidate=%d screen=%s depth=%d",
               back_swipe.candidate,
               lv_screen_active() == gui_library_get_files_screen() ? "files" : "other",
               gui_navigation_get_depth());
#ifdef UI_PERF_TRACE
        ui_frame_trace_event(UI_FRAME_TRACE_EVENT_GESTURE_ORIGIN,
                             ((uint32_t)(uint16_t)p.x << 16) | (uint16_t)p.y,
                             (player_swipe.candidate ? 1u : 0u) |
                             (back_swipe.candidate ? 2u : 0u) |
                             (home_swipe.candidate ? 4u : 0u) |
                             (quick_drawer_drag_tracking ? 8u : 0u));
#endif
    }

    if (pressed && home_swipe.candidate && !home_swipe.tracking && !player_swipe.tracking && !back_swipe.tracking &&
        !transition_compositor_is_active()) {
        int32_t dx, dy, adx, ady;
        if (swipe_deadzone_exceeded(&home_swipe, p, &dx, &dy, &adx, &ady)) {
            if (dy < 0 && ady > adx) {
                /* EXPERIMENTAL reveal=true (see gui_navigation.h's own
                 * comment on slide_transition_ctx_t's reveal field): Home
                 * stays static, uncovered as the current screen slides up
                 * over it, instead of both panels moving together. A/B
                 * test against the two-panel style back-swipe/player-swipe
                 * still use -- not yet settled as the final behavior. */
                home_swipe.ctx = begin_slide_transition_ex(gui_shell_get_home_screen(), true, true, true);
                if (home_swipe.ctx) {
                    /* No navigation decision exists until release. A
                     * compositor failure during the live drag therefore
                     * recovers to from_scr and leaves the stack untouched. */
                    home_swipe.ctx->commit = false;
                    home_swipe.tracking = true;
                    home_swipe.just_confirmed = true;
                    home_swipe.last_v = 0;
                    home_swipe.last_velocity = 0;
#ifdef UI_GESTURE_TRACE
                    printf("[GESTURE_TRACE] poll: home_swipe TRIGGERED (start_y=%d, cur_y=%d)\n",
                           (int)home_swipe.start_y, (int)p.y);
#endif
                    lv_indev_wait_release(indev);
                }
            } else if (adx > ady && !lv_indev_get_scroll_obj(indev)) {
                /* Same non-scrollable drag tap suppression as player-swipe/
                 * back-swipe, mirrored for a horizontal drag ruling out a
                 * vertical home-swipe. */
                lv_indev_wait_release(indev);
            }
            home_swipe.candidate = false;
        }
    }

    if (pressed && player_swipe.candidate && !player_swipe.tracking) {
        int32_t dx, dy, adx, ady;
        if (swipe_deadzone_exceeded(&player_swipe, p, &dx, &dy, &adx, &ady)) {
            /* Enough movement to judge direction. Horizontal-left-dominant
             * confirms it; anything else (vertical, or rightward) rules it
             * out for good -- either way, stop re-checking every tick. */
            if (dx < 0 && adx > ady) {
                player_swipe.ctx = begin_slide_transition(gui_player_get_screen(), true); /* see begin_slide_transition()'s own comment -- both sources are always owned copies now */
                if (player_swipe.ctx) {
                    /* No navigation decision exists until release. A
                     * compositor failure during the live drag therefore
                     * recovers to from_scr and leaves the stack untouched. */
                    player_swipe.ctx->commit = false;
                    player_swipe.tracking = true;
                    player_swipe.just_confirmed = true;
                    player_swipe.last_v = 0;
                    player_swipe.last_velocity = 0;
                    /* Same reasoning as nav_pop()'s own lv_indev_wait_release()
                     * call -- the overlay just created sits directly under
                     * this still-down finger, and without this, the eventual
                     * release would hit whatever's now underneath at that
                     * coordinate instead (a real screen swap mid-press, same
                     * PRESS_LOST-adjacent class of bug already found and
                     * fixed once for the drawer's own icons). */
                    lv_indev_wait_release(indev);
                }
            } else if (ady > adx && !lv_indev_get_scroll_obj(indev)) {
                /* If vertical drag exceeds deadzone on a non-scrollable screen,
                 * suppress the pending tap via lv_indev_wait_release() to prevent
                 * accidental row activation when an attempted scroll cannot occur. */
                lv_indev_wait_release(indev);
            }
            player_swipe.candidate = false;
        }
    }

    if (pressed && back_swipe.candidate && !back_swipe.tracking && !player_swipe.tracking) {
        int32_t dx, dy, adx, ady;
        if (swipe_deadzone_exceeded(&back_swipe, p, &dx, &dy, &adx, &ady)) {
            if (dx > 0 && adx > ady) {
                lv_obj_t * active = lv_screen_active();
                /* Search-open and Files-not-at-root consume a right-swipe
                 * as in-screen back (close the bar / step up a directory)
                 * rather than a stack pop. Dispatch those here so this
                 * path cannot steal them into a slide; wait_release so
                 * the still-down finger cannot also fire
                 * screen_gesture_event_cb's leftover fallback (which
                 * would then nav_pop after search already closed). */
                bool consumed_in_place = search_close_if_active_for_screen(active) ||
                                         file_browser_go_up_for_screen(active);
                DB_LOG("GESTURE", "back_swipe confirm screen=%s in_place=%d",
                       active == gui_library_get_files_screen() ? "files" : "other", consumed_in_place);
                if (consumed_in_place) {
                    lv_indev_wait_release(indev);
                } else {
                    back_swipe_target_scr = gui_navigation_get_screen_at(gui_navigation_get_depth() - 2);
                    if (back_swipe_target_scr) {
                        /* Reveal-style everywhere except leaving the Player
                         * screen itself, which keeps the original two-panel
                         * slide -- see local/notes/development/ISSUES.md's swipe-up-to-Home to-do
                         * entry for the same reveal field, still an active
                         * A/B test rather than settled for every gesture. */
                        bool reveal = active != gui_player_get_screen();
                        back_swipe.ctx = begin_slide_transition_ex(back_swipe_target_scr, false, false, reveal);
                        DB_LOG("GESTURE", "back_swipe slide target=%p ctx=%p",
                               (void *) back_swipe_target_scr, (void *) back_swipe.ctx);
                        if (back_swipe.ctx) {
                            /* No navigation decision exists until release.
                             * A compositor failure during the live drag
                             * therefore recovers to from_scr and leaves
                             * the stack untouched. */
                            back_swipe.ctx->commit = false;
                            back_swipe.tracking = true;
                            back_swipe.just_confirmed = true;
                            back_swipe.last_v = 0;
                            back_swipe.last_velocity = 0;
                            lv_indev_wait_release(indev);
                        }
                    }
                }
            } else if (ady > adx && !lv_indev_get_scroll_obj(indev)) {
                /* Same non-scrollable vertical-drag tap suppression as
                 * player-swipe. Harmless if that path already called
                 * wait_release on this press. */
                lv_indev_wait_release(indev);
            }
            back_swipe.candidate = false;
        }
    }

    if (pressed && home_swipe.tracking) {
        int32_t v = p.y - home_swipe.start_y;
        if (v > 0) v = 0;  /* never past fully-closed (finger drifting back down just holds at 0) */
        if (v < -h) v = -h; /* never past fully-off (finger overshooting up of a full screen height) */
        swipe_track_update(&home_swipe, v);
    }

    if (pressed && player_swipe.tracking) {
        int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
        int32_t v = p.x - player_swipe.start_x;
        if (v > 0) v = 0;   /* never past fully-open (finger drifting back right of the start point just holds at 0) */
        if (v < -w) v = -w; /* never past fully-off (finger overshooting left of a full screen width) */
        swipe_track_update(&player_swipe, v);
    }

    if (pressed && back_swipe.tracking) {
        int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
        int32_t v = p.x - back_swipe.start_x;
        if (v < 0) v = 0;  /* never past fully-closed (finger drifting back left of the start point just holds at 0) */
        if (v > w) v = w;  /* never past fully-off (finger overshooting right of a full screen width) */
        swipe_track_update(&back_swipe, v);
    }

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

    if (pressed && quick_drawer_drag_tracking) {
        /* Deadzone before moving panel: suppresses minor jitter during taps
         * and long-presses so child widgets (icons) do not receive PRESS_LOST.
         * Beyond QUICK_DRAWER_DRAG_DEADZONE (10px), the deadzone is subtracted
         * so drag motion starts smoothly from zero. */
        int32_t raw_delta = p.y - quick_drawer_drag_touch_start_y;
        if (raw_delta > QUICK_DRAWER_DRAG_DEADZONE || raw_delta < -QUICK_DRAWER_DRAG_DEADZONE) {
            int32_t adjusted_delta = raw_delta > 0 ? raw_delta - QUICK_DRAWER_DRAG_DEADZONE
                                                    : raw_delta + QUICK_DRAWER_DRAG_DEADZONE;
            int32_t new_y = quick_drawer_drag_panel_start_y + adjusted_delta;
            if (new_y > 0) new_y = 0;
            if (new_y < -h) new_y = -h;
            /* Past the deadzone this is a drag, not a tap. The live drawer
             * does not cover the list while opening (it starts off-screen),
             * and bitmap motion hides the real panel behind a snapshot --
             * without wait_release(), LVGL re-hit-tests the still-down
             * finger onto whatever row is now underneath and fires CLICKED
             * on release. Same tool as the player-swipe path above. */
            if (!quick_drawer_drag_claimed) {
#ifdef UI_PERF_TRACE
                ui_frame_trace_event(UI_FRAME_TRACE_EVENT_DRAWER_CLAIM, 0, 0);
#endif
                quick_drawer_drag_claimed = true;
                lv_obj_move_foreground(quick_drawer);
                lv_obj_move_foreground(status_bar_band);
                lv_indev_wait_release(indev);
            }
            /* Per-tick velocity, in case the finger lifts mid-flick (see the
             * release branch below) -- a plain position delta rather than
             * lv_indev_get_vect() so it's driven by the exact same samples
             * this function already reads, not a second/possibly-
             * differently-timed source. */
            if (!quick_drawer_bitmap_motion) quick_drawer_begin_bitmap_motion();
            /* Sticky velocity -- see home_swipe's own comment on this same
             * pattern above. A zero-delta tick (finger momentarily still, or
             * settled back within the deadzone below) must not clobber the
             * last real direction, since a deliberate slow drag naturally
             * decelerates to near-zero right before it lifts. */
            int32_t quick_drawer_delta = new_y - quick_drawer_motion_y();
            if (quick_drawer_delta != 0) quick_drawer_last_velocity = quick_drawer_delta;
            quick_drawer_anim_y_cb(quick_drawer, new_y);
        }
    }

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

    if (!pressed && quick_drawer_was_pressed && home_swipe.tracking) {
        home_swipe.tracking = false;
        int32_t current_v = home_swipe.last_v;
        bool commit = swipe_release_commit(&home_swipe, -1, h); /* committing moves up, toward Home */
        home_swipe.ctx->commit = commit;
        if (commit) {
            /* Stack-only bookkeeping -- actual lv_screen_load() waits for
             * slide_transition_done_cb(). Cancel leaves the stack
             * untouched, matching player-swipe/back-swipe's own cancel path. */
            nav_reset_to_home_stack_only();
        }
        swipe_settle(home_swipe.ctx, current_v, commit ? -h : 0);
        home_swipe.ctx = NULL;
    }

    if (!pressed && quick_drawer_was_pressed && player_swipe.tracking) {
        /* Finger lifted mid-swipe. Same direction-vs-halfway decision as the
         * drawer's own release logic just above, just horizontal. */
        player_swipe.tracking = false;
        int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
        /* player_swipe.last_v stores the last sampled offset, updated every
         * tracking tick regardless of whether that tick actually presented
         * a frame (see player_swipe.just_confirmed). Reads it directly
         * rather than inspecting img_from, which is NULL when direct-
         * framebuffer compositing is active. */
        int32_t current_v = player_swipe.last_v;
        bool commit = swipe_release_commit(&player_swipe, -1, w); /* committing moves left, toward Player */
        player_swipe.ctx->commit = commit;
        if (commit) {
            /* Stack-only bookkeeping -- actual lv_screen_load() is deferred
             * until slide_transition_done_cb() runs when the settle animation
             * completes. nav_push_stack_only() updates the nav stack without
             * prematurely triggering screen load events. */
            if (!gui_navigation_is_top(gui_player_get_screen())) {
                nav_push_stack_only(gui_player_get_screen());
            }
        }
        swipe_settle(player_swipe.ctx, current_v, commit ? -w : 0);
        player_swipe.ctx = NULL;
    }

    if (!pressed && quick_drawer_was_pressed && back_swipe.tracking) {
        back_swipe.tracking = false;
        int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
        int32_t current_v = back_swipe.last_v;
        bool commit = swipe_release_commit(&back_swipe, 1, w); /* committing moves right, toward Back */
        back_swipe.ctx->commit = commit;
        if (commit) {
            /* Stack-only bookkeeping -- actual lv_screen_load() waits for
             * slide_transition_done_cb(). Cancel leaves the stack
             * untouched, matching player-swipe's own cancel path. */
            nav_pop_stack_only();
            /* Lyrics' own timer/backdrop teardown, normally done by
             * close_lyrics_screen() -- skipped entirely on cancel, since a
             * cancelled swipe leaves the user back on Lyrics with both
             * still needed. */
            if (back_swipe.ctx->from_scr == gui_lyrics_get_screen()) gui_lyrics_prepare_exit();
        }
        swipe_settle(back_swipe.ctx, current_v, commit ? w : 0);
        back_swipe.ctx = NULL;
        back_swipe_target_scr = NULL;
    }

    if (!pressed && quick_drawer_was_pressed) {
#ifdef UI_GESTURE_TRACE
        printf("[GESTURE_TRACE] poll: release observed (home_tracking=%d, drawer_tracking=%d, player_tracking=%d, back_tracking=%d)\n",
               home_swipe.tracking, quick_drawer_drag_tracking, player_swipe.tracking, back_swipe.tracking);
#endif
        player_swipe.candidate = false;
        back_swipe.candidate = false;
        back_swipe_owns_press = false;
        home_swipe.candidate = false;
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

    swipe_clear_flags(&home_swipe);
    if (home_swipe.ctx) {
        slide_transition_cancel(&home_swipe.ctx);
    }

    /* Cancel any active drawer animation or motion and restore deterministic closed state */
    lv_anim_delete(quick_drawer, quick_drawer_anim_y_cb);
    if (quick_drawer_bitmap_motion || quick_drawer_direct_motion) {
        quick_drawer_open = false;
        quick_drawer_finish_bitmap_motion();
    }

    swipe_clear_flags(&player_swipe);
    if (player_swipe.ctx) {
        slide_transition_cancel(&player_swipe.ctx);
    }
    swipe_clear_flags(&back_swipe);
    back_swipe_owns_press = false;
    back_swipe_target_scr = NULL;
    if (back_swipe.ctx) {
        slide_transition_cancel(&back_swipe.ctx);
    }
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
void gui_shell_player_swipe_recover(void * ctx) {
    slide_transition_ctx_t * sctx = (slide_transition_ctx_t *) ctx;
    if (sctx == player_swipe.ctx) player_swipe.ctx = NULL;
    swipe_clear_flags(&player_swipe);
    if (sctx == back_swipe.ctx) back_swipe.ctx = NULL;
    swipe_clear_flags(&back_swipe);
    back_swipe_owns_press = false;
    back_swipe_target_scr = NULL;
    /* Vertical Home gestures share the same compositor and recovery path
     * as horizontal player/back gestures. Clear the owner before the
     * navigation layer frees a failed transition context. */
    if (sctx == home_swipe.ctx) home_swipe.ctx = NULL;
    swipe_clear_flags(&home_swipe);
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
    if (home_swipe.candidate || home_swipe.tracking || home_swipe.just_confirmed ||
        player_swipe.candidate || player_swipe.tracking || player_swipe.just_confirmed ||
        back_swipe.candidate || back_swipe_owns_press || back_swipe.tracking ||
        back_swipe.just_confirmed) return true;
    if (!quick_drawer) return false;
    int32_t height = lv_display_get_vertical_resolution(lv_display_get_default());
    return height > 0 && quick_drawer_motion_y() > -height;
}
