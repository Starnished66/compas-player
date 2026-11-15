#include "src/ui/application/gui.h"
#include "src/ui/network/gui_network.h"
#include "src/ui/network/gui_network_internal.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "src/ui/player/screen/gui_player_internal.h"
#include "src/ui/player/queue/player_queue_model.h"
#include "src/ui/text/fonts/ui_fonts.h"
static pthread_t ui_thread;
static atomic_int starts, stops, controls;
static atomic_bool stream_active, start_fail, hold_start;
static int refreshes, errors, local_starts;
static double local_position;
static bool playing, paused, screen_checked;
static int pause_toggles, screen_refreshes;
static bool allow_stuck_bridge;
static atomic_bool receiver_active;
static atomic_bool bridge_stopped;
static bool usb_owned, usb_pending;
lv_obj_t *__wrap_add_pill_toggle_row(lv_obj_t *parent, const char *text, bool checked, lv_event_cb_t cb) {
    (void)cb;
    if (!strcmp(text, "AirPlay")) { screen_checked = checked; screen_refreshes++; }
    return lv_obj_create(parent);
}
bool __wrap_airplay_control_is_active(void) { return atomic_load(&receiver_active); }
bool __wrap_audio_is_playing(void) { return playing; }
bool __wrap_audio_is_paused(void) { return paused; }
void __wrap_audio_toggle_pause(void) { playing = !playing; paused = !playing; pause_toggles++; }
void __wrap_settings_save(const player_settings_t *settings) { (void)settings; }
void __wrap_gui_player_queue_checkpoint_urgent(void) {}
void __wrap_gui_shell_refresh_quick_drawer_expansion_toggles(void) { refreshes++; }
void __wrap_show_error_toast(const char *text) { assert(text && *text); errors++; }
bool __wrap_airplay_bridge_has_active_stream(void) { return atomic_load(&stream_active); }
bool __wrap_seed_track_metadata(int index, track_metadata_t *meta) {
    (void)index; memset(meta, 0, sizeof(*meta)); return true;
}
void __wrap_prepare_track_metadata(int i, bool n, bool p, bool a, bool s) {
    (void)i; (void)n; (void)p; (void)a; (void)s;
}
void __wrap_clear_selection_metadata_seed(void) {}
void __wrap_track_metadata_count_play(const char *path) { (void)path; }
void __wrap_request_track_audio_metadata(int i) { (void)i; }
void __wrap_arm_next_track_for_audio(int i) { (void)i; }
void __wrap_set_play_button_state(bool playing) { (void)playing; }
lv_obj_t *__wrap_gui_player_get_screen(void) { return lv_screen_active(); }
double __wrap_audio_get_resume_position_seconds(void) { return 42.5; }
void __wrap_audio_play_file_at_with_replaygain_mode(const char *p, double pos, int mode) {
    (void)p; (void)mode;
    assert(allow_stuck_bridge || atomic_load(&bridge_stopped));
    local_starts++; local_position = pos;
}
bool __wrap_wifi_feature_guard(void) { return true; }
void __wrap_settings_save_async(const player_settings_t *settings) { (void)settings; }
bool __wrap_gui_shell_usb_dac_owns_output(void) { return usb_owned; }
bool __wrap_gui_network_usb_dac_pending(void) { return usb_pending; }
void __wrap_disable_usb_dac_mode(void) { current_settings.usb_dac_mode_enabled = false; }
bool __wrap_gui_shell_bt_dac_busy(void) { return false; }
void __wrap_start_bt_apply_output_settings(bool enabled, bool sync) { (void)enabled; (void)sync; }
void __wrap_gui_network_extend_bt_pairing_window(void) {}
bool __wrap_airplay_control_start(const char *name) {
    (void)name;
    assert(!pthread_equal(ui_thread, pthread_self()));
    assert(atomic_fetch_add(&controls, 1) == 0);
    atomic_fetch_add(&starts, 1);
    while (atomic_load(&hold_start)) usleep(1000);
    bool ok = !atomic_load(&start_fail);
    if (ok) atomic_store(&bridge_stopped, false);
    atomic_store(&receiver_active, ok);
    atomic_fetch_sub(&controls, 1);
    return ok;
}
void __wrap_airplay_control_stop(void) {
    assert(!pthread_equal(ui_thread, pthread_self()));
    assert(atomic_fetch_add(&controls, 1) == 0);
    atomic_fetch_add(&stops, 1);
    atomic_store(&stream_active, false);
    atomic_store(&receiver_active, false);
    atomic_fetch_sub(&controls, 1);
}
bool __wrap_airplay_bridge_is_stopped(void) { return atomic_load(&bridge_stopped); }
static void settle(void) {
    for (int i = 0; i < 2000 && gui_network_airplay_stop_pending(); ++i) usleep(1000);
    assert(!gui_network_airplay_stop_pending());
}
static unsigned timer_count(void) {
    unsigned count = 0;
    for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) count++;
    return count;
}
static void await_local_start(int expected) {
    uint32_t start = lv_tick_get();
    while (local_starts < expected && lv_tick_elaps(start) < 150) {
        lv_timer_handler();
        usleep(1000);
    }
    assert(local_starts == expected);
}
int main(void) {
    lv_init();
    assert(ui_fonts_init());
    assert(lv_sdl_window_create(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT));
    ui_thread = pthread_self();
    settings_load(&current_settings);
    audio_init();
    gui_init(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) lv_timer_pause(t);
    current_settings.usb_mode = USB_MODE_STORAGE;
    current_settings.usb_dac_mode_enabled = true;
    usb_owned = usb_pending = true;
    assert(gui_network_toggle_airplay());
    assert(gui_network_airplay_enable_pending() && gui_network_airplay_active() && !atomic_load(&starts));
    assert(screen_checked);
    usb_owned = false;
    gui_network_poll_airplay_overlay();
    assert(!atomic_load(&starts));
    usb_pending = false;
    gui_network_poll_airplay_overlay();
    settle();
    assert(atomic_load(&starts) == 1);
    bt_is_powered_cached = true;
    enable_bt_dac_mode();
    assert(gui_network_bt_dac_active() && !current_settings.bt_dac_mode_enabled);
    gui_network_bt_dac_poll_enable();
    assert(!current_settings.bt_dac_mode_enabled);
    atomic_store(&bridge_stopped, true);
    settle();
    gui_network_bt_dac_poll_enable();
    assert(current_settings.bt_dac_mode_enabled && atomic_load(&stops) == 1);
    /* A newer AirPlay choice cancels a deferred Bluetooth enable. */
    disable_bt_dac_mode();
    assert(gui_network_toggle_airplay());
    settle();
    enable_bt_dac_mode();
    assert(!current_settings.bt_dac_mode_enabled);
    assert(gui_network_toggle_airplay());
    atomic_store(&bridge_stopped, true);
    settle();
    gui_network_poll_airplay_overlay();
    settle();
    assert(!current_settings.bt_dac_mode_enabled && current_settings.wifi_dac_mode_enabled);
    disable_airplay_mode();
    atomic_store(&bridge_stopped, true);
    settle();
    /* A delayed start and an off/on/off sequence stay under one owner. */
    atomic_store(&hold_start, true);
    assert(gui_network_toggle_airplay());
    for (int i = 0; i < 2000 && !atomic_load(&controls); ++i) usleep(1000);
    assert(atomic_load(&controls) == 1);
    assert(!gui_network_toggle_airplay());
    assert(gui_network_toggle_airplay() && gui_network_airplay_active());
    assert(!gui_network_toggle_airplay());
    atomic_store(&hold_start, false);
    for (int i = 0; i < 2000 && atomic_load(&controls); ++i) usleep(1000);
    atomic_store(&bridge_stopped, true);
    settle();
    assert(!gui_network_airplay_active());

    /* Local play waits for output, then restarts only the still-wanted receiver. */
    assert(gui_network_toggle_airplay());
    settle();
    char **paths = calloc(1, sizeof(*paths));
    paths[0] = strdup("/test/local.flac");
    player_queue_model_install_take(player_queue_install_new(paths, 1));
    atomic_store(&stream_active, true);
    int before = atomic_load(&starts);
    unsigned idle_timers = timer_count();
    play_track_at_from(0, 17.0);
    assert(timer_count() == idle_timers + 1);
    gui_player_poll_confirmed_playback();
    assert(local_starts == 0 && atomic_load(&starts) == before);
    atomic_store(&bridge_stopped, true);
    settle();
    await_local_start(1);
    assert(timer_count() == idle_timers);
    assert(local_starts == 1 && local_position == 17.0);
    gui_network_poll_airplay_overlay();
    settle();
    assert(atomic_load(&starts) == before + 1);

    /* Paused/resumed local audio keeps its checkpoint when AirPlay takes over. */
    atomic_store(&stream_active, true);
    toggle_play_pause();
    assert(local_starts == 1);
    disable_airplay_mode();
    before = atomic_load(&starts);
    atomic_store(&bridge_stopped, true);
    settle();
    await_local_start(2);
    gui_network_poll_airplay_overlay();
    assert(local_starts == 2 && local_position == 42.5);
    assert(atomic_load(&starts) == before && !gui_network_airplay_active());

    /* Turning off first must still defer local playback until release. */
    assert(gui_network_toggle_airplay());
    settle();
    atomic_store(&stream_active, true);
    disable_airplay_mode();
    play_track_at_from(0, 23.0);
    gui_player_poll_confirmed_playback();
    assert(local_starts == 2);
    atomic_store(&bridge_stopped, true);
    settle();
    gui_player_poll_confirmed_playback();
    assert(local_starts == 3 && local_position == 23.0);

    /* Stopping an idle receiver leaves ordinary local pause/resume intact. */
    assert(gui_network_toggle_airplay());
    settle();
    playing = true;
    disable_airplay_mode();
    toggle_play_pause();
    assert(!playing && paused && pause_toggles == 1 && local_starts == 3);
    toggle_play_pause();
    assert(playing && !paused && pause_toggles == 2 && local_starts == 3);
    playing = false;
    atomic_store(&bridge_stopped, true);
    settle();

    /* A stuck bridge cannot hold local play beyond the two-second budget. */
    assert(gui_network_toggle_airplay());
    settle();
    atomic_store(&stream_active, true);
    allow_stuck_bridge = true;
    play_track_at_from(0, 31.0);
    uint32_t handoff_start = lv_tick_get();
    while (local_starts == 3 && lv_tick_elaps(handoff_start) < 2200) {
        lv_timer_handler();
        usleep(1000);
    }
    assert(local_starts == 4 && local_position == 31.0);
    assert(lv_tick_elaps(handoff_start) >= 2000);
    assert(!atomic_load(&bridge_stopped));
    assert(gui_network_airplay_stop_pending());
    disable_airplay_mode();
    atomic_store(&bridge_stopped, true);
    settle();
    allow_stuck_bridge = false;

    /* Failed asynchronous enable updates the desired state and both controls. */
    atomic_store(&start_fail, true);
    assert(gui_network_toggle_airplay());
    int refreshed = refreshes, screen_refreshed = screen_refreshes;
    assert(screen_checked);
    settle();
    assert(!gui_network_airplay_active() && errors == 1 && refreshes > refreshed);
    assert(!screen_checked && screen_refreshes > screen_refreshed);
    puts("Receiver USB ownership, asynchronous teardown and Bluetooth cancellation passed");
}
