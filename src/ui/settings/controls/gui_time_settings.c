#include "gui_time_settings.h"
#include "../../../core/system/runtime/app_clock.h"
#include "../../shell/gui_navigation.h"
#include "../../shell/gui_notifications.h"
#include "../../plugins/gui_plugins.h"
#include "../setup/gui_setup.h"
#include "../../shell/gui_shell.h"
#include "../../text/i18n/i18n.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../../core/persistence/settings.h"
#include "../../../core/time/timezone_apply.h"
#include "../../../core/time/timezone_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t * clock_screen;
static lv_obj_t * clock_set_time_screen;
static lv_obj_t * clock_hour_roller;
static lv_obj_t * clock_minute_roller;
static lv_obj_t * clock_ampm_roller;
static lv_obj_t * clock_set_time_row;
static lv_obj_t * clock_timezone_row;
static lv_obj_t * clock_timezone_value_label;


/* ---- Time Zone picker ----------------------------------------------------
 * Region -> City selection. TIMEZONE_TABLE (timezone_data.h) is sorted
 * by UTC offset then city name. Region names are the top-level IANA region
 * components present in TIMEZONE_TABLE. */
static const char * const TIMEZONE_REGIONS[] = {
    N_("Africa"), N_("America"), N_("Antarctica"), N_("Arctic"), N_("Asia"), N_("Atlantic"), N_("Australia"),
    N_("Europe"), N_("Indian"), N_("Pacific")
};
#define TIMEZONE_REGION_COUNT (sizeof(TIMEZONE_REGIONS) / sizeof(TIMEZONE_REGIONS[0]))

static lv_obj_t * timezone_region_screen;
static lv_obj_t * timezone_city_screen;
/* Maps a City-screen row index (as passed to timezone_city_row_click_cb)
 * back to its real TIMEZONE_TABLE index -- the City screen only ever shows
 * one region's subset, so row index != table index. */
static int timezone_city_indices[TIMEZONE_TABLE_COUNT];
static int timezone_city_count;

static void clock_timezone_indicator_update(void) {
    if (!clock_timezone_value_label) return;
    const char * zone = current_settings.timezone[0]
        ? current_settings.timezone : TR("Not selected (UTC)");
    for (int i = 0; current_settings.timezone[0] && i < TIMEZONE_TABLE_COUNT; i++) {
        if (strcmp(current_settings.timezone, TIMEZONE_TABLE[i].iana_id) == 0) {
            zone = TIMEZONE_TABLE[i].display_name;
            break;
        }
    }
    lv_label_set_text(clock_timezone_value_label, zone);
}

static void timezone_city_row_click_cb(int row_index) {
    if (row_index < 0 || row_index >= timezone_city_count) return;
    const timezone_entry_t * entry = &TIMEZONE_TABLE[timezone_city_indices[row_index]];
    if (!timezone_apply(entry->iana_id)) {
        show_error_toast(TR("Failed to apply time zone"));
        return;
    }
    snprintf(current_settings.timezone, sizeof(current_settings.timezone), "%s", entry->iana_id);
    settings_save(&current_settings);
    refresh_clock_label(); /* topbar clock reflects the new zone immediately, not just on the next periodic refresh */
    clock_timezone_indicator_update();
    /* Skip the intermediate Region entry before starting navigation. Two
     * immediate nav_pop() calls race their slide transitions: the first
     * can finish later and restore Region over the intended Clock screen. */
    int depth = gui_navigation_get_depth();
    if (depth >= 3) nav_remove_stack_slot(depth - 2);
    nav_pop(); /* City -> Clock -- picking a leaf city completes the choice */
}

