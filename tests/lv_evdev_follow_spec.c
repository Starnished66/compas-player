/* Host test for the patched LVGL evdev driver (patches/lvgl_evdev_frames.patch):
 * the pointer must follow exactly one finger. The driver source is included
 * with the LVGL indev/display services it uses replaced by small stubs, so the
 * real _evdev_read runs against synthetic input_event streams written to a
 * pipe. Built and run by scripts/run_lv_evdev_follow_spec.sh, with
 * LV_USE_GESTURE_RECOGNITION both off (the shipped setting) and on. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl/src/lvgl.h"
#include "lvgl/src/core/lv_global.h"
#include "lvgl/src/display/lv_display_private.h"

#if !LV_USE_EVDEV
#error "build without HOST_BUILD so LV_USE_EVDEV is enabled"
#endif

/* ---- LVGL stubs ---- */
lv_global_t lv_global;
static lv_display_t fake_display = { .hor_res = 480, .ver_res = 800 };
static struct {
    lv_indev_type_t type;
    lv_indev_read_cb_t read_cb;
    void * driver_data;
} fake_indev;

void * lv_malloc_zeroed(size_t size) { return calloc(1, size); }
void lv_free(void * p) { free(p); }
lv_indev_t * lv_indev_create(void) { return (lv_indev_t *)&fake_indev; }
void lv_indev_set_type(lv_indev_t * i, lv_indev_type_t t) { (void)i; fake_indev.type = t; }
lv_indev_type_t lv_indev_get_type(const lv_indev_t * i) { (void)i; return fake_indev.type; }
void lv_indev_set_read_cb(lv_indev_t * i, lv_indev_read_cb_t cb) { (void)i; fake_indev.read_cb = cb; }
void lv_indev_set_driver_data(lv_indev_t * i, void * d) { (void)i; fake_indev.driver_data = d; }
void * lv_indev_get_driver_data(const lv_indev_t * i) { (void)i; return fake_indev.driver_data; }
lv_display_t * lv_indev_get_display(const lv_indev_t * i) { (void)i; return &fake_display; }
lv_indev_read_cb_t lv_indev_get_read_cb(lv_indev_t * i) { (void)i; return fake_indev.read_cb; }
lv_indev_t * lv_indev_get_next(lv_indev_t * i) { (void)i; return NULL; }
void lv_indev_delete(lv_indev_t * i) { (void)i; }
void lv_indev_wait_release(lv_indev_t * i) { (void)i; }
void lv_indev_add_event_cb(lv_indev_t * i, lv_event_cb_t cb, lv_event_code_t c, void * u) {
    (void)i; (void)cb; (void)c; (void)u;
}
void * lv_event_get_target(lv_event_t * e) { (void)e; return NULL; }
lv_result_t lv_async_call(lv_async_cb_t cb, void * u) { (void)cb; (void)u; return LV_RESULT_OK; }
lv_result_t lv_async_call_cancel(lv_async_cb_t cb, void * u) { (void)cb; (void)u; return LV_RESULT_OK; }
lv_timer_t * lv_timer_create(lv_timer_cb_t cb, uint32_t p, void * u) { (void)cb; (void)p; (void)u; return NULL; }
void lv_timer_delete(lv_timer_t * t) { (void)t; }
int lv_snprintf(char * b, size_t n, const char * f, ...) { (void)b; (void)n; (void)f; return 0; }
int lv_strncmp(const char * a, const char * b, size_t n) { return strncmp(a, b, n); }
#if LV_USE_GESTURE_RECOGNITION
void lv_indev_gesture_recognizers_update(lv_indev_t * i, lv_indev_touch_data_t * d, uint16_t n) {
    (void)i; (void)d; (void)n;
}
void lv_indev_gesture_recognizers_set_data(lv_indev_t * i, lv_indev_data_t * d) { (void)i; (void)d; }
#endif

#include "lvgl/src/drivers/evdev/lv_evdev.c"

/* ---- harness ---- */
static int pipe_fds[2];
static lv_indev_t * indev;

static void put(uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev = { .type = type, .code = code, .value = value };
    assert(write(pipe_fds[1], &ev, sizeof(ev)) == (ssize_t)sizeof(ev));
}
static void abs_(uint16_t code, int v) { put(EV_ABS, code, v); }
static void sync_(void) { put(EV_SYN, SYN_REPORT, 0); }
static void mt_sync(void) { put(EV_SYN, SYN_MT_REPORT, 0); }
static void touch(int v) { put(EV_KEY, BTN_TOUCH, v); }

