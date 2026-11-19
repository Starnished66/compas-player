/* Run the real UI policy with deterministic gadget and cable observations. */
#include "src/ui/network/gui_network_usb.c"
#include <assert.h>

player_settings_t current_settings;
static battery_external_power_state_t power_state;
static bool host_present, bt_active, output_owned, scan_active, apply_ok = true;
static bool airplay_stopping;
static bool screen_visible, policy_held, policy_ok = true;
static atomic_bool policy_block, apply_block;
static int policy_calls;
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
bool usb_mode_control_apply_with_status(usb_mode_t mode, bool *busy) {
    (void)mode;
    while (atomic_load(&apply_block)) usleep(1000);
    *busy = false;
    return apply_ok;
}
bool usb_mode_control_hold_dac_policy(bool hold) {
    while (atomic_load(&policy_block)) usleep(1000);
    policy_calls++;
    if (policy_ok) policy_held = hold;
    return policy_ok;
}
bool gui_network_usb_dac_screen_visible(void) { return screen_visible; }
static bool persistence_pending;
bool idle_shutdown_pending(void) { return persistence_pending; }
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
    screen_visible = policy_held = false;
    policy_ok = true;
    usb_dac_policy_held = usb_dac_policy_retry_wait = false;
    usb_dac_policy_known = true;
    usb_dac_power_owner = USB_DAC_POWER_NONE;
    policy_calls = 0;
}
/* A failed hold or release stays pending and retries once the throttle passes. */
static void test_policy_retry(void) {
    reset();
    screen_visible = current_settings.usb_dac_mode_enabled = true;
    policy_ok = false;
    poll_usb_mode_switch(); finish_switch();
    assert(policy_calls == 1 && !usb_dac_policy_held);
    poll_usb_mode_switch();
    assert(!usb_mode_switch_active && policy_calls == 1);
    tick += USB_DAC_POLICY_RETRY_MS;
    policy_ok = true;
    poll_usb_mode_switch(); finish_switch();
    assert(policy_calls == 2 && usb_dac_policy_held && policy_held);
    screen_visible = false;
    policy_ok = false; /* e.g. an unreadable gadget */
    poll_usb_mode_switch(); finish_switch();
    assert(policy_calls == 3 && usb_dac_policy_held && policy_held);
    policy_ok = true;
    poll_usb_mode_switch();
    assert(!usb_mode_switch_active);
    tick += USB_DAC_POLICY_RETRY_MS;
    poll_usb_mode_switch(); finish_switch();
    assert(policy_calls == 4 && !usb_dac_policy_held && !policy_held);
    /* After a failed job the state is unknown, so the latest request is
     * reconciled even when it matches the last confirmed one. */
    for (int hold = 0; hold < 2; ++hold) {
        reset();
        current_settings.usb_dac_mode_enabled = true;
        screen_visible = !hold;
        if (!hold) { poll_usb_mode_switch(); finish_switch(); assert(policy_held); }
        screen_visible = hold;
        policy_ok = false; /* a partial failure: the hardware may be either way */
        poll_usb_mode_switch(); finish_switch();
        assert(!usb_dac_policy_known);
        screen_visible = !hold; /* back to what was last confirmed */
        policy_ok = true;
        tick += USB_DAC_POLICY_RETRY_MS;
        int calls = policy_calls;
        poll_usb_mode_switch(); finish_switch();
        assert(policy_calls == calls + 1 && usb_dac_policy_known && policy_held == !hold);
    }
}
/* Power-off waits a bounded time for a stuck release, scan or not; a cancelled
 * power action brings the hold back. */
