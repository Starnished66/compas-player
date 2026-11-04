#pragma once
/* Shared by the gui_settings*.c files. Screen slots stay private to the file
 * that owns them; only the entry points, helpers and the few slots the
 * Settings root and More screens open directly are exported here. */
#include <lvgl/lvgl.h>
#include <stdbool.h>
#include <stdint.h>
#include "../shell/gui_navigation.h"
#include "../../plugins/manager/plugin_manager.h"

/* Summary labels on the category screens; refreshed by
 * settings_summary_loaded_cb() whenever one of those screens loads. */
extern lv_obj_t * settings_eq_summary;
extern lv_obj_t * settings_car_summary;
extern lv_obj_t * settings_sleep_summary;
void settings_summary_loaded_cb(lv_event_t * e);

/* Category screens the Settings root opens through push_screen_slot_cb(). */
extern lv_obj_t * settings_display_screen;
extern lv_obj_t * settings_power_screen;
extern lv_obj_t * settings_system_screen;

/* Lazily builds the screen in `slot` on first use, then opens it. */
static inline void push_cached_screen(lv_obj_t ** slot, lv_obj_t * (*build)(void)) {
    if (!*slot) *slot = build();
    if (*slot) nav_push(*slot);
}

void push_screen_slot_cb(lv_event_t * e);

/* Click handler for a plugin-registered list row: forwards the row's plugin
 * index (carried in user_data) to the list's *_list_item_clicked function. */
#define PLUGIN_ROW_CLICK_CB(name, clicked_fn) \
    static void name(lv_event_t * e) { \
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) \
            clicked_fn((int) (intptr_t) lv_event_get_user_data(e)); \
    }

static inline void delete_screen(lv_obj_t ** slot) {
    if (*slot) { lv_obj_delete(*slot); *slot = NULL; }
}

/* Plugin row helpers shared by the Sound, Display and Library screens. */
int append_grouped_plugin_rows(pill_list_item_t * items, int count, int max_items,
                               const char * list_id, const char * group,
                               plugin_list_item_count_cb_t get_count_fn,
                               plugin_list_item_label_cb_t get_label_fn,
                               plugin_list_item_options_cb_t get_options_fn,
                               lv_event_cb_t click_cb);
int count_plugin_rows_in_group(const char * list_id, int plugin_count, const char * group);

/* Rows the More screen and Settings root reuse from the category files. */
void music_profiles_menu_cb(lv_event_t * e);
bool gui_settings_profiles_download_available(void);
void themes_row_cb(lv_event_t * e);
void player_layout_choice_settings_row_cb(lv_event_t * e);
void settings_updates_row_cb(lv_event_t * e);

/* Per-group screen construction and teardown, called from gui_settings_init()
 * and gui_settings_teardown() at the positions their order requires. */
void settings_system_build_screens(void);
void settings_power_build_screens(void);
void settings_music_build_screens(void);
void settings_display_build_screens(void);
void build_factory_reset_popup(void);
void build_hostname_reboot_popup(void);
void settings_system_teardown_popups(void);
void settings_system_teardown_screens(void);
void settings_power_teardown_screens(void);
void settings_music_teardown_screens(void);
void settings_display_teardown_screens(void);
