#include "gui_emulator_store.h"
#include "../../text/i18n/i18n.h"

#include "../../../network/store/catalog/emulator_store.h"
#include "../../shell/gui_navigation.h"
#include "../../network/gui_network.h"
#include "../../shell/gui_notifications.h"
#include "../../shell/gui_shell.h"
#include "../../appearance/gui_theme.h"
#include "../../widgets/builders/screen_builders.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { ACTION_PRIMARY, ACTION_REMOVE, ACTION_CANCEL } store_action_t;

static lv_obj_t * store_screen;
static lv_obj_t * store_list;
static emulator_store_result_t * store_rows;
static size_t store_row_count;
static int store_selected_index = -1;
static char store_selected_id[33];
static store_action_t detail_primary_action;
static bool store_ui_active;        /* a worker the busy screen is showing */
static bool store_is_refresh;       /* that worker is the catalog load */
static bool store_open_when_ready;  /* push the list once the catalog is in */
static bool store_push_pending;     /* ...after the busy screen has slid away */
static lv_obj_t * store_open_origin;
static gui_busy_handle_t store_busy;
static gui_popup_t detail_popup, remove_popup;
static lv_obj_t * detail_title, * detail_body, * remove_title;

static void populate_store_screen(void);

bool gui_emulator_store_operation_active(void) {
    return store_ui_active || store_push_pending || emulator_store_busy();
}

static bool result_is_installed(const emulator_store_result_t * row) {
    return row->state == EMULATOR_STORE_INSTALLED || row->state == EMULATOR_STORE_UPDATE;
}

static const char * status_label(const emulator_store_result_t * row, char * out, size_t cap) {
    switch (row->state) {
        case EMULATOR_STORE_INCOMPATIBLE:
            return TR("Needs newer firmware");
        case EMULATOR_STORE_UPDATE:
            snprintf(out, cap, TR("Update available · %s"), row->version);
            return out;
        case EMULATOR_STORE_INSTALLED:
            snprintf(out, cap, TR("Installed · %s"), row->version);
            return out;
        default:
            snprintf(out, cap, TR("Available · %s"), row->version);
            return out;
    }
}

static void store_action_cb(lv_event_t * e);

static void popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&detail_popup);
    gui_popup_hide(&remove_popup);
}

