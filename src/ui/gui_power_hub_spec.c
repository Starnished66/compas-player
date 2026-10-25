#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lvgl.h>
#include "screen_builders.h"
#include "i18n.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "gui_settings.h"
#include "gui_shell.h"
#include "firmware_update.h"
#include "idle_shutdown.h"
#include "backlight.h"
#include "frosted_glass.h"
#include "transition_compositor.h"
#include "ui_fonts.h"

uint32_t gui_anim_ms(uint32_t ms);
bool gui_anims_off(void);
void show_error_toast(const char *message);
const char *i18n_tr(const char *text) { return text; }

#include "gui_power_hub.c"

static char action_trace[8192];
static size_t action_trace_len;
static lv_obj_t *test_backdrop;
static lv_obj_t *test_panel;
static lv_obj_t *test_status;
static lv_obj_t *test_actions[5];
static unsigned shutdown_calls, reboot_calls, bootloader_calls;
static unsigned screen_off_calls, sleep_timer_calls, error_calls;
static bool animations_disabled;

static void trace_event(const char *event) {
    int n = snprintf(action_trace + action_trace_len, sizeof(action_trace) - action_trace_len,
                     "%s\n", event);
    if (n > 0 && (size_t)n < sizeof(action_trace) - action_trace_len) action_trace_len += (size_t)n;
}
lv_draw_buf_t *__wrap_lv_snapshot_take(lv_obj_t *obj, lv_color_format_t format) {
    (void)obj; (void)format; trace_event("snapshot-fallback"); return NULL;
}

static void flush_ready(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    (void)area; (void)pixels; lv_display_flush_ready(display);
}

lv_obj_t *build_power_hub_panel(lv_obj_t *parent, lv_event_cb_t action_cb,
                                lv_obj_t **status, lv_obj_t **actions) {
    test_backdrop = parent;
    test_panel = lv_obj_create(parent);
    assert(test_panel);
    test_status = lv_label_create(test_panel);
    assert(test_status);
    if (status) *status = test_status;
    static const char *names[5] = {"power", "restart", "screen", "sleep", "cancel"};
    for (int i = 0; i < 5; i++) {
        test_actions[i] = lv_obj_create(test_panel);
        assert(test_actions[i]);
        if (actions) actions[i] = test_actions[i];
        lv_obj_add_event_cb(test_actions[i], action_cb, LV_EVENT_ALL, (void *)(intptr_t)i);
        (void)names[i];
    }
    trace_event("panel-build");
    return test_panel;
}

void gui_popup_show(gui_popup_t *popup) {
    if (!popup) return;
    popup->visible = true;
    if (popup->backdrop) lv_obj_remove_flag(popup->backdrop, LV_OBJ_FLAG_HIDDEN);
    if (popup->popup) lv_obj_remove_flag(popup->popup, LV_OBJ_FLAG_HIDDEN);
    trace_event("popup-show");
}
void gui_popup_hide(gui_popup_t *popup) {
    if (!popup) return;
    popup->visible = false;
    if (popup->backdrop) lv_obj_add_flag(popup->backdrop, LV_OBJ_FLAG_HIDDEN);
    if (popup->popup) lv_obj_add_flag(popup->popup, LV_OBJ_FLAG_HIDDEN);
    trace_event("popup-hide");
}
void gui_popup_teardown(gui_popup_t *popup) {
    if (!popup) return;
    gui_popup_hide(popup);
    if (popup->popup) { lv_obj_delete(popup->popup); popup->popup = NULL; }
    if (popup->backdrop) { lv_obj_delete(popup->backdrop); popup->backdrop = NULL; }
    if (test_panel && popup->popup == NULL) {
        test_panel = test_status = NULL;
        for (int i = 0; i < 5; i++) test_actions[i] = NULL;
    }
    if (popup->backdrop == NULL) test_backdrop = NULL;
    trace_event("popup-teardown");
}
bool gui_popup_is_visible(const gui_popup_t *popup) { return popup && popup->visible; }

bool firmware_update_busy(void) { return false; }
void idle_shutdown_now(void) { shutdown_calls++; trace_event("shutdown"); }
void idle_shutdown_reboot_now(void) { reboot_calls++; trace_event("reboot"); }
bool idle_shutdown_reboot_to_bootloader(char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    bootloader_calls++; trace_event("bootloader"); return true;
}
void backlight_set_screen_on(bool on) { if (!on) screen_off_calls++; trace_event(on ? "screen-on" : "screen-off"); }
void gui_settings_open_sleep_timer(void) { sleep_timer_calls++; trace_event("sleep-timer"); }
bool quick_drawer_sleep_timer_is_active(void) { return false; }
int quick_drawer_sleep_timer_remaining_seconds(void) { return 0; }
uint8_t *frosted_glass_blur_screen_rgb565(lv_obj_t *screen, int ww, int wh, int radius,
                                           int passes, int darken_num, int darken_den,
                                           int out_width, int out_height) {
    (void)screen; (void)ww; (void)wh; (void)radius; (void)passes;
    (void)darken_num; (void)darken_den;
    trace_event("blur");
    return calloc((size_t)out_width * (size_t)out_height, 2);
}
bool transition_compositor_begin_bottom_sheet(const lv_draw_buf_t *sheet) { (void)sheet; return false; }
bool transition_compositor_bottom_sheet_frame(int32_t y) { (void)y; return false; }
void transition_compositor_end(void) { trace_event("compositor-end"); }
uint32_t gui_anim_ms(uint32_t ms) { return ms; }
bool gui_anims_off(void) { return animations_disabled; }
void show_error_toast(const char *message) { (void)message; error_calls++; trace_event("error-toast"); }

