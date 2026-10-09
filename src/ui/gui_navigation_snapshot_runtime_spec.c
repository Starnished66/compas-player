#define _POSIX_C_SOURCE 200809L
#include "gui_navigation.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>

player_settings_t current_settings;
static bool test_screen_on = true;
static bool test_input_pressed;
static unsigned snapshot_calls;
static slide_transition_ctx_t * scheduled_test_ctx;
static bool fail_next_timer_create;
static bool fail_next_anim_start;

bool backlight_screen_is_on(void) { return test_screen_on; }
uint32_t gui_anim_ms(uint32_t ms) { return ms; }
lv_obj_t * gui_player_get_screen(void) { return NULL; }
lv_obj_t * gui_player_get_dismiss_btn(void) { return NULL; }
lv_obj_t * gui_shell_get_home_screen(void) { return NULL; }
lv_obj_t * gui_lyrics_get_screen(void) { return NULL; }
lv_obj_t * gui_lock_screen_get_screen(void) { return NULL; }
lv_obj_t * gui_shell_get_home_indicator_band(void) { return NULL; }
void gui_shell_set_home_indicator_visible(bool visible) { (void)visible; }
void gui_shell_player_swipe_recover(void *ctx) { (void)ctx; }
void gui_lock_screen_swipe_recover(void *ctx) { (void)ctx; }
void sync_player_topbar_visibility(lv_obj_t *screen) { (void)screen; }
bool transition_compositor_is_active(void) { return false; }
bool transition_compositor_frame(int32_t value) { (void)value; return true; }
void transition_compositor_end(void) {}

lv_timer_t *__real_lv_timer_create(lv_timer_cb_t callback, uint32_t period, void *user_data);
lv_timer_t *__wrap_lv_timer_create(lv_timer_cb_t callback, uint32_t period, void *user_data) {
    if (fail_next_timer_create) {
        fail_next_timer_create = false;
        return NULL;
    }
    return __real_lv_timer_create(callback, period, user_data);
}
lv_anim_t *__real_lv_anim_start(const lv_anim_t *anim);
lv_anim_t *__wrap_lv_anim_start(const lv_anim_t *anim) {
    if (fail_next_anim_start) {
        fail_next_anim_start = false;
        return NULL;
    }
    return __real_lv_anim_start(anim);
}

lv_draw_buf_t * __real_lv_snapshot_take(lv_obj_t *obj, lv_color_format_t format);
lv_draw_buf_t * __wrap_lv_snapshot_take(lv_obj_t *obj, lv_color_format_t format) {
    snapshot_calls++;
    return __real_lv_snapshot_take(obj, format);
}

static void flush_ready(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    (void) area;
    (void) pixels;
    lv_display_flush_ready(display);
}

static void read_input(lv_indev_t *indev, lv_indev_data_t *data) {
    (void) indev;
    data->state = test_input_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = 10;
    data->point.y = 10;
}

static void pass_ms(uint32_t ms) {
    lv_tick_inc(ms);
    (void) lv_timer_handler();
}

static void schedule_test_transition_cb(lv_timer_t *timer) {
    (void)timer;
    fixed_transition_schedule_start(scheduled_test_ctx);
    scheduled_test_ctx = NULL;
}

static slide_transition_ctx_t *make_test_transition(lv_obj_t *from, lv_obj_t *to,
                                                     lv_obj_t *moving_image,
                                                     lv_obj_t *destination_image) {
    slide_transition_ctx_t *ctx = lv_malloc(sizeof(*ctx));
    assert(ctx);
    memset(ctx, 0, sizeof(*ctx));
    ctx->from_scr = from;
    ctx->to_scr = to;
    ctx->img_from = moving_image;
    ctx->img_to = destination_image;
    ctx->to_offset = 100;
    ctx->commit = true;
    slide_transition_active = true;
    return ctx;
}

