#include "../application/gui.h"
#include "gui_settings.h"
#include "../appearance/gui_accent.h"
#include "../appearance/fonts/gui_custom_font.h"
#include "display/gui_display_timing_settings.h"
#include "controls/gui_setting_switches.h"
#include "controls/gui_shell_settings.h"
#include "../text/i18n/i18n.h"
#include "../player/screen/gui_lyrics.h"
#include "../widgets/builders/screen_builders.h"
#include "../plugins/manage/gui_plugin_store.h"
#include "../appearance/gui_themes.h"
#include "../shell/gui_reload.h"
#include "../player/layout/player_layouts.h"
#include <stdio.h>
#include <string.h>
#include "gui_settings_internal.h"

static lv_obj_t * settings_appearance_screen;
static lv_obj_t * settings_player_layout_screen;
static lv_obj_t * settings_gestures_screen;
lv_obj_t * settings_display_screen;
static lv_obj_t * animation_speed_screen;
static lv_obj_t * animation_speed_list;
static lv_obj_t * keyboard_layout_screen;
static lv_obj_t * keyboard_layout_list;
static lv_obj_t * player_layout_choice_screen;
static lv_obj_t * player_layout_choice_list;

/* Shared click handler for every plugin-registered Display list row below --
 * same index-not-object shape plugin_settings_list_item_click_cb() (defined
 * further down, alongside build_settings_screen()) already uses. */
PLUGIN_ROW_CLICK_CB(plugin_display_list_item_click_cb, plugin_manager_display_list_item_clicked)

static void lyrics_keep_screen_on_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.lyrics_keep_screen_on =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
}

typedef struct {
    int scale;
    const char * label;
} animation_speed_option_t;

static const animation_speed_option_t animation_speed_options[] = {
    { 0, N_("Off") }, { 25, "0.25x" }, { 50, "0.5x" }, { 75, "0.75x" }, { 100, "1x" },
};
#define ANIMATION_SPEED_OPTION_COUNT (sizeof(animation_speed_options) / sizeof(animation_speed_options[0]))

static void animation_speed_option_row_cb(lv_event_t * e);

static void populate_animation_speed_screen(void) {
    if (!animation_speed_list) return;
    lv_obj_clean(animation_speed_list);
    for (size_t i = 0; i < ANIMATION_SPEED_OPTION_COUNT; i++) {
        bool selected = current_settings.animation_scale == animation_speed_options[i].scale;
        add_pill_option_row(animation_speed_list, TR(animation_speed_options[i].label),
                            selected, animation_speed_option_row_cb, (void *) (intptr_t) i);
    }
}

static void animation_speed_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.animation_scale = animation_speed_options[index].scale;
    settings_save_async(&current_settings);
    populate_animation_speed_screen();
}

static lv_obj_t * build_animation_speed_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Animation Speed"), &title_label, &animation_speed_list);
}

static void animation_speed_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_animation_speed_screen();
    nav_push(animation_speed_screen);
}

typedef struct {
    int layout;
    const char * label;
} keyboard_layout_option_t;

static const keyboard_layout_option_t keyboard_layout_options[] = {
    { KEYBOARD_LAYOUT_T9, N_("T9") },
    { KEYBOARD_LAYOUT_QWERTY, N_("QWERTY") },
    { KEYBOARD_LAYOUT_AZERTY, "AZERTY (Français)" },
    { KEYBOARD_LAYOUT_QWERTZ, "QWERTZ (Deutsch)" },
    { KEYBOARD_LAYOUT_RUSSIAN, "Русский (ЙЦУКЕН)" },
    { KEYBOARD_LAYOUT_UKRAINIAN, "Українська (ЙЦУКЕН)" },
};
#define KEYBOARD_LAYOUT_OPTION_COUNT (sizeof(keyboard_layout_options) / sizeof(keyboard_layout_options[0]))

size_t gui_settings_keyboard_layout_count(void) { return KEYBOARD_LAYOUT_OPTION_COUNT; }

