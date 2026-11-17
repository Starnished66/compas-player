#include "../application/gui.h"
#include "gui_settings.h"
#include "input/gui_button_mapping.h"
#include "../player/settings/gui_equalizer.h"
#include "controls/gui_setting_switches.h"
#include "controls/gui_volume_settings.h"
#include "../text/i18n/i18n.h"
#include "../../hardware/input/hw_buttons.h"
#include "../widgets/builders/screen_builders.h"
#include "../assets/assets.h"
#include "../../core/device/device_info.h"
#include "../../core/persistence/storage_paths.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../../audio/output/audio_output.h"
#include "gui_settings_internal.h"
#include "gui_native_audio.h"
#include "gui_play_through.h"
#include "../../core/persistence/native_audio_settings.h"
#include "../../audio/effects/peq.h"

static lv_obj_t * settings_crossfade_toggle_img = NULL;
static lv_obj_t * settings_gapless_toggle_img = NULL;
static lv_obj_t * settings_library_screen;
static lv_obj_t * settings_sorting_screen;
enum { SORTING_CONTEXT_FILES, SORTING_CONTEXT_ALBUMS, SORTING_CONTEXT_ALL_SONGS, SORTING_CONTEXT_ARTISTS,
       SORTING_CONTEXT_ALBUM_ARTISTS, SORTING_CONTEXT_COUNT };
static const struct {
    int context;
    int sort;
    const char * labels[2];
} sorting_orders[] = {
    { SORTING_CONTEXT_FILES, 0, { N_("Name (A–Z)"), N_("Name (Z–A)") } },
    { SORTING_CONTEXT_FILES, 1, { N_("Newest modified"), N_("Oldest modified") } },
    { SORTING_CONTEXT_ALBUMS, 0, { N_("Name (A–Z)"), N_("Name (Z–A)") } },
    { SORTING_CONTEXT_ALBUMS, 1, { N_("Recently added"), N_("Oldest added") } },
    { SORTING_CONTEXT_ALBUMS, 2, { N_("Release year (oldest first)"), N_("Release year (newest first)") } },
    { SORTING_CONTEXT_ALBUMS, 3, { N_("Album artist (A–Z)"), N_("Album artist (Z–A)") } },
    { SORTING_CONTEXT_ALBUMS, METADATA_DB_ALBUM_SORT_RATING, { N_("Rating"), NULL } },
    { SORTING_CONTEXT_ALL_SONGS, 0, { N_("Name (A–Z)"), N_("Name (Z–A)") } },
    { SORTING_CONTEXT_ALL_SONGS, 1, { N_("Rating"), NULL } },
    { SORTING_CONTEXT_ARTISTS, 0, { N_("Name (A–Z)"), N_("Name (Z–A)") } },
    { SORTING_CONTEXT_ARTISTS, 1, { N_("Recently added"), N_("Oldest added") } },
    { SORTING_CONTEXT_ARTISTS, 2, { N_("Release year (oldest first)"), N_("Release year (newest first)") } },
    { SORTING_CONTEXT_ALBUM_ARTISTS, 0, { N_("Name (A–Z)"), N_("Name (Z–A)") } },
    { SORTING_CONTEXT_ALBUM_ARTISTS, 1, { N_("Recently added"), N_("Oldest added") } },
    { SORTING_CONTEXT_ALBUM_ARTISTS, 2, { N_("Release year (oldest first)"), N_("Release year (newest first)") } },
};
enum { SORTING_OPTION_COUNT = sizeof(sorting_orders) / sizeof(sorting_orders[0]) * 2 };
static const gui_library_sort_scope_t sorting_scopes[] = {
    GUI_LIBRARY_SORT_FILES, GUI_LIBRARY_SORT_ALBUMS, GUI_LIBRARY_SORT_ALL_SONGS,
    GUI_LIBRARY_SORT_ARTIST_ALBUMS, GUI_LIBRARY_SORT_ALBUM_ARTIST_ALBUMS
};
static lv_obj_t * settings_sorting_options[SORTING_CONTEXT_COUNT];
static lv_obj_t * dac_settings_screen;
static lv_obj_t * dac_settings_list;
static lv_obj_t * dac_filter_menu;
static lv_obj_t * dac_dre_row;
static lv_obj_t * dac_nos_row, * dac_nos_hint;
static lv_obj_t * usb_dsd_settings_screen;
static lv_obj_t * usb_dsd_settings_list;
static lv_obj_t * dsd_gain_menu;
static lv_obj_t * settings_eq_profiles_menu_screen;
static lv_obj_t * music_playback_screen;
static lv_obj_t * music_audio_screen;
static lv_obj_t * advanced_sound_screen;
static lv_obj_t * buttons_screen;
static lv_obj_t * audio_summaries[7];
static const char * dac_filter_summary(void);