static void test_fixed_transition_start_uses_fresh_tick(lv_obj_t *from, lv_obj_t *to) {
    lv_obj_t *moving = lv_obj_create(lv_layer_top());
    lv_obj_t *destination = lv_obj_create(lv_layer_top());
    assert(moving && destination);
    lv_obj_set_pos(moving, 7, 0);
    scheduled_test_ctx = make_test_transition(from, to, moving, destination);
    slide_transition_ctx_t *active = scheduled_test_ctx;
    lv_timer_t *launcher = lv_timer_create(schedule_test_transition_cb, 1, NULL);
    assert(launcher);
    lv_timer_set_repeat_count(launcher, 1);

    /* The launcher creates the one-shot while lv_timer_handler is already
     * running. The restarted timer-list traversal sees the same frozen tick,
     * so the transition cannot start in that stale handler. */
    pass_ms(1);
    assert(fixed_transition_start_timer);
    assert(lv_anim_get(active, slide_transition_anim_x_cb) == NULL);
    pass_ms(1);
    assert(fixed_transition_start_timer == NULL);
    lv_anim_t *started = lv_anim_get(active, slide_transition_anim_x_cb);
    assert(started);
    assert(started->last_timer_run == lv_tick_get());
    assert(started->start_value == 0 && started->end_value == -100);
    assert(started->duration == NAV_ANIM_TIME_MS);
    pass_ms(16);
    started = lv_anim_get(active, slide_transition_anim_x_cb);
    assert(started);
    assert(started->act_time > 0 && started->act_time < started->duration);
    slide_transition_cancel(&active);

    /* Cancellation while a second fixed start is pending deletes its timer
     * before freeing the context; later handlers must not invoke it. */
    slide_transition_ctx_t *pending = make_test_transition(from, to, moving, destination);
    fixed_transition_schedule_start(pending);
    assert(fixed_transition_start_timer);
    slide_transition_cancel(&pending);
    assert(fixed_transition_start_timer == NULL);
    assert(fixed_transition_start_ctx == NULL);
    assert(!slide_transition_active);
    pass_ms(10);
    lv_obj_delete(moving);
    lv_obj_delete(destination);
}

static void test_fixed_transition_teardown_cancels_pending(void) {
    lv_obj_t *from = lv_obj_create(NULL);
    lv_obj_t *to = lv_obj_create(NULL);
    lv_obj_t *moving = lv_obj_create(lv_layer_top());
    lv_obj_t *destination = lv_obj_create(lv_layer_top());
    assert(from && to && moving && destination);
    slide_transition_ctx_t *pending = make_test_transition(from, to, moving, destination);
    fixed_transition_schedule_start(pending);
    assert(fixed_transition_start_timer);
    gui_navigation_teardown();
    assert(fixed_transition_start_timer == NULL);
    assert(fixed_transition_start_ctx == NULL);
    assert(!slide_transition_active);
    pass_ms(10);
    lv_obj_delete(moving);
    lv_obj_delete(destination);
    lv_obj_delete(from);
    lv_obj_delete(to);
}

static void test_fixed_transition_oom_paths(lv_obj_t *from, lv_obj_t *to) {
    lv_obj_t *moving = lv_obj_create(lv_layer_top());
    lv_obj_t *destination = lv_obj_create(lv_layer_top());
    assert(moving && destination);

    /* Timer OOM falls back to starting the animation immediately. */
    slide_transition_ctx_t *ctx = make_test_transition(from, to, moving, destination);
    fail_next_timer_create = true;
    fixed_transition_schedule_start(ctx);
    assert(!fixed_transition_start_timer);
    assert(lv_anim_get(ctx, slide_transition_anim_x_cb));
    slide_transition_cancel(&ctx);

    /* Animation OOM commits the destination and releases the transition. */
    ctx = make_test_transition(from, to, moving, destination);
    fail_next_anim_start = true;
    fixed_transition_schedule_start(ctx);
    assert(fixed_transition_start_timer);
    pass_ms(1);
    assert(!fixed_transition_start_timer);
    assert(!slide_transition_active);
    assert(lv_screen_active() == to);
    assert(lv_anim_count_running() == 0);
    lv_obj_delete(moving);
    lv_obj_delete(destination);
}

static uint16_t expected_565(uint32_t color) {
    lv_color_t c = lv_color_hex(color);
    return lv_color_to_u16(c);
}

static uint16_t snapshot_pixel(lv_draw_buf_t *buffer) {
    assert(buffer);
    uint16_t *pixel = lv_draw_buf_goto_xy(buffer, 20, 20);
    assert(pixel);
    return *pixel;
}

static void set_all_screen_colors(lv_obj_t **screens, uint32_t rgb) {
    for (int i = 0; i < STATIC_SNAPSHOT_SCREEN_COUNT; ++i)
        lv_obj_set_style_bg_color(screens[i], lv_color_hex(rgb), LV_PART_MAIN);
}

