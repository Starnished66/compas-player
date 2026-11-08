#include "gui_shell_internal.h"

/* Swipe-up-to-Home, swipe-left-to-Player and swipe-right-to-Back live
 * tracking. gui_shell_gestures.c owns the raw-touch poll timer and calls the
 * per-stage entry points below in its fixed per-tick order. */

swipe_state_t home_swipe, player_swipe, back_swipe;
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
bool back_swipe_owns_press = false;
static lv_obj_t * back_swipe_target_scr = NULL;
bool gui_shell_back_swipe_owns_press(void) {
    return back_swipe_owns_press;
}

/* Screens that own their own exit (lock, text entry/view, busy, USB DAC
 * overlay): no swipe-back and no navigation prefetch from them. */
bool shell_screen_blocks_swipe(lv_obj_t * screen) {
    return screen == gui_lock_screen_get_screen() || screen == gui_text_input_get_screen() ||
           screen == gui_text_view_get_screen() ||
           (screen == gui_busy_get_screen() && !gui_busy_is_cancellable()) ||
           screen == gui_network_get_usb_dac_overlay();
}

/* Arms a swipe for a new press at p; it stays provisional until the
 * deadzone confirms or rules it out. */
static void swipe_arm(swipe_state_t * s, bool candidate, lv_point_t p) {
    s->candidate = candidate;
    s->start_x = p.x;
    s->start_y = p.y;
    s->tracking = false;
}

/* The compositor overlay exists: start following the finger from offset 0.
 * No navigation decision exists until release, so a compositor failure
 * during the live drag recovers to from_scr and leaves the stack untouched. */
static void swipe_begin_tracking(swipe_state_t * s) {
    s->ctx->commit = false;
    s->tracking = true;
    s->just_confirmed = true;
    s->last_v = 0;
    s->last_velocity = 0;
}

/* gesture_home_config_t for a press-down at screen height h. */
static void home_swipe_fill_config(gesture_home_config_t * home_cfg, int32_t h) {
    home_cfg->swipe_up_home_enabled = current_settings.swipe_up_home_enabled;
    home_cfg->quick_drawer_open = quick_drawer_open;
    home_cfg->is_usb_dac_overlay = (lv_screen_active() == gui_network_get_usb_dac_overlay());
    home_cfg->is_lyrics_screen = (lv_screen_active() == gui_lyrics_get_screen());
    home_cfg->is_lock_screen = (lv_screen_active() == gui_lock_screen_get_screen());
    home_cfg->has_background_work = gui_library_navigation_blocked();
    home_cfg->screen_height = h;
    /* Slightly expand only the raw press-down target. The overlay band and
     * its visible pill retain their existing dimensions. */
    home_cfg->band_height = HOME_INDICATOR_BAND_HEIGHT + HOME_SWIPE_HIT_EXTRA_PX;
}

/* Home candidate: gesture_home_config_t's fields are shared with
 * gui_lock_screen.c's own independent use of gesture_home_state_is_eligible(),
 * which doesn't need these -- text-entry and the busy overlay skip
 * finalize_screen_navigation() because leaving them needs teardown
 * (in-progress input, modal ownership) that a stack-only reset to Home would
 * bypass.
 *
 * Deliberately NOT gated on drag_adjust_press_owned (unlike player-
 * swipe/back-swipe): home_indicator_band already sits on lv_layer_top(),
 * CLICKABLE, sized to this exact band, so nothing on the active screen can
 * genuinely receive a press inside this strip. point_in_swipe_dead_zone() is
 * purely coordinate-based with no z-order awareness, so it was vetoing this
 * gesture whenever some OTHER, actually-covered widget's registered dead zone
 * happened to overlap this band's coordinates -- e.g. a bottom-aligned EQ
 * screen or a plugin slider card extending into the last ~31px of the
 * screen. Keep this ungated so the indicator band's own hitbox keeps real
 * priority over whatever is positioned underneath it. */
static bool home_swipe_candidate_at(lv_point_t p, int32_t h) {
    gesture_home_config_t home_cfg;
    home_swipe_fill_config(&home_cfg, h);
    return gesture_home_state_is_eligible(&home_cfg, p.y) &&
           lv_screen_active() != gui_shell_get_home_screen() &&
           lv_screen_active() != gui_text_input_get_screen() &&
           lv_screen_active() != gui_busy_get_screen();
}

/* Player-swipe: eligible unless claimed by the drawer drag, the drawer is
 * open, the player screen is already active, or the press started on a
 * drag-adjust widget/dead-zone. Excluded on lyrics, track info, lock screen,
 * both DAC mode overlays (leaving them is only by closing DAC mode), and
 * while library navigation is blocked. Candidate only until sufficient
 * displacement accumulates to determine gesture direction. */
