#include "src/ui/shell/gui_shell_internal.h"
#include "src/ui/text/fonts/ui_fonts.h"
#include <assert.h>
#include <stdio.h>
static int opened;
void __wrap_start_power_off_countdown(void) { ++opened; }
int main(void) {
    lv_init(); assert(ui_fonts_init());
    assert(lv_sdl_window_create(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT));
    settings_load(&current_settings); audio_init();
    gui_init(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    lv_obj_t * icon = quick_drawer_toggle_icon[QD_TOGGLE_POWER_OFF];
    assert(icon);
    assert(quick_drawer_toggle_src(QD_TOGGLE_POWER_OFF, true) ==
           quick_drawer_toggle_src(QD_TOGGLE_POWER_OFF, false));
    assert(!strcmp(lv_label_get_text(quick_drawer_toggle_name[QD_TOGGLE_POWER_OFF]), "Power off"));
    assert(!lv_label_get_text(quick_drawer_toggle_state[QD_TOGGLE_POWER_OFF])[0]);
    lv_obj_send_event(icon, LV_EVENT_LONG_PRESSED, NULL);
    assert(opened == 0);
    lv_obj_send_event(icon, LV_EVENT_SHORT_CLICKED, NULL);
    assert(opened == 1);
    puts("Drawer power action uses the hardware-button confirmation entry point");
}