static void refresh_audio_summaries(lv_event_t * e) {
    (void)e;
    if (audio_summaries[0]) lv_label_set_text_fmt(audio_summaries[0], TR("Max %d%%"), current_settings.max_volume_percent);
    if (audio_summaries[1]) lv_label_set_text(audio_summaries[1], dac_filter_summary());
    native_mseb_state_t mseb; native_audio_get_mseb(&mseb);
    soundstage_config_t stage; int preset; native_audio_get_soundstage(&stage, &preset);
    if (audio_summaries[2]) lv_label_set_text(audio_summaries[2], mseb.config.enabled ? TR("On") : TR("Off"));
    if (audio_summaries[3]) lv_label_set_text(audio_summaries[3], stage.enabled ? TR("On") : TR("Off"));
    if (audio_summaries[4]) lv_label_set_text(audio_summaries[4], peq_get_crossfeed() > 0 ? TR("On") : TR("Off"));
    static const char * const rg[] = { N_("Off"), N_("Per track"), N_("Per album") };
    int mode = current_settings.replaygain_mode;
    if (audio_summaries[5]) lv_label_set_text(audio_summaries[5], TR(rg[mode >= 0 && mode <= 2 ? mode : 0]));
    static const char * const usb[] = { N_("PCM"), N_("Native DSD"), N_("DoP") };
    mode = current_settings.usb_dsd_output;
    if (audio_summaries[6]) lv_label_set_text(audio_summaries[6], TR(usb[mode >= 0 && mode <= 2 ? mode : 0]));
}

static lv_obj_t * build_library_category_screen(void);

/* Not declared in any header: defined in gui_library_scan.c. */
extern void show_library_refresh_all_covers_prompt(void);


void gui_settings_open_playback(void) {
    if (!music_playback_screen) return;
    nav_push(music_playback_screen);
}

void gui_settings_open_sound(void) {
    if (!music_audio_screen) return;
    nav_push(music_audio_screen);
}

/* Shared click handler for every plugin-registered Playback list row below
 * -- same index-not-object shape plugin_display_list_item_click_cb() already
 * uses. */
PLUGIN_ROW_CLICK_CB(plugin_playback_list_item_click_cb, plugin_manager_playback_list_item_clicked)

PLUGIN_ROW_CLICK_CB(plugin_music_audio_list_item_click_cb, plugin_manager_music_audio_list_item_clicked)

PLUGIN_ROW_CLICK_CB(plugin_music_controls_list_item_click_cb, plugin_manager_music_controls_list_item_clicked)

PLUGIN_ROW_CLICK_CB(plugin_music_library_list_item_click_cb, plugin_manager_music_library_list_item_clicked)

static void open_advanced_sound(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && advanced_sound_screen) nav_push(advanced_sound_screen);
}
static void open_buttons(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && buttons_screen) nav_push(buttons_screen);
}