int gui_settings_keyboard_layout_at(size_t index, const char ** label) {
    if (index >= KEYBOARD_LAYOUT_OPTION_COUNT) return -1;
    if (label) *label = keyboard_layout_options[index].label;
    return keyboard_layout_options[index].layout;
}

static void keyboard_layout_option_row_cb(lv_event_t * e);

static void populate_keyboard_layout_screen(void) {
    if (!keyboard_layout_list) return;
    lv_obj_clean(keyboard_layout_list);
    for (size_t i = 0; i < KEYBOARD_LAYOUT_OPTION_COUNT; i++) {
        bool selected = current_settings.keyboard_layout == keyboard_layout_options[i].layout;
        add_pill_option_row(keyboard_layout_list, TR(keyboard_layout_options[i].label),
                            selected, keyboard_layout_option_row_cb, (void *) (intptr_t) i);
    }
}

/* Takes effect the next time a text field or inline search opens -- the
 * shared keyboard picks its layout at session start (gui_text_input.c). */
static void keyboard_layout_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.keyboard_layout = keyboard_layout_options[index].layout;
    settings_save_async(&current_settings);
    populate_keyboard_layout_screen();
}

static lv_obj_t * build_keyboard_layout_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Keyboard"), &title_label, &keyboard_layout_list);
}

static void keyboard_layout_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_keyboard_layout_screen();
    nav_push(keyboard_layout_screen);
}

static void upside_down_screen_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.screen_upside_down = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
    gui_display_apply_rotation(current_settings.screen_upside_down);
}

static void artist_images_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.show_artist_images = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
    gui_library_set_artist_images_enabled(current_settings.show_artist_images);
}

void gui_display_apply_rotation(bool upside_down) {
    lv_display_t * disp = lv_display_get_default();
    if (!disp) return;
    lv_display_rotation_t target_rot = upside_down ? LV_DISPLAY_ROTATION_180 : LV_DISPLAY_ROTATION_0;
    if (lv_display_get_rotation(disp) == target_rot) return;
    lv_display_set_rotation(disp, target_rot);
    gui_navigation_invalidate_theme_snapshots();
    lv_obj_t * act = lv_screen_active();
    if (act) lv_obj_invalidate(act);
}

void themes_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_themes_open_picker();
}

