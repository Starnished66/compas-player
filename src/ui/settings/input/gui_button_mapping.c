#include "gui_button_mapping.h"

#include "gui_button_bindings.h"
#include "../../shell/gui_navigation.h"
#include "../../shell/gui_notifications.h"
#include "../../appearance/gui_theme.h"
#include "../../text/i18n/i18n.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../../core/persistence/settings.h"

#include <stdint.h>
#include <stdio.h>

static lv_obj_t * button_mapping_screen;
static lv_obj_t * button_mapping_rows[BUTTON_MAPPING_BUTTON_COUNT];
static lv_obj_t * button_mapping_summaries[BUTTON_MAPPING_BUTTON_COUNT];
static lv_obj_t * button_detail_screens[BUTTON_MAPPING_BUTTON_COUNT];
static lv_obj_t * button_detail_summaries[BUTTON_MAPPING_BUTTON_COUNT][3];
static lv_obj_t * button_action_screens[BUTTON_MAPPING_BUTTON_COUNT][3];
static lv_obj_t * button_action_lists[BUTTON_MAPPING_BUTTON_COUNT][3];
static gui_popup_t button_mapping_reset_popup;
static lv_obj_t * settings_button_mapping_summary;

static const char * button_mapping_button_label(int button) {
    switch (button) {
        case BUTTON_MAPPING_POWER: return TR("Power");
        case BUTTON_MAPPING_PLAY_PAUSE: return TR("Play/Pause");
        case BUTTON_MAPPING_NEXT: return TR("Next Track");
        case BUTTON_MAPPING_PREVIOUS: return TR("Previous Track");
#if defined(BOARD_R3II_2025)
        case BUTTON_MAPPING_VOLUME_UP: return TR("Clockwise");
        case BUTTON_MAPPING_VOLUME_DOWN: return TR("Counterclockwise");
#else
        case BUTTON_MAPPING_VOLUME_UP: return TR("Volume Up");
        case BUTTON_MAPPING_VOLUME_DOWN: return TR("Volume Down");
#endif
        default: return TR("Button");
    }
}

static bool button_mapping_button_available(int button) {
    if (button < 0 || button >= BUTTON_MAPPING_BUTTON_COUNT) return false;
#if BOARD_HAS_PREVIOUS_BUTTON
    return true;
#else
    return button != BUTTON_MAPPING_PREVIOUS;
#endif
}

static const char * button_mapping_action_label(button_mapping_action_t action) {
    static const char * const labels[BUTTON_MAPPING_ACTION_COUNT] = {
        N_("Default (native behavior)"), N_("Play/Pause"), N_("Next Track"),
        N_("Previous Track"), N_("Volume Up"), N_("Volume Down"),
        N_("Seek Forward"), N_("Seek Backward"), N_("Toggle Screen"),
        N_("Power Menu"), N_("Do Nothing")
    };
    if (action < BUTTON_MAPPING_ACTION_DEFAULT || action >= BUTTON_MAPPING_ACTION_COUNT)
        action = BUTTON_MAPPING_ACTION_DEFAULT;
    return TR(labels[action]);
}

static button_mapping_action_t * button_mapping_binding_action(int button, int gesture) {
    button_mapping_binding_t *binding = &current_settings.button_bindings[button];
    if (gesture == 0) return &binding->tap;
    if (gesture == 1) return &binding->double_tap;
    return &binding->hold;
}

static const char * button_mapping_gesture_label(int gesture) {
    if (gesture == 0) return TR("Single press");
    if (gesture == 1) return TR("Double press");
    return TR("Long press");
}

static const char * button_mapping_gesture_short_label(int gesture) {
    if (gesture == 0) return TR("Single");
    if (gesture == 1) return TR("Double");
    return TR("Long");
}

static bool button_mapping_button_single_press_only(int button) {
#if defined(BOARD_R3II_2025)
    return button == BUTTON_MAPPING_VOLUME_UP || button == BUTTON_MAPPING_VOLUME_DOWN;
#else
    (void)button;
    return false;
#endif
}