static void test_policy_power(void) {
    for (int scan = 0; scan < 2; ++scan) {
        reset();
        screen_visible = current_settings.usb_dac_mode_enabled = true;
        poll_usb_mode_switch(); finish_switch();
        assert(policy_held);
        scan_active = scan;
        atomic_store(&policy_block, true);
        assert(!gui_network_usb_dac_shutdown(USB_DAC_POWER_MENU));
        assert(usb_dac_policy_job && !gui_network_usb_has_work());
        assert(!gui_network_usb_dac_shutdown(USB_DAC_POWER_MENU));
        tick += USB_DAC_POWER_POLICY_WAIT_MS;
        assert(gui_network_usb_dac_shutdown(USB_DAC_POWER_MENU));
        atomic_store(&policy_block, false);
        finish_switch();
        assert(!policy_held && gui_network_usb_dac_shutdown(USB_DAC_POWER_MENU));
        gui_network_usb_dac_power_cancelled(USB_DAC_POWER_IDLE); /* not the owner */
        poll_usb_mode_switch();
        assert(!usb_mode_switch_active && !policy_held);
        gui_network_usb_dac_power_cancelled(USB_DAC_POWER_MENU);
        poll_usb_mode_switch(); finish_switch();
        assert(policy_held);
    }
    /* The first live action owns the wait: a car request during the menu's
     * wait neither takes it over nor restarts it, and the car's cancel does
     * not end the menu's. */
    reset();
    screen_visible = current_settings.usb_dac_mode_enabled = true;
    poll_usb_mode_switch(); finish_switch();
    atomic_store(&policy_block, true);
    assert(!gui_network_usb_dac_shutdown(USB_DAC_POWER_MENU));
    tick += USB_DAC_POWER_POLICY_WAIT_MS / 2;
    assert(!gui_network_usb_dac_shutdown(USB_DAC_POWER_CAR));
    gui_network_usb_dac_power_cancelled(USB_DAC_POWER_CAR);
    assert(usb_dac_power_owner == USB_DAC_POWER_MENU);
    tick += USB_DAC_POWER_POLICY_WAIT_MS / 2;
    assert(gui_network_usb_dac_shutdown(USB_DAC_POWER_CAR));
    atomic_store(&policy_block, false);
    finish_switch();
    /* Handed to persistence: reopening the screen, power returning or an
     * unrelated refusal does not take the hold back; a failed power-off does. */
    persistence_pending = true;
    screen_visible = false;
    poll_usb_mode_switch();
    screen_visible = true;
    gui_network_usb_dac_power_cancelled(USB_DAC_POWER_MENU);
    gui_network_usb_dac_power_cancelled(USB_DAC_POWER_CAR);
    poll_usb_mode_switch();
    assert(!usb_mode_switch_active && !policy_held);
    persistence_pending = false;
    gui_network_usb_dac_power_off_failed();
    poll_usb_mode_switch(); finish_switch();
    assert(policy_held);
    /* Car hands off; power returning while persistence runs keeps the release. */
    assert(gui_network_usb_dac_shutdown(USB_DAC_POWER_CAR) == false);
    finish_switch();
    assert(gui_network_usb_dac_shutdown(USB_DAC_POWER_CAR) && !policy_held);
    persistence_pending = true;
    gui_network_usb_dac_power_cancelled(USB_DAC_POWER_CAR);
    poll_usb_mode_switch();
    assert(!usb_mode_switch_active && !policy_held && usb_dac_power_owner == USB_DAC_POWER_CAR);
    persistence_pending = false;
    gui_network_usb_dac_power_cancelled(USB_DAC_POWER_CAR); /* a refusal before handoff */
    poll_usb_mode_switch(); finish_switch();
    assert(policy_held);
    /* The deadline starts once per action: an ordinary switch kept pending
     * does not restart it, and the scan escape applies once it expires. */
    reset();
    screen_visible = current_settings.usb_dac_mode_enabled = true;
    poll_usb_mode_switch(); finish_switch();
    assert(policy_held);
    atomic_store(&apply_block, true);
    assert(start_usb_mode_switch(USB_MODE_STORAGE));
    scan_active = true;
    assert(!gui_network_usb_dac_shutdown(USB_DAC_POWER_MENU));
    tick += USB_DAC_POWER_POLICY_WAIT_MS / 2;
    assert(!gui_network_usb_dac_shutdown(USB_DAC_POWER_MENU));
    tick += USB_DAC_POWER_POLICY_WAIT_MS / 2;
    assert(gui_network_usb_dac_shutdown(USB_DAC_POWER_MENU));
    atomic_store(&apply_block, false);
    finish_switch();
    scan_active = false;
}
/* The DAC policy follows the USB DAC screen, not the host. */
static void test_screen_policy(void) {
    reset();
    screen_visible = true;
    poll_usb_mode_switch();
    assert(!usb_mode_switch_active && !policy_calls); /* USB DAC off: nothing held */
    current_settings.usb_mode = USB_MODE_ADB;
    assert(!gui_network_toggle_usb_dac()); /* rejected ON */
    current_settings.usb_dac_mode_enabled = true;
    assert(!gui_network_toggle_usb_dac()); /* rejected ON with intent kept from a pause */
    current_settings.usb_dac_mode_enabled = false;
    current_settings.usb_mode = USB_MODE_STORAGE;
    assert(gui_network_toggle_usb_dac()); /* accepted ON */
    assert(current_settings.usb_dac_mode_enabled && !gui_network_usb_dac_active());
    poll_usb_mode_switch(); finish_switch();
    assert(policy_held && policy_calls == 1 && current_settings.usb_mode == USB_MODE_STORAGE);
    poll_usb_mode_switch();
    assert(!usb_mode_switch_active && policy_calls == 1);
    screen_visible = false;
    poll_usb_mode_switch(); finish_switch();
    assert(!policy_held && policy_calls == 2);
    /* Leaving during a session asks for release; the session keeps running and
     * usb_mode_control keeps the policy while its gadget is bound. */
    screen_visible = true;
    poll_usb_mode_switch(); finish_switch();
    power_state = BATTERY_EXTERNAL_POWER_CONNECTED; host_present = true;
    poll_usb_storage_hotplug(); finish_switch();
    assert(gui_network_usb_dac_active() && policy_held);
    screen_visible = false;
    poll_usb_mode_switch(); finish_switch();
    assert(!policy_held && gui_network_usb_dac_active());
    assert(!gui_network_toggle_usb_dac()); /* OFF */
    poll_usb_mode_switch(); finish_switch();
    assert(!current_settings.usb_dac_mode_enabled && !policy_held);
    /* Power-off releases the hold and waits for that, then a wake re-arms it. */
    test_policy_retry();
    test_policy_power();
    reset();
    screen_visible = true;
    current_settings.usb_dac_mode_enabled = true;
    poll_usb_mode_switch(); finish_switch();
    assert(policy_held);
    assert(!gui_network_usb_dac_shutdown(USB_DAC_POWER_IDLE));
    finish_switch();
    assert(!policy_held && gui_network_usb_dac_shutdown(USB_DAC_POWER_IDLE));
    gui_network_usb_dac_resume(); /* waking from the idle suspend */
    poll_usb_mode_switch(); finish_switch();
    assert(policy_held);
}
int main(void) {
    test_screen_policy();
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
    assert(gui_network_usb_dac_shutdown(USB_DAC_POWER_MENU));
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
    assert(gui_network_usb_dac_shutdown(USB_DAC_POWER_MENU));
    scan_active = false;
    poll_usb_mode_switch(); finish_switch();
    assert(current_settings.usb_mode == USB_MODE_STORAGE && !usb_dac_pause_pending);
    /* A failed teardown is tried once, not every poll. */
    reset();
    power_state = BATTERY_EXTERNAL_POWER_CONNECTED; host_present = true;
    enable_usb_dac_mode(); finish_switch();
    apply_ok = false;
    gui_network_usb_dac_pause(); poll_usb_mode_switch(); finish_switch();
    assert(!usb_dac_pause_pending && !usb_mode_switch_active && gui_network_usb_dac_shutdown(USB_DAC_POWER_MENU));
    assert(saves && releases);
    puts("USB DAC cable/boot/pause/Stop lifecycle tests passed");
}
