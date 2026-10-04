#include "gui_setup_plugins.h"

#include "gui_network.h"
#include "gui_notifications.h"
#include "gui_plugin_store.h"
#include "gui_reload.h"
#include "gui_theme.h"
#include "i18n.h"
#include "plugin_store.h"
#include "screen_builders.h"
#include "settings.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The picker/store owns the same backend. Its activity predicate includes
 * visible store UI and a launch waiting for navigation, not only worker I/O. */
extern player_settings_t current_settings;
extern lv_color_t accent_lv_color(void);

#define SETUP_PLUGIN_LIMIT SETTINGS_SETUP_PLUGIN_MAX
#define SETUP_BATCH_LIMIT (SETUP_PLUGIN_LIMIT + 1)
#define SETUP_PLUGIN_ID_CAP SETTINGS_SETUP_PLUGIN_ID_MAX
#define SETUP_PLUGIN_NAME_CAP 65
typedef struct {
    char id[SETUP_PLUGIN_ID_CAP];
    char name[SETUP_PLUGIN_NAME_CAP];
} setup_plugin_t;

typedef enum {
    BATCH_IDLE,
    BATCH_REFRESHING,
    BATCH_INSTALLING,
    BATCH_RETRYING,
    BATCH_RESULT
} batch_stage_t;

static setup_plugin_t selected[SETUP_PLUGIN_LIMIT];
static size_t selected_count;
static setup_plugin_t batch[SETUP_BATCH_LIMIT];
static bool batch_is_layout[SETUP_BATCH_LIMIT];
static bool batch_succeeded[SETUP_BATCH_LIMIT];
static bool batch_failed[SETUP_BATCH_LIMIT];
static setup_plugin_t selected_layout;
static bool layout_ready;
static plugin_store_result_t catalog[PLUGIN_STORE_MAX_RESULTS];
static size_t batch_count;
static size_t batch_index;
static size_t batch_completed;
static size_t batch_retry_total;
static bool batch_retry_pass;
static bool batch_catalog_retried;
static bool batch_refresh_retry;
static size_t catalog_count;
static size_t last_displayed_completed = (size_t) -1;
static bool batch_changed;
static batch_stage_t batch_stage;
static void (*batch_on_complete)(void);
static gui_busy_handle_t batch_busy;
static lv_timer_t * batch_timer;
static bool batch_ui_attached;

static gui_popup_t result_popup;
static lv_obj_t * result_body;
static lv_obj_t * result_title;

static void batch_poll_cb(lv_timer_t * timer);
static void schedule_batch_timer(void);
static void start_next_batch_item(void);
static void result_ack_cb(lv_event_t * e);
static void result_backdrop_cb(lv_event_t * e);
static int selected_index(const char * id);

static void show_batch_result(void);

static void clear_selection(void) {
    memset(selected, 0, sizeof(selected));
    selected_count = 0;
}

static bool setup_plugin_id_valid(const char * id) {
    if (!id || !id[0] || strlen(id) >= SETUP_PLUGIN_ID_CAP ||
        !((*id >= 'a' && *id <= 'z') || (*id >= '0' && *id <= '9'))) return false;
    for (const unsigned char * p = (const unsigned char *) id; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '_' || *p == '-')) return false;
    }
    return true;
}

/* Copy the in-memory staging state into the settings snapshot before saving.
 * Successful regular plugins are removed from this list after each batch;
 * the independent layout choice remains staged until setup finishes. */
static void save_pending_selection(void) {
    memset(current_settings.setup_plugin_ids, 0, sizeof(current_settings.setup_plugin_ids));
    size_t count = selected_count < SETUP_PLUGIN_LIMIT ? selected_count : SETUP_PLUGIN_LIMIT;
    for (size_t i = 0; i < count; ++i) {
        if (setup_plugin_id_valid(selected[i].id))
            snprintf(current_settings.setup_plugin_ids[i], sizeof(current_settings.setup_plugin_ids[i]), "%s", selected[i].id);
    }
    if (setup_plugin_id_valid(selected_layout.id))
        snprintf(current_settings.setup_layout_plugin_id, sizeof(current_settings.setup_layout_plugin_id), "%s", selected_layout.id);
    else
        current_settings.setup_layout_plugin_id[0] = '\0';
    settings_save(&current_settings);
}