static bool player_swipe_candidate_at(lv_point_t p) {
    return !quick_drawer_drag_tracking && !quick_drawer_open &&
           lv_screen_active() != gui_player_get_screen() &&
           lv_screen_active() != gui_lyrics_get_screen() &&
           lv_screen_active() != gui_track_info_get_screen() &&
           lv_screen_active() != gui_lock_screen_get_screen() &&
           lv_screen_active() != gui_text_view_get_screen() &&
           lv_screen_active() != gui_network_get_usb_dac_overlay() &&
           !gui_library_navigation_blocked() &&
           !player_swipe_press_excluded(p);
}

/* Back candidate: depth > 1 is the stack-pop precondition. Lyrics owns its
 * own right-swipe (lyrics_gesture_event_cb -> close_lyrics_screen). Lock,
 * text-entry, USB DAC overlay, and the busy overlay skip
 * finalize_screen_navigation() -- a live pop here would bypass their
 * leave-confirmation, teardown, or modal-ownership. Same drawer/dead-zone/
 * drag-adjust exclusions as player-swipe: a slider drag must never become a
 * back-swipe. Lyrics is NOT excluded -- it now uses this same live-tracking
 * swipe-back (reveal-style, exiting to Player) instead of its own
 * lyrics_gesture_event_cb()'s plain nav_pop(); that handler still exists for
 * the auto-close-on-track-change-with-no-lyrics path, but stands down for a
 * user swipe once this candidate owns the press (see
 * gui_shell_back_swipe_owns_press()). */
static bool back_swipe_candidate_at(lv_point_t p) {
    return !quick_drawer_drag_tracking && !quick_drawer_open &&
           gui_navigation_get_depth() > 1 &&
           !shell_screen_blocks_swipe(lv_screen_active()) &&
           !gui_library_navigation_blocked() &&
           !player_swipe_press_excluded(p);
}

