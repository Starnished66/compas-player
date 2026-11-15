#include "src/ui/application/gui.h"
#include "src/ui/network/gui_network.h"
#include "src/ui/network/gui_network_internal.h"
#include "src/ui/text/fonts/ui_fonts.h"
#include "src/ui/appearance/gui_theme.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned tree_count(lv_obj_t *obj) {
    unsigned count = 1;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        count += tree_count(lv_obj_get_child(obj, i));
    return count;
}
static bool has_text(lv_obj_t *obj, const char *text) {
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        if (has_text(lv_obj_get_child(obj, i), text)) return true;
    return false;
}
int main(void) {
    lv_init();
    assert(ui_fonts_init());
    assert(lv_sdl_window_create(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT));
    settings_load(&current_settings);
    current_settings.setup_complete = true;
    audio_init();
    gui_init(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    /* Rebuilding one screen leaves the other's dot-mode labels reading "..." until
     * it is shown, so each is rebuilt and read on its own, usb first. */
    lv_obj_t *bt = gui_network_get_bt_dac_screen();
    gui_network_open_usb_dac();
    lv_obj_t *usb = lv_screen_active();
    nav_pop_immediate();
    assert(bt && usb);
    unsigned counts[2][2];
    for (int on = 0; on < 2; ++on) {
        current_settings.usb_dac_mode_enabled = !on; /* any change rebuilds the usb screen */
        gui_network_usb_dac_refresh();
        current_settings.usb_dac_mode_enabled = on;
        gui_network_usb_dac_refresh();
        assert(has_text(usb, on ? "Stop USB DAC" : "Enable USB DAC"));
        if (on) assert(has_text(usb, "Waiting for USB audio…"));
        counts[on][0] = tree_count(usb);
        current_settings.bt_dac_mode_enabled = on;
        populate_bt_dac_screen();
        assert(has_text(bt, on ? "Stop Bluetooth DAC" : "Enable Bluetooth DAC"));
        counts[on][1] = tree_count(bt);
    }
    assert(counts[0][0] == counts[0][1]);
    /* Bluetooth alone adds its three controls and their row. */
    assert(counts[1][1] == counts[1][0] + 4);
    gui_network_open_usb_dac();
    assert(lv_screen_active() == usb);
    nav_pop_immediate();
    assert(lv_screen_active() != usb);
    assert(current_settings.usb_dac_mode_enabled);
    /* Rebuild and re-open after a theme reload must not retain dead widgets. */
    nav_reset_to_home();
    gui_network_teardown();
    gui_network_init();
    gui_network_open_usb_dac();
    assert(has_text(lv_screen_active(), "Stop USB DAC"));
    puts("Shared Bluetooth/USB DAC screen and navigation tests passed");
}