static void test_teardown(void) { gui_power_hub_teardown(); gui_power_hub_teardown(); }

static void trace_hub_state(const char *tag) {
    char event[256];
    snprintf(event, sizeof(event), "%s:visible=%d;pending=%d;status=%s", tag,
             gui_popup_is_visible(&power_action_overlay), power_action_pending,
             test_status ? lv_label_get_text(test_status) : "<gone>");
    trace_event(event);
}
static void pass_ms(uint32_t ms) { lv_tick_inc(ms); (void)lv_timer_handler(); }
static void tap(int index, lv_event_code_t code) { lv_obj_send_event(test_actions[index], code, NULL); }

static void test_build_show_and_cancel_entrance(void) {
    animations_disabled = false;
    build_power_off_countdown_popup();
    assert(test_panel && test_backdrop && test_status && test_actions[0]);
    assert(lv_obj_has_flag(test_backdrop, LV_OBJ_FLAG_HIDDEN));
    start_power_off_countdown();
    assert(gui_popup_is_visible(&power_action_overlay));
    assert(!lv_obj_has_flag(test_backdrop, LV_OBJ_FLAG_HIDDEN));
    assert(power_action_start_timer); /* Snapshot fallback still schedules entrance. */
    trace_hub_state("shown-pending-entrance");
    test_teardown();
    assert(!test_panel && !test_backdrop);
    pass_ms(10); /* No cancelled entrance callback may touch the deleted popup. */
    trace_event("entrance-cancelled");
}

static void test_backdrop_confirmation_longpress_and_shortcuts(void) {
    animations_disabled = true;
    build_power_off_countdown_popup();
    start_power_off_countdown();
    lv_obj_send_event(test_backdrop, LV_EVENT_CLICKED, NULL);
    assert(!gui_popup_is_visible(&power_action_overlay));
    trace_hub_state("backdrop-cancel");

    start_power_off_countdown();
    tap(0, LV_EVENT_CLICKED);
    assert(shutdown_calls == 0 && power_action_pending == 0);
    trace_hub_state("power-first-tap");
    tap(0, LV_EVENT_CLICKED);
    assert(shutdown_calls == 1 && power_action_pending == -1);
    trace_event("power-off-confirmed");

    start_power_off_countdown();
    tap(1, LV_EVENT_PRESSED);
    tap(1, LV_EVENT_LONG_PRESSED);
    assert(bootloader_calls == 1 && power_action_pending == -1);
    tap(1, LV_EVENT_CLICKED); /* LVGL release click must be swallowed. */
    assert(reboot_calls == 0 && power_action_pending == -1);
    trace_event("restart-long-press-swallowed-click");

    start_power_off_countdown();
    tap(2, LV_EVENT_CLICKED);
    assert(screen_off_calls == 1 && !gui_popup_is_visible(&power_action_overlay));
    start_power_off_countdown();
    tap(3, LV_EVENT_CLICKED);
    assert(sleep_timer_calls == 1 && !gui_popup_is_visible(&power_action_overlay));
    trace_event("actions-verified");
}

static void test_repeat_teardown_rebuild(void) {
    test_teardown();
    assert(!test_panel && !test_backdrop);
    build_power_off_countdown_popup();
    assert(test_panel && test_backdrop);
    start_power_off_countdown();
    test_teardown();
    assert(!test_panel && !test_backdrop);
    trace_event("repeat-rebuild-verified");
}

int main(void) {
    lv_init();
    assert(ui_fonts_init());
    lv_display_t *display = lv_display_create(480, 800);
    assert(display);
    static uint8_t display_buffer[480 * 16 * 4];
    lv_display_set_buffers(display, display_buffer, NULL, sizeof(display_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_ready);
    lv_screen_load(lv_obj_create(NULL));

    test_build_show_and_cancel_entrance();
    test_backdrop_confirmation_longpress_and_shortcuts();
    test_repeat_teardown_rebuild();
    test_teardown();
    printf("power-hub-trace:\n%scounts:shutdown=%u;reboot=%u;bootloader=%u;screen_off=%u;sleep_timer=%u;errors=%u\n",
           action_trace, shutdown_calls, reboot_calls, bootloader_calls,
           screen_off_calls, sleep_timer_calls, error_calls);
    lv_display_delete(display);
    lv_deinit();
    return 0;
}