static void test_coalescing_slicing_and_latest_pixels(lv_obj_t **screens) {
    unsigned initial = snapshot_calls;
    set_all_screen_colors(screens, 0xCC2200);
    gui_navigation_invalidate_theme_snapshots();
    assert(snapshot_calls == initial);
    for (int i = 0; i < STATIC_SNAPSHOT_SCREEN_COUNT; ++i) assert(!static_snapshot_buf[i]);

    set_all_screen_colors(screens, 0x0044CC);
    pass_ms(100);
    assert(snapshot_calls == initial);
    gui_navigation_invalidate_theme_snapshots(); /* Trailing invalidation coalesces. */
    set_all_screen_colors(screens, 0x20A060);
    pass_ms(200);
    assert(snapshot_calls == initial);
    pass_ms(100);
    assert(snapshot_calls == initial + 1);
    assert(static_snapshot_buf[0]);
    assert(snapshot_pixel(static_snapshot_buf[0]) == expected_565(0x20A060));
    for (int i = 1; i < STATIC_SNAPSHOT_SCREEN_COUNT; ++i) assert(!static_snapshot_buf[i]);

    /* Each later eligible timer pass performs only one additional snapshot. */
    pass_ms(THEME_SNAPSHOT_REBUILD_PERIOD_MS);
    assert(snapshot_calls == initial + 2);
    assert(static_snapshot_buf[1]);
}

static void test_input_and_transition_defer(void) {
    unsigned before = snapshot_calls;
    lv_indev_t *indev = lv_indev_get_next(NULL);
    assert(indev);
    test_input_pressed = true;
    lv_indev_read(indev);
    assert(lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED);
    pass_ms(THEME_SNAPSHOT_REBUILD_PERIOD_MS);
    pass_ms(THEME_SNAPSHOT_REBUILD_PERIOD_MS);
    assert(snapshot_calls == before);
    test_input_pressed = false;
    lv_indev_read(indev);
    assert(lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED);
    pass_ms(THEME_SNAPSHOT_REBUILD_PERIOD_MS);
    assert(snapshot_calls == before + 1);

    before = snapshot_calls;
    slide_transition_active = true;
    pass_ms(THEME_SNAPSHOT_REBUILD_PERIOD_MS);
    assert(snapshot_calls == before);
    slide_transition_active = false;
    pass_ms(THEME_SNAPSHOT_REBUILD_PERIOD_MS);
    assert(snapshot_calls == before + 1);
}

static void test_screen_off_resume_and_teardown(lv_obj_t **screens, lv_indev_t *indev) {
    unsigned before = snapshot_calls;
    test_screen_on = false;
    gui_navigation_poll_snapshot_rebuild();
    assert(lv_timer_get_paused(theme_snapshot_rebuild_timer));
    pass_ms(1000);
    assert(snapshot_calls == before);
    test_screen_on = true;
    gui_navigation_poll_snapshot_rebuild();
    assert(!lv_timer_get_paused(theme_snapshot_rebuild_timer));
    pass_ms(THEME_SNAPSHOT_REBUILD_PERIOD_MS);
    assert(snapshot_calls == before + 1);

    gui_navigation_teardown();
    assert(theme_snapshot_rebuild_timer == NULL);
    for (int i = 0; i < STATIC_SNAPSHOT_SCREEN_COUNT; ++i) {
        assert(static_snapshot_buf[i] == NULL);
        assert(static_snapshot_screen[i] == NULL);
    }
    before = snapshot_calls;
    for (int i = 0; i < 5; ++i) pass_ms(THEME_SNAPSHOT_REBUILD_PERIOD_MS);
    assert(snapshot_calls == before);

    lv_indev_delete(indev);
    for (int i = 0; i < STATIC_SNAPSHOT_SCREEN_COUNT; ++i) lv_obj_delete(screens[i]);
}

int main(void) {
    lv_init();
    lv_tick_set_cb(NULL);
    lv_display_t *display = lv_display_create(100, 100);
    assert(display);
    static uint8_t display_buffer[100 * 20 * 4];
    lv_display_set_buffers(display, display_buffer, NULL, sizeof(display_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_ready);

    lv_indev_t *indev = lv_indev_create();
    assert(indev);
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, display);
    lv_indev_set_read_cb(indev, read_input);

    lv_obj_t *screens[STATIC_SNAPSHOT_SCREEN_COUNT];
    for (int i = 0; i < STATIC_SNAPSHOT_SCREEN_COUNT; ++i) {
        screens[i] = lv_obj_create(NULL);
        assert(screens[i]);
        lv_obj_set_size(screens[i], 100, 100);
        lv_obj_set_style_bg_opa(screens[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(screens[i], lv_color_hex(0x112233), LV_PART_MAIN);
        register_static_snapshot(i, screens[i]);
        assert(static_snapshot_buf[i]);
    }
    assert(snapshot_calls == STATIC_SNAPSHOT_SCREEN_COUNT);

    test_coalescing_slicing_and_latest_pixels(screens);
    test_input_and_transition_defer();
    test_fixed_transition_start_uses_fresh_tick(screens[0], screens[1]);
    test_fixed_transition_oom_paths(screens[0], screens[1]);
    test_screen_off_resume_and_teardown(screens, indev);
    test_fixed_transition_teardown_cancels_pending();
    puts("gui_navigation snapshot scheduler runtime spec passed");
    return 0;
}