/* Submenu listing the plugin rows registered under `group` of the Sound list. */
static lv_obj_t * build_music_audio_group_screen(const char * title, const char * group) {
    pill_list_item_t rows[PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS];
    int count = append_grouped_plugin_rows(rows, 0, PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS,
        "music_audio", group, plugin_manager_get_music_audio_list_item_count,
        plugin_manager_get_music_audio_list_item_label, plugin_manager_get_music_audio_list_item_options,
        plugin_music_audio_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(title, generic_back_cb, rows, count,
        gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_download_profiles_screen(void) {
    return build_music_audio_group_screen(TR("Download profiles"), "profiles");
}

void music_profiles_menu_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    push_cached_screen(&settings_eq_profiles_menu_screen, build_download_profiles_screen);
}

static void volume_lock_switch_event_cb(lv_event_t * e);

static lv_obj_t * build_music_playback_screen(void) {
    lv_obj_t * list;
    lv_obj_t * scr = build_subsonic_list_screen(TR("Playback"), NULL, &list);
    lv_obj_t * car_row = NULL;
    lv_obj_t * sleep_row = NULL;
    pill_list_item_t items[6 + PLUGIN_MAX_PLAYBACK_LIST_ITEMS] = {
        { TR("Resume last track"), PILL_ACCESSORY_CHEVRON, false, resume_mode_settings_row_cb },
        { TR("Crossfade"), PILL_ACCESSORY_TOGGLE, current_settings.crossfade_enabled,
          NULL, crossfade_switch_event_cb, NULL, &settings_crossfade_toggle_img },
        { TR("Gapless"), PILL_ACCESSORY_TOGGLE, current_settings.gapless_enabled,
          NULL, gapless_switch_event_cb, NULL, &settings_gapless_toggle_img },
    };
    add_pill_list_rows(list, items, 3);
    gui_native_audio_add_play_through_rows(list);
    items[0] = (pill_list_item_t){ TR("Sleep timer"), PILL_ACCESSORY_CHEVRON, false, gui_settings_sleep_timer_row_cb };
    items[0].out_row = &sleep_row;
    add_pill_list_rows(list, items, 1);
    add_pill_chevron_row(list, TR("Buttons"), open_buttons);
    items[0] = (pill_list_item_t){ TR("Car mode"), PILL_ACCESSORY_CHEVRON, false, gui_volume_settings_car_mode_row_cb };
    items[0].out_row = &car_row;
    int count = append_plugin_list_rows(items, 1, PLUGIN_MAX_PLAYBACK_LIST_ITEMS,
        plugin_manager_get_playback_list_item_count, plugin_manager_get_playback_list_item_label,
        plugin_manager_get_playback_list_item_options, plugin_playback_list_item_click_cb);
    add_pill_list_rows(list, items, count);
    settings_sleep_summary = add_pill_summary_label(sleep_row);
    settings_car_summary = add_pill_summary_label(car_row);
    settings_summary_loaded_cb(NULL);
    lv_obj_add_event_cb(scr, settings_summary_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_buttons_screen(void) {
    lv_obj_t * list;
    lv_obj_t * scr = build_subsonic_list_screen(TR("Buttons"), NULL, &list);
    lv_obj_t * button_row = NULL;
    lv_obj_t * volume_lock_row = NULL;
    pill_list_item_t controls[3 + PLUGIN_MAX_MUSIC_CONTROLS_LIST_ITEMS] = {
        { TR("Button mapping"), PILL_ACCESSORY_CHEVRON, false, gui_button_mapping_row_cb },
        { TR("Volume lock (screen off)"), PILL_ACCESSORY_TOGGLE, current_settings.volume_lock_screen_off, NULL, volume_lock_switch_event_cb },
        { TR("In-line remote"), PILL_ACCESSORY_TOGGLE, current_settings.inline_remote_enabled, NULL, inline_remote_switch_event_cb },
    };
    controls[0].out_row = &button_row;
    controls[1].out_row = &volume_lock_row;
    int count = append_plugin_list_rows(controls, 3, PLUGIN_MAX_MUSIC_CONTROLS_LIST_ITEMS,
        plugin_manager_get_music_controls_list_item_count, plugin_manager_get_music_controls_list_item_label,
        plugin_manager_get_music_controls_list_item_options, plugin_music_controls_list_item_click_cb);
    add_pill_list_rows(list, controls, count);
    wrap_row_label(volume_lock_row);
    gui_button_mapping_set_category_summary(add_pill_summary_label(button_row));
    settings_summary_loaded_cb(NULL);
    lv_obj_add_event_cb(scr, settings_summary_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static bool native_dac_controls_supported(void) {
#ifdef HOST_BUILD
    return false;
#else
    return audio_output_dac_controls_supported();
#endif
}

static bool native_dop_supported(void) {
#ifdef HOST_BUILD
    return false;
#else
    return audio_output_dop_supported();
#endif
}

static bool native_dac_dre_supported(void) {
#ifdef HOST_BUILD
    return false;
#else
    return audio_output_dac_dre_supported();
#endif
}

static void apply_dac_preferences(void) {
#ifndef HOST_BUILD
    audio_output_set_dac_preferences(current_settings.dac_filter, current_settings.dac_nos,
                                     current_settings.dac_dre);
#endif
}

static void populate_dac_settings_screen(void);

static void wrapped_label_size_changed(lv_event_t * e) {
    lv_obj_t * label = lv_event_get_target(e);
    lv_obj_t * row = lv_obj_get_parent(label);
    lv_obj_set_height(row, LV_MAX(ui_list_row_height(), lv_obj_get_height(label) + BOARD_SCALE_PX(32)));
}

/* Wraps the first-child label of a pill row at 78% width so a long option
 * never runs under the row's accessory. */
void wrap_row_label(lv_obj_t * row) {
    /* Toggle builders already wrap around the measured switch width. */
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i)
        if (lv_obj_check_type(lv_obj_get_child(row, (int32_t)i), &lv_switch_class)) return;
    lv_obj_t * label = lv_obj_get_child(row, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, lv_pct(78));
    lv_obj_remove_flag(label, LV_OBJ_FLAG_USER_3);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(label, 0, 0);
    lv_obj_add_event_cb(label, wrapped_label_size_changed, LV_EVENT_SIZE_CHANGED, NULL);
    lv_obj_update_layout(row);
    lv_obj_set_height(row, LV_MAX(ui_list_row_height(), lv_obj_get_height(label) + BOARD_SCALE_PX(32)));
}

static bool native_dac_nos_supported(void) {
#ifdef HOST_BUILD
    return false;
#else
    return audio_output_dac_nos_supported();
#endif
}

/* A DAC without NOS offers only the apodizing and linear-phase filters. */
static int dac_filter_count(void) {
    return native_dac_nos_supported() ? 4 : 2;
}

static void dac_filter_selected(unsigned filter) {
    if (native_dac_nos_supported() && current_settings.dac_nos) return;
    if (filter >= (unsigned)dac_filter_count()) return;
    current_settings.dac_filter = (int)filter;
    apply_dac_preferences();
    settings_save_async(&current_settings);
    if (dac_filter_menu) gui_option_menu_set_selected(dac_filter_menu, filter);
}

static void dac_nos_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.dac_nos = !current_settings.dac_nos;
    apply_dac_preferences();
    settings_save_async(&current_settings);
    if (dac_settings_list) populate_dac_settings_screen();
}

static void dac_dre_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.dac_dre = !current_settings.dac_dre;
    gui_pill_toggle_row_set_checked(dac_dre_row, current_settings.dac_dre);
    apply_dac_preferences();
    settings_save_async(&current_settings);
}

static const char * const dac_filter_labels[4] = {
    N_("Fast roll-off, low latency"),
    N_("Fast roll-off, phase compensated"),
    N_("Slow roll-off, low latency"),
    N_("Slow roll-off, phase compensated")
};
static const char * const dac_filter_labels_no_nos[2] = {
    N_("Apodizing fast roll-off"),
    N_("Linear phase fast roll-off")
};

static const char * dac_filter_summary(void) {
    if (native_dac_nos_supported() && current_settings.dac_nos) return TR("Bypassed by NOS");
    int index = current_settings.dac_filter;
    if (index < 0 || index >= dac_filter_count()) index = 0;
    const char * const * labels = native_dac_nos_supported() ? dac_filter_labels : dac_filter_labels_no_nos;
    return TR(labels[index]);
}

static void populate_dac_settings_screen(void) {
    if (!dac_settings_list) return;
    if (lv_obj_get_child_count(dac_settings_list) != 0) {
        gui_option_menu_set_selected(dac_filter_menu, (unsigned)current_settings.dac_filter);
        bool nos = native_dac_nos_supported() && current_settings.dac_nos;
        if (dac_filter_menu) lv_obj_set_state(dac_filter_menu, LV_STATE_DISABLED, nos);
        if (dac_nos_hint) lv_obj_set_flag(dac_nos_hint, LV_OBJ_FLAG_HIDDEN, !nos);
        gui_pill_toggle_row_set_checked(dac_nos_row, nos);
        gui_pill_toggle_row_set_checked(dac_dre_row, current_settings.dac_dre);
        return;
    }
    const char * const * labels = native_dac_nos_supported() ? dac_filter_labels : dac_filter_labels_no_nos;
    char options[512] = "";
    for (int i = 0; i < dac_filter_count(); ++i) {
        size_t used = strlen(options);
        snprintf(options + used, sizeof(options) - used, "%s%s", i ? "\n" : "", TR(labels[i]));
    }
    dac_filter_menu = add_pill_option_menu_row(dac_settings_list, TR("Filter"), options,
        (unsigned)current_settings.dac_filter, dac_filter_selected);
    if (native_dac_nos_supported()) {
        dac_nos_hint = add_wrapped_section_header(dac_settings_list, TR("Turn off NOS to choose a filter"));
        lv_obj_set_flag(dac_nos_hint, LV_OBJ_FLAG_HIDDEN, !current_settings.dac_nos);
        if (dac_filter_menu) lv_obj_set_state(dac_filter_menu, LV_STATE_DISABLED, current_settings.dac_nos);
        dac_nos_row = add_pill_toggle_row(dac_settings_list, TR("NOS mode"),
                                        current_settings.dac_nos, dac_nos_event_cb);
        wrap_row_label(dac_nos_row);
    }
    if (native_dac_dre_supported()) {
        dac_dre_row = add_pill_toggle_row(dac_settings_list, TR("DRE"),
            current_settings.dac_dre, dac_dre_event_cb);
        wrap_row_label(dac_dre_row);
    }
    add_wrapped_section_header(dac_settings_list, native_dac_nos_supported()
        ? TR("Filters and NOS affect local PCM playback. NOS bypasses interpolation and ignores the selected filter. Changes apply when playback restarts.")
        : TR("The filter affects local PCM playback. Changes apply when playback restarts."));
    if (native_dac_dre_supported()) {
        add_wrapped_section_header(dac_settings_list,
            TR("DRE (Dynamic Range Enhancement) affects local PCM and DSD playback."));
    }
}

static const char * const usb_dsd_output_labels[] = { N_("PCM"), N_("Native DSD"), N_("DoP") };
static void usb_dsd_output_event_cb(lv_event_t * e);

static void populate_usb_dsd_settings_screen(void) {
    if (!usb_dsd_settings_list) return;
    if (lv_obj_get_child_count(usb_dsd_settings_list) == 0) {
        for (int i = USB_DSD_OUTPUT_PCM; i <= USB_DSD_OUTPUT_DOP; ++i) {
            wrap_row_label(add_pill_option_row(usb_dsd_settings_list, TR(usb_dsd_output_labels[i]),
                false, usb_dsd_output_event_cb, (void *) (intptr_t) i));
        }
        add_wrapped_section_header(usb_dsd_settings_list,
            TR("Native DSD and DoP bypass software volume and DSP. Use DoP only with a compatible DAC. Unavailable formats or rates use PCM."));
    }
    gui_pill_option_row_set_selected(lv_obj_get_child(usb_dsd_settings_list,
        current_settings.usb_dsd_output), true);
    for (int i = USB_DSD_OUTPUT_PCM; i <= USB_DSD_OUTPUT_DOP; ++i)
        if (i != current_settings.usb_dsd_output)
            gui_pill_option_row_set_selected(lv_obj_get_child(usb_dsd_settings_list, i), false);
}

static void usb_dsd_output_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int output = (int) (intptr_t) lv_event_get_user_data(e);
    if (output < USB_DSD_OUTPUT_PCM || output > USB_DSD_OUTPUT_DOP) return;
    gui_pill_option_list_set_selected(usb_dsd_settings_list, lv_event_get_current_target(e));
    current_settings.usb_dsd_output = output;
#ifndef HOST_BUILD
    audio_output_set_usb_dsd_mode(output);
#endif
    settings_save_async(&current_settings);
}

static lv_obj_t * build_usb_dsd_settings_screen(void) {
    lv_obj_t * title = NULL;
    lv_obj_t * scr = build_subsonic_list_screen(TR("USB output"), &title, &usb_dsd_settings_list);
    (void) title;
    populate_usb_dsd_settings_screen();
    finalize_screen_navigation(scr);
    return scr;
}

static void usb_dsd_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !usb_dsd_settings_screen) return;
    populate_usb_dsd_settings_screen();
    nav_push(usb_dsd_settings_screen);
}

