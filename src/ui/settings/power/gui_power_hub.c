#include "gui_power_hub.h"

#include "../../../hardware/indicators/backlight.h"
#include "../../../core/updates/firmware_update.h"
#include "../../effects/frosted_glass.h"
#include "../../shell/gui_notifications.h"
#include "../gui_settings.h"
#include "../../shell/gui_shell.h"
#include "../../appearance/gui_theme.h"
#include "../../text/i18n/i18n.h"
#include "../../../core/system/lifecycle/idle_shutdown.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../effects/transition_compositor.h"
#include "../../diagnostics/ui_frame_trace.h"
#include "src/draw/lv_draw_buf.h"
#include "src/draw/snapshot/lv_snapshot.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Power Hub -- shown by a physical power-button hold over any screen.
 * Built once on lv_layer_sys() and shown/hidden as a modal, with explicit
 * confirmation for shutdown/restart and shortcuts for display and timer. */
static gui_popup_t power_action_overlay;
static lv_obj_t * power_action_blur_img;
static uint8_t * power_action_blur_bytes;
static lv_image_dsc_t power_action_blur_dsc;
static lv_obj_t * power_action_status;
/* Destructive actions require a second, explicit tap. A dismissed/reopened
 * menu never retains an armed action. */
static int power_action_pending = -1;
static bool power_restart_selector_long_press_fired;

#define POWER_OVERLAY_BLUR_WORK_WIDTH 60
#define POWER_OVERLAY_BLUR_WORK_HEIGHT 100
#define POWER_OVERLAY_BLUR_RADIUS 5
#define POWER_OVERLAY_BLUR_PASSES 3
#define POWER_OVERLAY_DARKEN_NUM 3
#define POWER_OVERLAY_DARKEN_DEN 5
#define POWER_HUB_ENTRANCE_MS 320

static lv_obj_t * power_action_motion_img;
static lv_draw_buf_t * power_action_motion_frame;
static lv_timer_t * power_action_start_timer;
static bool power_action_direct_motion;

#ifdef UI_PERF_TRACE
static uint64_t power_action_last_frame_us;
#endif

static void power_action_motion_exec(void * obj, int32_t y) {
#ifdef UI_PERF_TRACE
    uint64_t now_us = ui_perf_now_us();
    printf("PERF power slide y=%d gap_us=%llu\n", (int) y,
           (unsigned long long) (power_action_last_frame_us ? now_us - power_action_last_frame_us : 0));
    power_action_last_frame_us = now_us;
#endif
    lv_obj_set_y(obj, y);
    if (power_action_direct_motion) {
        if (transition_compositor_bottom_sheet_frame(y)) return;
        /* Failed presentation ends its compositor session. Continue through
         * the prepared LVGL image at the same position. */
        power_action_direct_motion = false;
        lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
        lv_obj_invalidate(obj);
    }
}

static void power_action_slide_exec(void * obj, int32_t offset) {
    /* A translation preserves the sheet's bottom alignment and flex layout. */
    lv_obj_set_style_translate_y(obj, offset, 0);
}

/* Motion art is deliberately transient: drop LVGL's decoded-image cache
 * before releasing the caller-owned snapshot buffer. */
