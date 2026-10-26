#include "gui_custom_font.h"

#include "../../text/rendering/fallback_font.h"
#include "../../player/screen/gui_lyrics.h"
#include "../../shell/gui_navigation.h"
#include "../../shell/gui_notifications.h"
#include "../../settings/gui_settings.h"
#include "../../shell/gui_shell.h"
#include "../gui_theme.h"
#include "../../text/i18n/i18n.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../../core/persistence/settings.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static lv_obj_t * custom_font_screen;

static lv_obj_t * custom_font_list = NULL;
static lv_obj_t * custom_font_preview_latin = NULL;
static lv_obj_t * custom_font_preview_note = NULL;
static char discovered_custom_fonts[MAX_CUSTOM_FONTS_DISCOVERED][64];
static int discovered_custom_font_count = 0;

static void custom_font_option_cb(lv_event_t * e);
static void custom_font_apply_timer_cb(lv_timer_t * timer);

static void populate_custom_font_screen(void) {
    if (!custom_font_list) return;
    lv_obj_clean(custom_font_list);

    discovered_custom_font_count = fallback_font_discover_custom(discovered_custom_fonts, MAX_CUSTOM_FONTS_DISCOVERED);

    const char * active_name = fallback_font_get_custom_name();

    /* 1. Default (built-in Montserrat) option */
    bool default_selected = (strcmp(active_name, "Default") == 0 || !current_settings.custom_font[0]);
    add_pill_option_row(custom_font_list, TR("Default (Built-in)"),
                        default_selected, custom_font_option_cb, (void *) (intptr_t) -1);

    /* 2. Discovered fonts from <SD>/Fonts */
    for (int i = 0; i < discovered_custom_font_count; i++) {
        bool selected = (strcmp(active_name, discovered_custom_fonts[i]) == 0);
        add_pill_option_row(custom_font_list, discovered_custom_fonts[i],
                            selected, custom_font_option_cb, (void *) (intptr_t) i);
    }

    if (discovered_custom_font_count == 0) {
        lv_obj_t * empty_note = lv_label_create(custom_font_list);
        lv_label_set_text(empty_note, TR("No .ttf fonts found in /Fonts"));
        lv_obj_add_style(empty_note, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(empty_note, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_set_style_pad_top(empty_note, 12, 0);
    }
}

static void custom_font_apply_timer_cb(lv_timer_t * timer) {
    lv_obj_t * mask = (lv_obj_t *)lv_timer_get_user_data(timer);
    int index = (int)(intptr_t)lv_obj_get_user_data(mask) - 2;
    lv_timer_delete(timer);

    if (index < -1 || index >= discovered_custom_font_count) {
        lv_obj_delete(mask);
        show_error_toast(TR("Font selection is no longer available"));
        return;
    }
    const char * target = index < 0 ? "Default" : discovered_custom_fonts[index];
    if (!fallback_font_apply_custom(target)) {
        lv_obj_delete(mask);
        show_error_toast(TR("Failed to load font. Check format & memory."));
        return;
    }

    /* The stable app_font_* addresses now contain the candidate descriptors.
     * Perform the same bounded, one-shot refresh as live Font Size while the
     * black input mask is still covering the display.  Custom Font also
     * changes the Latin face of app_font_lyrics (but not its independent
     * size), so its existing layout gets one explicit refresh here. */
    gui_navigation_invalidate_font_snapshots();
    snprintf(current_settings.custom_font, sizeof(current_settings.custom_font), "%s",
             index < 0 ? "" : target);
    settings_save(&current_settings);
    lv_obj_report_style_change(NULL);
    /* Same full-display walk as font_size_apply_timer_cb() -- Custom Font
     * also rewrites app_font_* metrics in place, so unvisited Settings
     * submenus need their USER_3 label boxes recomputed too. */
    screen_builders_refresh_all_font_geometry();
    gui_settings_refresh_font_geometry();
    compact_list_refresh_all();
    gui_lyrics_refresh_layout();
    quick_drawer_mark_snapshot_dirty();
    nav_reset_to_home();
    screen_builders_refresh_font_geometry(lv_screen_active());
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(lv_layer_sys());

    lv_obj_delete(mask);
}

static void custom_font_option_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    const char * target = (index < 0) ? "Default" : discovered_custom_fonts[index];

    /* Match Font Size's true no-op: do not copy/validate/rebuild the active
     * font, flash the display, save settings, or reset navigation. */
    const char * active = fallback_font_get_custom_name();
    if ((index < 0 && strcmp(active, "Default") == 0) ||
        (index >= 0 && strcmp(active, target) == 0)) return;

    lv_obj_t * mask = lv_obj_create(lv_layer_sys());
    /* Encode -1 (Default) as 1 and discovered indices as 2..N+1.  The
     * discovered-name table is static and the input-blocking mask prevents
     * it from being repopulated before the one-shot callback consumes it. */
    lv_obj_set_user_data(mask, (void *)(intptr_t)(index + 2));
    lv_display_t * display = lv_display_get_default();
    lv_obj_set_size(mask,
                    lv_display_get_horizontal_resolution(display),
                    lv_display_get_vertical_resolution(display));
    lv_obj_align(mask, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_set_style_radius(mask, 0, 0);
    lv_obj_set_style_pad_all(mask, 0, 0);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(mask);
    lv_obj_invalidate(mask);

    /* As with Font Size, let the regular refresh timer paint black before
     * any SD I/O, TTF validation or cache construction begins. */
    lv_timer_create(custom_font_apply_timer_cb, 35, mask);
}

void gui_custom_font_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_custom_font_screen();
    nav_push(custom_font_screen);
}

static lv_obj_t * build_custom_font_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, TR("Font"), generic_back_cb, NULL, NULL);

    lv_obj_t * body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + BOARD_SCALE_PX(8);
    lv_obj_set_size(body, lv_pct(100), BOARD_SCREEN_HEIGHT - top - BOARD_SCALE_PX(24));
    lv_obj_set_pos(body, 0, top);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, BOARD_SCALE_PX(8), 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * preview_card = lv_obj_create(body);
    lv_obj_set_size(preview_card, lv_pct(90), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(preview_card, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_row(preview_card, BOARD_SCALE_PX(6), 0);
    lv_obj_set_flex_flow(preview_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(preview_card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(preview_card, 0, 0);
    lv_obj_set_style_radius(preview_card, 10, 0);
    lv_obj_remove_flag(preview_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * preview_title = lv_label_create(preview_card);
    lv_label_set_text(preview_title, TR("Preview"));
    lv_obj_add_style(preview_title, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(preview_title, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(preview_title, lv_pct(100));
    lv_label_set_long_mode(preview_title, LV_LABEL_LONG_WRAP);

    custom_font_preview_latin = lv_label_create(preview_card);
    lv_label_set_text(custom_font_preview_latin, TR("The quick brown fox jumps 123"));
    lv_obj_add_style(custom_font_preview_latin, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(custom_font_preview_latin, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_set_width(custom_font_preview_latin, lv_pct(100));
    lv_label_set_long_mode(custom_font_preview_latin, LV_LABEL_LONG_WRAP);

    /* Custom fonts intentionally replace only the Latin face.  Rendering a
     * sample spanning every file-backed fallback here made this screen take
     * seconds to enter and leave on slow flash, while not previewing anything
     * the selected custom font can change. */
    custom_font_preview_note = lv_label_create(preview_card);
    lv_label_set_text(custom_font_preview_note, TR("Custom fonts affect Latin text only."));
    lv_obj_add_style(custom_font_preview_note, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(custom_font_preview_note, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(custom_font_preview_note, lv_pct(100));
    lv_label_set_long_mode(custom_font_preview_note, LV_LABEL_LONG_WRAP);

    lv_obj_t * hint = lv_label_create(preview_card);
    lv_label_set_text(hint, TR("Place .ttf fonts in SD /Fonts folder."));
    lv_obj_add_style(hint, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(hint, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);

    /* Scrollable font list */
    custom_font_list = lv_obj_create(body);
    lv_obj_set_size(custom_font_list, lv_pct(100), 0);
    lv_obj_set_flex_grow(custom_font_list, 1);
    lv_obj_set_scrollbar_mode(custom_font_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_opa(custom_font_list, 0, 0);
    lv_obj_set_style_border_width(custom_font_list, 0, 0);
    lv_obj_set_style_pad_all(custom_font_list, 0, 0);
    lv_obj_set_flex_flow(custom_font_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(custom_font_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(custom_font_list, 6, 0);

    finalize_screen_navigation(scr);
    return scr;
}


void gui_custom_font_build_screen(void) {
    custom_font_screen = build_custom_font_screen();
}

void gui_custom_font_teardown(void) {
    if (custom_font_screen) { lv_obj_delete(custom_font_screen); custom_font_screen = NULL; }
}

lv_obj_t * gui_settings_get_custom_font_screen(void) { return custom_font_screen; }