/* Rebuilt (delete + rebuild, same idiom as gui_library_get_all_songs_screen() after a library
 * rescan -- see poll_library_rescan()) every time a region is opened,
 * filtered down to just that region's entries, rather than built once --
 * needed both because the region changes on every open and so the
 * "(current)" marker always reflects current_settings.timezone as of the
 * most recent selection. Cheap even at TIMEZONE_TABLE_COUNT entries since
 * build_compact_list_screen is virtualized (screen_builders.h) -- only a
 * small pool of row widgets actually gets created, not one per entry.
 * `labels` is this function's own long-lived backing array for the
 * "(current)"-suffixed copy build_compact_list_screen's own doc comment
 * requires (label pointers must outlive the screen, not just this call) --
 * previous generation's entries are freed up to prev_count (the region
 * filter means the count varies per open, unlike a fixed-size table) right
 * before this rebuild, once the previous screen holding those pointers has
 * already been deleted by the caller. */
static void build_timezone_city_screen_items(compact_list_item_t * items, const char * region) {
    static char * labels[TIMEZONE_TABLE_COUNT];
    static int prev_count = 0;
    for (int i = 0; i < prev_count; i++) {
        free(labels[i]);
        labels[i] = NULL;
    }

    size_t region_len = strlen(region);
    timezone_city_count = 0;
    for (int i = 0; i < TIMEZONE_TABLE_COUNT; i++) {
        const char * id = TIMEZONE_TABLE[i].iana_id;
        if (strncmp(id, region, region_len) != 0 || id[region_len] != '/') continue;

        int row = timezone_city_count;
        bool is_current = strcmp(current_settings.timezone, id) == 0;
        if (is_current) {
            size_t len = strlen(TIMEZONE_TABLE[i].display_name) + strlen(TR("%s (current)")) + 1;
            labels[row] = malloc(len);
            snprintf(labels[row], len, TR("%s (current)"), TIMEZONE_TABLE[i].display_name);
        } else {
            labels[row] = strdup(TIMEZONE_TABLE[i].display_name);
        }
        timezone_city_indices[row] = i;
        items[row] = (compact_list_item_t){ labels[row] };
        timezone_city_count++;
    }
    prev_count = timezone_city_count;
}

static lv_obj_t * build_timezone_city_screen(const char * region) {
    compact_list_item_t * items = malloc(sizeof(compact_list_item_t) * (size_t) TIMEZONE_TABLE_COUNT);
    build_timezone_city_screen_items(items, region);
    lv_obj_t * scr = build_compact_list_screen(region, generic_back_cb, items, timezone_city_count, timezone_city_row_click_cb, NULL, NULL, NULL, LIST_ROW_WIDTH, false, NULL);
    free(items);
    finalize_screen_navigation(scr);
    return scr;
}

static void open_timezone_city_screen(const char * region) {
    if (timezone_city_screen) lv_obj_delete(timezone_city_screen);
    timezone_city_screen = build_timezone_city_screen(region);
    nav_push(timezone_city_screen);
}

static void timezone_region_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    open_timezone_city_screen(TIMEZONE_REGIONS[index]); /* English key, matched against TIMEZONE_TABLE */
}

/* Small (10 rows) and fixed -- built once at startup like every other
 * screen, unlike the per-region City screen above. */