/* One type A contact. id < 0 omits ABS_MT_TRACKING_ID. */
static void contact(int id, int x, int y) {
    if(id >= 0) abs_(ABS_MT_TRACKING_ID, id);
    abs_(ABS_MT_POSITION_X, x);
    abs_(ABS_MT_POSITION_Y, y);
    mt_sync();
}

/* Pointer coordinates equal the raw ones on a 480x800 display when the
 * calibration range is the display size (set below). */
static void expect(lv_indev_state_t state, int x, int y, const char * what) {
    lv_indev_data_t data = { 0 };
    fake_indev.read_cb(indev, &data);
    if(data.state != state || data.point.x != x || data.point.y != y) {
        fprintf(stderr, "FAIL %s: got state=%d (%d,%d), want state=%d (%d,%d)\n", what,
                data.state, data.point.x, data.point.y, state, x, y);
        assert(0);
    }
}
static void expect_pressed(int x, int y, const char * what) { expect(LV_INDEV_STATE_PRESSED, x, y, what); }
static void expect_released(int x, int y, const char * what) { expect(LV_INDEV_STATE_RELEASED, x, y, what); }

static void setup(void) {
    memset(&fake_indev, 0, sizeof(fake_indev));
    assert(pipe(pipe_fds) == 0);
    /* lv_evdev_create_fd owns the read end; ioctls fail on a pipe, which the
     * driver tolerates when the indev type is given. */
    indev = lv_evdev_create_fd(LV_INDEV_TYPE_POINTER, pipe_fds[0]);
    assert(indev);
    lv_evdev_set_calibration(indev, 0, 0, 479, 799);
}
static void teardown(void) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    lv_free(fake_indev.driver_data);
}

/* Single finger on a type A device with tracking IDs and BTN_TOUCH (GT9xx
 * style): tap and drag. */
static void test_single_finger(void) {
    setup();
    contact(0, 100, 200); touch(1); sync_();
    expect_pressed(100, 200, "tap down");
    contact(0, 110, 230); sync_();
    expect_pressed(110, 230, "drag");
    contact(0, 150, 300); sync_();
    expect_pressed(150, 300, "drag 2");
    touch(0); sync_();
    expect_released(150, 300, "tap up");
    /* A second press after a release starts fresh. */
    contact(0, 20, 30); touch(1); sync_();
    expect_pressed(20, 30, "second press");
    touch(0); mt_sync(); sync_();
    expect_released(20, 30, "second release");
    teardown();
}

/* Press and release queued together stay two separate samples. */
static void test_press_release_queued(void) {
    setup();
    contact(0, 40, 50); touch(1); sync_();
    touch(0); sync_();
    expect_pressed(40, 50, "queued press");
    expect_released(40, 50, "queued release");
    teardown();
}

/* R1 style: each finger reported on alternate frames, an empty off-frame for
 * a lone finger, release is BTN_TOUCH 0 plus an empty contact. */
static void test_r1_alternating(void) {
    setup();
    contact(0, 100, 200); touch(1); sync_();
    expect_pressed(100, 200, "A down");
    touch(1); sync_(); /* off-frame of a lone finger: nothing moves */
    expect_pressed(100, 200, "empty off-frame keeps state");
    contact(1, 400, 700); touch(1); sync_(); /* second finger lands on an off-frame */
    expect_pressed(100, 200, "B appears, pointer stays on A");
    contact(0, 102, 205); touch(1); sync_();
    expect_pressed(102, 205, "A moves");
    contact(1, 401, 702); touch(1); sync_();
    expect_pressed(102, 205, "B frame, A kept");
    contact(0, 104, 210); contact(1, 402, 704); touch(1); sync_(); /* both in one frame */
    expect_pressed(104, 210, "both contacts, A wins even though B is last");
    contact(0, 106, 215); touch(1); sync_();
    expect_pressed(106, 215, "A moves again");
    touch(0); mt_sync(); sync_();
    expect_released(106, 215, "release");
    teardown();
}

