#!/usr/bin/env python3
"""Exercise the production GUI reload scheduler with mocked LVGL callbacks."""
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
src = (root / 'src/ui/gui_reload.c').read_text()


def function(name):
    match = re.search(r'^(?:static )?(?:int|void|bool) ' + name +
                      r'\([^;]*?\) \{.*?^\}', src, re.M | re.S)
    if not match:
        raise RuntimeError('missing production function ' + name)
    return match.group()


constants = re.search(r'^#define RELOAD_RETRY_PERIOD_MS .*?^#define RELOAD_COOLDOWN_MS .*?^\n',
                      src, re.M | re.S)
assert constants, 'missing production reload timing constants'
functions = '\n'.join(function(name) for name in (
    'reload_trigger_cb', 'gui_reload_schedule', 'gui_reload_request',
    'gui_reload_request_preserving_plugin_manager_page',
    'gui_player_layout_reload_request'))

body = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct lv_timer_t { uint32_t period; bool deleted; } lv_timer_t;
typedef struct { int pages[4]; int depth; int32_t plugin_manager_scroll_y; } gui_nav_reload_state_t;
typedef void (*lv_timer_cb_t)(lv_timer_t *);
#define LV_LOG_WARN(...) ((void)0)
''' + constants.group() + r'''

static bool reload_scheduled;
static bool reload_in_progress;
static bool reload_preserve_artwork;
static bool reload_restore_plugin_manager;
static uint32_t last_reload_tick;
static lv_timer_t timer_slots[8];
static unsigned timer_count;
static lv_timer_t *pending_timer;
static uint32_t now_tick;
static bool transition_blocked;
static bool library_blocked;
static bool rebuild_result[8];
static unsigned rebuild_result_count, rebuild_calls;
static uint32_t rebuild_widths[8], rebuild_heights[8];
static uint32_t display_width = 320, display_height = 240;
static unsigned soft_reload_calls, capture_calls;
static bool captured_path;
static bool soft_reload_got_path;
static bool soft_reload_got_preserve;

static uint32_t lv_tick_get(void) { return now_tick; }
static uint32_t lv_tick_elaps(uint32_t since) { return now_tick - since; }
static bool gui_navigation_transition_in_progress(void) { return transition_blocked; }
static bool gui_library_navigation_blocked(void) { return library_blocked; }
static uint32_t reload_display_width(void) { return display_width; }
static uint32_t reload_display_height(void) { return display_height; }
static lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *data) {
    (void)cb; (void)data;
    if (timer_count >= sizeof(timer_slots) / sizeof(timer_slots[0])) return NULL;
    lv_timer_t *timer = &timer_slots[timer_count++];
    timer->period = period; timer->deleted = false;
    pending_timer = timer;
    return timer;
}
static void lv_timer_del(lv_timer_t *timer) { timer->deleted = true; }
static void lv_timer_set_period(lv_timer_t *timer, uint32_t period) { timer->period = period; }
static bool gui_player_rebuild_layout(uint32_t width, uint32_t height) {
    rebuild_widths[rebuild_calls] = width;
    rebuild_heights[rebuild_calls] = height;
    unsigned index = rebuild_calls++;
    return index < rebuild_result_count ? rebuild_result[index] : true;
}
static bool gui_navigation_capture_plugin_manager_path(gui_nav_reload_state_t *state) {
    capture_calls++;
    state->depth = 3; state->pages[0] = 10; state->pages[1] = 20;
    state->pages[2] = 30; state->plugin_manager_scroll_y = 77;
    return captured_path;
}
static void gui_soft_reload_with_artwork(bool preserve, const gui_nav_reload_state_t *state) {
    soft_reload_calls++;
    soft_reload_got_preserve = preserve;
    soft_reload_got_path = state && state->depth == 3 && state->pages[2] == 30 &&
                           state->plugin_manager_scroll_y == 77;
}

''' + functions + r'''