void gui_button_mapping_refresh_summaries(void) {
    for (int button = 0; button < BUTTON_MAPPING_BUTTON_COUNT; ++button) {
        button_mapping_binding_t *binding = &current_settings.button_bindings[button];
        button_mapping_action_t actions[3] = { binding->tap, binding->double_tap, binding->hold };
        const int gesture_count = button_mapping_button_single_press_only(button) ? 1 : 3;
        bool is_default = true;
        char summary[192] = "";
        size_t used = 0;
        for (int gesture = 0; gesture < gesture_count; ++gesture) {
            if (actions[gesture] == BUTTON_MAPPING_ACTION_DEFAULT) continue;
            is_default = false;
            const char * action = button_mapping_action_label(actions[gesture]);
            int written = snprintf(summary + used, sizeof(summary) - used, "%s%s: %s",
                                   used ? " · " : "", button_mapping_gesture_short_label(gesture), action);
            if (written < 0 || (size_t)written >= sizeof(summary) - used) {
                summary[sizeof(summary) - 1] = '\0';
                break;
            }
            used += (size_t)written;
        }
        if (is_default) snprintf(summary, sizeof(summary), "%s", TR("Default"));
        if (button_mapping_summaries[button])
            lv_label_set_text(button_mapping_summaries[button], summary);
        for (int gesture = 0; gesture < gesture_count; ++gesture) {
            if (!button_detail_summaries[button][gesture]) continue;
            button_mapping_action_t action = actions[gesture];
            if (gesture == 1 && action == BUTTON_MAPPING_ACTION_DEFAULT)
                lv_label_set_text(button_detail_summaries[button][gesture], TR("Disabled"));
            else
                lv_label_set_text(button_detail_summaries[button][gesture], button_mapping_action_label(action));
        }
    }
    if (settings_button_mapping_summary) {
        bool is_default = true;
        for (int button = 0; button < BUTTON_MAPPING_BUTTON_COUNT; ++button) {
            if (!button_mapping_button_available(button)) continue;
            button_mapping_binding_t *binding = &current_settings.button_bindings[button];
            if (binding->tap != BUTTON_MAPPING_ACTION_DEFAULT ||
                (!button_mapping_button_single_press_only(button) &&
                 (binding->double_tap != BUTTON_MAPPING_ACTION_DEFAULT ||
                  binding->hold != BUTTON_MAPPING_ACTION_DEFAULT))) {
                is_default = false;
                break;
            }
        }
        lv_label_set_text(settings_button_mapping_summary, is_default ? TR("Default") : TR("Custom"));
    }
}

static void button_mapping_summaries_loaded_cb(lv_event_t * e) {
    (void)e;
    gui_button_mapping_refresh_summaries();
}

static void button_detail_open_cb(lv_event_t * e);
static void button_action_open_cb(lv_event_t * e);
static void button_mapping_reset_cb(lv_event_t * e);
static void button_mapping_reset_confirm_cb(lv_event_t * e);
static void button_mapping_reset_cancel_cb(lv_event_t * e);
static void button_mapping_reset_backdrop_cb(lv_event_t * e);