/* R1 without tracking IDs: follow the contact nearest the previous position. */
static void test_nearest_without_ids(void) {
    setup();
    contact(-1, 100, 200); touch(1); sync_();
    expect_pressed(100, 200, "A down");
    contact(-1, 105, 203); contact(-1, 400, 700); touch(1); sync_();
    expect_pressed(105, 203, "nearest of two, other last");
    contact(-1, 400, 705); touch(1); sync_(); /* a lone contact is the followed finger */
    expect_pressed(400, 705, "lone contact follows whatever the distance");
    contact(-1, 100, 200); contact(-1, 110, 210); touch(1); sync_();
    expect_pressed(400, 705, "two contacts, none near: first miss");
    contact(-1, 100, 201); contact(-1, 110, 211); touch(1); sync_();
    expect_released(400, 705, "none near twice: considered lifted");
    contact(-1, 400, 707); touch(1); sync_();
    expect_released(400, 705, "rest ignored until release");
    touch(0); mt_sync(); sync_();
    expect_released(400, 705, "release");
    contact(-1, 300, 300); touch(1); sync_();
    expect_pressed(300, 300, "new press starts from first contact");
    touch(0); mt_sync(); sync_();
    teardown();
}

/* Reading drains frames one sample at a time, so the missed-frame sequence
 * above is read frame by frame; drain helper keeps the counts honest. */

/* GT9xx style: every contact in every frame, release by tracking ID. */
static void test_gt9xx_second_finger(void) {
    setup();
    contact(0, 100, 200); touch(1); sync_();
    expect_pressed(100, 200, "A down");
    contact(0, 101, 201); contact(3, 300, 400); sync_();
    expect_pressed(101, 201, "second finger added, pointer stays");
    contact(3, 305, 405); contact(0, 102, 203); sync_(); /* other order in the frame */
    expect_pressed(102, 203, "order in frame does not matter");
    contact(0, 103, 205); sync_(); /* second finger lifted: BTN_TOUCH stays 1 */
    expect_pressed(103, 205, "second finger lifted");
    contact(0, 104, 206); contact(5, 10, 10); sync_();
    expect_pressed(104, 206, "third finger ignored");
    touch(0); sync_();
    expect_released(104, 206, "release");
    teardown();
}

/* The followed finger lifts while another stays: release, then ignore the
 * remaining finger until all fingers are up. */
static void test_gt9xx_followed_finger_lifts(void) {
    setup();
    contact(0, 100, 200); touch(1); sync_();
    expect_pressed(100, 200, "A down");
    contact(0, 100, 200); contact(1, 300, 400); sync_();
    expect_pressed(100, 200, "B added");
    contact(1, 301, 401); sync_();
    expect_pressed(100, 200, "A missing once, kept");
    contact(1, 302, 402); sync_();
    expect_released(100, 200, "A gone, released");
    contact(1, 303, 403); sync_();
    expect_released(100, 200, "B ignored");
    touch(0); sync_();
    expect_released(100, 200, "all up");
    contact(1, 50, 60); touch(1); sync_();
    expect_pressed(50, 60, "B pressed again later");
    touch(0); sync_();
    teardown();
}

/* Type B: slots keep the previous behavior (every position event applies). */
static void test_type_b(void) {
    setup();
    abs_(ABS_MT_SLOT, 0); abs_(ABS_MT_TRACKING_ID, 10); abs_(ABS_MT_POSITION_X, 100); abs_(ABS_MT_POSITION_Y, 200);
    touch(1); sync_();
    expect_pressed(100, 200, "slot 0 down");
    abs_(ABS_MT_SLOT, 1); abs_(ABS_MT_TRACKING_ID, 11); abs_(ABS_MT_POSITION_X, 300); abs_(ABS_MT_POSITION_Y, 400); sync_();
    expect_pressed(300, 400, "slot 1 wins as before");
    abs_(ABS_MT_SLOT, 1); abs_(ABS_MT_TRACKING_ID, -1); sync_();
    expect_released(300, 400, "slot 1 tracking id -1 releases as before");
    teardown();
}

/* Single-touch devices: ABS_X/ABS_Y with BTN_TOUCH. */
static void test_single_touch(void) {
    setup();
    abs_(ABS_X, 10); abs_(ABS_Y, 20); touch(1); sync_();
    expect_pressed(10, 20, "st down");
    abs_(ABS_X, 15); abs_(ABS_Y, 25); sync_();
    expect_pressed(15, 25, "st move");
    touch(0); sync_();
    expect_released(15, 25, "st up");
    teardown();
}