void gui_setup_plugins_restore_selection(void) {
    clear_selection();
    memset(&selected_layout, 0, sizeof(selected_layout));
    layout_ready = false;
    if (current_settings.setup_complete) return;

    for (size_t i = 0; i < SETTINGS_SETUP_PLUGIN_MAX && selected_count < SETUP_PLUGIN_LIMIT; ++i) {
        const char * id = current_settings.setup_plugin_ids[i];
        if (!setup_plugin_id_valid(id) || selected_index(id) >= 0) continue;
        setup_plugin_t * item = &selected[selected_count++];
        snprintf(item->id, sizeof(item->id), "%s", id);
        snprintf(item->name, sizeof(item->name), "%s", id);
    }
    if (setup_plugin_id_valid(current_settings.setup_layout_plugin_id)) {
        snprintf(selected_layout.id, sizeof(selected_layout.id), "%s", current_settings.setup_layout_plugin_id);
        snprintf(selected_layout.name, sizeof(selected_layout.name), "%s", current_settings.setup_layout_plugin_id);
    }
}

static void clear_batch(void) {
    memset(batch, 0, sizeof(batch));
    memset(batch_is_layout, 0, sizeof(batch_is_layout));
    memset(batch_succeeded, 0, sizeof(batch_succeeded));
    memset(batch_failed, 0, sizeof(batch_failed));
    batch_count = batch_index = batch_completed = 0;
    batch_retry_total = 0;
    batch_retry_pass = false;
    batch_catalog_retried = false;
    batch_refresh_retry = false;
    batch_changed = false;
    batch_stage = BATCH_IDLE;
    batch_on_complete = NULL;
    catalog_count = 0;
    last_displayed_completed = (size_t) -1;
}

static void mark_current_complete(bool succeeded) {
    if (batch_index >= batch_count) return;
    if (succeeded) {
        batch_succeeded[batch_index] = true;
        batch_failed[batch_index] = false;
    } else {
        batch_failed[batch_index] = true;
        batch_succeeded[batch_index] = false;
    }
    batch_completed++;
    batch_index++;
}

static bool batch_item_waits_for_worker(size_t index_before) {
    return batch_index == index_before;
}

static int aggregate_progress(size_t completed, size_t total, int current_percent) {
    if (!total) return 0;
    if (current_percent < 0) current_percent = 0;
    if (current_percent > 100) current_percent = 100;
    if (completed > total) completed = total;
    if (completed == total) return 100;
    return (int) ((completed * 100 + (size_t) current_percent) / total);
}

static size_t failed_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < batch_count; ++i) if (batch_failed[i]) count++;
    return count;
}

static int batch_aggregate_progress(int current_percent) {
    if (batch_retry_pass) {
        int retry = aggregate_progress(batch_completed, batch_retry_total, current_percent);
        return 50 + retry / 2;
    }
    if (failed_count() == 0 && batch_completed >= batch_count) return 100;
    return aggregate_progress(batch_completed, batch_count, current_percent) / 2;
}

static int selected_index(const char * id) {
    if (!id || !id[0]) return -1;
    for (size_t i = 0; i < selected_count; ++i)
        if (strcmp(selected[i].id, id) == 0) return (int) i;
    return -1;
}

size_t gui_setup_plugins_selected_count(void) {
    if (current_settings.setup_complete) clear_selection();
    return selected_count;
}

void gui_setup_plugins_clear_selection(void) {
    if (batch_stage != BATCH_IDLE) return;
    clear_selection();
    save_pending_selection();
}

bool gui_setup_plugins_is_selected(const char * id) {
    if (current_settings.setup_complete) clear_selection();
    return selected_index(id) >= 0;
}

void gui_setup_plugins_toggle(const char * id, const char * name) {
    if (current_settings.setup_complete) {
        clear_selection();
        return;
    }
    if (batch_stage != BATCH_IDLE || !id || !id[0] || strlen(id) >= SETUP_PLUGIN_ID_CAP) return;
    int found = selected_index(id);
    if (found >= 0) {
        for (size_t i = (size_t) found; i + 1 < selected_count; ++i) selected[i] = selected[i + 1];
        memset(&selected[--selected_count], 0, sizeof(selected[0]));
        save_pending_selection();
        return;
    }
    if (selected_count >= SETUP_PLUGIN_LIMIT) {
        show_info_toast(TR("You can select up to 32 plugins"));
        return;
    }
    setup_plugin_t * item = &selected[selected_count++];
    snprintf(item->id, sizeof(item->id), "%s", id);
    snprintf(item->name, sizeof(item->name), "%s", name && name[0] ? name : id);
    save_pending_selection();
}