static void reset(void) {
    memset(timer_slots, 0, sizeof(timer_slots)); timer_count = 0; pending_timer = NULL;
    reload_scheduled = reload_in_progress = reload_preserve_artwork = false;
    reload_restore_plugin_manager = false; last_reload_tick = 0; now_tick = 1000;
    transition_blocked = library_blocked = false;
    memset(rebuild_result, 0, sizeof(rebuild_result)); rebuild_result_count = rebuild_calls = 0;
    memset(rebuild_widths, 0, sizeof(rebuild_widths)); memset(rebuild_heights, 0, sizeof(rebuild_heights));
    display_width = 320; display_height = 240;
    soft_reload_calls = capture_calls = 0; captured_path = true;
    soft_reload_got_path = soft_reload_got_preserve = false;
}

static void fire_pending(void) { assert(pending_timer && !pending_timer->deleted); reload_trigger_cb(pending_timer); }

int main(void) {
    /* Layout-only refresh ignores a long library scan and leaves the full
     * reload machinery, plugin manager, and artwork lifecycle untouched. */
    reset(); library_blocked = true;
    gui_player_layout_reload_request();
    assert(reload_scheduled && reload_preserve_artwork && pending_timer->period == RELOAD_RETRY_PERIOD_MS);
    display_width = 480; display_height = 272;
    fire_pending();
    assert(rebuild_calls == 1 && rebuild_widths[0] == 480 && rebuild_heights[0] == 272);
    assert(soft_reload_calls == 0 && capture_calls == 0);
    assert(!reload_scheduled && pending_timer->deleted && !reload_preserve_artwork);

    /* Layout changes bypass the full-reload cooldown. Player interactions
     * still defer the queued request; retries see the latest dimensions. */
    reset(); last_reload_tick = now_tick - 100;
    rebuild_result[0] = false; rebuild_result[1] = true; rebuild_result_count = 2;
    gui_player_layout_reload_request();
    transition_blocked = true; fire_pending();
    assert(rebuild_calls == 0 && reload_scheduled && !pending_timer->deleted);
    transition_blocked = false; display_width = 800; display_height = 480;
    fire_pending();
    assert(rebuild_calls == 1 && rebuild_widths[0] == 800 && rebuild_heights[0] == 480);
    assert(reload_scheduled && !pending_timer->deleted);
    display_width = 1024; display_height = 600;
    fire_pending();
    assert(rebuild_calls == 2 && rebuild_widths[1] == 1024 && rebuild_heights[1] == 600);
    assert(!reload_scheduled && pending_timer->deleted && soft_reload_calls == 0);

    /* A full request coalesced with a queued layout request takes precedence.
     * Library scans defer it at the slower retry cadence, then plugin-manager
     * path restoration runs through the existing full reload path. */
    reset(); library_blocked = true;
    gui_player_layout_reload_request();
    gui_reload_request();
    assert(reload_scheduled && !reload_preserve_artwork);
    assert(gui_reload_request_preserving_plugin_manager_page());
    fire_pending();
    assert(pending_timer->period == RELOAD_BUSY_RETRY_MS && reload_scheduled);
    assert(soft_reload_calls == 0 && capture_calls == 0 && !pending_timer->deleted);
    library_blocked = false;
    fire_pending();
    assert(rebuild_calls == 0 && soft_reload_calls == 1 && capture_calls == 1);
    assert(soft_reload_got_path && !soft_reload_got_preserve);
    assert(!reload_scheduled && pending_timer->deleted && !reload_restore_plugin_manager);
    assert(last_reload_tick == now_tick);

    /* A layout request arriving while a full reload is queued coalesces into
     * that full reload rather than scheduling a second timer. */
    reset();
    gui_reload_request(); gui_player_layout_reload_request();
    assert(timer_count == 1 && reload_scheduled && !reload_preserve_artwork);
    fire_pending();
    assert(soft_reload_calls == 1 && rebuild_calls == 0);

    puts("gui player layout reload scheduler regression passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='compas-player-layout-reload-') as folder:
    folder = pathlib.Path(folder)
    c_file = folder / 'test.c'
    binary = folder / 'test'
    c_file.write_text(body)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(c_file), '-o', str(binary)], check=True, cwd=root)
    subprocess.run([str(binary)], check=True, cwd=root)