static void set_dsd_gain(int db) {
    if (db < 0 || db > 6) return;
    current_settings.dsd_gain_db = db;
#ifndef HOST_BUILD
    audio_output_set_dsd_gain_db(db);
#endif
    settings_save_async(&current_settings);
}

static void dsd_gain_selected(unsigned selected) {
    set_dsd_gain((int)selected);
    gui_option_menu_set_selected(dsd_gain_menu, (unsigned)current_settings.dsd_gain_db);
}

static void dsd_gain_refresh(lv_event_t * e) {
    (void)e;
    if (dsd_gain_menu) gui_option_menu_set_selected(dsd_gain_menu, (unsigned)current_settings.dsd_gain_db);
}

/* Filters, NOS and DRE sit in their own list so a refresh keeps the DSD Gain row above them. */
static lv_obj_t * build_dac_settings_screen(void) {
    lv_obj_t * list;
    lv_obj_t * scr = build_subsonic_list_screen(TR("DAC"), NULL, &list);
    if (native_dop_supported()) {
        char options[256];
        snprintf(options, sizeof(options), "%s", TR("Off"));
        for (int db = 1; db <= 6; ++db) {
            size_t used = strlen(options);
            snprintf(options + used, sizeof(options) - used, "\n");
            used = strlen(options);
            snprintf(options + used, sizeof(options) - used, TR("+%d dB"), db);
        }
        dsd_gain_menu = add_pill_option_menu_row(list, TR("DSD gain"), options,
            (unsigned)current_settings.dsd_gain_db, dsd_gain_selected);
        add_wrapped_section_header(list,
            TR("Raises native DSD on this player's headphone outputs. It cannot go past the loudest volume step."));
        lv_obj_add_event_cb(scr, dsd_gain_refresh, LV_EVENT_SCREEN_LOADED, NULL);
    }
    if (native_dac_controls_supported()) {
        dac_settings_list = lv_obj_create(list);
        lv_obj_set_size(dac_settings_list, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(dac_settings_list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(dac_settings_list, 0, 0);
        lv_obj_set_style_pad_all(dac_settings_list, 0, 0);
        lv_obj_set_style_pad_row(dac_settings_list, GUI_ROW_GAP, 0);
        lv_obj_set_flex_flow(dac_settings_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(dac_settings_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(dac_settings_list, LV_OBJ_FLAG_SCROLLABLE);
        populate_dac_settings_screen();
    }
    finalize_screen_navigation(scr);
    return scr;
}

static void dac_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !dac_settings_screen) return;
    populate_dac_settings_screen();
    nav_push(dac_settings_screen);
}

static void add_audio_plugin_rows(lv_obj_t * list, const char * group) {
    pill_list_item_t items[PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS];
    int count = append_grouped_plugin_rows(items, 0, PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS,
        "music_audio", group, plugin_manager_get_music_audio_list_item_count,
        plugin_manager_get_music_audio_list_item_label, plugin_manager_get_music_audio_list_item_options,
        plugin_music_audio_list_item_click_cb);
    add_pill_list_rows(list, items, count);
}

static lv_obj_t * build_music_audio_screen(void) {
    lv_obj_t * list;
    lv_obj_t * scr = build_subsonic_list_screen(TR("Sound"), NULL, &list);
    lv_obj_t * volume = add_pill_chevron_row(list, TR("Volume limits"), gui_volume_settings_volume_row_cb);
    audio_summaries[0] = add_pill_summary_label(volume);
    lv_obj_t * eq_row = NULL;
    pill_list_item_t eq = { TR("Equalizer"), PILL_ACCESSORY_CHEVRON, false, gui_equalizer_eq_row_event_cb };
    eq.out_row = &eq_row;
    add_pill_list_rows(list, &eq, 1);
    gui_native_audio_add_gain_row(list);
    add_pill_chevron_row(list, TR("Advanced sound"), open_advanced_sound);
    settings_eq_summary = add_pill_summary_label(eq_row);
    settings_summary_loaded_cb(NULL);
    lv_obj_add_event_cb(scr, settings_summary_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(scr, refresh_audio_summaries, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_advanced_sound_screen(void) {
    lv_obj_t * list;
    lv_obj_t * scr = build_subsonic_list_screen(TR("Advanced sound"), NULL, &list);
    lv_obj_t * row;
    if (dac_settings_screen) {
        row = add_pill_chevron_row(list, TR("DAC"), dac_settings_row_cb);
        audio_summaries[1] = add_pill_summary_label(row);
    }
    lv_obj_t * mseb = NULL, * stage = NULL;
    gui_native_audio_add_effect_rows(list, &mseb, &stage);
    if (mseb) audio_summaries[2] = add_pill_summary_label(mseb);
    if (stage) audio_summaries[3] = add_pill_summary_label(stage);
    row = add_pill_chevron_row(list, TR("Crossfeed"), gui_equalizer_crossfeed_row_event_cb);
    audio_summaries[4] = add_pill_summary_label(row);
    row = add_pill_chevron_row(list, TR("ReplayGain"), replaygain_mode_settings_row_cb);
    audio_summaries[5] = add_pill_summary_label(row);
    row = add_pill_chevron_row(list, TR("USB output"), usb_dsd_settings_row_cb);
    audio_summaries[6] = add_pill_summary_label(row);
    add_audio_plugin_rows(list, NULL);
    if (count_plugin_rows_in_group("music_audio", plugin_manager_get_music_audio_list_item_count(), "effects") > 0) {
        add_section_header(list, TR("Effects"));
        add_audio_plugin_rows(list, "effects");
    }
    refresh_audio_summaries(NULL);
    lv_obj_add_event_cb(scr, refresh_audio_summaries, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static void volume_lock_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.volume_lock_screen_off =
        lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
    hw_buttons_set_volume_lock_screen_off(current_settings.volume_lock_screen_off);
}

static void refresh_all_metadata_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) show_library_refresh_all_metadata_prompt();
}

static void refresh_all_covers_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) show_library_refresh_all_covers_prompt();
}

/* Menu indices count only directions that have labels. */
static int sorting_option_decode(int context, unsigned selected) {
    for (int i = 0; i < SORTING_OPTION_COUNT; i++) {
        if (sorting_orders[i / 2].context != context || !sorting_orders[i / 2].labels[i % 2]) continue;
        if (selected-- == 0) return i;
    }
    return -1;
}

static bool sorting_uses_album_order(int context) {
    return context == SORTING_CONTEXT_ALBUMS || context == SORTING_CONTEXT_ARTISTS ||
           context == SORTING_CONTEXT_ALBUM_ARTISTS;
}

static void sorting_settings(int ** modes, bool ** reverses) {
    modes[SORTING_CONTEXT_FILES] = &current_settings.file_sort_mode;
    modes[SORTING_CONTEXT_ALBUMS] = &current_settings.album_sort_mode;
    modes[SORTING_CONTEXT_ALL_SONGS] = &current_settings.all_songs_sort_mode;
    modes[SORTING_CONTEXT_ARTISTS] = &current_settings.artist_albums_sort_mode;
    modes[SORTING_CONTEXT_ALBUM_ARTISTS] = &current_settings.album_artist_albums_sort_mode;
    reverses[SORTING_CONTEXT_FILES] = &current_settings.file_sort_reverse;
    reverses[SORTING_CONTEXT_ALBUMS] = &current_settings.album_sort_reverse;
    reverses[SORTING_CONTEXT_ALL_SONGS] = &current_settings.all_songs_sort_reverse;
    reverses[SORTING_CONTEXT_ARTISTS] = &current_settings.artist_albums_sort_reverse;
    reverses[SORTING_CONTEXT_ALBUM_ARTISTS] = &current_settings.album_artist_albums_sort_reverse;
}

/* Refreshes one context's check mark, or all of them for -1. Album contexts
 * query the database, so a single non-album menu never waits on it. */
static void refresh_sorting_options(int only) {
    int * modes[SORTING_CONTEXT_COUNT];
    bool * reverses[SORTING_CONTEXT_COUNT];
    sorting_settings(modes, reverses);
    for (int context = 0; context < SORTING_CONTEXT_COUNT; context++) {
        if (only >= 0 && context != only) continue;
        int selected_sort = *modes[context];
        if (sorting_uses_album_order(context) &&
            ((context != SORTING_CONTEXT_ALBUMS && selected_sort > METADATA_DB_ALBUM_SORT_YEAR) ||
             !metadata_db_album_sort_available((metadata_db_album_sort_t) selected_sort)))
            selected_sort = 0;
        unsigned selected = 0;
        for (unsigned index = 0; ; index++) {
            int i = sorting_option_decode(context, index);
            if (i < 0) break;
            if (sorting_orders[i / 2].sort == selected_sort &&
                (i % 2 != 0) == (reverses[context] && *reverses[context] &&
                                 sorting_orders[i / 2].labels[1] != NULL))
                selected = index;
        }
        if (settings_sorting_options[context])
            gui_option_menu_set_selected(settings_sorting_options[context], selected);
    }
}

static void sorting_select(int context, unsigned selected) {
    int option = sorting_option_decode(context, selected);
    if (option < 0) return;
    int sort = sorting_orders[option / 2].sort;
    bool reverse = option % 2 != 0;
    if (sorting_uses_album_order(context) && sort != METADATA_DB_ALBUM_SORT_NAME &&
        !metadata_db_album_sort_available((metadata_db_album_sort_t) sort)) {
        show_error_toast(TR("Update music database to enable this album order"));
        return;
    }
    int * modes[SORTING_CONTEXT_COUNT];
    bool * reverses[SORTING_CONTEXT_COUNT];
    sorting_settings(modes, reverses);
    if (*modes[context] == sort && (!reverses[context] || *reverses[context] == reverse)) return;
    *modes[context] = sort;
    if (reverses[context]) *reverses[context] = reverse;
    settings_save_async(&current_settings);
    gui_library_apply_sorting(sorting_scopes[context]);
    refresh_sorting_options(context);
}
static void sorting_files(unsigned selected) { sorting_select(SORTING_CONTEXT_FILES, selected); }
static void sorting_albums(unsigned selected) { sorting_select(SORTING_CONTEXT_ALBUMS, selected); }
static void sorting_all_songs(unsigned selected) { sorting_select(SORTING_CONTEXT_ALL_SONGS, selected); }
static void sorting_artists(unsigned selected) { sorting_select(SORTING_CONTEXT_ARTISTS, selected); }
static void sorting_album_artists(unsigned selected) { sorting_select(SORTING_CONTEXT_ALBUM_ARTISTS, selected); }

static lv_obj_t * build_sorting_screen(void) {
    lv_obj_t * title, * list;
    lv_obj_t * screen = build_subsonic_list_screen(TR("Sorting"), &title, &list);
    static const char * group_titles[] = {
        N_("Files (folders stay first)"), N_("Albums"), N_("All songs"), N_("Artist pages"), N_("Album artist pages")
    };
    static const gui_option_selected_cb_t callbacks[] = {
        sorting_files, sorting_albums, sorting_all_songs, sorting_artists, sorting_album_artists
    };
    for (int context = 0; context < SORTING_CONTEXT_COUNT; context++) {
        char options[2048] = "";
        size_t used = 0;
        for (unsigned index = 0; ; index++) {
            int i = sorting_option_decode(context, index);
            if (i < 0) break;
            int n = snprintf(options + used, sizeof(options) - used, "%s%s", used ? "\n" : "",
                             TR(sorting_orders[i / 2].labels[i % 2]));
            if (n < 0 || (size_t)n >= sizeof(options) - used) break;
            used += (size_t)n;
        }
        settings_sorting_options[context] = add_pill_option_menu_row(list, TR(group_titles[context]),
            options, 0, callbacks[context]);
    }
    add_wrapped_section_header(list,
        TR("Files sort by modification date; albums by newest track added. Release year keeps unknown years last. Run Update music database to load years."));
    return screen;
}

void gui_settings_open_sorting(gui_library_sort_scope_t scope) {
    int context;
    for (context = 0; context < SORTING_CONTEXT_COUNT; context++)
        if (sorting_scopes[context] == scope) break;
    if (context == SORTING_CONTEXT_COUNT) return;
    if (!settings_sorting_screen) settings_sorting_screen = build_sorting_screen();
    if (!settings_sorting_screen) return;
    refresh_sorting_options(context);
    gui_option_menu_open(settings_sorting_options[context]);
}

static void sorting_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_sorting_screen) settings_sorting_screen = build_sorting_screen();
    if (!settings_sorting_screen) return;
    refresh_sorting_options(-1);
    nav_push(settings_sorting_screen);
}

