#pragma once
#include "../../network/wifi/wifi_control.h"
#include "../../network/bluetooth/control/bluetooth_control.h"
#include <lvgl/lvgl.h>

/* Declarations shared between the gui_network_*.c files. Everything in
 * here is private to src/ui/network; the public API is gui_network.h. */


#define WIFI_MAX_RESULTS 32
#define WIFI_MAX_SAVED 32

/* gui_network.c */
void configure_network_row_line(lv_obj_t * label, int32_t width, const lv_font_t * font);
int32_t network_row_text_height(int32_t base_height, const lv_font_t * const fonts[], int count);
void align_network_row_text_stack(lv_obj_t * row, lv_obj_t * const labels[],
                                  const lv_font_t * const fonts[], int count, int32_t left);
lv_obj_t * add_network_explanation(lv_obj_t * list, const char * text);
lv_obj_t * network_list_screen(const char * title, lv_obj_t ** list);
lv_obj_t * network_row_create(lv_obj_t * parent, int32_t height, lv_event_cb_t cb, void * user_data);
bool network_list_touch_active(lv_obj_t * list);
void network_delete_screen(lv_obj_t ** screen);
int rotate_to_front(int position, int front);
bool wifi_feature_guard(void);

/* gui_network_wifi.c */
extern wifi_network_t wifi_scan_results[WIFI_MAX_RESULTS];
extern int wifi_scan_result_count;
extern wifi_saved_network_t wifi_saved_results[WIFI_MAX_SAVED];
extern int wifi_saved_result_count;
extern lv_obj_t * wifi_list;
extern lv_obj_t * wifi_rescan_btn;
extern bool wifi_enable_pending_feedback;
extern bool wifi_settings_snapshot_active;
extern bool wifi_settings_snapshot_enabled;
extern wifi_info_t wifi_cached_info;
extern bool wifi_cached_info_connected;
bool wifi_link_op_busy(void);
bool wifi_pending_ssid_set(const char * text);
void start_wifi_connect_saved(int id, const char * ssid);
void start_wifi_disconnect(void);
void wifi_password_entered_cb(const char * text, void * user_data);
void wifi_row_click_cb(lv_event_t * e);
void wifi_rescan_btn_cb(lv_event_t * e);
void wifi_settings_snapshot_invalidate(void);
void wifi_settings_snapshot_start(void);
int wifi_saved_current_index(const char * ssid);
bool wifi_cached_info_is_current(void);
bool wifi_network_is_connected(const wifi_network_t * net);
bool wifi_ssid_is_saved(const char * ssid);
int wifi_saved_display_index(int candidate);
const char * wifi_network_status_text(const wifi_network_t * net, bool connected, bool saved);
const char * wifi_signal_icon(int level);
int wifi_scan_connected_index(void);
bool start_wifi_scan(void);
void start_wifi_forget(int id);
void gui_network_wifi_teardown(void);
bool gui_network_wifi_has_work(void);
void gui_network_wifi_cancel(void);

/* gui_network_wifi_screen.c */
extern lv_obj_t * wifi_screen;
extern lv_obj_t * wifi_info_screen;
void populate_wifi_info_screen(void);
void populate_wifi_screen(bool enabled);
void wifi_tile_cb(lv_event_t * e);
void gui_network_wifi_screen_build(void);
void gui_network_wifi_screen_teardown(void);

/* gui_network_bt.c */
void bt_tile_cb(lv_event_t * e);
void gui_network_bt_build(void);
void gui_network_bt_teardown(void);
bool gui_network_bt_has_work(void);
void gui_network_bt_cancel(void);

/* gui_network_pairing.c */
void gui_network_pairing_build(void);
void gui_network_pairing_teardown(void);

/* gui_network_receivers.c */
extern lv_obj_t * usb_dac_overlay_screen;
void stop_local_playback(void);
void disable_bt_dac_mode(void);
void disable_airplay_mode(void);
void bt_dac_settings_row_cb(lv_event_t * e);
void format_rate(char * out, size_t size, unsigned int rate);
void populate_airplay_screen(void);
void airplay_tile_cb(lv_event_t * e);
void gui_network_receivers_build(void);
void gui_network_receivers_teardown(void);

/* gui_network_usb.c */
void gui_network_usb_build(void);
void gui_network_usb_teardown(void);
bool gui_network_usb_has_work(void);
void gui_network_usb_cancel(void);

/* gui_network_services.c */
extern lv_obj_t * remote_control_screen;
void remote_control_refresh_pin_label(void);
void remote_control_refresh_address(void);
void remote_control_tile_cb(lv_event_t * e);
void dlna_tile_cb(lv_event_t * e);
void gui_network_services_build(void);
void gui_network_services_teardown(void);
bool gui_network_services_has_work(void);
void gui_network_services_cancel(void);

/* gui_network_choice.c */
void gui_network_choice_settings_build(void);
void gui_network_choice_settings_teardown(void);


/* gui_network_bt_dac.c */
void gui_network_bt_dac_build(void);
void gui_network_bt_dac_refresh(void);
void gui_network_bt_dac_teardown(void);