static lv_obj_t * build_timezone_region_screen(void) {
    static pill_list_item_t items[TIMEZONE_REGION_COUNT];
    for (size_t i = 0; i < TIMEZONE_REGION_COUNT; i++) {
        items[i] = (pill_list_item_t){ TR(TIMEZONE_REGIONS[i]), PILL_ACCESSORY_CHEVRON, false, timezone_region_row_cb, NULL,
                                        (void *) (intptr_t) i };
    }
    lv_obj_t * scr = build_pill_list_screen(TR("Time Zone"), generic_back_cb, items, (int) TIMEZONE_REGION_COUNT, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

void gui_setup_open_timezone(void) {
    nav_push(timezone_region_screen);
}

static void timezone_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(timezone_region_screen);
}

static void clock_persist(void) {
    app_clock_get_persistence(&current_settings.clock_manual_epoch,
                              &current_settings.clock_system_reference);
    settings_save(&current_settings);
}

static void clock_update_set_time_enabled(void) {
    if (!clock_set_time_row) return;
    if (current_settings.clock_automatic) {
        lv_obj_clear_flag(clock_set_time_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(clock_set_time_row, LV_OPA_40, 0);
    } else {
        lv_obj_add_flag(clock_set_time_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(clock_set_time_row, LV_OPA_COVER, 0);
    }
}

static void clock_automatic_changed_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool automatic = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    app_clock_set_automatic(automatic);
    current_settings.clock_automatic = automatic;
    clock_update_set_time_enabled();
    clock_persist();
    refresh_clock_label();
}

static void clock_set_time_save_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int hour = (int) lv_roller_get_selected(clock_hour_roller);
    int minute = (int) lv_roller_get_selected(clock_minute_roller);
    if (!current_settings.clock_24h) {
        hour = (hour + 1) % 12;
        if (lv_roller_get_selected(clock_ampm_roller) == 1) hour += 12;
    }
    app_clock_set_local_time(hour, minute);
    current_settings.clock_automatic = false;
    clock_persist();
    refresh_clock_label();
    nav_pop();
}

static void clock_set_time_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (current_settings.clock_automatic) {
        show_info_toast(TR("Turn off Automatic to set the clock"));
        return;
    }
    struct tm local;
    app_clock_localtime(&local);
    if (current_settings.clock_24h) {
        lv_roller_set_options(clock_hour_roller,
            "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23",
            LV_ROLLER_MODE_NORMAL);
        lv_roller_set_selected(clock_hour_roller, (uint32_t) local.tm_hour, LV_ANIM_OFF);
        lv_obj_add_flag(clock_ampm_roller, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_roller_set_options(clock_hour_roller, "01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12",
                              LV_ROLLER_MODE_NORMAL);
        lv_roller_set_selected(clock_hour_roller, (uint32_t) ((local.tm_hour + 11) % 12), LV_ANIM_OFF);
        lv_roller_set_selected(clock_ampm_roller, local.tm_hour >= 12 ? 1 : 0, LV_ANIM_OFF);
        lv_obj_remove_flag(clock_ampm_roller, LV_OBJ_FLAG_HIDDEN);
    }
    lv_roller_set_selected(clock_minute_roller, (uint32_t) local.tm_min, LV_ANIM_OFF);
    nav_push(clock_set_time_screen);
}

void gui_time_settings_clock_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(clock_screen);
}