static void join_albums_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.join_albums = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
    /* Tools on a computer read the setting from the card (device.json). */
    if (sd_card_root_is_mounted()) (void) device_info_publish(SD_COMPAS_ROOT);
    show_library_refresh_all_metadata_prompt();
}

/* Update checks for added, removed and changed files; Refresh re-reads the
 * tags of every song even when a file looks unchanged. Distinct icons keep
 * the two from reading as the same action. */
static lv_obj_t * build_library_category_screen(void) {
    pill_list_item_t items[5 + PLUGIN_MAX_MUSIC_LIBRARY_LIST_ITEMS];
    lv_obj_t * native_rows[4] = { NULL };
    items[0] = (pill_list_item_t){ .label = TR("Update music database"),
        .accessory = PILL_ACCESSORY_CHEVRON, .on_click = update_music_database_row_cb,
        .out_row = &native_rows[0] };
    items[1] = (pill_list_item_t){ .label = TR("Refresh all metadata"),
        .accessory = PILL_ACCESSORY_CHEVRON, .on_click = refresh_all_metadata_row_cb,
        .out_row = &native_rows[1] };
    items[2] = (pill_list_item_t){ .label = TR("Refresh all covers"),
        .accessory = PILL_ACCESSORY_CHEVRON, .on_click = refresh_all_covers_row_cb,
        .out_row = &native_rows[2] };
    items[3] = (pill_list_item_t){ .label = TR("Sorting"), .accessory = PILL_ACCESSORY_CHEVRON,
        .on_click = sorting_row_cb, .out_row = &native_rows[3] };
    items[4] = (pill_list_item_t){ .label = TR("Join albums without album artist"),
        .accessory = PILL_ACCESSORY_TOGGLE, .toggle_initial_state = current_settings.join_albums,
        .on_toggle_change = join_albums_switch_event_cb };
    int count = append_plugin_list_rows(items, 5, PLUGIN_MAX_MUSIC_LIBRARY_LIST_ITEMS,
        plugin_manager_get_music_library_list_item_count,
        plugin_manager_get_music_library_list_item_label,
        plugin_manager_get_music_library_list_item_options,
        plugin_music_library_list_item_click_cb);
    lv_obj_t * scr = build_pill_list_screen(TR("Library"), generic_back_cb, items, count,
        gui_theme_accent_style(), GUI_ROW_GAP, 100);
    decorate_category_row(native_rows[0], "submenu/update_database.png");
    decorate_category_row(native_rows[1], "submenu/refresh_metadata.png");
    decorate_category_row(native_rows[2], "submenu/refresh_covers.png");
    /* Sorting has its own Breeze art; without it, it shares Library's icon. */
    const char * sorting_breeze = asset_path_plain("breeze_rows/library_sorting.png");
    decorate_category_row(native_rows[3], sorting_breeze && access(sorting_breeze, R_OK) == 0
                                              ? "breeze_rows/library_sorting.png" : "settings/library.png");
    finalize_screen_navigation(scr);
    return scr;
}

