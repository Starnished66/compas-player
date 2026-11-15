/* Run the real UI policy with deterministic gadget and cable observations. */
#include "src/ui/network/gui_network_usb.c"
#include <assert.h>

player_settings_t current_settings;
static battery_external_power_state_t power_state;
static bool host_present, bt_active, output_owned, scan_active, apply_ok = true;
static bool airplay_stopping;
static int saves, releases;
static uint32_t tick;
const char *i18n_tr(const char *s) { return s; }
void show_info_toast(const char *s) { (void)s; }
void show_error_toast(const char *s) { (void)s; }
void settings_save_async(const player_settings_t *s) { (void)s; saves++; }
void gui_network_usb_dac_refresh(void) {}
void gui_shell_refresh_quick_drawer_expansion_toggles(void) {}
void gui_shell_usb_dac_release(void) { releases++; }
bool gui_shell_usb_dac_owns_output(void) { return output_owned; }
bool gui_network_bt_dac_active(void) { return bt_active; }
bool gui_shell_bt_dac_busy(void) { return false; }
bool gui_network_airplay_enable_pending(void) { return false; }
bool gui_network_airplay_stop_pending(void) { return airplay_stopping; }
void disable_airplay_mode(void) { current_settings.wifi_dac_mode_enabled = false; airplay_stopping = true; }
void disable_bt_dac_mode(void) { bt_active = false; }
bool gui_library_scan_active(void) { return scan_active; }
bool firmware_update_busy(void) { return false; }
void gui_library_suspend_boot_prompt(void) {}
void gui_library_request_files_changed_prompt(void) {}
bool gui_library_auto_rescan_enabled(void) { return false; }
void gui_settings_sync_adb_toggle(void) {}
void gui_settings_sync_usb_dac_charging_toggle(void) {}
battery_external_power_state_t battery_get_external_power_state(void) { return power_state; }
bool usb_mode_control_host_connected(void) { return host_present; }
bool usb_mode_control_storage_host_configured(void) { return host_present && current_settings.usb_mode == USB_MODE_STORAGE; }
bool usb_mode_control_storage_write_active(void) { return false; }
bool usb_mode_control_detect_current(usb_mode_t *mode) { *mode = current_settings.usb_mode; return true; }
bool usb_mode_control_apply_with_status(usb_mode_t mode, bool *busy) { (void)mode; *busy = false; return apply_ok; }
void usb_dac_bridge_start(void) {}
void usb_dac_bridge_get_stream_info(usb_dac_stream_info_t *out) { *out = (usb_dac_stream_info_t){.bridge_running=true}; }
uint32_t lv_tick_get(void) { return tick += 500; }
uint32_t lv_tick_elaps(uint32_t prev) { return tick - prev; }
void lv_obj_clean(lv_obj_t *obj) { (void)obj; assert(false); }
lv_obj_t *add_pill_option_row(lv_obj_t *parent, const char *text, bool selected,
                             lv_event_cb_t cb, void *data) {
    (void)parent; (void)text; (void)selected; (void)cb; (void)data; assert(false); return NULL;
}
void lv_obj_remove_flag(lv_obj_t *obj, lv_obj_flag_t flag) { (void)obj; (void)flag; }
void lv_obj_set_style_opa(lv_obj_t *obj, lv_opa_t value, lv_style_selector_t sel) {
    (void)obj; (void)value; (void)sel;
}
lv_event_code_t lv_event_get_code(lv_event_t *e) { (void)e; return LV_EVENT_CLICKED; }
void *lv_event_get_user_data(lv_event_t *e) { (void)e; return NULL; }
void gui_network_open_usb_dac(void) {}
/* No screens are built; link-time GC retains only the policy under test. */
static void finish_switch(void) {
    if (usb_mode_switch_active) {
        while (!atomic_load(&usb_mode_switch_done_flag)) usleep(1000);
        poll_usb_mode_switch();
    }
}
static void reset(void) {
    finish_switch();
    current_settings = (player_settings_t){0};
    usb_dac_paused = true;
    usb_dac_pause_pending = usb_dac_restore_pending = usb_dac_teardown_failed = false;
    usb_cable_state_initialized = usb_cable_was_connected = false;
    usb_storage_rebind_pending = false;
    usb_storage_session = (usb_storage_session_t){0};
    power_state = BATTERY_EXTERNAL_POWER_DISCONNECTED;
    host_present = bt_active = output_owned = scan_active = false;
    apply_ok = true;
}
int main(void) {
    reset();
    host_present = true;
    current_settings.wifi_dac_mode_enabled = true;
    enable_usb_dac_mode();
    assert(airplay_stopping && usb_mode_switch_queued);
    poll_usb_mode_switch();
    assert(usb_mode_switch_queued);
    airplay_stopping = false;
    poll_usb_mode_switch();
    finish_switch();
    assert(current_settings.usb_mode == USB_MODE_DAC);
    reset();
    current_settings.usb_dac_mode_enabled = true;
    gui_network_usb_dac_boot_reconcile();
    poll_usb_storage_hotplug();
    assert(!gui_network_usb_dac_active() && !usb_mode_switch_active);
    /* Wall-charger power can bind Storage but must never restore DAC. */
    power_state = BATTERY_EXTERNAL_POWER_CONNECTED;
    poll_usb_storage_hotplug(); finish_switch();
    assert(current_settings.usb_mode == USB_MODE_STORAGE);
    assert(current_settings.usb_dac_mode_enabled && !gui_network_usb_dac_active());
    host_present = true;
    poll_usb_storage_hotplug(); finish_switch();
    assert(gui_network_usb_dac_active());
    assert(!gui_network_usb_storage_session_active());
    power_state = BATTERY_EXTERNAL_POWER_UNKNOWN;
    poll_usb_storage_hotplug();
    assert(gui_network_usb_dac_active());
    power_state = BATTERY_EXTERNAL_POWER_DISCONNECTED; host_present = false;
    poll_usb_storage_hotplug(); poll_usb_mode_switch(); finish_switch();
    assert(current_settings.usb_mode == USB_MODE_STORAGE);
    assert(current_settings.usb_dac_mode_enabled && !gui_network_usb_dac_active());
    power_state = BATTERY_EXTERNAL_POWER_CONNECTED; host_present = true;
    poll_usb_storage_hotplug(); finish_switch();
    assert(gui_network_usb_dac_active());
    /* Sleep pauses until a fresh cable or an explicit resume request. */
    gui_network_usb_dac_pause(); poll_usb_mode_switch(); finish_switch();
    poll_usb_storage_hotplug(); finish_switch();
    assert(!gui_network_usb_dac_active() && current_settings.usb_dac_mode_enabled);
    gui_network_usb_dac_resume(); poll_usb_storage_hotplug(); finish_switch();
    assert(gui_network_usb_dac_active());
    disable_usb_dac_mode(); poll_usb_mode_switch(); finish_switch();
    assert(!current_settings.usb_dac_mode_enabled);
    gui_network_usb_dac_resume(); poll_usb_storage_hotplug();
    assert(!gui_network_usb_dac_active());
    /* Unplug while enable is in flight queues teardown after completion. */
    enable_usb_dac_mode();
    assert(usb_mode_switch_active);
    gui_network_usb_dac_pause(); finish_switch(); poll_usb_mode_switch(); finish_switch();
    assert(current_settings.usb_dac_mode_enabled && current_settings.usb_mode == USB_MODE_STORAGE);
    assert(gui_network_usb_dac_shutdown());
    /* A failed enable stays paused with the intent kept and does not retry by itself. */
    reset();
    power_state = BATTERY_EXTERNAL_POWER_CONNECTED; host_present = true;
    apply_ok = false;
    enable_usb_dac_mode(); finish_switch();
    assert(current_settings.usb_dac_mode_enabled && !gui_network_usb_dac_active() && !usb_mode_switch_active);
    poll_usb_mode_switch();
    assert(!usb_mode_switch_active);
    /* Teardown is not attempted during a library scan, and shutdown does not wait for it. */
    apply_ok = true;
    enable_usb_dac_mode(); finish_switch();
    assert(gui_network_usb_dac_active());
    scan_active = true;
    gui_network_usb_dac_pause(); poll_usb_mode_switch();
    assert(!usb_mode_switch_active && usb_dac_pause_pending);
    assert(gui_network_usb_dac_shutdown());
    scan_active = false;
    poll_usb_mode_switch(); finish_switch();
    assert(current_settings.usb_mode == USB_MODE_STORAGE && !usb_dac_pause_pending);
    /* A failed teardown is tried once, not every poll. */
    reset();
    power_state = BATTERY_EXTERNAL_POWER_CONNECTED; host_present = true;
    enable_usb_dac_mode(); finish_switch();
    apply_ok = false;
    gui_network_usb_dac_pause(); poll_usb_mode_switch(); finish_switch();
    assert(!usb_dac_pause_pending && !usb_mode_switch_active && gui_network_usb_dac_shutdown());
    assert(saves && releases);
    puts("USB DAC cable/boot/pause/Stop lifecycle tests passed");
}
