/* Characterize and guard LVGL evdev frames queued while a pointer callback
 * blocks. The real driver and LVGL indev/object processing run against a pipe. */
#include <assert.h>
#include <errno.h>
#include <linux/input.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lvgl/src/lvgl.h"

#if !LV_USE_EVDEV
#error "compile without HOST_BUILD so the target evdev driver is enabled"
#endif

static bool mock_touch_held;
static bool mock_key_query_available;
static unsigned key_state_queries;

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    if(_IOC_TYPE(request) == _IOC_TYPE(EVIOCGKEY(0)) &&
       _IOC_NR(request) == _IOC_NR(EVIOCGKEY(0))) {
        if(!mock_key_query_available) {
            key_state_queries++;
            errno = ENOTTY;
            return -1;
        }
        va_list args;
        va_start(args, request);
        unsigned long * keys = va_arg(args, unsigned long *);
        va_end(args);
        memset(keys, 0, _IOC_SIZE(request));
        if(mock_touch_held) {
            const unsigned bits = (unsigned)(sizeof(unsigned long) * 8);
            keys[BTN_TOUCH / bits] |= 1UL << (BTN_TOUCH % bits);
        }
        key_state_queries++;
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

#include "lvgl/src/drivers/evdev/lv_evdev.c"

/* Optional host-core references retained by the cached LVGL build. */
uint32_t gui_anim_ms(uint32_t duration) { return duration; }
lv_font_t lv_font_montserrat_16 = { 0 };

static int fds[2];
static lv_indev_t * indev;
static lv_indev_read_cb_t evdev_read;
static lv_obj_t * game;
static lv_obj_t * back;
static unsigned game_clicks;
static unsigned back_clicks;
static bool simulate_held_exit;
static bool type_a_frames;
static bool raw_touch_require_release;
static bool callback_reported_held;
static unsigned callback_read_count;
static unsigned raw_press_suppressed;

#define READY_FRAME_PUMP_LIMIT 32

static void event_put(uint16_t type, uint16_t code, int32_t value)
{
    struct input_event event = { .type = type, .code = code, .value = value };
    assert(write(fds[1], &event, sizeof(event)) == (ssize_t)sizeof(event));
}

static void touch_frame(int x, int y, int pressed)
{
    if(type_a_frames) {
        event_put(EV_ABS, ABS_MT_TRACKING_ID, pressed ? 1 : -1);
        event_put(EV_ABS, ABS_MT_POSITION_X, x);
        event_put(EV_ABS, ABS_MT_POSITION_Y, y);
        event_put(EV_SYN, SYN_MT_REPORT, 0);
        event_put(EV_KEY, BTN_TOUCH, pressed);
        event_put(EV_SYN, SYN_REPORT, 0);
        return;
    }
    event_put(EV_ABS, ABS_X, x);
    event_put(EV_ABS, ABS_Y, y);
    event_put(EV_KEY, BTN_TOUCH, pressed);
    event_put(EV_SYN, SYN_REPORT, 0);
}

/* Model gui_shell's read wrapper: it wraps the evdev callback, so the
 * driver's discard API must work without comparing read_cb to _evdev_read. */
static void wrapped_evdev_read(lv_indev_t * device, lv_indev_data_t * data)
{
    callback_read_count++;
    evdev_read(device, data);
    if(raw_touch_require_release && data->state == LV_INDEV_STATE_PRESSED) {
        raw_press_suppressed++;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if(raw_touch_require_release) raw_touch_require_release = false;
}

static void back_clicked(lv_event_t * event)
{
    (void)event;
    back_clicks++;
}

static void game_clicked(lv_event_t * event)
{
    (void)event;
    game_clicks++;

    /* The callback stands in for the synchronous emulator launch. These
     * frames arrive while the game owns the panel, before callback return. */
    touch_frame(220, 40, 1);
    if(!simulate_held_exit) touch_frame(220, 40, 0);

    mock_touch_held = simulate_held_exit;
    callback_reported_held = lv_evdev_discard_pending(indev);
    raw_touch_require_release = callback_reported_held;
}

static void pump_frame(lv_indev_mode_t mode, int x, int y, int pressed)
{
    touch_frame(x, y, pressed);
    /* TIMER mode drains multiple frames in one LVGL read; EVENT mode reads
     * one frame per main-loop readiness notification. */
    lv_indev_read(indev);
    if(mode == LV_INDEV_MODE_EVENT) assert(callback_read_count > 0);
}

static void pump_ready_event_frames(void)
{
    for(unsigned i = 0; i < READY_FRAME_PUMP_LIMIT; i++) {
        struct pollfd ready = { .fd = fds[0], .events = POLLIN };
        int result = poll(&ready, 1, 0);
        assert(result >= 0);
        if(result == 0) return;
        assert(ready.revents & POLLIN);
        lv_indev_read(indev);
    }

    /* Keep this readiness pump bounded even if a test bug or broken driver
     * leaves the pipe continuously readable. */
    struct pollfd ready = { .fd = fds[0], .events = POLLIN };
    assert(poll(&ready, 1, 0) == 0);
}

static void run_case(lv_indev_mode_t mode, bool held_at_exit, bool type_a,
                     bool live_key_query)
{
    assert(pipe(fds) == 0);
    indev = lv_evdev_create_fd(LV_INDEV_TYPE_POINTER, fds[0]);
    assert(indev);
    lv_evdev_set_calibration(indev, 0, 0, 479, 799);
    lv_indev_set_display(indev, lv_display_get_default());
    lv_indev_set_mode(indev, mode);
    evdev_read = lv_indev_get_read_cb(indev);
    assert(evdev_read);
    lv_indev_set_read_cb(indev, wrapped_evdev_read);

    game_clicks = back_clicks = 0;
    callback_read_count = key_state_queries = 0;
    raw_press_suppressed = 0;
    simulate_held_exit = held_at_exit;
    type_a_frames = type_a;
    mock_key_query_available = live_key_query;
    raw_touch_require_release = false;
    callback_reported_held = false;

    game = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(game, 10, 10);
    lv_obj_set_size(game, 100, 100);
    lv_obj_add_flag(game, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(game, game_clicked, LV_EVENT_CLICKED, NULL);
    back = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(back, 200, 10);
    lv_obj_set_size(back, 100, 100);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_update_layout(lv_screen_active());

    /* The launch click itself is a normal tap on Game. */
    mock_touch_held = false;
    pump_frame(mode, 40, 40, 1);
    pump_frame(mode, 40, 40, 0);
    if(mode == LV_INDEV_MODE_EVENT) pump_ready_event_frames();
    assert(game_clicks == 1);
    assert(back_clicks == 0);
    assert(callback_reported_held == held_at_exit);
    assert(key_state_queries == 1);

    if(held_at_exit) {
        /* Motion while the exit finger is still down must pass through the
         * wrapped reader's held-contact guard without clicking Back. */
        mock_touch_held = true;
        unsigned suppressed_before = raw_press_suppressed;
        pump_frame(mode, 230, 50, 1);
        if(mode == LV_INDEV_MODE_EVENT) pump_ready_event_frames();
        assert(raw_press_suppressed > suppressed_before);
        assert(back_clicks == 0);

        /* The exit contact's eventual lift must not click Back either. */
        mock_touch_held = false;
        pump_frame(mode, 220, 40, 0);
        if(mode == LV_INDEV_MODE_EVENT) pump_ready_event_frames();
        assert(back_clicks == 0);
    }

    /* The first complete fresh tap after resume remains usable. */
    mock_touch_held = false;
    pump_frame(mode, 220, 40, 1);
    pump_frame(mode, 220, 40, 0);
    assert(back_clicks == 1);

    lv_evdev_delete(indev);
    close(fds[1]);
    lv_obj_delete(game);
    lv_obj_delete(back);
}

int main(void)
{
    lv_init();
    lv_display_t * display = lv_display_create(480, 800);
    assert(display);

    for(int type_a = 0; type_a <= 1; type_a++) {
        for(int mode = LV_INDEV_MODE_TIMER; mode <= LV_INDEV_MODE_EVENT; mode++) {
            for(int live_key_query = 0; live_key_query <= 1; live_key_query++) {
                run_case((lv_indev_mode_t)mode, false, type_a != 0, live_key_query != 0);
                run_case((lv_indev_mode_t)mode, true, type_a != 0, live_key_query != 0);
            }
        }
    }
    puts("lv_evdev_external_handoff_spec: stale input discarded; held/fresh taps pass TIMER/EVENT, single/type-A, ioctl/fallback cases");
    return 0;
}
