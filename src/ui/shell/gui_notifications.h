#pragma once
#include <lvgl/lvgl.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint32_t gui_busy_handle_t;

void gui_notifications_init(void);

void show_error_toast(const char * msg);
void show_info_toast(const char * msg);
void show_info_toast_for(const char * msg, uint32_t duration_ms);
bool gui_notifications_toast_visible(void);

gui_busy_handle_t gui_busy_show(const char * title, const char * msg);
void gui_busy_set_progress(gui_busy_handle_t handle, int percent);
/* Status line under the progress bar; empty or NULL hides it. */
void gui_busy_set_detail(gui_busy_handle_t handle, const char * text);
/* Opt-in: shows a Cancel button and lets the Back gesture leave. on_cancel
 * runs once, when the user presses Cancel or the screen is left any other
 * way. Pass NULL to turn it off; hiding the busy screen also clears it. */
void gui_busy_set_cancel(gui_busy_handle_t handle, void (*on_cancel)(void *), void * user);
bool gui_busy_is_cancellable(void);
void gui_busy_hide(gui_busy_handle_t handle);
/* Dismiss an interstitial with an immediate stack pop/load so the caller can
 * push its final destination without a pending transition overriding it. */
void gui_busy_hide_immediate(gui_busy_handle_t handle);
static inline void gui_busy_dismiss(gui_busy_handle_t handle) { gui_busy_hide(handle); }
lv_obj_t * gui_busy_get_screen(void);

typedef struct {
    const char * label;
    lv_event_cb_t cb;
    bool destructive;
    bool cancel;
    void * user_data;
} menu_popup_row_t;

lv_obj_t * build_menu_popup(const menu_popup_row_t * rows, int row_count, lv_event_cb_t backdrop_cb, lv_obj_t ** out_backdrop);
