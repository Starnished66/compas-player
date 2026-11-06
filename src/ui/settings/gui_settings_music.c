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

static lv_obj_t * settings_crossfade_toggle_img = NULL;
static lv_obj_t * settings_gapless_toggle_img = NULL;
static lv_obj_t * settings_library_screen;
static lv_obj_t * settings_sorting_screen;
enum { SORTING_CONTEXT_FILES, SORTING_CONTEXT_ALBUMS, SORTING_CONTEXT_ARTISTS,
       SORTING_CONTEXT_ALBUM_ARTISTS, SORTING_CONTEXT_COUNT };
static const struct {
    int context;
    int sort;
    const char * labels[2];
} sorting_orders[] = {
    { SORTING_CONTEXT_FILES, 0, { N_("Name (A–Z)"), N_("Name (Z–A)") } },
    { SORTING_CONTEXT_FILES, 1, { N_("Newest Modified"), N_("Oldest Modified") } },
    { SORTING_CONTEXT_ALBUMS, 0, { N_("Name (A–Z)"), N_("Name (Z–A)") } },
    { SORTING_CONTEXT_ALBUMS, 1, { N_("Recently Added"), N_("Oldest Added") } },
    { SORTING_CONTEXT_ALBUMS, 2, { N_("Release Year (oldest first)"), N_("Release Year (newest first)") } },
    { SORTING_CONTEXT_ALBUMS, 3, { N_("Album Artist (A–Z)"), N_("Album Artist (Z–A)") } },
    { SORTING_CONTEXT_ARTISTS, 0, { N_("Name (A–Z)"), N_("Name (Z–A)") } },
    { SORTING_CONTEXT_ARTISTS, 1, { N_("Recently Added"), N_("Oldest Added") } },
    { SORTING_CONTEXT_ARTISTS, 2, { N_("Release Year (oldest first)"), N_("Release Year (newest first)") } },
    { SORTING_CONTEXT_ALBUM_ARTISTS, 0, { N_("Name (A–Z)"), N_("Name (Z–A)") } },
    { SORTING_CONTEXT_ALBUM_ARTISTS, 1, { N_("Recently Added"), N_("Oldest Added") } },
    { SORTING_CONTEXT_ALBUM_ARTISTS, 2, { N_("Release Year (oldest first)"), N_("Release Year (newest first)") } },
};
enum { SORTING_OPTION_COUNT = sizeof(sorting_orders) / sizeof(sorting_orders[0]) * 2 };
static lv_obj_t * settings_sorting_options[SORTING_OPTION_COUNT];
static lv_obj_t * settings_sound_effects_screen;
static lv_obj_t * dac_settings_screen;
static lv_obj_t * dac_settings_list;
static lv_obj_t * usb_dsd_settings_screen;
static lv_obj_t * usb_dsd_settings_list;
static lv_obj_t * dsd_gain_settings_screen;
static lv_obj_t * dsd_gain_settings_list;
static lv_obj_t * settings_eq_profiles_menu_screen;
static lv_obj_t * music_playback_screen;
static lv_obj_t * music_audio_screen;
static lv_obj_t * music_controls_screen;

static lv_obj_t * build_library_category_screen(void);
static lv_obj_t * build_music_controls_screen(void);

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

static void music_controls_menu_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    push_cached_screen(&music_controls_screen, build_music_controls_screen);
}

PLUGIN_ROW_CLICK_CB(plugin_music_library_list_item_click_cb, plugin_manager_music_library_list_item_clicked)

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

static lv_obj_t * build_sound_effects_screen(void) {
    return build_music_audio_group_screen(TR("Sound Effects"), "effects");
}

static lv_obj_t * build_download_profiles_screen(void) {
    return build_music_audio_group_screen(TR("Download Profiles"), "profiles");
}

static void music_effects_menu_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    push_cached_screen(&settings_sound_effects_screen, build_sound_effects_screen);
}

void music_profiles_menu_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    push_cached_screen(&settings_eq_profiles_menu_screen, build_download_profiles_screen);
}

