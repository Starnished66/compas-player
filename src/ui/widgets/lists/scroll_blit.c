#include "scroll_blit.h"
#include "../../effects/transition_compositor.h"
#include "../../shell/gui_navigation.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SCROLL_BLIT_OFF_FILE "/usr/data/mnt/sd_0/compas_scroll_blit.off"
#define SCROLL_BLIT_RECHECK_MS 500

static bool switch_on(void) {
#ifdef UI_PERF_TRACE
    static bool on = true;
    static uint32_t checked_at;
    static bool checked;
    if (!checked || lv_tick_elaps(checked_at) >= SCROLL_BLIT_RECHECK_MS) {
        const char * env = getenv("COMPAS_SCROLL_BLIT");
        on = !(env && strcmp(env, "0") == 0) && access(SCROLL_BLIT_OFF_FILE, F_OK) != 0;
        checked_at = lv_tick_get();
        checked = true;
    }
    return on;
#else
    return true;
#endif
}

static bool gate(lv_display_t * disp, lv_obj_t * obj, const void * on_screen_px) {
    (void) obj;
    if (!switch_on()) return false;
    if (transition_compositor_is_active() || gui_navigation_transition_in_progress()) return false;
#if LV_USE_LINUX_FBDEV
    /* LVGL's idea of the visible page must be the page the panel scans out. */
    return lv_linux_fbdev_get_active_page(disp) == on_screen_px;
#else
    (void) disp; (void) on_screen_px;
    return false;
#endif
}

void ui_scroll_blit_enable(lv_obj_t * list) {
    lv_scroll_blit_set_gate(gate);
    lv_obj_set_scroll_blit(list, true);
}