static lv_obj_t * build_settings_appearance_screen(void) {
    pill_list_item_t items[9 + PLUGIN_MAX_DISPLAY_LIST_ITEMS];
    items[0] = (pill_list_item_t){ TR("Accent Color"), PILL_ACCESSORY_CHEVRON, false, gui_accent_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("Font"), PILL_ACCESSORY_CHEVRON, false, gui_custom_font_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ TR("Font Size"), PILL_ACCESSORY_CHEVRON, false, font_size_settings_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ TR("Keyboard"), PILL_ACCESSORY_CHEVRON, false, keyboard_layout_settings_row_cb, NULL, NULL };
    items[4] = (pill_list_item_t){ TR("Quick Drawer"), PILL_ACCESSORY_CHEVRON, false, gui_shell_settings_quick_drawer_row_cb, NULL, NULL };
    items[5] = (pill_list_item_t){ TR("Music menu"), PILL_ACCESSORY_CHEVRON, false,
                                    gui_shell_settings_music_order_row_cb, NULL, (void *)(intptr_t)UI_ORDER_MUSIC };
    items[6] = (pill_list_item_t){ TR("Top Bar"), PILL_ACCESSORY_CHEVRON, false,
                                    gui_shell_settings_topbar_row_cb, NULL, NULL };
    items[7] = (pill_list_item_t){ TR("Artist Images"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.show_artist_images, NULL,
                                    artist_images_switch_event_cb, NULL };
    items[8] = (pill_list_item_t){ TR("Themes"), PILL_ACCESSORY_CHEVRON, false, themes_row_cb, NULL, NULL };
    int count = append_grouped_plugin_rows(items, 9, PLUGIN_MAX_DISPLAY_LIST_ITEMS,
        "display", "appearance", plugin_manager_get_display_list_item_count,
        plugin_manager_get_display_list_item_label, plugin_manager_get_display_list_item_options,
        plugin_display_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Appearance"), generic_back_cb, items, count,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

/* Player layout choice: the built-in layout plus every registered one (XML
 * files, exported C layouts, a plugin's session layout). Applying a choice
 * replaces the Player in the background without rebuilding this picker. */
static void player_layout_choice_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const player_layout_info_t * info = player_layouts_get((int) (intptr_t) lv_event_get_user_data(e));
    if (!info) return;
    if (strcmp(info->id, player_layouts_effective_id()) == 0) return;
    /* Picking a layout here replaces a plugin's session layout too. */
    player_layouts_session_clear_selection();
    snprintf(current_settings.player_layout, sizeof(current_settings.player_layout), "%s",
             strcmp(info->id, PLAYER_LAYOUT_ID_DEFAULT) == 0 ? "" : info->id);
    settings_save_async(&current_settings);
    /* The picker stays alive now. Update its selection ring in place so
     * the current scroll position and event targets remain unchanged. */
    lv_obj_t * selected_card = lv_event_get_current_target(e);
    lv_obj_t * grid = lv_obj_get_parent(selected_card);
    for (uint32_t i = 0; grid && i < lv_obj_get_child_count(grid); ++i) {
        lv_obj_t * card = lv_obj_get_child(grid, (int32_t) i);
        lv_obj_set_style_outline_width(card, card == selected_card ? BOARD_SCALE_PX(2) : 0, 0);
    }
    gui_player_layout_reload_request();
}

static void player_layout_download_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    (void) gui_plugin_store_open_player_layouts();
}

static void populate_player_layout_picker(lv_obj_t * list) {
    if (!list) return;
    lv_obj_clean(list);
    player_layouts_rescan();
    lv_obj_t * download = add_pill_chevron_row(list, TR("Download"), player_layout_download_cb);
    lv_obj_set_width(download, lv_pct(100));
    const char * active = player_layouts_effective_id();
    lv_obj_t * grid = lv_obj_create(list);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    configure_cover_card_grid(grid, 2);
    /* The outline extends outside each card. Reserve space at the grid's
     * edges so its first and last rows retain the complete selection ring. */
    lv_obj_set_style_pad_top(grid, BOARD_SCALE_PX(6), 0);
    lv_obj_set_style_pad_bottom(grid, BOARD_SCALE_PX(6), 0);
    int layout_count = player_layouts_count();
    for (int i = 0; i < layout_count; i++) {
        const player_layout_info_t * info = player_layouts_get(i);
        bool selected = strcmp(info->id, active) == 0;
        char preview[512], resolved[520];
        const char * preview_src = NULL;
        if (player_layouts_get_preview(i, preview, sizeof(preview))) {
            snprintf(resolved, sizeof(resolved), "S:%s", preview);
            preview_src = resolved;
        }
        lv_obj_t * card = add_cover_card(grid, info->name, preview_src, 2,
                                          player_layout_choice_row_cb, (void *) (intptr_t) i);
        if (!card) continue;
        lv_obj_set_style_radius(card, BOARD_SCALE_PX(10), 0);
        lv_obj_set_style_outline_width(card, selected ? BOARD_SCALE_PX(2) : 0, 0);
        lv_obj_set_style_outline_color(card, accent_lv_color(), 0);
        lv_obj_set_style_outline_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_outline_pad(card, BOARD_SCALE_PX(2), 0);
    }
}

void player_layout_choice_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!player_layout_choice_screen) return;
    populate_player_layout_picker(player_layout_choice_list);
    nav_push(player_layout_choice_screen);
}

static lv_obj_t * build_player_layout_choice_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Layout"), &title_label, &player_layout_choice_list);
}

