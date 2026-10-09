#define _POSIX_C_SOURCE 200809L
#include "gui_navigation.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>

player_settings_t current_settings;
static bool test_screen_on = true;
static bool test_input_pressed;
static unsigned snapshot_calls;

bool backlight_screen_is_on(void) { return test_screen_on; }
uint32_t gui_anim_ms(uint32_t ms) { return ms; }
lv_obj_t * gui_player_get_screen(void) { return NULL; }
lv_obj_t * gui_player_get_dismiss_btn(void) { return NULL; }

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
    test_screen_off_resume_and_teardown(screens, indev);
    puts("gui_navigation snapshot scheduler runtime spec passed");
    return 0;
}
