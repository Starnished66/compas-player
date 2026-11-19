/* An idle power action that loses eligibility while the panel stays dark
 * hands the USB DAC screen its policy back; a requested power-off keeps it. */
#include "src/ui/application/gui_screen_power.c"
#include <assert.h>

player_settings_t current_settings;
static usb_dac_power_action_t owner = USB_DAC_POWER_NONE;
static bool charging, pending, library_busy;
static uint32_t now_ms;
uint32_t lv_tick_get(void) { return now_ms; }
uint32_t lv_tick_elaps(uint32_t prev) { return now_ms - prev; }
/* Same contract as gui_network_usb.c: only the owner, and never after handoff. */
void gui_network_usb_dac_power_cancelled(usb_dac_power_action_t action) {
    if (action == owner && !pending) owner = USB_DAC_POWER_NONE;
}
bool idle_shutdown_pending(void) { return pending; }
bool battery_is_charging(void) { return charging; }
bool audio_is_playing(void) { return false; }
bool gui_network_usb_dac_pending(void) { return false; }
bool gui_shell_usb_dac_owns_output(void) { return false; }
bool gui_shell_bt_dac_in_use(void) { return false; }
bool gui_network_airplay_enable_pending(void) { return false; }
bool gui_lock_screen_has_background_work(bool b) { (void)b; return false; }
bool gui_library_has_background_work(void) { return library_busy; }
bool gui_subsonic_has_background_work(void) { return false; }
bool gui_network_has_background_work(void) { return false; }
bool gui_lyrics_has_background_work(void) { return false; }
bool gui_player_has_background_work(void) { return false; }
bool gui_shell_has_background_work(void) { return false; }
bool plugin_manager_has_background_work(void) { return false; }
bool playlist_files_has_active_write(void) { return false; }
bool screenshot_is_busy(void) { return false; }
bool firmware_ota_busy(void) { return false; }
bool plugin_store_busy(void) { return false; }
bool emulator_store_busy(void) { return false; }
bool gui_network_prepare_radio_sleep(void) { return true; }

/* The poll's first step once ready: its USB DAC wait, owned by idle. */
static void idle_waits(void) {
    assert(idle_power_action_ready(false));
    if (owner == USB_DAC_POWER_NONE) owner = USB_DAC_POWER_IDLE;
}

int main(void) {
    current_settings.idle_shutdown_enabled = true;
    current_settings.idle_shutdown_minutes = 1;
    now_ms = 120000; /* the panel has been dark long enough */
    idle_waits();
    library_busy = true; /* background work starts after the first wait */
    assert(!idle_power_action_ready(false) && owner == USB_DAC_POWER_NONE);
    library_busy = false;
    idle_waits();
    charging = true; /* a cable arrives, panel still dark */
    assert(!idle_power_action_ready(false) && owner == USB_DAC_POWER_NONE);
    charging = false;
    idle_waits();
    owner = USB_DAC_POWER_MENU; /* another action's wait is not idle's to end */
    library_busy = true;
    assert(!idle_power_action_ready(false) && owner == USB_DAC_POWER_MENU);
    library_busy = false;
    owner = USB_DAC_POWER_IDLE;
    pending = true; /* power-off already requested: keep the release */
    idle_shutdown_attempted = true;
    assert(!idle_power_action_ready(false) && owner == USB_DAC_POWER_IDLE);
    puts("Idle USB DAC power cancellation passed");
}
