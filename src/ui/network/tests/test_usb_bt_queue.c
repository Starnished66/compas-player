#include "src/ui/application/gui.h"
#include "src/ui/network/gui_network.h"
#include "src/ui/network/gui_network_internal.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

static atomic_bool hold_apply, entered;
static atomic_int applies;
static bool profile_applied, profile_teardown;
static usb_mode_t gadget = USB_MODE_STORAGE;
static int suspended;

void __wrap_settings_save_async(const player_settings_t *s) { (void)s; }
void __wrap_show_info_toast(const char *s) { (void)s; }
void __wrap_show_error_toast(const char *s) { fprintf(stderr, "%s\n", s); assert(0); }
void __wrap_gui_shell_refresh_quick_drawer_expansion_toggles(void) {}
void __wrap_gui_library_suspend_boot_prompt(void) { suspended++; }
bool __wrap_gui_library_scan_active(void) { return false; }
bool __wrap_firmware_update_busy(void) { return false; }
bool __wrap_usb_mode_control_host_connected(void) { return true; }
bool __wrap_usb_mode_control_storage_host_configured(void) { return false; }
bool __wrap_usb_mode_control_detect_current(usb_mode_t *mode) { *mode = gadget; return true; }
bool __wrap_usb_mode_control_apply_with_status(usb_mode_t mode, bool *busy) {
    *busy = false;
    atomic_fetch_add(&applies, 1);
    atomic_store(&entered, true);
    while (atomic_load(&hold_apply)) usleep(1000);
    gadget = mode;
    return true;
}
/* Profile application is the hardware boundary. Both network state machines
 * and their pending/disable paths are linked unchanged. */
bool __wrap_gui_shell_bt_dac_busy(void) { return profile_applied || profile_teardown; }
void __wrap_start_bt_apply_output_settings(bool enabled, bool sync) {
    (void)sync;
    if (!enabled && profile_applied) profile_teardown = true;
    profile_applied = enabled;
}
void __wrap_gui_network_extend_bt_pairing_window(void) {}
static void settle_usb(void) {
    for (int i = 0; i < 2000 && gui_network_usb_has_work(); i++) {
        poll_usb_mode_switch();
        usleep(1000);
    }
    poll_usb_mode_switch();
    assert(!gui_network_usb_has_work());
    assert(!gui_network_usb_dac_pending());
}
int main(void) {
    bt_is_powered_cached = true;
    current_settings.usb_mode = USB_MODE_STORAGE;
    enable_bt_dac_mode();
    assert(profile_applied);
    enable_usb_dac_mode();
    assert(gui_network_usb_dac_pending());
    assert(gui_network_usb_has_work());
    assert(!atomic_load(&applies));
    enable_bt_dac_mode();
    assert(profile_applied);
    assert(!gui_network_usb_dac_pending());
    assert(!gui_network_usb_has_work());
    poll_usb_mode_switch();
    assert(!atomic_load(&applies));
    assert(!gui_network_firmware_update_usb_blocked());
    assert(gui_network_usb_dac_shutdown());
    assert(gui_network_usb_dac_shutdown()); /* restart uses the same gate */
    assert(suspended == 1);
    assert(start_usb_mode_switch(USB_MODE_ADB));
    settle_usb();
    assert(current_settings.usb_mode == USB_MODE_ADB);
    assert(start_usb_mode_switch(USB_MODE_STORAGE));
    settle_usb();

    /* A newer explicit gadget choice can supersede the queued DAC too. */
    enable_usb_dac_mode();
    assert(gui_network_usb_dac_pending());
    assert(start_usb_mode_switch(USB_MODE_ADB));
    settle_usb();
    assert(current_settings.usb_mode == USB_MODE_ADB);
    assert(start_usb_mode_switch(USB_MODE_STORAGE));
    settle_usb();

    /* Once launched, the worker must land before its pause can leave DAC. */
    disable_bt_dac_mode();
    profile_teardown = false;
    atomic_store(&hold_apply, true);
    atomic_store(&entered, false);
    enable_usb_dac_mode();
    for (int i = 0; i < 2000 && !atomic_load(&entered); i++) usleep(1000);
    assert(atomic_load(&entered));
    enable_bt_dac_mode();
    assert(gui_network_usb_dac_pending());
    assert(gui_network_usb_has_work());
    assert(!gui_network_usb_dac_shutdown());
    atomic_store(&hold_apply, false);
    /* A second poll starts Storage after the DAC worker completes. */
    for (int i = 0; i < 2000 && gui_network_usb_dac_pending(); i++) {
        poll_usb_mode_switch();
        usleep(1000);
    }
    settle_usb();
    assert(current_settings.usb_mode == USB_MODE_STORAGE);
    assert(atomic_load(&applies) == 6);
    assert(gui_network_usb_dac_shutdown());
    assert(!gui_network_firmware_update_usb_blocked());
    puts("USB queued cancellation and launched completion passed");
}