static void build_popups(void) {
    build_titled_popup(&detail_popup, popup_backdrop_cb, &detail_title, &detail_body);
    add_popup_action_row(detail_popup.popup, TR("Install"), accent_lv_color(), store_action_cb,
                         (void *) (intptr_t) ACTION_PRIMARY);
    add_popup_action_row(detail_popup.popup, TR("Remove"), lv_color_hex(GUI_BREEZE_NEGATIVE), store_action_cb,
                         (void *) (intptr_t) ACTION_REMOVE);
    add_popup_action_row(detail_popup.popup, TR("Cancel"), lv_color_hex(GUI_BREEZE_NEGATIVE), store_action_cb,
                         (void *) (intptr_t) ACTION_CANCEL);

    build_titled_popup(&remove_popup, popup_backdrop_cb, &remove_title, NULL);
    lv_obj_t * body = lv_label_create(remove_popup.popup);
    lv_label_set_text(body, TR("Bundled games and their saves will be removed. Games in SD/Games are kept."));
    lv_obj_set_width(body, lv_pct(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(body, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(body, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    add_popup_action_row(remove_popup.popup, TR("Remove"), lv_color_hex(GUI_BREEZE_NEGATIVE), store_action_cb,
                         (void *) (intptr_t) ACTION_REMOVE);
    add_popup_action_row(remove_popup.popup, TR("Cancel"), lv_color_hex(GUI_BREEZE_NEGATIVE), store_action_cb,
                         (void *) (intptr_t) ACTION_CANCEL);
}

static void show_details(void) {
    if (store_selected_index < 0 || (size_t) store_selected_index >= store_row_count) return;
    const emulator_store_result_t * row = &store_rows[store_selected_index];
    snprintf(store_selected_id, sizeof(store_selected_id), "%s", row->id);
    emulator_store_details_t details = {0};
    bool has_details = emulator_store_get_details(row->id, &details);
    char status[96], body[900], extra[160] = "";
    const char * state = status_label(row, status, sizeof(status));
    size_t used = (size_t) snprintf(body, sizeof(body), "%s\n\n%s\n\n%s",
                                    has_details && details.description[0] ? details.description : TR("No description"),
                                    row->systems, state);
    if (has_details && details.author[0]) {
        snprintf(extra, sizeof(extra), TR("By %s"), details.author);
        used += (size_t) snprintf(body + used, sizeof(body) - used, "\n\n%s", extra);
    }
    if (has_details && details.license[0]) {
        snprintf(extra, sizeof(extra), TR("License: %s"), details.license);
        used += (size_t) snprintf(body + used, sizeof(body) - used, "\n%s", extra);
    }
    if (has_details && details.size) {
        char size[32];
        snprintf(size, sizeof(size), "%.1f MB", (double) details.size / (1024.0 * 1024.0));
        snprintf(extra, sizeof(extra), TR("Size: %s"), size);
        snprintf(body + used, sizeof(body) - used, "\n%s", extra);
    }
    lv_label_set_text(detail_title, row->name[0] ? row->name : row->id);
    bool incompatible = row->state == EMULATOR_STORE_INCOMPATIBLE;
    bool update = row->state == EMULATOR_STORE_UPDATE;
    if (update)
        lv_label_set_text_fmt(detail_body, "%s\n\n%s",
                             TR("Updating replaces bundled games and their saves. Games in SD/Games are kept."), body);
    else
        lv_label_set_text(detail_body, body);

    bool installed = result_is_installed(row);
    lv_obj_t * primary = lv_obj_get_child(detail_popup.popup, 2);
    lv_obj_t * remove_row = lv_obj_get_child(detail_popup.popup, 3);
    if (primary) {
        lv_label_set_text(lv_obj_get_child(primary, 0), update ? TR("Update") : TR("Install"));
        detail_primary_action = ACTION_PRIMARY;
        lv_obj_set_flag(primary, LV_OBJ_FLAG_HIDDEN, installed && !update);
        if (incompatible) lv_obj_add_flag(primary, LV_OBJ_FLAG_HIDDEN);
    }
    if (remove_row) lv_obj_set_flag(remove_row, LV_OBJ_FLAG_HIDDEN, !installed);
    lv_obj_scroll_to_y(lv_obj_get_parent(detail_body), 0, LV_ANIM_OFF);
    gui_popup_show(&detail_popup);
}

static void store_row_clicked_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    store_selected_index = (int) (intptr_t) lv_event_get_user_data(e);
    show_details();
}

static lv_obj_t * add_row_label(lv_obj_t * parent, const char * text, const lv_font_t * font, lv_style_t * style) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_add_style(label, style, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

/* Name, systems and state on one tappable row, laid out like the Plugin
 * Store's rows. */
static void add_emulator_row(int index) {
    const emulator_store_result_t * row = &store_rows[index];
    char status[64];
    const char * name = row->name[0] ? row->name : row->id;
    lv_obj_t * pill = add_pill_row_base(store_list, name);
    lv_obj_delete(lv_obj_get_child(pill, 0)); /* Replace the base row's one-line title. */
    const lv_font_t * body_font = gui_theme_font(GUI_FONT_ROLE_BODY);
    const lv_font_t * subtext_font = gui_theme_font(GUI_FONT_ROLE_SUBTEXT);
    int32_t row_height = lv_font_get_line_height(body_font) + 2 * lv_font_get_line_height(subtext_font) +
                         2 * BOARD_SCALE_PX(4) + 2 * BOARD_SCALE_PX(10);
    if (row_height < GUI_SETTINGS_ROW_HEIGHT + BOARD_SCALE_PX(8)) row_height = GUI_SETTINGS_ROW_HEIGHT + BOARD_SCALE_PX(8);
    lv_obj_set_height(pill, row_height);

    lv_obj_t * text = lv_obj_create(pill);
    lv_obj_remove_style_all(text);
    lv_obj_set_size(text, pill_row_default_width() - GUI_TEXT_INSET - BOARD_SCALE_PX(72), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(text, BOARD_SCALE_PX(4), 0);
    lv_obj_align(text, LV_ALIGN_LEFT_MID, GUI_TEXT_INSET, 0);
    lv_obj_remove_flag(text, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    add_row_label(text, name, body_font, &style_theme_text_primary);
    add_row_label(text, row->systems, subtext_font, &style_theme_text_muted);
    add_row_label(text, status_label(row, status, sizeof(status)), subtext_font, &style_theme_text_muted);
    build_navigation_chevron(pill, BOARD_SCALE_PX(20), false, 0);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pill, store_row_clicked_cb, LV_EVENT_CLICKED, (void *) (intptr_t) index);
}

static void add_section(const char * title, bool (*match)(const emulator_store_result_t *)) {
    bool any = false;
    for (size_t i = 0; i < store_row_count; i++) {
        if (!match(&store_rows[i])) continue;
        if (!any) add_section_header(store_list, title);
        any = true;
        add_emulator_row((int) i);
    }
}

static bool row_is_update(const emulator_store_result_t * row) { return row->state == EMULATOR_STORE_UPDATE; }
static bool row_is_current(const emulator_store_result_t * row) { return row->state == EMULATOR_STORE_INSTALLED; }
static bool row_is_available(const emulator_store_result_t * row) {
    return row->state == EMULATOR_STORE_AVAILABLE || row->state == EMULATOR_STORE_INCOMPATIBLE;
}

static void refresh_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_emulator_store_open();
}

static void populate_store_screen(void) {
    if (!store_screen || !store_list || !store_rows) return;
    plugin_store_status_t status;
    emulator_store_get_status(&status, store_rows, EMULATOR_STORE_MAX_RESULTS);
    store_row_count = status.result_count;
    lv_obj_clean(store_list);
    if (store_row_count == 0 && status.state == PLUGIN_STORE_READY) {
        lv_obj_t * empty = lv_label_create(store_list);
        lv_label_set_text(empty, TR("No emulators are available in the catalog."));
        lv_obj_add_style(empty, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(empty, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_set_width(empty, lv_pct(90));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(empty, BOARD_SCALE_PX(48), 0);
        add_pill_chevron_row(store_list, TR("Refresh emulator catalog"), refresh_row_cb);
        return;
    }
    add_section(TR("Updates"), row_is_update);
    add_section(TR("Installed"), row_is_current);
    add_section(TR("Available"), row_is_available);
    lv_obj_update_layout(store_list);
}

static void store_screen_loaded_cb(lv_event_t * e) {
    (void) e;
    populate_store_screen();
}

static bool start_operation(plugin_store_state_t state) {
    bool started = state == PLUGIN_STORE_INSTALLING ? emulator_store_install(store_selected_id) :
                   state == PLUGIN_STORE_UPDATING ? emulator_store_update(store_selected_id) :
                   emulator_store_uninstall(store_selected_id);
    if (!started) {
        show_error_toast(TR("Could not start the emulator operation"));
        return false;
    }
    store_ui_active = true;
    store_is_refresh = false;
    const char * title = state == PLUGIN_STORE_INSTALLING ? TR("Installing emulator") :
                         state == PLUGIN_STORE_UPDATING ? TR("Updating emulator") : TR("Removing emulator");
    store_busy = gui_busy_show(title, TR("This may take a while"));
    gui_busy_set_progress(store_busy, 0);
    return true;
}

static void store_action_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    store_action_t action = (store_action_t) (intptr_t) lv_event_get_user_data(e);
    if (action == ACTION_CANCEL) {
        gui_popup_hide(&detail_popup);
        gui_popup_hide(&remove_popup);
        return;
    }
    if (action == ACTION_REMOVE && gui_popup_is_visible(&detail_popup)) {
        char title[128];
        snprintf(title, sizeof(title), TR("Remove %s?"), store_rows[store_selected_index].name);
        lv_label_set_text(remove_title, title);
        gui_popup_hide(&detail_popup);
        gui_popup_show(&remove_popup);
        return;
    }
    if (action == ACTION_REMOVE) {
        gui_popup_hide(&remove_popup);
        (void) start_operation(PLUGIN_STORE_UNINSTALLING);
        return;
    }
    /* ACTION_PRIMARY: Install, or Update when the row has a newer version. */
    bool update = store_selected_index >= 0 && (size_t) store_selected_index < store_row_count &&
                  store_rows[store_selected_index].state == EMULATOR_STORE_UPDATE;
    gui_popup_hide(&detail_popup);
    (void) start_operation(update ? PLUGIN_STORE_UPDATING : PLUGIN_STORE_INSTALLING);
}

void gui_emulator_store_open(void) {
    if (!gui_shell_wifi_connected()) {
        show_error_toast(TR("No network detected. Connect to a network to download emulators."));
        return;
    }
    if (!store_rows || !gui_shell_wifi_effective_enabled()) {
        show_error_toast(store_rows ? TR("Turn on Wi-Fi and connect first") : TR("Emulator operation failed"));
        return;
    }
    if (gui_emulator_store_operation_active()) {
        show_error_toast(TR("An emulator operation is already in progress"));
        return;
    }
    if (!emulator_store_refresh()) {
        show_error_toast(TR("Could not start the emulator operation"));
        return;
    }
    store_open_origin = lv_screen_active();
    store_ui_active = true;
    store_is_refresh = true;
    store_open_when_ready = (store_open_origin != store_screen);
    store_busy = gui_busy_show(TR("Loading emulators"), TR("This may take a while"));
}

void poll_emulator_store(void) {
    if (store_push_pending) {
        /* Pushing while the busy screen still slides away would let that
         * animation load the screen underneath over the list. */
        if (gui_navigation_transition_in_progress()) return;
        store_push_pending = false;
        if (store_open_origin && !gui_navigation_is_top(store_open_origin)) {
            store_open_origin = NULL;
            return;
        }
        store_open_origin = NULL;
        nav_push(store_screen);
        return;
    }
    if (!store_ui_active) return;
    if (store_open_when_ready && store_is_refresh && !gui_navigation_is_top(gui_busy_get_screen())) {
        /* The user backed out of the loading screen. Let the catalog worker
         * finish, but do not open the list afterwards. */
        store_open_when_ready = false;
        store_open_origin = NULL;
    }
    plugin_store_status_t status;
    emulator_store_get_status(&status, NULL, 0);
    switch (status.state) {
        case PLUGIN_STORE_IDLE: /* reset underneath us by a UI reload */
            gui_busy_hide(store_busy);
            store_ui_active = false;
            store_open_when_ready = false;
            break;
        case PLUGIN_STORE_FAILED:
            gui_busy_hide(store_busy);
            store_ui_active = false;
            store_open_when_ready = false;
            populate_store_screen();
            show_error_toast(status.error[0] ? status.error : TR("Emulator operation failed"));
            break;
        case PLUGIN_STORE_READY:
            gui_busy_hide(store_busy);
            store_ui_active = false;
            populate_store_screen();
            if (store_open_when_ready) {
                store_open_when_ready = false;
                store_push_pending = true;
            }
            store_is_refresh = false;
            break;
        default:
            gui_busy_set_progress(store_busy, status.percent);
            break;
    }
}

void gui_emulator_store_init(void) {
    store_rows = calloc(EMULATOR_STORE_MAX_RESULTS, sizeof(*store_rows));
    store_row_count = 0;
    lv_obj_t * title_label;
    store_screen = build_subsonic_list_screen(TR("Download emulators"), &title_label, &store_list);
    lv_obj_add_event_cb(store_screen, store_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    build_popups();
}

void gui_emulator_store_teardown(void) {
    gui_popup_teardown(&detail_popup);
    gui_popup_teardown(&remove_popup);
    if (store_screen) {
        lv_obj_delete(store_screen);
        store_screen = NULL;
        store_list = NULL;
    }
    free(store_rows);
    store_rows = NULL;
    store_row_count = 0;
    store_push_pending = false;
    store_open_when_ready = false;
    store_open_origin = NULL;
    /* A running worker keeps reporting to the rebuilt screens; otherwise
     * start over. */
    if (!emulator_store_busy()) {
        emulator_store_reset();
        store_ui_active = false;
    }
}
