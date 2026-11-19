/* The power menu hands the USB DAC screen its policy back when its power-off
 * cannot start: no timer to wait on, or the queue is still saving. */
#include "src/ui/settings/power/gui_power_hub.c"
#include <assert.h>

static usb_dac_power_action_t cancelled = USB_DAC_POWER_NONE;
static int cancels;
static bool queue_busy;
void gui_network_usb_dac_power_cancelled(usb_dac_power_action_t action) { cancelled = action; cancels++; }
bool gui_network_usb_dac_shutdown(usb_dac_power_action_t action) { (void)action; return false; }
lv_timer_t * lv_timer_create(lv_timer_cb_t cb, uint32_t period, void * data) { (void)cb; (void)period; (void)data; return NULL; }
lv_event_code_t lv_event_get_code(lv_event_t * e) { (void)e; return LV_EVENT_CLICKED; }
void * lv_event_get_user_data(lv_event_t * e) { (void)e; return (void *) (intptr_t) 0; }
bool firmware_update_busy(void) { return false; }
void gui_popup_hide(gui_popup_t * popup) { (void)popup; }
bool gui_player_queue_write_busy(void) { return queue_busy; }
void show_info_toast(const char * s) { (void)s; }
const char * i18n_tr(const char * s) { return s; }
/* Reached by the power menu but not by these taps. */
void backlight_set_screen_on(bool on) { (void)on; assert(false); }
void gui_settings_open_sleep_timer(void) { assert(false); }
static int power_offs;
bool gui_shell_bt_dac_shutdown(bool restore_source) { (void)restore_source; return true; }
void idle_shutdown_now(void) { power_offs++; }
void idle_shutdown_reboot_now(void) { assert(false); }
bool transition_compositor_bottom_sheet_frame(int32_t y) { (void)y; assert(false); return false; }
void transition_compositor_end(void) { assert(false); }
bool lv_anim_delete(void * var, lv_anim_exec_xcb_t exec) { (void)var; (void)exec; assert(false); return false; }
void lv_draw_buf_destroy(lv_draw_buf_t * buf) { (void)buf; assert(false); }
void lv_image_cache_drop(const void * src) { (void)src; assert(false); }
void lv_label_set_text(lv_obj_t * obj, const char * text) { (void)obj; (void)text; assert(false); }
void lv_obj_delete(lv_obj_t * obj) { (void)obj; assert(false); }
lv_result_t lv_obj_invalidate(const lv_obj_t * obj) { (void)obj; assert(false); return LV_RESULT_OK; }
void lv_obj_remove_flag(lv_obj_t * obj, lv_obj_flag_t f) { (void)obj; (void)f; assert(false); }
void lv_obj_set_style_opa(lv_obj_t * obj, lv_opa_t v, lv_style_selector_t sel) { (void)obj; (void)v; (void)sel; assert(false); }
void lv_obj_set_style_translate_y(lv_obj_t * obj, int32_t v, lv_style_selector_t sel) { (void)obj; (void)v; (void)sel; assert(false); }
void lv_obj_set_y(lv_obj_t * obj, int32_t y) { (void)obj; (void)y; assert(false); }
void lv_timer_delete(lv_timer_t * t) { (void)t; assert(false); }
void * lv_timer_get_user_data(lv_timer_t * t) { (void)t; assert(false); return NULL; }

int main(void) {
    power_action_pending = 0; /* the confirming second tap on Power off */
    power_action_clicked_cb(NULL);
    assert(cancels == 1 && cancelled == USB_DAC_POWER_MENU);
    queue_busy = true;
    power_finish(0);
    assert(cancels == 2 && cancelled == USB_DAC_POWER_MENU && !power_offs);
    /* After the request the menu cancels its own wait; the owner check keeps
     * it if the power-off was handed off. */
    queue_busy = false;
    power_finish(0);
    assert(power_offs == 1 && cancels == 3 && cancelled == USB_DAC_POWER_MENU);
    puts("Power menu USB DAC power cancellation passed");
}