bool gui_settings_profiles_download_available(void) {
    return count_plugin_rows_in_group("music_audio", plugin_manager_get_music_audio_list_item_count(), "profiles") > 0;
}

void settings_music_build_screens(void) {
    music_playback_screen = build_music_playback_screen();
    buttons_screen = build_buttons_screen();
    usb_dsd_settings_screen = build_usb_dsd_settings_screen();
    if (native_dop_supported() || native_dac_controls_supported()) dac_settings_screen = build_dac_settings_screen();
    music_audio_screen = build_music_audio_screen();
    advanced_sound_screen = build_advanced_sound_screen();
}

void settings_music_teardown_screens(void) {
    gui_play_through_deinit();
    delete_screen(&settings_library_screen);
    delete_screen(&settings_sorting_screen);
    memset(settings_sorting_options, 0, sizeof(settings_sorting_options));
    delete_screen(&settings_eq_profiles_menu_screen);
    delete_screen(&music_playback_screen);
    delete_screen(&buttons_screen);
    settings_crossfade_toggle_img = settings_gapless_toggle_img = NULL;
    delete_screen(&music_audio_screen);
    delete_screen(&advanced_sound_screen);
    delete_screen(&usb_dsd_settings_screen);
    delete_screen(&dac_settings_screen);
    dac_settings_list = NULL;
    dac_filter_menu = dac_dre_row = dac_nos_row = dac_nos_hint = NULL;
    usb_dsd_settings_list = NULL;
    dsd_gain_menu = NULL;
    memset(audio_summaries, 0, sizeof(audio_summaries));
}

lv_obj_t * gui_settings_get_advanced_sound_screen(void) { return advanced_sound_screen; }
lv_obj_t * gui_settings_get_buttons_screen(void) { return buttons_screen; }
lv_obj_t * gui_settings_get_sound_screen(void) { return music_audio_screen; }
lv_obj_t * gui_settings_get_playback_screen(void) { return music_playback_screen; }

void gui_settings_sync_gapless_toggle(void) {
    if (!settings_gapless_toggle_img) return;
    lv_obj_set_state(settings_gapless_toggle_img, LV_STATE_CHECKED, current_settings.gapless_enabled);
}

void gui_settings_sync_crossfade_toggle(void) {
    if (!settings_crossfade_toggle_img) return;
    /* Real lv_switch now (see PILL_ACCESSORY_TOGGLE in screen_builders_menus.c) --
     * CHECKED state alone drives its visual, no sprite swap needed. */
    lv_obj_set_state(settings_crossfade_toggle_img, LV_STATE_CHECKED, current_settings.crossfade_enabled);
}

void gui_settings_open_library(void) {
    push_cached_screen(&settings_library_screen, build_library_category_screen);
}