/* Press-down stage: home, player and back swipe candidates. */
void gui_shell_swipes_poll_press(lv_point_t p, int32_t h) {
    swipe_arm(&home_swipe, home_swipe_candidate_at(p, h), p);

#ifdef UI_GESTURE_TRACE
    printf("[GESTURE_TRACE] poll: press-down at (%d, %d), res_h=%d\n", (int)p.x, (int)p.y, (int)h);
    printf("[GESTURE_TRACE] poll: home_swipe eval: enabled=%d, tracking=%d\n",
           current_settings.swipe_up_home_enabled, home_swipe.candidate);
#endif

    swipe_arm(&player_swipe, player_swipe_candidate_at(p), p);
    swipe_arm(&back_swipe, back_swipe_candidate_at(p), p);
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

/* Deadzone stage: confirm or rule out the home (swipe up) gesture. */
static void home_swipe_poll_deadzone(lv_indev_t * indev, lv_point_t p, bool pressed) {
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
                    swipe_begin_tracking(&home_swipe);
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
}

/* Deadzone stage: confirm or rule out the player (swipe left) gesture. */
static void player_swipe_poll_deadzone(lv_indev_t * indev, lv_point_t p, bool pressed) {
    if (pressed && player_swipe.candidate && !player_swipe.tracking) {
        int32_t dx, dy, adx, ady;
        if (swipe_deadzone_exceeded(&player_swipe, p, &dx, &dy, &adx, &ady)) {
            /* Enough movement to judge direction. Horizontal-left-dominant
             * confirms it; anything else (vertical, or rightward) rules it
             * out for good -- either way, stop re-checking every tick. */
            if (dx < 0 && adx > ady) {
                player_swipe.ctx = begin_slide_transition(gui_player_get_screen(), true); /* see begin_slide_transition()'s own comment -- both sources are always owned copies now */
                if (player_swipe.ctx) {
                    swipe_begin_tracking(&player_swipe);
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
}

/* Starts the live back slide to the screen below the active one. */
static void back_swipe_begin_slide(lv_indev_t * indev, lv_obj_t * active) {
    back_swipe_target_scr = gui_navigation_get_screen_at(gui_navigation_get_depth() - 2);
    if (!back_swipe_target_scr) return;
    /* Reveal-style everywhere except leaving the Player screen itself, which
     * keeps the original two-panel slide -- see
     * local/notes/development/ISSUES.md's swipe-up-to-Home to-do entry for
     * the same reveal field, still an active A/B test rather than settled for
     * every gesture. */
    bool reveal = active != gui_player_get_screen();
    back_swipe.ctx = begin_slide_transition_ex(back_swipe_target_scr, false, false, reveal);
    DB_LOG("GESTURE", "back_swipe slide target=%p ctx=%p",
           (void *) back_swipe_target_scr, (void *) back_swipe.ctx);
    if (back_swipe.ctx) {
        swipe_begin_tracking(&back_swipe);
        lv_indev_wait_release(indev);
    }
}

/* A confirmed rightward drag. Search-open and Files-not-at-root consume a
 * right-swipe as in-screen back (close the bar / step up a directory) rather
 * than a stack pop. Dispatch those here so this path cannot steal them into a
 * slide; wait_release so the still-down finger cannot also fire
 * screen_gesture_event_cb's leftover fallback (which would then nav_pop after
 * search already closed). */
static void back_swipe_confirm(lv_indev_t * indev) {
    lv_obj_t * active = lv_screen_active();
    bool consumed_in_place = search_close_if_active_for_screen(active) ||
                             file_browser_go_up_for_screen(active);
    DB_LOG("GESTURE", "back_swipe confirm screen=%s in_place=%d",
           active == gui_library_get_files_screen() ? "files" : "other", consumed_in_place);
    if (consumed_in_place) lv_indev_wait_release(indev);
    else back_swipe_begin_slide(indev, active);
}

/* Deadzone stage: confirm or rule out the back (swipe right) gesture. */
static void back_swipe_poll_deadzone(lv_indev_t * indev, lv_point_t p, bool pressed) {
    if (pressed && back_swipe.candidate && !back_swipe.tracking && !player_swipe.tracking) {
        int32_t dx, dy, adx, ady;
        if (swipe_deadzone_exceeded(&back_swipe, p, &dx, &dy, &adx, &ady)) {
            if (dx > 0 && adx > ady) {
                back_swipe_confirm(indev);
            } else if (ady > adx && !lv_indev_get_scroll_obj(indev)) {
                /* Same non-scrollable vertical-drag tap suppression as
                 * player-swipe. Harmless if that path already called
                 * wait_release on this press. */
                lv_indev_wait_release(indev);
            }
            back_swipe.candidate = false;
        }
    }
}

/* Deadzone stages, in home, player, back order. */
void gui_shell_swipes_poll_deadzones(lv_indev_t * indev, lv_point_t p, bool pressed) {
    home_swipe_poll_deadzone(indev, p, pressed);
    player_swipe_poll_deadzone(indev, p, pressed);
    back_swipe_poll_deadzone(indev, p, pressed);
}

/* Tracking stage: feed the finger offset to the live home/player/back swipes. */
void gui_shell_swipes_poll_tracking(lv_point_t p, int32_t h, bool pressed) {
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
}

/* Release stage: home swipe commit or cancel. */
static void home_swipe_poll_release(int32_t h, bool pressed) {
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
}

/* Release stage: player swipe commit or cancel. */
static void player_swipe_poll_release(bool pressed) {
    if (!pressed && quick_drawer_was_pressed && player_swipe.tracking) {
        /* Finger lifted mid-swipe. Same direction-vs-halfway decision as the
         * drawer's own release logic, just horizontal. */
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
}

/* Release stage: back swipe commit or cancel. */
static void back_swipe_poll_release(bool pressed) {
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
}

/* Release stages, in home, player, back order. */
void gui_shell_swipes_poll_release(int32_t h, bool pressed) {
    home_swipe_poll_release(h, pressed);
    player_swipe_poll_release(pressed);
    back_swipe_poll_release(pressed);
}

/* Release observed: no swipe candidate or press ownership survives it. */
void gui_shell_swipes_clear_candidates(void) {
    player_swipe.candidate = false;
    back_swipe.candidate = false;
    back_swipe_owns_press = false;
    home_swipe.candidate = false;
}

/* Reset, home part: clear the flags and cancel its live overlay. */
void gui_shell_swipes_reset_home(void) {
    swipe_clear_flags(&home_swipe);
    if (home_swipe.ctx) {
        slide_transition_cancel(&home_swipe.ctx);
    }
}

/* Reset, player and back part (runs after the drawer motion is settled). */
void gui_shell_swipes_reset_player_back(void) {
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

/* True while any swipe is a candidate, tracking or just confirmed. */
bool gui_shell_swipes_busy(void) {
    return home_swipe.candidate || home_swipe.tracking || home_swipe.just_confirmed ||
           player_swipe.candidate || player_swipe.tracking || player_swipe.just_confirmed ||
           back_swipe.candidate || back_swipe_owns_press || back_swipe.tracking ||
           back_swipe.just_confirmed;
}