static void power_action_motion_cleanup(bool restore_panel, bool cancel_motion_anim) {
    if (power_action_direct_motion) {
        transition_compositor_end();
        power_action_direct_motion = false;
    }
    if (power_action_start_timer) {
        lv_timer_delete(power_action_start_timer);
        power_action_start_timer = NULL;
    }
    if (cancel_motion_anim && power_action_motion_img)
        lv_anim_delete(power_action_motion_img, power_action_motion_exec);
    if (power_action_overlay.popup)
        lv_anim_delete(power_action_overlay.popup, power_action_slide_exec);
    if (power_action_motion_img) {
        lv_image_cache_drop(power_action_motion_frame);
        lv_obj_delete(power_action_motion_img);
        power_action_motion_img = NULL;
    }
    if (power_action_motion_frame) {
        lv_draw_buf_destroy(power_action_motion_frame);
        power_action_motion_frame = NULL;
    }
    if (power_action_overlay.popup) {
        lv_obj_set_style_translate_y(power_action_overlay.popup, 0, 0);
        if (restore_panel) lv_obj_remove_flag(power_action_overlay.popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void power_action_motion_done(lv_anim_t * anim) {
    (void) anim;
    power_action_motion_cleanup(true, false);
}

/* Preparation runs with the main loop's frozen tick. Starting on a later
 * loop prevents blur/snapshot time from consuming the entrance duration. */
static void power_action_start_animation(lv_timer_t * timer) {
    lv_timer_delete(timer);
    power_action_start_timer = NULL;
    lv_obj_t * panel = power_action_overlay.popup;
    if (!panel || !gui_popup_is_visible(&power_action_overlay)) return;
#ifdef UI_PERF_TRACE
    power_action_last_frame_us = 0;
#endif
    lv_anim_t anim;
    lv_anim_init(&anim);
    if (power_action_direct_motion) lv_anim_set_early_apply(&anim, false);
    if (power_action_motion_img) {
        lv_anim_set_var(&anim, power_action_motion_img);
        lv_anim_set_exec_cb(&anim, power_action_motion_exec);
        lv_anim_set_values(&anim, lv_obj_get_y(power_action_motion_img), lv_obj_get_y(panel));
        lv_anim_set_completed_cb(&anim, power_action_motion_done);
    } else {
        lv_anim_set_var(&anim, panel);
        lv_anim_set_exec_cb(&anim, power_action_slide_exec);
        lv_anim_set_values(&anim, lv_obj_get_height(panel), 0);
    }
    lv_anim_set_duration(&anim, gui_anim_ms(POWER_HUB_ENTRANCE_MS));
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    if (!lv_anim_start(&anim)) power_action_motion_cleanup(true, true);
}

static void power_action_begin_slide(lv_timer_t * timer) {
    lv_timer_delete(timer);
    power_action_start_timer = NULL;
    if (!power_action_overlay.popup || !gui_popup_is_visible(&power_action_overlay)) return;
    bool use_direct_motion = true;
#ifdef UI_PERF_TRACE
    /* Diagnostic comparison uses the same binary and preparation path. */
    use_direct_motion = getenv("COMPAS_POWER_SLIDE_LVGL") == NULL;
#endif
    if (use_direct_motion && power_action_motion_img &&
        transition_compositor_begin_bottom_sheet(power_action_motion_frame)) {
        power_action_direct_motion = true;
        /* Keep the moving hit target without asking LVGL to draw the art. */
        lv_obj_set_style_opa(power_action_motion_img, LV_OPA_TRANSP, 0);
        if (!transition_compositor_bottom_sheet_frame(lv_obj_get_y(power_action_motion_img))) {
            power_action_direct_motion = false;
            lv_obj_set_style_opa(power_action_motion_img, LV_OPA_COVER, 0);
        }
    }

    /* Pixel conversion and the first vsync can be expensive. Refresh the
     * loop tick once more before measuring the animation's duration. */
    power_action_start_timer = lv_timer_create(power_action_start_animation, 1, NULL);
    if (!power_action_start_timer) power_action_motion_cleanup(true, true);
}

static void power_action_hide(void) {
    power_action_motion_cleanup(true, true);
    gui_popup_hide(&power_action_overlay);
    power_action_pending = -1;
}

static void power_action_show(void) {
    lv_obj_t * panel = power_action_overlay.popup;
    power_action_motion_cleanup(true, true);
    if (!panel) {
        gui_popup_show(&power_action_overlay);
        return;
    }
    if (gui_anims_off()) {
        gui_popup_show(&power_action_overlay);
        return;
    }
    lv_anim_delete(panel, power_action_slide_exec);
    gui_popup_show(&power_action_overlay);
    lv_obj_update_layout(panel);
    int32_t width = lv_obj_get_width(panel);
    int32_t height = lv_obj_get_height(panel);
    if (width <= 0 || height <= 0 || width > BOARD_SCREEN_WIDTH || height > BOARD_SCREEN_HEIGHT) return;

    power_action_motion_frame = lv_snapshot_take(panel, LV_COLOR_FORMAT_ARGB8888);
    if (power_action_motion_frame &&
        (power_action_motion_frame->header.w != (uint32_t) width ||
         power_action_motion_frame->header.h != (uint32_t) height)) {
        lv_draw_buf_destroy(power_action_motion_frame);
        power_action_motion_frame = NULL;
    }
    if (!power_action_motion_frame) goto live_slide;

    power_action_motion_img = lv_image_create(power_action_overlay.backdrop);
    if (!power_action_motion_img) {
        lv_draw_buf_destroy(power_action_motion_frame);
        power_action_motion_frame = NULL;
        goto live_slide;
    }
    lv_image_set_src(power_action_motion_img, power_action_motion_frame);
    lv_obj_set_size(power_action_motion_img, width, height);
    lv_obj_set_pos(power_action_motion_img, lv_obj_get_x(panel), lv_obj_get_y(panel) + height);
    lv_obj_remove_flag(power_action_motion_img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    /* Intercept taps over the moving sheet so action cards cannot click
     * through to the hidden live panel. The uncovered backdrop still cancels. */
    lv_obj_add_flag(power_action_motion_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

    goto schedule_slide;

live_slide:
    /* Allocation failure retains the live slide without snapshot storage. */
    lv_obj_set_style_translate_y(panel, height, 0);

schedule_slide:
    /* Do not mark this timer ready: it must wait for a fresh loop tick. */
    power_action_start_timer = lv_timer_create(power_action_begin_slide, 1, NULL);
    if (!power_action_start_timer) power_action_motion_cleanup(true, true);
}

static void power_action_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    power_action_hide();
}

static void power_restart_selector_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_PRESSED)
        power_restart_selector_long_press_fired = false;
}

static void power_restart_selector_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    power_restart_selector_long_press_fired = true;
    power_action_pending = -1;
    char error[128] = {0};
    if (!idle_shutdown_reboot_to_bootloader(error, sizeof(error)))
        show_error_toast(error[0] ? error : TR("Could not restart into the boot selector"));
}

static void power_action_clicked_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int action = (int) (intptr_t) lv_event_get_user_data(e);
    if (action == 1 && power_restart_selector_long_press_fired) {
        /* LVGL sends CLICKED after a long press is released. Consume that
         * release so it cannot start the ordinary two-tap restart flow. */
        power_restart_selector_long_press_fired = false;
        return;
    }
    if (action == 0 || action == 1) {
        if (firmware_update_busy()) {
            lv_label_set_text(power_action_status, TR("An update is in progress. Please wait."));
            return;
        }
        if (power_action_pending != action) {
            power_action_pending = action;
            lv_label_set_text(power_action_status, action == 0
                ? TR("Tap Power off again to confirm") : TR("Tap Restart again to confirm"));
            return;
        }
        power_action_hide();
        if (action == 0) idle_shutdown_now();
        else idle_shutdown_reboot_now();
        return;
    }
    power_action_hide();
    if (action == 2) {
        /* The normal screen-state edge detector disables touch/refresh and
         * enables audio low-power mode. Playback itself keeps running. */
        backlight_set_screen_on(false);
    } else if (action == 3) {
        gui_settings_open_sleep_timer();
    }
}