static lv_obj_t * build_clock_screen(void) {
    pill_list_item_t items[] = {
        { TR("Automatic"), PILL_ACCESSORY_TOGGLE, current_settings.clock_automatic,
          NULL, clock_automatic_changed_cb, NULL },
        { .label = TR("Set Time"), .accessory = PILL_ACCESSORY_CHEVRON,
          .on_click = clock_set_time_row_cb, .out_row = &clock_set_time_row },
        { TR("24-Hour Clock"), PILL_ACCESSORY_TOGGLE, current_settings.clock_24h,
          NULL, clock_24h_switch_event_cb, NULL },
        { .label = TR("Time Zone"), .accessory = PILL_ACCESSORY_CHEVRON,
          .on_click = timezone_settings_row_cb, .out_row = &clock_timezone_row },
    };
    lv_obj_t * scr = build_pill_list_screen(TR("Clock"), generic_back_cb, items, 4,
gui_theme_accent_style(), GUI_ROW_GAP, 100);
    if (clock_timezone_row) {
        lv_obj_t * title = lv_obj_get_child(clock_timezone_row, 0);
        if (title) lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, -18);
        clock_timezone_value_label = lv_label_create(clock_timezone_row);
        lv_obj_add_style(clock_timezone_value_label, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(clock_timezone_value_label,
                                   gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        configure_scrolling_row_label(clock_timezone_value_label, 370);
        lv_obj_align(clock_timezone_value_label, LV_ALIGN_LEFT_MID, 24, 23);
        clock_timezone_indicator_update();
    }
    clock_update_set_time_enabled();
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_clock_set_time_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    build_screen_header(scr, TR("Set Time"), generic_back_cb, NULL, NULL);

    lv_obj_t * row = lv_obj_create(scr);
#if defined(BOARD_TEMPOTEC_V1)
    int32_t row_top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + BOARD_SCALE_PX(18);
    int32_t save_height = BOARD_SCALE_PX(78);
    int32_t save_gap = BOARD_SCALE_PY(8);
    int32_t row_height = BOARD_SCREEN_HEIGHT - row_top - HOME_INDICATOR_CONTENT_INSET - save_height - save_gap;
    lv_obj_set_size(row, lv_pct(92), row_height);
#else
    lv_obj_set_size(row, lv_pct(92), BOARD_SCALE_PX(360));
#endif
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, row_top);
#else
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 18);
#endif
    lv_obj_add_style(row, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    clock_hour_roller = lv_roller_create(row);
    clock_minute_roller = lv_roller_create(row);
    clock_ampm_roller = lv_roller_create(row);
    lv_roller_set_options(clock_minute_roller,
        "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59",
        LV_ROLLER_MODE_NORMAL);
    lv_roller_set_options(clock_ampm_roller, "AM\nPM", LV_ROLLER_MODE_NORMAL);
    lv_obj_t * rollers[] = { clock_hour_roller, clock_minute_roller, clock_ampm_roller };
    for (int i = 0; i < 3; i++) {
        lv_obj_set_size(rollers[i], i == 2 ? BOARD_SCALE_PX(105) : BOARD_SCALE_PX(120), BOARD_SCALE_PX(300));
        lv_obj_set_style_text_font(rollers[i], gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
        lv_obj_add_style(rollers[i], gui_theme_accent_style(), LV_PART_SELECTED);
        /* style_accent deliberately sets both background and text to the
         * accent color (needed by several non-roller users), which makes a
         * roller's selected value disappear into its selection band. Keep
         * the accent background but explicitly restore themed primary text
         * for the selected part; adding this after the accent style gives
         * it precedence and still follows live theme color changes. */
        lv_obj_add_style(rollers[i], &style_theme_text_primary, LV_PART_SELECTED);
    }

    lv_obj_t * save = lv_button_create(scr);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_size(save, BOARD_SCALE_PX(220), save_height);
    lv_obj_align(save, LV_ALIGN_BOTTOM_MID, 0, -HOME_INDICATOR_CONTENT_INSET);
#else
    lv_obj_set_size(save, BOARD_SCALE_PX(220), BOARD_SCALE_PX(78));
    lv_obj_align_to(save, row, LV_ALIGN_OUT_BOTTOM_MID, 0, 28);
#endif
    lv_obj_add_style(save, gui_theme_accent_style(), 0);
    lv_obj_add_event_cb(save, clock_set_time_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * save_label = lv_label_create(save);
    lv_label_set_text(save_label, TR("Save"));
    lv_obj_add_style(save_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(save_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(save_label);
    finalize_screen_navigation(scr);
    return scr;
}

/* Refresh the top-bar AM/PM sprites immediately when this switch changes. */
void clock_24h_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.clock_24h = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    app_clock_get_persistence(&current_settings.clock_manual_epoch,
                              &current_settings.clock_system_reference);
    settings_save_async(&current_settings);
    refresh_clock_label();
}

void gui_time_settings_build_screens(void) {
    timezone_region_screen = build_timezone_region_screen();
    clock_set_time_screen = build_clock_set_time_screen();
    clock_screen = build_clock_screen();
}

void gui_time_settings_teardown(void) {
    if (timezone_region_screen) { lv_obj_delete(timezone_region_screen); timezone_region_screen = NULL; }
    if (clock_screen) { lv_obj_delete(clock_screen); clock_screen = NULL; }
    if (clock_set_time_screen) { lv_obj_delete(clock_set_time_screen); clock_set_time_screen = NULL; }
    clock_hour_roller = clock_minute_roller = clock_ampm_roller = NULL;
    clock_set_time_row = NULL;
    clock_timezone_row = clock_timezone_value_label = NULL;
    /* The function-local City labels and prev_count intentionally survive teardown;
     * the next City build frees that generation after deleting its screen. */
    if (timezone_city_screen) { lv_obj_delete(timezone_city_screen); timezone_city_screen = NULL; }
}