static lv_obj_t * build_settings_player_layout_screen(void) {
    pill_list_item_t items[5 + PLUGIN_MAX_DISPLAY_LIST_ITEMS];
    items[0] = (pill_list_item_t){ TR("Layout"), PILL_ACCESSORY_CHEVRON, false,
                                    player_layout_choice_settings_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("Lyrics"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.lyrics_enabled, NULL, lyrics_switch_event_cb, NULL };
    items[2] = (pill_list_item_t){ TR("Lyrics Text Size"), PILL_ACCESSORY_CHEVRON, false,
                                    lyrics_font_size_settings_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ TR("Hide Player/Lyrics Top Bar"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.hide_player_topbar, NULL,
                                    hide_player_topbar_switch_event_cb, NULL };
    items[4] = (pill_list_item_t){ TR("Keep Lyrics Screen On"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.lyrics_keep_screen_on, NULL,
                                    lyrics_keep_screen_on_switch_event_cb, NULL };
    int count = append_grouped_plugin_rows(items, 5, PLUGIN_MAX_DISPLAY_LIST_ITEMS,
        "display", "player_layout", plugin_manager_get_display_list_item_count,
        plugin_manager_get_display_list_item_label, plugin_manager_get_display_list_item_options,
        plugin_display_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Player Layout"), generic_back_cb, items, count,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_settings_gestures_screen(void) {
    pill_list_item_t items[] = {
        { TR("Swipe Up for Home"), PILL_ACCESSORY_TOGGLE,
          current_settings.swipe_up_home_enabled, NULL, swipe_up_home_switch_event_cb, NULL },
        { TR("Animation Speed"), PILL_ACCESSORY_CHEVRON, false,
          animation_speed_settings_row_cb, NULL, NULL },
        { TR("Upside Down Screen"), PILL_ACCESSORY_TOGGLE,
          current_settings.screen_upside_down, NULL, upside_down_screen_switch_event_cb, NULL },
    };
    lv_obj_t * scr = build_pill_list_screen(TR("Gestures & Orientation"), generic_back_cb, items,
                                            (int) (sizeof(items) / sizeof(items[0])),
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static void settings_appearance_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    push_cached_screen(&settings_appearance_screen, build_settings_appearance_screen);
}

static void settings_player_layout_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    push_cached_screen(&settings_player_layout_screen, build_settings_player_layout_screen);
}

static void settings_gestures_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    push_cached_screen(&settings_gestures_screen, build_settings_gestures_screen);
}

static lv_obj_t * build_settings_display_screen(void) {
    pill_list_item_t items[5 + PLUGIN_MAX_DISPLAY_LIST_ITEMS];
    items[0] = (pill_list_item_t){ TR("Screen Timeout"), PILL_ACCESSORY_CHEVRON, false, gui_display_timing_settings_timeout_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("Screen Dimming"), PILL_ACCESSORY_CHEVRON, false, gui_display_timing_settings_dimming_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ TR("Appearance"), PILL_ACCESSORY_CHEVRON, false, settings_appearance_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ TR("Player Layout"), PILL_ACCESSORY_CHEVRON, false, settings_player_layout_row_cb, NULL, NULL };
    items[4] = (pill_list_item_t){ TR("Gestures & Orientation"), PILL_ACCESSORY_CHEVRON, false, settings_gestures_row_cb, NULL, NULL };
    int count = append_grouped_plugin_rows(items, 5, PLUGIN_MAX_DISPLAY_LIST_ITEMS,
        "display", NULL, plugin_manager_get_display_list_item_count,
        plugin_manager_get_display_list_item_label, plugin_manager_get_display_list_item_options,
        plugin_display_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Display"), generic_back_cb, items, count,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

void settings_display_build_screens(void) {
    animation_speed_screen = build_animation_speed_screen();
    keyboard_layout_screen = build_keyboard_layout_screen();
    player_layout_choice_screen = build_player_layout_choice_screen();
    settings_display_screen = build_settings_display_screen();
}

void settings_display_teardown_screens(void) {
    delete_screen(&settings_appearance_screen);
    delete_screen(&settings_player_layout_screen);
    delete_screen(&settings_gestures_screen);
    delete_screen(&animation_speed_screen);
    animation_speed_list = NULL;
    delete_screen(&keyboard_layout_screen);
    keyboard_layout_list = NULL;
    delete_screen(&player_layout_choice_screen);
    player_layout_choice_list = NULL;
    delete_screen(&settings_display_screen);
}
