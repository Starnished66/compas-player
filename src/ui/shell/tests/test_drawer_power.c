#include "src/ui/shell/gui_shell_internal.h"
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
}