void gui_setup_plugins_set_layout(const char * id, const char * name) {
    if (current_settings.setup_complete || batch_stage != BATCH_IDLE) return;
    memset(&selected_layout, 0, sizeof(selected_layout));
    if (!id || !id[0] || strlen(id) >= sizeof(selected_layout.id)) {
        save_pending_selection();
        return;
    }
    snprintf(selected_layout.id, sizeof(selected_layout.id), "%s", id);
    snprintf(selected_layout.name, sizeof(selected_layout.name), "%s", name && name[0] ? name : id);
    save_pending_selection();
}

bool gui_setup_plugins_layout_ready(void) {
    return layout_ready;
}

static void hide_popups(void) {
    gui_popup_hide(&result_popup);
}

static void result_backdrop_cb(lv_event_t * e) { (void)e; }

static void popup_add_action(lv_obj_t * popup, const char * text, lv_color_t color,
                             lv_event_cb_t callback) {
    lv_obj_t * row = lv_obj_create(popup);
    style_popup_action_row(row);
    lv_obj_set_style_min_height(row, BOARD_SCALE_PX(64), 0);
    lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

static gui_popup_t * create_batch_popup(gui_popup_t * popup, const char * title_text,
                                        lv_obj_t ** body_out, lv_event_cb_t backdrop_cb,
                                        const char * confirm_text, lv_color_t confirm_color,
                                        lv_event_cb_t confirm_cb, lv_event_cb_t cancel_cb,
                                        lv_obj_t ** title_out) {
    if (popup->popup) return popup;
    popup->popup = build_popup_surface(backdrop_cb, &popup->backdrop);
    int32_t screen_height = lv_display_get_vertical_resolution(lv_display_get_default());
    lv_obj_set_height(popup->popup, LV_SIZE_CONTENT);
    if (screen_height > 0)
        lv_obj_set_style_max_height(popup->popup,
            screen_height - 2 * STATUS_BAR_CLEARANCE, 0);
    lv_obj_remove_flag(popup->popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(popup->popup, LV_DIR_NONE);
    lv_obj_set_style_pad_row(popup->popup, BOARD_SCALE_PX(8), 0);

    lv_obj_t * title = lv_label_create(popup->popup);
    lv_label_set_text(title, title_text);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    if (title_out) *title_out = title;

    lv_obj_t * body = lv_label_create(popup->popup);
    lv_label_set_text(body, "");
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_height(body, BOARD_SCALE_PX(72));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_add_style(body, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(body, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    if (body_out) *body_out = body;

    popup_add_action(popup->popup, confirm_text, confirm_color, confirm_cb);
    if (cancel_cb) popup_add_action(popup->popup, TR("Cancel"), lv_color_make(255, 100, 100), cancel_cb);
    return popup;
}

static bool create_popups(void) {
    create_batch_popup(&result_popup, TR("Plugin setup complete"), &result_body,
                       result_backdrop_cb, TR("Continue setup"), accent_lv_color(),
                       result_ack_cb, NULL, &result_title);
    return result_popup.popup != NULL;
}

static void remove_successful_selections(void) {
    bool removed = false;
    for (size_t i = 0; i < batch_count; ++i) {
        if (!batch_succeeded[i]) continue;
        int found = selected_index(batch[i].id);
        if (found < 0) continue;
        for (size_t j = (size_t) found; j + 1 < selected_count; ++j) selected[j] = selected[j + 1];
        memset(&selected[--selected_count], 0, sizeof(selected[0]));
        removed = true;
    }
    if (removed) save_pending_selection();
}

static void update_progress(int current_percent) {
    if (!batch_ui_attached || !batch_count) return;
    int progress = batch_aggregate_progress(current_percent);
    if (last_displayed_completed != batch_completed) {
        char title[96];
        snprintf(title, sizeof(title), batch_retry_pass ? TR("Retrying plugins %zu/%zu") : TR("Downloading and installing plugins %zu/%zu"),
                 batch_completed, batch_retry_pass ? batch_retry_total : batch_count);
        batch_busy = gui_busy_show(title, batch_index < batch_count ? batch[batch_index].name : "");
        batch_ui_attached = true;
        last_displayed_completed = batch_completed;
    } else if (batch_index < batch_count) {
        gui_busy_set_detail(batch_busy, batch[batch_index].name);
    }
    gui_busy_set_progress(batch_busy, progress);
}

static void attach_progress(void) {
    if (batch_ui_attached || batch_stage == BATCH_IDLE) return;
    char title[96];
    snprintf(title, sizeof(title), batch_retry_pass ? TR("Retrying plugins %zu/%zu") : TR("Downloading and installing plugins %zu/%zu"),
             batch_completed, batch_retry_pass ? batch_retry_total : batch_count);
    batch_busy = gui_busy_show(title, batch_index < batch_count ? batch[batch_index].name : "");
    gui_busy_set_progress(batch_busy, batch_retry_pass ? 50 + (batch_retry_total ? (int)(batch_completed * 50 / batch_retry_total) : 0) : (batch_count ? (int) (batch_completed * 50 / batch_count) : 0));
    batch_ui_attached = true;
    last_displayed_completed = batch_completed;
}

static void detach_progress(void) {
    if (batch_ui_attached) gui_busy_hide(batch_busy);
    batch_ui_attached = false;
    batch_busy = 0;
    last_displayed_completed = (size_t) -1;
}

static void delete_batch_timer(void) {
    if (batch_timer) {
        lv_timer_delete(batch_timer);
        batch_timer = NULL;
    }
}

static void schedule_batch_timer(void) {
    if (!batch_timer) batch_timer = lv_timer_create(batch_poll_cb, 250, NULL);
}

static bool completed_layout_ready(void) {
    for (size_t i = 0; i < batch_count; ++i)
        if (batch_is_layout[i]) return batch_succeeded[i];
    return false;
}

static void finish_batch(bool success, const char * failure_message) {
    (void) success;
    (void) failure_message;
    delete_batch_timer();
    detach_progress();
    remove_successful_selections();
    layout_ready = completed_layout_ready();
    show_batch_result();
}

static void fail_current_and_continue(const char * message) {
    (void) message;
    if (batch_index < batch_count) mark_current_complete(false);
    start_next_batch_item();
}

static void show_batch_result(void) {
    if (!create_popups()) return;
    char body[SETUP_BATCH_LIMIT * (SETUP_PLUGIN_NAME_CAP + 4) + 256];
    size_t used = 0, failures = failed_count();
    if (failures) {
        int n = snprintf(body, sizeof(body), TR("Failed plugins:\n"));
        used = n > 0 ? (size_t)n : 0;
        for (size_t i = 0; i < batch_count && used < sizeof(body); ++i) {
            if (!batch_failed[i]) continue;
            n = snprintf(body + used, sizeof(body) - used, "- %s\n", batch[i].name);
            if (n < 0) break;
            used += (size_t)n < sizeof(body) - used ? (size_t)n : sizeof(body) - used - 1;
        }
    } else {
        int n = snprintf(body, sizeof(body), TR("Selected plugins are ready.\n"));
        used = n > 0 ? (size_t)n : 0;
    }
    if (used < sizeof(body))
        snprintf(body + used, sizeof(body) - used,
                 TR("Manage plugins later in Settings > System > Plugin Manager to search, update, or remove them."));
    if (result_title) lv_label_set_text(result_title, failures ? TR("Plugin setup needs attention") : TR("Plugin setup complete"));
    if (result_body) lv_label_set_text(result_body, body);
    if (result_body) {
        int32_t screen = lv_display_get_vertical_resolution(lv_display_get_default());
        int32_t cap = screen - 2 * STATUS_BAR_CLEARANCE - BOARD_SCALE_PX(176);
        int32_t desired = (int32_t) (failures + 4) * BOARD_SCALE_PX(30) + BOARD_SCALE_PX(24);
        if (desired < BOARD_SCALE_PX(72)) desired = BOARD_SCALE_PX(72);
        if (desired > cap) desired = cap;
        lv_obj_set_height(result_body, desired);
    }
    batch_stage = BATCH_RESULT;
    gui_popup_show(&result_popup);
}

static void result_ack_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || batch_stage != BATCH_RESULT) return;
    gui_popup_hide(&result_popup);
    void (*done)(void) = batch_on_complete;
    bool changed = batch_changed;
    clear_batch();
    if (done) done();
    else if (changed) gui_reload_request();
}

static int find_catalog_item(const char * id) {
    for (size_t i = 0; i < catalog_count; ++i) {
        if (catalog[i].id[0] && strcmp(catalog[i].id, id) == 0) return (int) i;
    }
    return -1;
}

static void update_batch_names_from_catalog(void) {
    for (size_t i = 0; i < batch_count; ++i) {
        int item_index = find_catalog_item(batch[i].id);
        if (item_index < 0 || !catalog[item_index].name[0]) continue;
        snprintf(batch[i].name, sizeof(batch[i].name), "%s", catalog[item_index].name);
        if (batch_is_layout[i]) {
            if (strcmp(selected_layout.id, batch[i].id) == 0)
                snprintf(selected_layout.name, sizeof(selected_layout.name), "%s", catalog[item_index].name);
        } else {
            int selected_item = selected_index(batch[i].id);
            if (selected_item >= 0)
                snprintf(selected[selected_item].name, sizeof(selected[selected_item].name), "%s", catalog[item_index].name);
        }
    }
}

static bool begin_install(void) {
    if (batch_index >= batch_count) return false;
    int item_index = find_catalog_item(batch[batch_index].id);
    if (item_index < 0) {
        mark_current_complete(false);
        return true;
    }
    plugin_store_result_t * item = &catalog[item_index];
    if (item->incompatible || item->state == PLUGIN_STORE_PLUGIN_INCOMPATIBLE ||
        item->state == PLUGIN_STORE_PLUGIN_REMOVED) {
        mark_current_complete(false);
        return true;
    }
    bool started = item->state == PLUGIN_STORE_PLUGIN_UPDATE
        ? plugin_store_update(item->id, true)
        : plugin_store_install(item->id, true);
    if (!started) {
        mark_current_complete(false);
        return true;
    }
    batch_stage = batch_retry_pass ? BATCH_RETRYING : BATCH_INSTALLING;
    if (batch_ui_attached) gui_busy_set_detail(batch_busy, batch[batch_index].name);
    return true;
}

static void start_next_batch_item(void) {
    while (batch_index < batch_count) {
        if (batch_retry_pass && !batch_failed[batch_index]) {
            batch_index++;
            continue;
        }
        size_t item_before = batch_index;
        if (!begin_install()) return;
        if (batch_item_waits_for_worker(item_before)) {
            attach_progress();
            schedule_batch_timer();
            return;
        }
    }
    if (!batch_retry_pass && failed_count()) {
        batch_retry_pass = true;
        batch_retry_total = failed_count();
        batch_completed = 0;
        batch_index = 0;
        batch_stage = BATCH_RETRYING;
        last_displayed_completed = (size_t)-1;
        attach_progress();
        start_next_batch_item();
        return;
    }
    finish_batch(true, NULL);
}

static void batch_poll_cb(lv_timer_t * timer) {
    (void) timer;
    if (current_settings.setup_complete) {
        clear_selection();
        memset(&selected_layout, 0, sizeof(selected_layout));
        delete_batch_timer();
        hide_popups();
        detach_progress();
        clear_batch();
        return;
    }
    if (batch_stage == BATCH_IDLE) {
        delete_batch_timer();
        return;
    }
    plugin_store_status_t status;
    plugin_store_get_status(&status, catalog, PLUGIN_STORE_MAX_RESULTS);
    if (batch_stage == BATCH_REFRESHING) {
        if (status.state == PLUGIN_STORE_REFRESHING) {
            update_progress(0);
            return;
        }
        if (status.state == PLUGIN_STORE_FAILED || status.state == PLUGIN_STORE_IDLE) {
            if (!batch_catalog_retried) {
                batch_catalog_retried = true;
                batch_refresh_retry = true;
                if (plugin_store_refresh()) {
                    batch_stage = BATCH_REFRESHING;
                    return;
                }
            }
            for (size_t i = 0; i < batch_count; ++i) batch_failed[i] = true;
            batch_retry_pass = true;
            batch_retry_total = batch_count;
            batch_completed = batch_count;
            batch_index = batch_count;
            finish_batch(false, status.error[0] ? status.error : TR("Could not load the plugin catalog."));
            return;
        }
        if (status.state == PLUGIN_STORE_READY) {
            catalog_count = status.result_count;
            update_batch_names_from_catalog();
            if (batch_refresh_retry) {
                batch_refresh_retry = false;
                batch_retry_pass = true;
                batch_retry_total = batch_count;
                batch_completed = batch_index = 0;
                for (size_t i = 0; i < batch_count; ++i) batch_failed[i] = true;
            }
            batch_stage = batch_retry_pass ? BATCH_RETRYING : BATCH_INSTALLING;
            if (!batch_retry_pass) batch_index = batch_completed = 0;
            start_next_batch_item();
            return;
        }
        return;
    }
    if (batch_stage != BATCH_INSTALLING && batch_stage != BATCH_RETRYING) return;
    if (status.state == PLUGIN_STORE_INSTALLING || status.state == PLUGIN_STORE_UPDATING) {
        update_progress(status.percent);
        return;
    }
    if (status.state == PLUGIN_STORE_NEEDS_CONFIRM) {
        /* Setup selections explicitly consent to replacing local plugin files. */
        batch_changed = batch_changed || status.changed;
        fail_current_and_continue(TR("Could not replace plugin files."));
        return;
    }
    if (status.state == PLUGIN_STORE_FAILED || status.state == PLUGIN_STORE_IDLE) {
        batch_changed = batch_changed || status.changed;
        mark_current_complete(false);
        start_next_batch_item();
        return;
    }
    if (status.state == PLUGIN_STORE_READY) {
        batch_changed = batch_changed || status.changed;
        mark_current_complete(true);
        update_progress(0);
        start_next_batch_item();
    }
}

bool gui_setup_plugins_start(void (*on_complete)(void)) {
    if (current_settings.setup_complete) {
        clear_selection();
        memset(&selected_layout, 0, sizeof(selected_layout));
    }
    if (batch_stage != BATCH_IDLE || plugin_store_busy() || gui_plugin_store_operation_active()) {
        show_error_toast(TR("A plugin operation is already in progress"));
        return false;
    }
    clear_batch();
    layout_ready = false;
    batch_on_complete = on_complete;
    if (selected_count == 0 && selected_layout.id[0] == '\0') {
        void (*done)(void) = batch_on_complete;
        batch_on_complete = NULL;
        if (done) done();
        return true;
    }
    if (!gui_network_setup_wifi_connected()) {
        clear_batch();
        show_error_toast(TR("No network detected. Connect to a network to download plugins."));
        return false;
    }
    if (!create_popups()) {
        clear_batch();
        show_error_toast(TR("Could not start the plugin operation"));
        return false;
    }
    memcpy(batch, selected, selected_count * sizeof(selected[0]));
    batch_count = selected_count;
    if (selected_layout.id[0]) {
        batch[batch_count] = selected_layout;
        batch_is_layout[batch_count] = true;
        batch_count++;
    }
    if (!plugin_store_refresh()) {
        clear_batch();
        show_error_toast(TR("Could not start refreshing the plugin catalog."));
        return false;
    }
    batch_stage = BATCH_REFRESHING;
    attach_progress();
    schedule_batch_timer();
    return true;
}

bool gui_setup_plugins_busy(void) {
    return batch_stage != BATCH_IDLE;
}

void gui_setup_plugins_teardown(void) {
    if (batch_timer) {
        lv_timer_delete(batch_timer);
        batch_timer = NULL;
    }
    hide_popups();
    gui_popup_teardown(&result_popup);
    result_body = result_title = NULL;
    detach_progress();
    if (current_settings.setup_complete) {
        clear_selection();
        memset(&selected_layout, 0, sizeof(selected_layout));
        clear_batch();
    }
}

void gui_setup_plugins_resume(void) {
    if (current_settings.setup_complete) {
        clear_selection();
        memset(&selected_layout, 0, sizeof(selected_layout));
        clear_batch();
        return;
    }
    if (batch_stage == BATCH_IDLE) return;
    if (!create_popups()) return;
    if (batch_stage == BATCH_RESULT) {
        show_batch_result();
    } else {
        attach_progress();
        schedule_batch_timer();
    }
}

#ifdef GUI_SETUP_PLUGINS_STATE_TEST
/* A deterministic state-only check for batch completion and progress.
 * It deliberately calls no catalog/network/UI entry point. Build this file with
 * -DGUI_SETUP_PLUGINS_STATE_TEST -ffunction-sections -fdata-sections and
 * --gc-sections to run the checks with the tiny main below. */
int main(void) {
    clear_selection();
    clear_batch();
    batch_count = 2;
    batch_stage = BATCH_INSTALLING;

    /* Already installed first: it completes synchronously and the second
     * available item must still wait for its worker rather than being
     * mistaken for a second synchronous skip. */
    mark_current_complete(true);
    if (batch_index != 1 || batch_completed != 1 || batch_item_waits_for_worker(0)) return 1;
    size_t available_index = batch_index;
    if (!batch_item_waits_for_worker(available_index)) return 2;
    mark_current_complete(true);
    if (batch_index != 2 || batch_completed != 2 || !batch_succeeded[0] || !batch_succeeded[1]) return 3;

    /* Two ordinary successes produce the same fully-complete counters. */
    clear_batch();
    batch_count = 2;
    mark_current_complete(true);
    mark_current_complete(true);
    if (batch_completed != 2 || batch_index != 2) return 4;

    int progress[] = {
        aggregate_progress(0, 2, 0), aggregate_progress(0, 2, 50), aggregate_progress(0, 2, 100),
        aggregate_progress(1, 2, 0), aggregate_progress(1, 2, 50), aggregate_progress(1, 2, 100),
        aggregate_progress(2, 2, 0)
    };
    for (size_t i = 1; i < sizeof(progress) / sizeof(progress[0]); ++i)
        if (progress[i] < progress[i - 1]) return 7;

    /* Retry pass contains failed IDs only. A successful retry clears its
     * failure; a second failure remains visible and is not eligible again. */
    clear_batch();
    batch_count = 3;
    mark_current_complete(false);
    mark_current_complete(true);
    mark_current_complete(false);
    batch_retry_pass = true;
    batch_retry_total = failed_count();
    batch_completed = batch_index = 0;
    size_t retry_visits = 0;
    for (size_t i = 0; i < batch_count; ++i) {
        if (!batch_failed[i]) continue;
        retry_visits++;
        batch_index = i;
        if (i == 0) mark_current_complete(true); /* retry succeeds */
        else mark_current_complete(false);        /* retry still fails */
    }
    if (retry_visits != 2 || batch_failed[0] || !batch_failed[2] || batch_failed[1]) return 8;
    size_t remaining_retry_visits = 0;
    for (size_t i = 0; i < batch_count; ++i) if (batch_failed[i]) remaining_retry_visits++;
    if (remaining_retry_visits != 1) return 9;

    /* Partial failure: remove only a completed success, leaving the failed
     * and not-yet-attempted IDs selected for retry. */
    clear_selection();
    snprintf(selected[0].id, sizeof(selected[0].id), "done");
    snprintf(selected[1].id, sizeof(selected[1].id), "failed");
    snprintf(selected[2].id, sizeof(selected[2].id), "pending");
    selected_count = 3;
    clear_batch();
    batch_count = 3;
    snprintf(batch[0].id, sizeof(batch[0].id), "done");
    batch_succeeded[0] = true;
    remove_successful_selections();
    if (selected_count != 2 || strcmp(selected[0].id, "failed") || strcmp(selected[1].id, "pending")) return 10;

    /* The batch supports 32 regular selections plus one independently
     * replaceable layout result, and clear_batch leaves the result readable. */
    clear_batch();
    batch_count = SETUP_BATCH_LIMIT;
    batch_is_layout[SETUP_BATCH_LIMIT - 1] = true;
    batch_succeeded[SETUP_BATCH_LIMIT - 1] = true;
    layout_ready = completed_layout_ready();
    if (!layout_ready) return 11;
    clear_batch();
    if (!layout_ready) return 12;
    batch_failed[SETUP_BATCH_LIMIT - 1] = true;
    batch_succeeded[SETUP_BATCH_LIMIT - 1] = false;
    layout_ready = completed_layout_ready();
    if (layout_ready) return 13;

    return 0;
}
#endif
