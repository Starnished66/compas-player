#!/usr/bin/env python3
"""Compile the production lyrics screen timeout policy against strict mocks."""
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
source = (ROOT / "src/ui/gui.c").read_text()
start = source.index("    uint32_t screen_inactive_ms = lv_display_get_inactive_time(NULL);")
end = source.index("    /* Battery:", start)
timeout_block = source[start:end]

test_source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct lv_obj_t { int id; } lv_obj_t;
typedef struct {
    bool lyrics_keep_screen_on;
    bool screen_timeout_enabled;
    int screen_timeout_seconds;
    bool screen_dimming_enabled;
    int screen_dim_delay_seconds;
} player_settings_t;

static player_settings_t current_settings;
static lv_obj_t lyrics_view = { 1 };
static lv_obj_t * active_screen;
static lv_obj_t * lyrics_screen;
static bool playing;
static bool screen_on;
static bool dimmed;
static bool inactivity_dimmed;
static bool interactive_ui_started;
static bool lyrics_timeout_held;
static uint32_t inactive_ms;
static uint32_t interactive_ui_start_tick;
static unsigned dim_on_calls;
static unsigned dim_off_calls;
static unsigned power_off_calls;
static unsigned power_on_calls;
static unsigned reset_baseline_calls;

static uint32_t lv_display_get_inactive_time(void *display) {
    (void) display;
    return inactive_ms;
}
static uint32_t lv_tick_elaps(uint32_t tick) { (void) tick; return inactive_ms; }
static bool backlight_screen_is_on(void) { return screen_on; }
static void backlight_set_dimmed(bool value) {
    dimmed = value;
    if (value) dim_on_calls++;
    else dim_off_calls++;
}
static void request_screen_power(bool on) {
    if (on) power_on_calls++;
    else { power_off_calls++; screen_on = false; }
}
static void gui_reset_interactive_timeout_baseline(void) { reset_baseline_calls++; }
static lv_obj_t *gui_lyrics_get_screen(void) { return lyrics_screen; }
static lv_obj_t *lv_screen_active(void) { return active_screen; }
static bool audio_is_playing(void) { return playing; }

static void tick_timeout_policy(void) {
    __TIMEOUT_BLOCK__
}

static void reset_state(void) {
    current_settings = (player_settings_t) {
        .lyrics_keep_screen_on = true,
        .screen_timeout_enabled = true,
        .screen_timeout_seconds = 2,
        .screen_dimming_enabled = true,
        .screen_dim_delay_seconds = 1,
    };
    active_screen = lyrics_screen = &lyrics_view;
    playing = true;
    screen_on = true;
    dimmed = false;
    inactivity_dimmed = false;
    interactive_ui_started = false;
    lyrics_timeout_held = false;
    inactive_ms = 0;
    interactive_ui_start_tick = 0;
    dim_on_calls = dim_off_calls = power_off_calls = power_on_calls = 0;
    reset_baseline_calls = 0;
}

static void normal_idle_to_dim_and_off(void) {
    inactive_ms = 1000;
    tick_timeout_policy();
    assert(dimmed && inactivity_dimmed && dim_on_calls == 1);
    assert(screen_on && power_off_calls == 0);
    inactive_ms = 2000;
    tick_timeout_policy();
    assert(!screen_on && power_off_calls == 1);
}

int main(void) {
    reset_state();
    inactive_ms = 10000;
    tick_timeout_policy();
    assert(screen_on && !dimmed && dim_on_calls == 0 && power_off_calls == 0);
    assert(lyrics_timeout_held);

    /* Pausing restores a fresh normal timeout countdown. */
    playing = false;
    tick_timeout_policy();
    assert(!lyrics_timeout_held && inactive_ms == 10000);
    assert(reset_baseline_calls == 1 && screen_on && !dimmed);
    inactive_ms = 999;
    tick_timeout_policy();
    assert(screen_on && !dimmed && power_off_calls == 0);
    inactive_ms = 1000;
    tick_timeout_policy();
    assert(dimmed && screen_on);
    inactive_ms = 2000;
    tick_timeout_policy();
    assert(!screen_on && power_off_calls == 1);

    /* Leaving the lyrics page while playing also restarts normal timing. */
    reset_state();
    inactive_ms = 5000;
    tick_timeout_policy();
    assert(lyrics_timeout_held);
    active_screen = NULL;
    tick_timeout_policy();
    assert(!lyrics_timeout_held && reset_baseline_calls == 1);
    inactive_ms = 1000;
    tick_timeout_policy();
    assert(dimmed && screen_on);
    inactive_ms = 2000;
    tick_timeout_policy();
    assert(!screen_on && power_off_calls == 1);

    /* Disabling the preference restores normal timeout behavior in lyrics. */
    reset_state();
    current_settings.lyrics_keep_screen_on = false;
    normal_idle_to_dim_and_off();

    /* Resuming playback removes an existing inactivity dim. */
    reset_state();
    playing = false;
    inactive_ms = 1000;
    tick_timeout_policy();
    assert(dimmed && screen_on);
    playing = true;
    tick_timeout_policy();
    assert(!dimmed && !inactivity_dimmed && dim_off_calls == 1 && screen_on);
    assert(power_off_calls == 0);

    /* An explicitly powered-off display is never woken by the policy. */
    reset_state();
    screen_on = false;
    inactive_ms = 10000;
    tick_timeout_policy();
    assert(!screen_on && power_on_calls == 0 && power_off_calls == 0);
    assert(!lyrics_timeout_held);

    /* A missing lyrics screen receives no exemption. */
    reset_state();
    lyrics_screen = NULL;
    normal_idle_to_dim_and_off();

    /* Disabling the global timeout still permits dimming but prevents poweroff. */
    reset_state();
    current_settings.screen_timeout_enabled = false;
    current_settings.lyrics_keep_screen_on = false;
    inactive_ms = 5000;
    tick_timeout_policy();
    assert(dimmed && screen_on && power_off_calls == 0);

    puts("lyrics screen timeout regression passed");
    return 0;
}
'''.replace("__TIMEOUT_BLOCK__", timeout_block)

with tempfile.TemporaryDirectory(prefix="compas-lyrics-timeout-") as folder:
    folder = pathlib.Path(folder)
    c_file = folder / "test.c"
    binary = folder / "test"
    c_file.write_text(test_source)
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
        str(c_file), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