/* SYN_DROPPED: the partial frame and everything up to the next SYN_REPORT is
 * discarded, and follow does not run on that report. */
static void test_syn_dropped(void) {
    setup();
    contact(0, 100, 200); touch(1); sync_();
    expect_pressed(100, 200, "A down");
    contact(0, 110, 210); /* torn frame */
    abs_(ABS_MT_TRACKING_ID, 7); abs_(ABS_MT_POSITION_X, 300);
    put(EV_SYN, SYN_DROPPED, 0);
    abs_(ABS_MT_POSITION_Y, 400); mt_sync(); /* rest of the dropped packet */
    contact(1, 300, 400);
    sync_(); /* ends the dropped packet */
    contact(0, 120, 220); sync_();
    expect_pressed(120, 220, "frame after the drop is clean");
    teardown();
}

/* One finger moving more than FOLLOW_MAX_JUMP between reports, no IDs. */
static void test_lone_finger_far_move(void) {
    setup();
    contact(-1, 100, 200); touch(1); sync_();
    expect_pressed(100, 200, "down");
    contact(-1, 400, 600); touch(1); sync_();
    expect_pressed(400, 600, "flick of 500 units");
    contact(-1, 450, 100); touch(1); sync_();
    expect_pressed(450, 100, "second flick");
    touch(0); mt_sync(); sync_();
    expect_released(450, 100, "release");
    teardown();
}

/* Tracking ID -1 for the followed contact is a miss inside the grace window,
 * with or without coordinates, and releases only after the second one. */
static void test_tracking_id_minus_one(void) {
    setup();
    contact(0, 100, 200); touch(1); sync_();
    expect_pressed(100, 200, "A down");
    abs_(ABS_MT_TRACKING_ID, -1); mt_sync(); touch(1); sync_();
    expect_pressed(100, 200, "-1 without coordinates, first miss");
    contact(0, 101, 201); touch(1); sync_();
    expect_pressed(101, 201, "A back");
    abs_(ABS_MT_TRACKING_ID, -1); abs_(ABS_MT_POSITION_X, 101); abs_(ABS_MT_POSITION_Y, 201);
    mt_sync(); touch(1); sync_();
    expect_pressed(101, 201, "-1 with coordinates, first miss");
    abs_(ABS_MT_TRACKING_ID, -1); mt_sync(); touch(1); sync_();
    expect_released(101, 201, "second miss releases");
    touch(0); mt_sync(); sync_();
    expect_released(101, 201, "BTN_TOUCH 0");
    teardown();
}

/* A real BTN_TOUCH 0 ends the gesture even inside the grace window. */
static void test_tracking_id_minus_one_touch_up(void) {
    setup();
    contact(0, 100, 200); touch(1); sync_();
    expect_pressed(100, 200, "A down");
    abs_(ABS_MT_TRACKING_ID, -1); mt_sync(); touch(0); sync_();
    expect_released(100, 200, "BTN_TOUCH 0 releases at once");
    teardown();
}

/* One stray ABS_MT_SLOT does not switch follow off for later frames. */
static void test_stray_slot(void) {
    setup();
    abs_(ABS_MT_SLOT, 0); contact(0, 100, 200); touch(1); sync_();
    expect_pressed(100, 200, "the frame with the slot is handled as before");
    contact(0, 110, 210); contact(1, 300, 400); sync_();
    expect_pressed(110, 210, "type A frame follows A, not the last contact");
    contact(0, 120, 220); contact(1, 310, 410); sync_();
    expect_pressed(120, 220, "still following A");
    touch(0); sync_();
    teardown();
}

int main(void) {
    test_single_finger();
    test_press_release_queued();
    test_r1_alternating();
    test_nearest_without_ids();
    test_gt9xx_second_finger();
    test_gt9xx_followed_finger_lifts();
    test_type_b();
    test_single_touch();
    test_syn_dropped();
    test_lone_finger_far_move();
    test_tracking_id_minus_one();
    test_tracking_id_minus_one_touch_up();
    test_stray_slot();
    puts("lv_evdev_follow_spec: ok");
    return 0;
}
