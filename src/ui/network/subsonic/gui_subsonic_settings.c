#include "../../application/gui.h"
#include "../../text/i18n/i18n.h"
#include "gui_subsonic.h"
#include "gui_subsonic_internal.h"
#include "../../../core/persistence/settings.h"
#include "../../../core/text/utf8_util.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../text/rendering/gui_text_input.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern player_settings_t current_settings;

static lv_obj_t * subsonic_settings_screen;
static lv_obj_t * subsonic_settings_list;
static lv_obj_t * subsonic_quality_screen;
static lv_obj_t * subsonic_quality_list;
static lv_obj_t * subsonic_quality_options[4];
static lv_obj_t * subsonic_quality_row;
static lv_obj_t * subsonic_download_settings_screen;
static lv_obj_t * subsonic_download_settings_list;
static lv_obj_t * subsonic_download_folder_row;
static lv_obj_t * subsonic_download_layout_options[2];
static lv_obj_t * subsonic_download_settings_row;

static const char * subsonic_quality_name(int quality) {
    static const char * names[] = { N_("Original"), N_("Low"), N_("Medium"), N_("High") };
    return TR(quality >= 0 && quality < 4 ? names[quality] : names[0]);
}

static void subsonic_quality_update_menu_label(void) {
    char label[64];
    snprintf(label, sizeof(label), TR("Stream quality: %s"),
             subsonic_quality_name(current_settings.subsonic_stream_quality));
    if (subsonic_quality_row) lv_label_set_text(lv_obj_get_child(subsonic_quality_row, 0), label);
}

static void subsonic_mark_option(lv_obj_t * const * options, int count, int selected) {
    for (int i = 0; i < count; i++)
        if (options[i]) lv_obj_set_style_border_width(options[i], i == selected ? 3 : 0, 0);
}

static void subsonic_quality_option_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.subsonic_stream_quality = (int) (intptr_t) lv_event_get_user_data(e);
    settings_save_async(&current_settings);
    subsonic_mark_option(subsonic_quality_options, 4, current_settings.subsonic_stream_quality);
    subsonic_quality_update_menu_label();
    nav_pop();
}

static void subsonic_quality_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) nav_push(subsonic_quality_screen);
}

static lv_obj_t * build_subsonic_quality_screen(void) {
    lv_obj_t * title;
    lv_obj_t * screen = build_subsonic_list_screen(TR("Stream Quality"), &title, &subsonic_quality_list);
    (void) title;
    add_section_header(subsonic_quality_list, TR("Applies to new streaming queues"));
    static const char * labels[] = { N_("Original"), N_("Low (96 kbps)"), N_("Medium (192 kbps)"), N_("High (320 kbps)") };
    for (int i = 0; i < 4; i++) {
        subsonic_quality_options[i] = add_pill_option_row(subsonic_quality_list, TR(labels[i]),
                                                          current_settings.subsonic_stream_quality == i,
                                                          subsonic_quality_option_cb, (void *) (intptr_t) i);
    }
    return screen;
}

static void subsonic_download_folder_update_row(void) {
    if (!subsonic_download_folder_row) return;
    char label[112];
    if (current_settings.subsonic_download_subfolder[0]) {
        char preview[49];
        utf8_truncate_safe(preview, current_settings.subsonic_download_subfolder, sizeof(preview));
        snprintf(label, sizeof(label), TR("Download folder: %s"), preview);
    } else {
        snprintf(label, sizeof(label), "%s", TR("Download folder: SD root"));
    }
    lv_label_set_text(lv_obj_get_child(subsonic_download_folder_row, 0), label);
}

static void subsonic_download_folder_done(const char * text, void * user_data) {
    (void)user_data;
    char checked[SETTINGS_SUBSONIC_DOWNLOAD_SUBFOLDER_MAX];
    if (!settings_validate_subsonic_download_subfolder(text ? text : "", checked, sizeof(checked))) {
        show_error_toast(TR("Invalid download folder name"));
        return;
    }
    snprintf(current_settings.subsonic_download_subfolder,
             sizeof(current_settings.subsonic_download_subfolder), "%s", checked);
    settings_save_async(&current_settings);
    subsonic_download_folder_update_row();
}

