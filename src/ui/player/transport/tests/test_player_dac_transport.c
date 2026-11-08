#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "src/core/persistence/settings.h"
#include "src/hardware/usb/usb_mode_control.h"
#include "src/ui/player/screen/gui_player.h"

player_settings_t current_settings;
bool bt_is_powered_cached;
static int toasts, mutations, starts;
static bool playing, dac_owned;
bool gui_shell_bt_dac_owns_output(void) { return dac_owned; }
static int selected = 3;
static uint64_t revision = 17;
const char *i18n_tr(const char *s) { return s; }
void show_error_toast(const char *s) { assert(s && *s); toasts++; }
bool audio_consume_track_advanced(void) { mutations++; return false; }
void __wrap_gui_player_handle_auto_advance(void) { mutations++; }
bool player_queue_model_select_displayed(uint64_t rev, int index, int mode, int *physical) {
    (void) mode;
    if (rev != revision) return false;
    selected = index; revision++; mutations++; *physical = index; return true;
}
bool player_queue_model_restart_displayed(int index, int mode, int *physical) {
    (void)mode; selected = index; revision++; mutations++; *physical = index; return true;
}
void __wrap_play_track_at(int index) { (void)index; starts++; playing = true; }
int main(void) {
    current_settings.bt_dac_mode_enabled = true;
    bt_is_powered_cached = true;
    assert(gui_player_local_playback_allowed());
    assert(!gui_player_transport_button(0));
    dac_owned = true;
    for (int direction = -1; direction <= 1; direction++) assert(gui_player_transport_button(direction));
    assert(toasts == 0 && mutations == 0 && starts == 0);
    assert(!gui_player_local_playback_allowed());
    assert(!gui_player_queue_select(revision, 7));
    assert(!gui_player_queue_restart_displayed(7));
    assert(selected == 3 && revision == 17 && mutations == 0 && !playing && starts == 0);
    bt_is_powered_cached = false; /* A failed power query cannot unlock an active receiver. */
    assert(gui_player_transport_button(0));
    assert(!gui_player_local_playback_allowed());
    assert(!gui_player_queue_select(revision, 7));
    assert(selected == 3 && revision == 17 && !playing && starts == 0);
    dac_owned = false; /* Armed idle allows local transport and explicit play. */
    assert(!gui_player_transport_button(0));
    assert(gui_player_local_playback_allowed());
    assert(gui_player_queue_select(revision, 7));
    assert(selected == 7 && revision == 18 && playing && starts == 1);
    current_settings.usb_mode = USB_MODE_DAC;
    assert(!gui_player_local_playback_allowed());
    assert(!gui_player_queue_select(revision, 9));
    assert(selected == 7 && revision == 18 && starts == 1);
    puts("player DAC transport/refusal tests passed");
}