static lv_obj_t * build_music_playback_screen(void) {
    pill_list_item_t items[5 + PLUGIN_MAX_PLAYBACK_LIST_ITEMS];
    lv_obj_t * car_row = NULL;
    items[0] = (pill_list_item_t){ TR("Resume Last Track"), PILL_ACCESSORY_CHEVRON, false, resume_mode_settings_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("Crossfade"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.crossfade_enabled, NULL, crossfade_switch_event_cb, NULL,
                                    &settings_crossfade_toggle_img };
    /* Directly below Crossfade: the two are coupled (crossfade needs an
     * armed next track, which only gapless provides), so seeing them
     * together is what makes that relationship legible when one flips the
     * other -- see gui_player_set_gapless_enabled(). */
    items[2] = (pill_list_item_t){ TR("Gapless"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.gapless_enabled, NULL, gapless_switch_event_cb, NULL,
                                    &settings_gapless_toggle_img };
    items[3] = (pill_list_item_t){ TR("Car Mode"), PILL_ACCESSORY_CHEVRON,
                                    false, gui_volume_settings_car_mode_row_cb, NULL, NULL };
    items[3].out_row = &car_row;
    items[4] = (pill_list_item_t){ TR("Buttons & Remote"), PILL_ACCESSORY_CHEVRON,
                                    false, music_controls_menu_cb, NULL, NULL };

    int count = 5;
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_PLAYBACK_LIST_ITEMS,
                                    plugin_manager_get_playback_list_item_count,
                                    plugin_manager_get_playback_list_item_label,
                                    plugin_manager_get_playback_list_item_options,
                                    plugin_playback_list_item_click_cb);

    lv_obj_t * scr = build_pill_list_screen(TR("Playback & Controls"), generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    settings_car_summary = add_pill_summary_label(car_row);
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

/* Wraps the first-child label of a pill row at 78% width so a long option
 * never runs under the row's accessory. */
static void wrap_row_label(lv_obj_t * row) {
    lv_obj_t * label = lv_obj_get_child(row, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, lv_pct(78));
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

static void dac_filter_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int filter = (int) (intptr_t) lv_event_get_user_data(e);
    if (filter < 0 || filter >= dac_filter_count()) return;
    current_settings.dac_filter = filter;
    apply_dac_preferences();
    settings_save_async(&current_settings);
    if (dac_settings_list) {
        populate_dac_settings_screen();
    }
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
    apply_dac_preferences();
    settings_save_async(&current_settings);
    if (dac_settings_list) populate_dac_settings_screen();
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

static void populate_dac_settings_screen(void) {
    if (!dac_settings_list) return;
    lv_obj_clean(dac_settings_list);
    const char * const * labels = native_dac_nos_supported() ? dac_filter_labels : dac_filter_labels_no_nos;
    for (int i = 0; i < dac_filter_count(); ++i) {
        wrap_row_label(add_pill_option_row(dac_settings_list, TR(labels[i]),
            current_settings.dac_filter == i, dac_filter_event_cb, (void *) (intptr_t) i));
    }
    if (native_dac_nos_supported())
        wrap_row_label(add_pill_toggle_row(dac_settings_list, TR("NOS Mode"),
            current_settings.dac_nos, dac_nos_event_cb));
    if (native_dac_dre_supported()) {
        wrap_row_label(add_pill_toggle_row(dac_settings_list, TR("DRE"),
            current_settings.dac_dre, dac_dre_event_cb));
    }
    add_wrapped_section_header(dac_settings_list, native_dac_nos_supported()
        ? TR("Filters and NOS affect local PCM playback. NOS bypasses interpolation and ignores the selected filter. Changes apply when playback restarts.")
        : TR("The filter affects local PCM playback. Changes apply when playback restarts."));
    if (native_dac_dre_supported()) {
        add_wrapped_section_header(dac_settings_list,
            TR("DRE (Dynamic Range Enhancement) affects local PCM and DSD playback."));
    }
}

static lv_obj_t * build_dac_settings_screen(void) {
    lv_obj_t * title = NULL;
    lv_obj_t * scr = build_subsonic_list_screen(TR("Local DAC Settings"), &title, &dac_settings_list);
    (void) title;
    populate_dac_settings_screen();
    finalize_screen_navigation(scr);
    return scr;
}

static void dac_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !dac_settings_screen) return;
    populate_dac_settings_screen();
    nav_push(dac_settings_screen);
}

static const char * const usb_dsd_output_labels[] = { N_("PCM"), N_("Native DSD"), N_("DoP") };
static void usb_dsd_output_event_cb(lv_event_t * e);

static void populate_usb_dsd_settings_screen(void) {
    if (!usb_dsd_settings_list) return;
    lv_obj_clean(usb_dsd_settings_list);
    for (int i = USB_DSD_OUTPUT_PCM; i <= USB_DSD_OUTPUT_DOP; ++i) {
        wrap_row_label(add_pill_option_row(usb_dsd_settings_list, TR(usb_dsd_output_labels[i]),
            current_settings.usb_dsd_output == i, usb_dsd_output_event_cb, (void *) (intptr_t) i));
    }
    add_wrapped_section_header(usb_dsd_settings_list,
        TR("Native DSD and DoP bypass software volume and DSP. Use DoP only with a compatible DAC. Unavailable formats or rates use PCM."));
}

static void usb_dsd_output_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int output = (int) (intptr_t) lv_event_get_user_data(e);
    if (output < USB_DSD_OUTPUT_PCM || output > USB_DSD_OUTPUT_DOP) return;
    current_settings.usb_dsd_output = output;
#ifndef HOST_BUILD
    audio_output_set_usb_dsd_mode(output);
#endif
    settings_save_async(&current_settings);
    populate_usb_dsd_settings_screen();
}

static lv_obj_t * build_usb_dsd_settings_screen(void) {
    lv_obj_t * title = NULL;
    lv_obj_t * scr = build_subsonic_list_screen(TR("USB DSD Output"), &title, &usb_dsd_settings_list);
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

static void dsd_gain_event_cb(lv_event_t * e);

static void populate_dsd_gain_settings_screen(void) {
    if (!dsd_gain_settings_list) return;
    lv_obj_clean(dsd_gain_settings_list);
    for (int db = 0; db <= 6; ++db) {
        char label[24];
        if (db == 0) snprintf(label, sizeof(label), "%s", TR("Off"));
        else snprintf(label, sizeof(label), TR("+%d dB"), db);
        wrap_row_label(add_pill_option_row(dsd_gain_settings_list, label,
            current_settings.dsd_gain_db == db, dsd_gain_event_cb, (void *) (intptr_t) db));
    }
    add_wrapped_section_header(dsd_gain_settings_list,
        TR("Raises native DSD on this player's headphone outputs. It cannot go past the loudest volume step."));
}

static void dsd_gain_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int db = (int) (intptr_t) lv_event_get_user_data(e);
    if (db < 0 || db > 6) return;
    current_settings.dsd_gain_db = db;
#ifndef HOST_BUILD
    audio_output_set_dsd_gain_db(db);
#endif
    settings_save_async(&current_settings);
    populate_dsd_gain_settings_screen();
}

static lv_obj_t * build_dsd_gain_settings_screen(void) {
    lv_obj_t * title = NULL;
    lv_obj_t * scr = build_subsonic_list_screen(TR("DSD Gain"), &title, &dsd_gain_settings_list);
    (void) title;
    populate_dsd_gain_settings_screen();
    finalize_screen_navigation(scr);
    return scr;
}

static void dsd_gain_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !dsd_gain_settings_screen) return;
    populate_dsd_gain_settings_screen();
    nav_push(dsd_gain_settings_screen);
}