static void subsonic_download_folder_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry(TR("Download subfolder"), current_settings.subsonic_download_subfolder,
                    false, false, subsonic_download_folder_done, NULL);
}

static void subsonic_download_layout_option_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.subsonic_download_layout = (int)(intptr_t)lv_event_get_user_data(e) == 1 ? 1 : 0;
    settings_save_async(&current_settings);
    subsonic_mark_option(subsonic_download_layout_options, 2, current_settings.subsonic_download_layout);
}

static void subsonic_download_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) nav_push(subsonic_download_settings_screen);
}

static lv_obj_t * build_subsonic_download_settings_screen(void) {
    lv_obj_t * title;
    lv_obj_t * screen = build_subsonic_list_screen(TR("Download Settings"), &title,
                                                    &subsonic_download_settings_list);
    (void)title;
    subsonic_download_folder_row = add_pill_chevron_row(subsonic_download_settings_list,
        TR("Subfolder: SD root"), subsonic_download_folder_row_cb);
    subsonic_download_folder_update_row();
    add_wrapped_section_header(subsonic_download_settings_list,
        TR("Subfolder is relative to SD root (example: Music/Offline); empty uses SD root"));
    add_section_header(subsonic_download_settings_list, TR("Folder layout for downloaded albums"));
    static const char * labels[] = { N_("Album Artist / Album"), N_("Album Artist - Album") };
    for (int i = 0; i < 2; i++)
        subsonic_download_layout_options[i] = add_pill_option_row(subsonic_download_settings_list,
            TR(labels[i]), current_settings.subsonic_download_layout == i,
            subsonic_download_layout_option_cb, (void *)(intptr_t)i);
    return screen;
}

void gui_subsonic_settings_open_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) nav_push(subsonic_settings_screen);
}

/* Header settings button target: the stream quality and download rows the menu used to hold. */
static void build_subsonic_settings_screen(void) {
    lv_obj_t * title;
    char quality_label[64];
    subsonic_settings_screen = build_subsonic_list_screen(TR("Settings"), &title, &subsonic_settings_list);
    snprintf(quality_label, sizeof(quality_label), TR("Stream quality: %s"),
             subsonic_quality_name(current_settings.subsonic_stream_quality));
    subsonic_quality_row = add_pill_chevron_row(subsonic_settings_list, quality_label, subsonic_quality_row_cb);
    lv_obj_set_style_text_font(lv_obj_get_child(subsonic_quality_row, 0), gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    subsonic_download_settings_row = add_pill_chevron_row(subsonic_settings_list,
        TR("Download settings"), subsonic_download_settings_row_cb);
    lv_obj_set_style_text_font(lv_obj_get_child(subsonic_download_settings_row, 0),
                               gui_theme_font(GUI_FONT_ROLE_BODY), 0);
}

void gui_subsonic_settings_build(void) {
    subsonic_quality_screen = build_subsonic_quality_screen();
    subsonic_download_settings_screen = build_subsonic_download_settings_screen();
    build_subsonic_settings_screen();
}

void gui_subsonic_settings_delete(void) {
    if (subsonic_settings_screen) { lv_obj_delete(subsonic_settings_screen); subsonic_settings_screen = NULL; }
    subsonic_settings_list = NULL;
    if (subsonic_quality_screen) { lv_obj_delete(subsonic_quality_screen); subsonic_quality_screen = NULL; }
    if (subsonic_download_settings_screen) {
        lv_obj_delete(subsonic_download_settings_screen);
        subsonic_download_settings_screen = NULL;
    }
    memset(subsonic_quality_options, 0, sizeof(subsonic_quality_options));
    subsonic_quality_list = NULL;
    subsonic_quality_row = NULL;
    subsonic_download_settings_list = NULL;
    subsonic_download_folder_row = NULL;
    memset(subsonic_download_layout_options, 0, sizeof(subsonic_download_layout_options));
    subsonic_download_settings_row = NULL;
}