static lv_obj_t * build_button_mapping_screen(void) {
    pill_list_item_t items[BUTTON_MAPPING_BUTTON_COUNT + 1] = { 0 };
    int count = 0;
    for (int button = 0; button < BUTTON_MAPPING_BUTTON_COUNT; ++button) {
        if (!button_mapping_button_available(button)) continue;
        items[count++] = (pill_list_item_t){
            .label = button_mapping_button_label(button),
            .accessory = PILL_ACCESSORY_CHEVRON,
            .on_click = button_detail_open_cb,
            .user_data = (void *)(intptr_t)button,
            .out_row = &button_mapping_rows[button],
        };
    }
    items[count++] = (pill_list_item_t){
        .label = TR("Reset All"),
        .accessory = PILL_ACCESSORY_NONE,
        .on_click = button_mapping_reset_cb,
    };
    lv_obj_t *scr = build_pill_list_screen(TR("Button Mapping"), generic_back_cb, items,
                                            count,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    for (int button = 0; button < BUTTON_MAPPING_BUTTON_COUNT; ++button)
        if (button_mapping_button_available(button) && button_mapping_rows[button])
            button_mapping_summaries[button] = add_pill_summary_label(button_mapping_rows[button]);
    gui_button_mapping_refresh_summaries();
    lv_obj_add_event_cb(scr, button_mapping_summaries_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_button_detail_screen(int button) {
    pill_list_item_t items[3] = { 0 };
    lv_obj_t *rows[3] = { NULL, NULL, NULL };
    int count = button_mapping_button_single_press_only(button) ? 1 : 3;
    for (int gesture = 0; gesture < count; ++gesture) {
        items[gesture] = (pill_list_item_t){
            .label = button_mapping_gesture_label(gesture),
            .accessory = PILL_ACCESSORY_CHEVRON,
            .on_click = button_action_open_cb,
            .user_data = (void *)(intptr_t)(button * 3 + gesture),
            .out_row = &rows[gesture],
        };
    }
    lv_obj_t *scr = build_pill_list_screen(button_mapping_button_label(button), generic_back_cb,
                                            items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    for (int gesture = 0; gesture < count; ++gesture)
        if (rows[gesture]) button_detail_summaries[button][gesture] = add_pill_summary_label(rows[gesture]);
    lv_obj_add_event_cb(scr, button_mapping_summaries_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static void button_action_option_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    intptr_t packed = (intptr_t)lv_event_get_user_data(e);
    int button = (int)(packed / 64);
    int remainder = (int)(packed % 64);
    int gesture = remainder / 16;
    int action = remainder % 16;
    if (!button_mapping_button_available(button) || gesture < 0 || gesture > 2 ||
        action < BUTTON_MAPPING_ACTION_DEFAULT || action >= BUTTON_MAPPING_ACTION_COUNT) return;
    button_mapping_action_t *binding_action = button_mapping_binding_action(button, gesture);
    button_mapping_action_t previous_action = *binding_action;
    *binding_action = (button_mapping_action_t)action;
    if (!gui_button_bindings_allow_wake(current_settings.button_bindings)) {
        *binding_action = previous_action;
        show_error_toast(TR("Keep a button assigned to Toggle Screen or Power Menu"));
        return;
    }
    if (button == BUTTON_MAPPING_PLAY_PAUSE) current_settings.play_pause_button_mode = 0;
    settings_save_async(&current_settings);
    gui_apply_button_bindings();
    gui_button_mapping_refresh_summaries();
    nav_pop();
}

static lv_obj_t * build_button_action_screen(int button, int gesture) {
    char title[96];
    snprintf(title, sizeof(title), "%s · %s", button_mapping_button_label(button),
             button_mapping_gesture_label(gesture));
    lv_obj_t *title_label = NULL;
    return build_subsonic_list_screen(title, &title_label,
                                      &button_action_lists[button][gesture]);
}

static void populate_button_action_screen(int button, int gesture) {
    lv_obj_t *list = button_action_lists[button][gesture];
    if (!list) return;
    lv_obj_clean(list);
    for (int action = 0; action < BUTTON_MAPPING_ACTION_COUNT; ++action) {
        bool selected = *button_mapping_binding_action(button, gesture) == (button_mapping_action_t)action;
        intptr_t packed = (intptr_t)(button * 64 + gesture * 16 + action);
        const char *label = action == BUTTON_MAPPING_ACTION_DEFAULT && gesture == 1
                                ? TR("Disabled")
                                : button_mapping_action_label((button_mapping_action_t)action);
        add_pill_option_row(list, label, selected,
                            button_action_option_cb, (void *)packed);
    }
}

void gui_button_mapping_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!button_mapping_screen) button_mapping_screen = build_button_mapping_screen();
    gui_button_mapping_refresh_summaries();
    if (button_mapping_screen) nav_push(button_mapping_screen);
}

static void button_detail_open_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int button = (int)(intptr_t)lv_event_get_user_data(e);
    if (!button_mapping_button_available(button)) return;
    if (!button_detail_screens[button]) button_detail_screens[button] = build_button_detail_screen(button);
    gui_button_mapping_refresh_summaries();
    if (button_detail_screens[button]) nav_push(button_detail_screens[button]);
}

static void button_action_open_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    intptr_t packed = (intptr_t)lv_event_get_user_data(e);
    int button = (int)(packed / 3);
    int gesture = (int)(packed % 3);
    if (!button_mapping_button_available(button) || gesture < 0 || gesture > 2) return;
    if (button_mapping_button_single_press_only(button) && gesture != 0) return;
    if (!button_action_screens[button][gesture])
        button_action_screens[button][gesture] = build_button_action_screen(button, gesture);
    populate_button_action_screen(button, gesture);
    if (button_action_screens[button][gesture]) nav_push(button_action_screens[button][gesture]);
}

static void button_mapping_reset_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_popup_hide(&button_mapping_reset_popup);
}