static lv_obj_t * build_music_audio_screen(void) {
    pill_list_item_t items[9 + PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS];
    lv_obj_t * eq_row = NULL;
    items[0] = (pill_list_item_t){ TR("Equalizer"), PILL_ACCESSORY_CHEVRON, false, gui_equalizer_eq_row_event_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("Crossfeed"), PILL_ACCESSORY_CHEVRON, false, gui_equalizer_crossfeed_row_event_cb, NULL, NULL };
    items[0].out_row = &eq_row;
    int count = append_grouped_plugin_rows(items, 2, PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS,
                                       "music_audio", NULL,
                                       plugin_manager_get_music_audio_list_item_count,
                                       plugin_manager_get_music_audio_list_item_label,
                                       plugin_manager_get_music_audio_list_item_options,
                                       plugin_music_audio_list_item_click_cb);
    items[count++] = (pill_list_item_t){ TR("Startup Volume"), PILL_ACCESSORY_CHEVRON, false, gui_volume_settings_startup_volume_row_cb, NULL, NULL };
    items[count++] = (pill_list_item_t){ TR("Max Volume"), PILL_ACCESSORY_CHEVRON, false, gui_volume_settings_max_volume_row_cb, NULL, NULL };
    items[count++] = (pill_list_item_t){ TR("ReplayGain"), PILL_ACCESSORY_CHEVRON, false, replaygain_mode_settings_row_cb, NULL, NULL };
    items[count++] = (pill_list_item_t){ TR("USB DSD Output"), PILL_ACCESSORY_CHEVRON, false, usb_dsd_settings_row_cb, NULL, NULL };
    if (native_dop_supported())
        items[count++] = (pill_list_item_t){ TR("DSD Gain"), PILL_ACCESSORY_CHEVRON, false, dsd_gain_settings_row_cb, NULL, NULL };
    if (native_dac_controls_supported())
        items[count++] = (pill_list_item_t){ TR("Local DAC Settings"), PILL_ACCESSORY_CHEVRON, false, dac_settings_row_cb, NULL, NULL };
    if (count_plugin_rows_in_group("music_audio", plugin_manager_get_music_audio_list_item_count(), "effects") > 0)
        items[count++] = (pill_list_item_t){ TR("Sound Effects"), PILL_ACCESSORY_CHEVRON, false, music_effects_menu_cb, NULL, NULL };

    lv_obj_t * scr = build_pill_list_screen(TR("Sound"), generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    settings_eq_summary = add_pill_summary_label(eq_row);
    settings_summary_loaded_cb(NULL);
    lv_obj_add_event_cb(scr, settings_summary_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
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

static lv_obj_t * build_music_controls_screen(void) {
    pill_list_item_t items[3 + PLUGIN_MAX_MUSIC_CONTROLS_LIST_ITEMS];
    lv_obj_t *button_mapping_row = NULL;
    items[0] = (pill_list_item_t){ TR("Button Mapping"), PILL_ACCESSORY_CHEVRON, false,
                                    gui_button_mapping_row_cb, NULL, NULL };
    items[0].out_row = &button_mapping_row;
    items[1] = (pill_list_item_t){ TR("Volume Lock (Screen Off)"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.volume_lock_screen_off, NULL, volume_lock_switch_event_cb, NULL };
    items[2] = (pill_list_item_t){ TR("In-line Remote"), PILL_ACCESSORY_TOGGLE,
                                    current_settings.inline_remote_enabled, NULL, inline_remote_switch_event_cb, NULL };

    int count = 3;
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_MUSIC_CONTROLS_LIST_ITEMS,
                                    plugin_manager_get_music_controls_list_item_count,
                                    plugin_manager_get_music_controls_list_item_label,
                                    plugin_manager_get_music_controls_list_item_options,
                                    plugin_music_controls_list_item_click_cb);

    lv_obj_t * scr = build_pill_list_screen(TR("Buttons & Remote"), generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    if (button_mapping_row) gui_button_mapping_set_category_summary(add_pill_summary_label(button_mapping_row));
    settings_summary_loaded_cb(NULL);
    lv_obj_add_event_cb(scr, settings_summary_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static void refresh_all_metadata_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) show_library_refresh_all_metadata_prompt();
}

static void refresh_all_covers_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) show_library_refresh_all_covers_prompt();
}

static void sorting_option_decode(int option, int * context, int * sort, bool * reverse) {
    *context = sorting_orders[option / 2].context;
    *sort = sorting_orders[option / 2].sort;
    *reverse = (option % 2) != 0;
}

static void sorting_settings(int ** modes, bool ** reverses) {
    modes[0] = &current_settings.file_sort_mode;
    modes[1] = &current_settings.album_sort_mode;
    modes[2] = &current_settings.artist_albums_sort_mode;
    modes[3] = &current_settings.album_artist_albums_sort_mode;
    reverses[0] = &current_settings.file_sort_reverse;
    reverses[1] = &current_settings.album_sort_reverse;
    reverses[2] = &current_settings.artist_albums_sort_reverse;
    reverses[3] = &current_settings.album_artist_albums_sort_reverse;
}

static void refresh_sorting_options(void) {
    int * modes[SORTING_CONTEXT_COUNT];
    bool * reverses[SORTING_CONTEXT_COUNT];
    sorting_settings(modes, reverses);
    for (int i = 0; i < SORTING_OPTION_COUNT; i++) {
        int context, sort;
        bool reverse;
        sorting_option_decode(i, &context, &sort, &reverse);
        int selected_sort = *modes[context];
        if (context != SORTING_CONTEXT_FILES &&
            ((context != SORTING_CONTEXT_ALBUMS && selected_sort > METADATA_DB_ALBUM_SORT_YEAR) ||
             !metadata_db_album_sort_available((metadata_db_album_sort_t) selected_sort)))
            selected_sort = 0;
        bool selected = selected_sort == sort && *reverses[context] == reverse;
        if (settings_sorting_options[i])
            lv_obj_set_style_border_width(settings_sorting_options[i], selected ? BOARD_SCALE_PX(3) : 0, 0);
    }
}

static void sorting_option_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int option = (int) (intptr_t) lv_event_get_user_data(e);
    if (option < 0 || option >= SORTING_OPTION_COUNT) return;
    int context, sort;
    bool reverse;
    sorting_option_decode(option, &context, &sort, &reverse);
    if (context != SORTING_CONTEXT_FILES && sort != METADATA_DB_ALBUM_SORT_NAME &&
        !metadata_db_album_sort_available((metadata_db_album_sort_t) sort)) {
        show_error_toast(TR("Update Music Database to enable this album order"));
        return;
    }
    int * modes[SORTING_CONTEXT_COUNT];
    bool * reverses[SORTING_CONTEXT_COUNT];
    sorting_settings(modes, reverses);
    if (*modes[context] == sort && *reverses[context] == reverse) return;
    *modes[context] = sort;
    *reverses[context] = reverse;
    static const gui_library_sort_scope_t scopes[] = {
        GUI_LIBRARY_SORT_FILES, GUI_LIBRARY_SORT_ALBUMS,
        GUI_LIBRARY_SORT_ARTIST_ALBUMS, GUI_LIBRARY_SORT_ALBUM_ARTIST_ALBUMS
    };
    settings_save_async(&current_settings);
    gui_library_apply_sorting(scopes[context]);
    refresh_sorting_options();
}

static lv_obj_t * build_sorting_screen(void) {
    lv_obj_t * title, * list;
    lv_obj_t * screen = build_subsonic_list_screen(TR("Sorting"), &title, &list);
    (void) title;
    static const char * group_titles[] = {
        N_("Files (folders stay first)"), N_("Albums"), N_("Artists"), N_("Album Artist")
    };
    int previous_context = -1;
    for (int option = 0; option < SORTING_OPTION_COUNT; option++) {
        int context, sort;
        bool reverse;
        sorting_option_decode(option, &context, &sort, &reverse);
        (void) sort;
        if (context != previous_context) {
            add_section_header(list, TR(group_titles[context]));
            previous_context = context;
        }
        settings_sorting_options[option] = add_pill_option_row(list,
            TR(sorting_orders[option / 2].labels[reverse]), false,
            sorting_option_cb, (void *) (intptr_t) option);
        lv_obj_add_style(settings_sorting_options[option], gui_theme_accent_outline_style(), 0);
    }
    add_wrapped_section_header(list,
        TR("Files use modification dates. Albums use the newest track added; Release Year keeps missing years last. Update Music Database once to read years from existing files."));
    refresh_sorting_options();
    return screen;
}

static void sorting_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!settings_sorting_screen) settings_sorting_screen = build_sorting_screen();
    if (!settings_sorting_screen) return;
    refresh_sorting_options();
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
    items[0] = (pill_list_item_t){ .label = TR("Update Music Database"),
        .accessory = PILL_ACCESSORY_NONE, .on_click = update_music_database_row_cb,
        .out_row = &native_rows[0] };
    items[1] = (pill_list_item_t){ .label = TR("Refresh All Metadata"),
        .accessory = PILL_ACCESSORY_NONE, .on_click = refresh_all_metadata_row_cb,
        .out_row = &native_rows[1] };
    items[2] = (pill_list_item_t){ .label = TR("Refresh All Covers"),
        .accessory = PILL_ACCESSORY_NONE, .on_click = refresh_all_covers_row_cb,
        .out_row = &native_rows[2] };
    items[3] = (pill_list_item_t){ .label = TR("Sorting"), .accessory = PILL_ACCESSORY_CHEVRON,
        .on_click = sorting_row_cb, .out_row = &native_rows[3] };
    items[4] = (pill_list_item_t){ .label = TR("Join albums without Album Artist"),
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
    if (native_dac_controls_supported()) dac_settings_screen = build_dac_settings_screen();
    usb_dsd_settings_screen = build_usb_dsd_settings_screen();
    if (native_dop_supported()) dsd_gain_settings_screen = build_dsd_gain_settings_screen();
    music_audio_screen = build_music_audio_screen();
    music_controls_screen = NULL; /* built on first open of Playback & Controls > Buttons & Remote */
}

void settings_music_teardown_screens(void) {
    delete_screen(&settings_library_screen);
    delete_screen(&settings_sorting_screen);
    memset(settings_sorting_options, 0, sizeof(settings_sorting_options));
    delete_screen(&settings_sound_effects_screen);
    delete_screen(&settings_eq_profiles_menu_screen);
    delete_screen(&music_playback_screen);
    settings_crossfade_toggle_img = settings_gapless_toggle_img = NULL;
    delete_screen(&music_audio_screen);
    delete_screen(&dac_settings_screen);
    delete_screen(&usb_dsd_settings_screen);
    delete_screen(&dsd_gain_settings_screen);
    dac_settings_list = NULL;
    usb_dsd_settings_list = NULL;
    dsd_gain_settings_list = NULL;
    delete_screen(&music_controls_screen);
}

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