void start_power_off_countdown(void) {
    if (!power_action_overlay.backdrop || !power_action_status) return;
    if (gui_popup_is_visible(&power_action_overlay)) return;
    power_action_pending = -1;
    int remaining = quick_drawer_sleep_timer_remaining_seconds();
    if (quick_drawer_sleep_timer_is_active()) {
        lv_label_set_text_fmt(power_action_status, TR("Sleep timer: %d min remaining"), (remaining + 59) / 60);
    } else {
        lv_label_set_text(power_action_status, TR("Sleep timer: Off"));
    }
    if (power_action_overlay.popup) lv_obj_scroll_to_y(power_action_overlay.popup, 0, LV_ANIM_OFF);
    /* Snapshot+blur whatever screen is currently behind the overlay fresh
     * on every show -- it can be a different screen each time, and this is
     * cheap enough (tiny work buffer) to do synchronously right before
     * gui_popup_show() rather than showing a stale or blank backdrop for a
     * frame while an async job catches up. */
    uint8_t * bytes = frosted_glass_blur_screen_rgb565(lv_screen_active(),
        POWER_OVERLAY_BLUR_WORK_WIDTH, POWER_OVERLAY_BLUR_WORK_HEIGHT,
        POWER_OVERLAY_BLUR_RADIUS, POWER_OVERLAY_BLUR_PASSES,
        POWER_OVERLAY_DARKEN_NUM, POWER_OVERLAY_DARKEN_DEN,
        BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    if (bytes) {
        free(power_action_blur_bytes);
        power_action_blur_bytes = bytes;
        memset(&power_action_blur_dsc, 0, sizeof(power_action_blur_dsc));
        power_action_blur_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        power_action_blur_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        power_action_blur_dsc.header.w = BOARD_SCREEN_WIDTH;
        power_action_blur_dsc.header.h = BOARD_SCREEN_HEIGHT;
        power_action_blur_dsc.header.stride = BOARD_SCREEN_WIDTH * 2;
        power_action_blur_dsc.data = power_action_blur_bytes;
        power_action_blur_dsc.data_size = (uint32_t) BOARD_SCREEN_WIDTH * BOARD_SCREEN_HEIGHT * 2;
        if (power_action_blur_img) {
            lv_image_set_src(power_action_blur_img, &power_action_blur_dsc);
            lv_obj_remove_flag(power_action_blur_img, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (power_action_blur_img) {
        /* Snapshot or allocation failed -- hide rather than show a stale
         * frame from a previous, unrelated screen. The backdrop's own
         * black tint still shows through, so the overlay is not blank. */
        lv_obj_add_flag(power_action_blur_img, LV_OBJ_FLAG_HIDDEN);
    }
    power_action_show();
}

void build_power_off_countdown_popup(void) {
    /* lv_layer_sys(), not lv_layer_top() -- the status bar band lives on
     * lv_layer_top() and several places in gui_shell.c explicitly
     * re-foreground it above every other lv_layer_top() sibling (quick
     * drawer, drag motion, etc: "the status bar stays above THAT"), which
     * fought back against this popup's own one-time move-to-foreground and
     * left the status bar rendering uncovered/unblurred as a strip across
     * the top. lv_layer_sys() is documented as always above both the
     * normal screen AND lv_layer_top(), so this needs no z-order fight at
     * all -- already the pattern used elsewhere in this codebase (see
     * gui_lyrics.c/gui_network.c/gui_settings.c's own mask objects) for
     * anything that must cover literally everything. */
    lv_obj_t * top = lv_layer_sys();

    power_action_overlay.backdrop = lv_obj_create(top);
    lv_obj_set_size(power_action_overlay.backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(power_action_overlay.backdrop, lv_color_black(), 0);
    /* Light residual tint over the frosted-glass backdrop image below, for
     * legibility -- the blur itself already darkens the snapshot (see
     * POWER_OVERLAY_DARKEN_NUM/DEN), so this is deliberately much lower
     * than a flat, image-less overlay would need. */
    lv_obj_set_style_bg_opa(power_action_overlay.backdrop, 76, 0); /* 30% -- LV_OPA_* presets only cover multiples of 10 */
    lv_obj_set_style_border_width(power_action_overlay.backdrop, 0, 0);
    /* Default theme padding otherwise insets LV_ALIGN_TOP_LEFT-positioned
     * children (power_action_blur_img below) from the content area, not
     * the true outer edge -- left the blurred backdrop image short of the
     * real top-left corner while its fixed BOARD_SCREEN_WIDTH/HEIGHT size
     * pushed it past the opposite edges instead of just resizing. */
    lv_obj_set_style_pad_all(power_action_overlay.backdrop, 0, 0);
    lv_obj_remove_flag(power_action_overlay.backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(power_action_overlay.backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(power_action_overlay.backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(power_action_overlay.backdrop, power_action_backdrop_cb, LV_EVENT_CLICKED, NULL);

    /* Bottom-most child (created first, so the sheet draws on top
     * of it) -- filled in fresh on every start_power_off_countdown() call
     * with a blurred snapshot of whichever screen is behind the overlay at
     * that moment. Hidden until the first real snapshot lands. */
    power_action_blur_img = lv_image_create(power_action_overlay.backdrop);
    lv_obj_set_size(power_action_blur_img, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    lv_obj_align(power_action_blur_img, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(power_action_blur_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(power_action_blur_img, LV_OBJ_FLAG_HIDDEN);

    /* Keep the sheet inside the sys-layer backdrop so both are hidden as
     * one modal and neither can fall behind the persistent status bar. */
    lv_obj_t * actions[5] = {0};
    power_action_overlay.popup = build_power_hub_panel(power_action_overlay.backdrop,
        power_action_clicked_cb, &power_action_status, actions);
    if (actions[1]) {
        lv_obj_add_event_cb(actions[1], power_restart_selector_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(actions[1], power_restart_selector_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    }
}


void gui_power_hub_teardown(void) {
    power_action_motion_cleanup(false, true);
    gui_popup_teardown(&power_action_overlay);
    power_action_status = NULL;
    power_action_pending = -1;
    power_action_blur_img = NULL; /* child of backdrop -- already deleted by the teardown above */
    free(power_action_blur_bytes);
    power_action_blur_bytes = NULL;
}
