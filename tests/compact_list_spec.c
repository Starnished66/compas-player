/* Focused lifecycle spec for the production compact list. Compile with
 * COMPACT_LIST_SPEC_BASELINE to source-include the pre-extraction TU; the
 * final form includes both owning TUs so the implementation stays real. */
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"
#include "src/core/settings.h"
#include "src/ui/assets.h"
#include "src/ui/fallback_font.h"
#include "src/ui/gui_theme.h"
#include "src/ui/ui_fonts.h"

#ifdef COMPACT_LIST_BASELINE_SOURCE
#include COMPACT_LIST_BASELINE_SOURCE
#elif defined(COMPACT_LIST_SPEC_BASELINE)
#include "../src/ui/screen_builders.c"
#else
#include "../src/ui/screen_builders.c"
#include "../src/ui/compact_list.c"
#endif

extern player_settings_t current_settings;

static int clicked_index = -1;
static int long_pressed_index = -1;

static void on_click(int index) { clicked_index = index; }
static void on_long_press(int index) { long_pressed_index = index; }

static void flush_ready(lv_display_t * display, const lv_area_t * area, uint8_t * pixels) {
    LV_UNUSED(area);
    LV_UNUSED(pixels);
    lv_display_flush_ready(display);
}

static void test_eager_recycling(void) {
    static const compact_list_item_t initial[] = {
        { .label = "first" }, { .label = "second" },
    };
    static const compact_list_item_t replacement[] = {
        { .label = "replacement" }, { .label = "other" },
    };
    lv_obj_t * list = build_compact_list_widget(lv_screen_active(), initial, 2,
                                                 on_click, on_long_press,
                                                 LIST_ROW_WIDTH, false, NULL);
    assert(list);
    compact_list_virtual_data_t * data = lv_obj_get_user_data(list);
    assert(data && data->row_logical_index[0] == 0);
    lv_obj_t * recycled_row = data->rows[0];

    lv_obj_send_event(recycled_row, LV_EVENT_LONG_PRESSED, NULL);
    assert(long_pressed_index == 0);
    lv_obj_send_event(recycled_row, LV_EVENT_CLICKED, NULL);
    assert(clicked_index == -1); /* release click after long press is suppressed */

    compact_list_set_items(list, replacement, 2);
    assert(data->rows[0] == recycled_row && data->row_logical_index[0] == 0);
    lv_obj_send_event(recycled_row, LV_EVENT_CLICKED, NULL);
    assert(clicked_index == 0);

    lv_obj_delete(list);
    compact_list_set_items(list, initial, 2); /* stale-list guard must return safely */
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool entered;
    bool release;
    bool finished;
} blocked_fetch_t;

static int blocking_fetch(void * opaque, int offset, int count,
                          compact_list_page_row_t out_rows[]) {
    blocked_fetch_t * state = opaque;
    pthread_mutex_lock(&state->mutex);
    state->entered = true;
    pthread_cond_broadcast(&state->changed);
    while(!state->release) pthread_cond_wait(&state->changed, &state->mutex);
    pthread_mutex_unlock(&state->mutex);

    int result = count < 2 ? count : 2;
    for(int i = 0; i < result; ++i) {
        out_rows[i].identity = offset + i;
        snprintf(out_rows[i].label, sizeof(out_rows[i].label), "row-%d", offset + i);
    }
    pthread_mutex_lock(&state->mutex);
    state->finished = true;
    pthread_cond_broadcast(&state->changed);
    pthread_mutex_unlock(&state->mutex);
    return result;
}

static void wait_for_fetch(blocked_fetch_t * state, bool finished) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    pthread_mutex_lock(&state->mutex);
    while(!(finished ? state->finished : state->entered)) {
        int result = pthread_cond_timedwait(&state->changed, &state->mutex, &deadline);
        assert(result == 0);
    }
    pthread_mutex_unlock(&state->mutex);
}

static void test_inflight_fetch_delete(void) {
    blocked_fetch_t state = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
    };
    lv_obj_t * list = build_compact_list_widget(lv_screen_active(), NULL, 0,
                                                 on_click, NULL,
                                                 LIST_ROW_WIDTH, false, NULL);
    assert(list);
    compact_list_set_paged_provider(list, blocking_fetch, &state, 500);
    wait_for_fetch(&state, false);
    lv_obj_delete(list); /* data dies; worker and callback context remain valid */

    pthread_mutex_lock(&state.mutex);
    state.release = true;
    pthread_cond_broadcast(&state.changed);
    pthread_mutex_unlock(&state.mutex);
    wait_for_fetch(&state, true);
    pthread_cond_destroy(&state.changed);
    pthread_mutex_destroy(&state.mutex);
}

int main(void) {
    player_settings_t loaded;
    settings_load(&loaded);
    current_settings = loaded;
    lv_init();
    assets_init();
    assert(ui_fonts_init());
    lv_display_t * display = lv_display_create(480, 800);
    assert(display);
    static uint8_t display_buffer[480 * 24 * 4];
    lv_display_set_buffers(display, display_buffer, NULL, sizeof(display_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_ready);
    gui_theme_init();
    fallback_font_load_now();
    lv_obj_t * screen = lv_obj_create(NULL);
    lv_screen_load(screen);
    test_eager_recycling();
    test_inflight_fetch_delete();
    puts("PASS: compact-list eager recycling, click/long-press, stale guard, and inflight fetch deletion");
    return 0;
}