static void button_mapping_reset_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_popup_hide(&button_mapping_reset_popup);
}

static void button_mapping_reset_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&button_mapping_reset_popup);
    for (int button = 0; button < BUTTON_MAPPING_BUTTON_COUNT; ++button) {
        current_settings.button_bindings[button].tap = BUTTON_MAPPING_ACTION_DEFAULT;
        current_settings.button_bindings[button].double_tap = BUTTON_MAPPING_ACTION_DEFAULT;
        current_settings.button_bindings[button].hold = BUTTON_MAPPING_ACTION_DEFAULT;
    }
    current_settings.play_pause_button_mode = 0;
    settings_save_async(&current_settings);
    gui_apply_button_bindings();
    gui_button_mapping_refresh_summaries();
}

static void button_mapping_reset_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_popup_show(&button_mapping_reset_popup);
}

void gui_button_mapping_build_reset_popup(void) {
    button_mapping_reset_popup.popup = build_confirm_popup(
        TR("Reset all button mappings?"), LV_LABEL_LONG_WRAP, NULL, NULL,
        TR("Reset"), accent_lv_color(), button_mapping_reset_confirm_cb, NULL,
        TR("Cancel"), lv_color_hex(GUI_BREEZE_NEGATIVE), button_mapping_reset_cancel_cb, NULL,
        button_mapping_reset_backdrop_cb, &button_mapping_reset_popup.backdrop);
}

void gui_button_mapping_set_category_summary(lv_obj_t * label) {
    settings_button_mapping_summary = label;
}

void gui_button_mapping_teardown_reset_popup(void) {
    gui_popup_teardown(&button_mapping_reset_popup);
}

void gui_button_mapping_teardown_screens(void) {
    settings_button_mapping_summary = NULL;
    for (int button = 0; button < BUTTON_MAPPING_BUTTON_COUNT; ++button) {
        for (int gesture = 0; gesture < 3; ++gesture) {
            if (button_action_screens[button][gesture])
                lv_obj_delete(button_action_screens[button][gesture]);
            button_action_screens[button][gesture] = NULL;
            button_action_lists[button][gesture] = NULL;
            button_detail_summaries[button][gesture] = NULL;
        }
        if (button_detail_screens[button]) lv_obj_delete(button_detail_screens[button]);
        button_detail_screens[button] = NULL;
        button_mapping_rows[button] = NULL;
        button_mapping_summaries[button] = NULL;
    }
    if (button_mapping_screen) { lv_obj_delete(button_mapping_screen); button_mapping_screen = NULL; }
}
