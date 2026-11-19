#include "src/ui/shell/gui_shell_internal.h"
#include "src/ui/network/gui_network.h"
#include "src/ui/text/fonts/ui_fonts.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    lv_init(); assert(ui_fonts_init());
    assert(lv_sdl_window_create(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT));
    settings_load(&current_settings); audio_init();
    gui_init(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    lv_obj_t * icon = quick_drawer_toggle_icon[QD_TOGGLE_POWER_OFF];
    assert(icon);
    assert(lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(quick_drawer_toggle_icon[QD_TOGGLE_WIFI], LV_OBJ_FLAG_HIDDEN));

    /* Saved layouts cannot expose the unavailable power action. */
    snprintf(current_settings.ui_customization.drawer_order_text,
             sizeof(current_settings.ui_customization.drawer_order_text),
             "@drawer2,wifi,power_off,bluetooth,plugins");
    gui_shell_apply_customization();
    assert(lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(quick_drawer_toggle_icon[QD_TOGGLE_WIFI], LV_OBJ_FLAG_HIDDEN));
    puts("Drawer power tile stays hidden in default and saved layouts");

    /* USB DAC ON from the drawer opens its screen once; OFF and a refused ON stay put. */
    gui_network_open_usb_dac();
    lv_obj_t * usb = lv_screen_active();
    nav_pop_immediate();
    lv_obj_t * home = lv_screen_active();
    assert(usb != home && !current_settings.usb_dac_mode_enabled);
    lv_obj_send_event(quick_drawer_usb_dac_icon, LV_EVENT_SHORT_CLICKED, NULL);
    assert(lv_screen_active() == usb && current_settings.usb_dac_mode_enabled);
    nav_pop_immediate();
    assert(lv_screen_active() == home);
    lv_obj_send_event(quick_drawer_usb_dac_icon, LV_EVENT_SHORT_CLICKED, NULL);
    assert(lv_screen_active() == home && !current_settings.usb_dac_mode_enabled);
    current_settings.usb_mode = USB_MODE_ADB;
    lv_obj_send_event(quick_drawer_usb_dac_icon, LV_EVENT_SHORT_CLICKED, NULL);
    assert(lv_screen_active() == home && !current_settings.usb_dac_mode_enabled);
    puts("Drawer USB DAC opens its screen only on an accepted ON");
}
