#include "settings.h"
#include "../../library/remote/subsonic/subsonic_saved_servers.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Settings load synchronizes the saved-server sidecar. Keep this test entirely
 * inside its private host working directory by replacing that unrelated store. */
void subsonic_saved_servers_load(subsonic_saved_server_t **rows, int *count) {
    *rows = NULL;
    *count = 0;
}

void subsonic_saved_servers_upsert(const char *url, const char *username,
                                   const char *password, bool verify_tls) {
    (void)url;
    (void)username;
    (void)password;
    (void)verify_tls;
}

typedef struct {
    const char *name;
    size_t offset;
} bool_field_t;

#define BOOL_FIELD(field) { #field, offsetof(player_settings_t, field) }
#define NESTED_BOOL_FIELD(field) \
    { "ui_customization." #field, \
      offsetof(player_settings_t, ui_customization) + offsetof(ui_customization_t, field) }

/* This list covers bool members serialized by settings_save(). bt_dac_mode_enabled
 * is deliberately excluded because settings.h documents it as runtime-only. */
static const bool_field_t persisted_booleans[] = {
    BOOL_FIELD(volume_lock_screen_off),
    BOOL_FIELD(accent_dynamic),
    BOOL_FIELD(startup_splash_tint),
    BOOL_FIELD(titlebar_icon_tint),
    BOOL_FIELD(crossfade_enabled),
    BOOL_FIELD(gapless_enabled),
    BOOL_FIELD(car_mode_enabled),
    BOOL_FIELD(car_mode_autoresume_enabled),
    BOOL_FIELD(inline_remote_enabled),
    BOOL_FIELD(lyrics_enabled),
    BOOL_FIELD(lyrics_keep_screen_on),
    BOOL_FIELD(subsonic_verify_tls),
    BOOL_FIELD(bt_volume_sync_enabled),
    BOOL_FIELD(dac_nos),
    BOOL_FIELD(dac_dre),
    BOOL_FIELD(bt_speexrate_enabled),
    BOOL_FIELD(bt_hide_unnamed_devices),
    BOOL_FIELD(wifi_dac_mode_enabled),
    BOOL_FIELD(dlna_renderer_enabled),
    BOOL_FIELD(remote_control_enabled),
    BOOL_FIELD(screen_timeout_enabled),
    BOOL_FIELD(screen_dimming_enabled),
    BOOL_FIELD(hide_player_topbar),
    BOOL_FIELD(screen_upside_down),
    BOOL_FIELD(led_indicator_enabled),
    BOOL_FIELD(db_logging_enabled),
    BOOL_FIELD(screenshot_combo_enabled),
    BOOL_FIELD(charge_limiter_enabled),
    BOOL_FIELD(safe_charging_enabled),
    BOOL_FIELD(show_battery_percent),
    BOOL_FIELD(show_artist_images),
    BOOL_FIELD(idle_shutdown_enabled),
    BOOL_FIELD(idle_suspend_enabled),
    BOOL_FIELD(idle_suspend_default_migrated),
    BOOL_FIELD(usb_dac_disable_charging),
    BOOL_FIELD(swipe_up_home_enabled),
    BOOL_FIELD(quick_drawer_volume_visible),
    NESTED_BOOL_FIELD(drawer_enabled),
    NESTED_BOOL_FIELD(drawer_eq_visible),
    BOOL_FIELD(startup_volume_fixed_enabled),
    BOOL_FIELD(clock_24h),
    BOOL_FIELD(clock_automatic),
    BOOL_FIELD(setup_complete),
    BOOL_FIELD(setup_intro_played),
    BOOL_FIELD(setup_scan_music)
};

#undef BOOL_FIELD
#undef NESTED_BOOL_FIELD

static bool round_trip(const player_settings_t *input, player_settings_t *output) {
    settings_save(input);
    return settings_load(output);
}

static bool append_raw_settings_lines(const char *lines) {
    FILE *file = fopen(".compas/settings.txt", "a");
    if (!file) return false;
    bool ok = fputs(lines, file) >= 0;
    return fclose(file) == 0 && ok;
}

static bool replace_raw_settings(const char *contents) {
    FILE *file = fopen(".compas/settings.txt", "w");
    if (!file) return false;
    bool ok = fputs(contents, file) >= 0;
    return fclose(file) == 0 && ok;
}

static bool check_bool_roundtrips(const player_settings_t *defaults) {
    for (size_t i = 0; i < sizeof(persisted_booleans) / sizeof(persisted_booleans[0]); ++i) {
        for (int wanted = 0; wanted <= 1; ++wanted) {
            player_settings_t input = *defaults;
            player_settings_t output;
            bool *in = (bool *)((char *)&input + persisted_booleans[i].offset);
            *in = wanted != 0;
            if (!round_trip(&input, &output)) {
                fprintf(stderr, "settings round trip could not reload %s\n", persisted_booleans[i].name);
                return false;
            }
            bool actual = *(bool *)((char *)&output + persisted_booleans[i].offset);
            if (actual != (wanted != 0)) {
                fprintf(stderr, "settings boolean round trip failed: %s\n", persisted_booleans[i].name);
                return false;
            }
        }
    }

    for (int wanted = 0; wanted <= 1; ++wanted) {
        player_settings_t input = *defaults;
        player_settings_t output;
        input.subsonic_saved_count = 1;
        snprintf(input.subsonic_saved[0].url, sizeof(input.subsonic_saved[0].url),
                 "https://saved.example.invalid");
        input.subsonic_saved[0].verify_tls = wanted != 0;
        if (!round_trip(&input, &output) || output.subsonic_saved_count != 1 ||
            output.subsonic_saved[0].verify_tls != (wanted != 0)) {
            fputs("settings boolean round trip failed: saved server verify_tls\n", stderr);
            return false;
        }
    }
    return true;
}

static bool check_int_roundtrip(const player_settings_t *defaults, const char *name,
                                size_t offset, int value, int expected) {
    player_settings_t input = *defaults;
    player_settings_t output;
    *(int *)((char *)&input + offset) = value;
    if (!round_trip(&input, &output)) {
        fprintf(stderr, "settings round trip could not reload %s\n", name);
        return false;
    }
    int actual = *(int *)((char *)&output + offset);
    if (actual != expected) {
        fprintf(stderr, "settings value round trip failed: %s\n", name);
        return false;
    }
    return true;
}

#define CHECK_INT(field, value, expected) \
    do { if (!check_int_roundtrip(defaults, #field, offsetof(player_settings_t, field), \
                                  (value), (expected))) return false; } while (0)

static bool check_enums_and_presets(const player_settings_t *defaults) {
    for (int value = 0; value <= 2; ++value) {
        CHECK_INT(resume_mode, value, value);
        CHECK_INT(play_pause_button_mode, value, value);
        CHECK_INT(replaygain_mode, value, value);
        CHECK_INT(topbar_clock_position, value, value);
        CHECK_INT(usb_mode, value, value);
        CHECK_INT(subsonic_download_layout, value, value == 1 ? 1 : 0);
    }
    for (int value = 0; value <= 3; ++value) {
        CHECK_INT(dac_filter, value, value);
        CHECK_INT(subsonic_stream_quality, value, value);
        CHECK_INT(genre_view_mode, value, value);
        CHECK_INT(usb_dsd_output, value, value <= USB_DSD_OUTPUT_DOP ? value : USB_DSD_OUTPUT_PCM);
    }
    for (int value = 0; value <= 4; ++value) CHECK_INT(play_mode, value, value);
    for (int value = 0; value <= 3; ++value) CHECK_INT(album_sort_mode, value, value);
    for (int value = 0; value <= 2; ++value) {
        CHECK_INT(artist_albums_sort_mode, value, value);
        CHECK_INT(album_artist_albums_sort_mode, value, value);
    }
    for (int value = 0; value <= 2; ++value) CHECK_INT(font_size_tier, value, value);
    for (int value = 1; value <= 2; ++value) CHECK_INT(lyrics_font_size_tier, value, value);
    for (int value = 0; value <= 7; ++value) CHECK_INT(setup_step, value, value);

    const int keyboard_values[] = {
        KEYBOARD_LAYOUT_T9, KEYBOARD_LAYOUT_QWERTY, KEYBOARD_LAYOUT_AZERTY,
        KEYBOARD_LAYOUT_QWERTZ, KEYBOARD_LAYOUT_RUSSIAN, KEYBOARD_LAYOUT_UKRAINIAN,
        KEYBOARD_LAYOUT_LEGACY_4, KEYBOARD_LAYOUT_LEGACY_5, KEYBOARD_LAYOUT_LEGACY_6
    };
    for (size_t i = 0; i < sizeof(keyboard_values) / sizeof(keyboard_values[0]); ++i) {
        int value = keyboard_values[i];
        CHECK_INT(keyboard_layout, value,
                  value >= KEYBOARD_LAYOUT_LEGACY_4 && value <= KEYBOARD_LAYOUT_LEGACY_6
                      ? KEYBOARD_LAYOUT_QWERTY : value);
    }

    const int animation_values[] = { 0, 25, 50, 75, 100 };
    for (size_t i = 0; i < sizeof(animation_values) / sizeof(animation_values[0]); ++i)
        CHECK_INT(animation_scale, animation_values[i], animation_values[i]);

    const int *step_sets[] = { SCREEN_TIMEOUT_STEPS, SCREEN_DIM_DELAY_STEPS,
                               IDLE_SHUTDOWN_STEPS, SLEEP_TIMER_STEPS };
    const int step_counts[] = { SCREEN_TIMEOUT_STEP_COUNT, SCREEN_DIM_DELAY_STEP_COUNT,
                                IDLE_SHUTDOWN_STEP_COUNT, SLEEP_TIMER_STEP_COUNT };
    const size_t step_offsets[] = { offsetof(player_settings_t, screen_timeout_seconds),
                                    offsetof(player_settings_t, screen_dim_delay_seconds),
                                    offsetof(player_settings_t, idle_shutdown_minutes),
                                    offsetof(player_settings_t, sleep_timer_minutes) };
    const char *step_names[] = { "screen_timeout_seconds", "screen_dim_delay_seconds",
                                 "idle_shutdown_minutes", "sleep_timer_minutes" };
    for (size_t set = 0; set < sizeof(step_counts) / sizeof(step_counts[0]); ++set)
        for (int i = 0; i < step_counts[set]; ++i)
            if (!check_int_roundtrip(defaults, step_names[set], step_offsets[set],
                                     step_sets[set][i], step_sets[set][i])) return false;

    const int action_count = BUTTON_MAPPING_ACTION_COUNT;
    for (int action = 0; action < action_count; ++action) {
        player_settings_t input = *defaults;
        player_settings_t output;
        for (int button = 0; button < BUTTON_MAPPING_BUTTON_COUNT; ++button) {
            input.button_bindings[button].tap = (button_mapping_action_t)action;
            input.button_bindings[button].double_tap = (button_mapping_action_t)action;
            input.button_bindings[button].hold = (button_mapping_action_t)action;
        }
        if (!round_trip(&input, &output)) return false;
        for (int button = 0; button < BUTTON_MAPPING_BUTTON_COUNT; ++button) {
            if (output.button_bindings[button].tap != (button_mapping_action_t)action ||
                output.button_bindings[button].double_tap != (button_mapping_action_t)action ||
                output.button_bindings[button].hold != (button_mapping_action_t)action) {
                fputs("button action enum did not survive settings round trip\n", stderr);
                return false;
            }
        }
    }

    for (int mode = 0; mode <= 1; ++mode) CHECK_INT(file_sort_mode, mode, mode);
    for (int source_kind = 0; source_kind <= 2; ++source_kind)
        CHECK_INT(last_source_kind, source_kind, source_kind);
    return true;
}

static bool check_invalid_values(const player_settings_t *defaults) {
    CHECK_INT(resume_mode, -1, 0);
    CHECK_INT(resume_mode, 3, 0);
    CHECK_INT(play_pause_button_mode, -1, 0);
    CHECK_INT(play_pause_button_mode, 3, 0);
    CHECK_INT(replaygain_mode, -1, 1);
    CHECK_INT(replaygain_mode, 3, 1);
    CHECK_INT(animation_scale, 1, 100);
    CHECK_INT(animation_scale, 101, 100);
    CHECK_INT(font_size_tier, -1, 1);
    CHECK_INT(font_size_tier, 3, 1);
    CHECK_INT(lyrics_font_size_tier, 0, 1);
    CHECK_INT(lyrics_font_size_tier, 3, 1);
    CHECK_INT(usb_mode, -1, 0);
    CHECK_INT(usb_mode, 3, 0);
    CHECK_INT(play_mode, -1, 0);
    CHECK_INT(play_mode, 5, 0);
    CHECK_INT(setup_step, -1, 0);
    CHECK_INT(setup_step, 8, 0);
    CHECK_INT(startup_volume_fixed_percent, -1, 20);
    CHECK_INT(startup_volume_fixed_percent, 101, 20);
    CHECK_INT(car_mode_volume_percent, -1, 80);
    CHECK_INT(car_mode_volume_percent, 101, 80);
    CHECK_INT(max_volume_percent, 0, 100);
    CHECK_INT(max_volume_percent, 101, 100);
    CHECK_INT(brightness_percent, -1, 80);
    CHECK_INT(brightness_percent, 101, 80);
    CHECK_INT(screen_timeout_seconds, 0, 15);
    CHECK_INT(screen_timeout_seconds, 99999, 1800);
    CHECK_INT(screen_dim_delay_seconds, 0, 5);
    CHECK_INT(screen_dim_delay_seconds, 99999, 300);
    CHECK_INT(idle_shutdown_minutes, 0, 10);
    CHECK_INT(idle_shutdown_minutes, 99999, 120);
    CHECK_INT(sleep_timer_minutes, 0, 5);
    CHECK_INT(sleep_timer_minutes, 99999, 180);
    CHECK_INT(subsonic_stream_quality, -1, 0);
    CHECK_INT(subsonic_stream_quality, 4, 0);
    CHECK_INT(subsonic_download_layout, -1, 0);
    CHECK_INT(subsonic_download_layout, 2, 0);
    CHECK_INT(dac_filter, -1, 0);
    CHECK_INT(dac_filter, 4, 0);
    CHECK_INT(album_sort_mode, -1, 0);
    CHECK_INT(album_sort_mode, 4, 0);
    CHECK_INT(artist_albums_sort_mode, -1, 0);
    CHECK_INT(artist_albums_sort_mode, 3, 0);
    CHECK_INT(album_artist_albums_sort_mode, -1, 0);
    CHECK_INT(album_artist_albums_sort_mode, 3, 0);
    CHECK_INT(genre_view_mode, -1, 0);
    CHECK_INT(genre_view_mode, 4, 0);
    return true;
}

#undef CHECK_INT

static bool check_strings_and_numeric_precision(const player_settings_t *defaults) {
    player_settings_t input = *defaults;
    player_settings_t output;
    snprintf(input.last_track, sizeof(input.last_track), "%s", "/sd/Música_日本/長い曲.flac");
    snprintf(input.last_source_name, sizeof(input.last_source_name), "%s", "Björk — 東京");
    snprintf(input.subsonic_url, sizeof(input.subsonic_url), "%s", "https://example.invalid/music");
    snprintf(input.subsonic_username, sizeof(input.subsonic_username), "%s", "qa-user-é");
    snprintf(input.subsonic_password, sizeof(input.subsonic_password), "%s", "synthetic-only-never-log");
    snprintf(input.remote_control_pin, sizeof(input.remote_control_pin), "%s", "012345678901");
    snprintf(input.timezone, sizeof(input.timezone), "%s", "America/Costa_Rica");
    snprintf(input.hostname, sizeof(input.hostname), "%s", "qa-player-ñ");
    snprintf(input.custom_font, sizeof(input.custom_font), "%s", "Fuente-日本.ttf");
    snprintf(input.player_layout, sizeof(input.player_layout), "%s", "compact_日本");
    snprintf(input.language, sizeof(input.language), "%s", "zh-Hant");
    snprintf(input.bt_codec, sizeof(input.bt_codec), "%s", "ldac_hq");
    snprintf(input.bt_last_output_mac, sizeof(input.bt_last_output_mac), "%s", "02:00:00:00:00:01");
    input.volume = 0.1236f;
    input.last_position = 123.4567;
    input.clock_manual_epoch = INT64_C(1700000000123);
    input.clock_system_reference = INT64_C(1800000000456);
    input.bt_sample_rate = 96000;
    input.bt_device_rate_count = 1;
    snprintf(input.bt_device_rates[0].mac, sizeof(input.bt_device_rates[0].mac), "%s", "02:00:00:00:00:02");
    input.bt_device_rates[0].rate = 88200;
    input.subsonic_saved_count = 1;
    snprintf(input.subsonic_saved[0].url, sizeof(input.subsonic_saved[0].url), "%s", "https://saved.invalid");
    snprintf(input.subsonic_saved[0].username, sizeof(input.subsonic_saved[0].username), "%s", "saved-qa-user");
    snprintf(input.subsonic_saved[0].password, sizeof(input.subsonic_saved[0].password), "%s", "synthetic-saved-only");
    input.subsonic_saved[0].verify_tls = false;
    input.ui_customization.music.count = 3;
    input.ui_customization.music.ids[0] = 5;
    input.ui_customization.music.ids[1] = 1;
    input.ui_customization.music.ids[2] = 6;
    input.ui_customization.drawer.count = 2;
    input.ui_customization.drawer.ids[0] = 9;
    input.ui_customization.drawer.ids[1] = 2;
    input.ui_customization.music_toolbar.count = 1;
    input.ui_customization.music_toolbar.ids[0] = 2;
    snprintf(input.setup_plugin_ids[SETTINGS_SETUP_PLUGIN_MAX - 1],
             sizeof(input.setup_plugin_ids[0]), "%s", "qa.plugin_2");
    snprintf(input.setup_layout_plugin_id, sizeof(input.setup_layout_plugin_id), "%s", "layout.qa-2");
    if (!round_trip(&input, &output)) return false;

#define CHECK_FIELD(condition, name) do { \
    if (condition) { fprintf(stderr, "settings round trip failed: %s\n", name); return false; } \
} while (0)
    CHECK_FIELD(strcmp(output.last_track, input.last_track), "last_track");
    CHECK_FIELD(strcmp(output.last_source_name, input.last_source_name), "last_source_name");
    CHECK_FIELD(strcmp(output.subsonic_url, input.subsonic_url), "subsonic_url");
    CHECK_FIELD(strcmp(output.subsonic_username, input.subsonic_username), "subsonic_username");
    CHECK_FIELD(strcmp(output.subsonic_password, input.subsonic_password), "subsonic_password");
    CHECK_FIELD(strcmp(output.remote_control_pin, input.remote_control_pin), "remote_control_pin");
    CHECK_FIELD(strcmp(output.timezone, input.timezone), "timezone");
    CHECK_FIELD(strcmp(output.hostname, input.hostname), "hostname");
    CHECK_FIELD(strcmp(output.custom_font, input.custom_font), "custom_font");
    CHECK_FIELD(strcmp(output.player_layout, input.player_layout), "player_layout");
    CHECK_FIELD(strcmp(output.language, input.language), "language");
    CHECK_FIELD(strcmp(output.bt_codec, input.bt_codec), "bt_codec");
    CHECK_FIELD(strcmp(output.bt_last_output_mac, input.bt_last_output_mac), "bt_last_output_mac");
    CHECK_FIELD(output.volume != 0.124f, "volume (three decimal places)");
    CHECK_FIELD(output.last_position != 123.457, "last_position (three decimal places)");
    CHECK_FIELD(output.clock_manual_epoch != input.clock_manual_epoch, "clock_manual_epoch");
    CHECK_FIELD(output.clock_system_reference != input.clock_system_reference, "clock_system_reference");
    CHECK_FIELD(output.bt_sample_rate != input.bt_sample_rate, "bt_sample_rate");
    CHECK_FIELD(output.bt_device_rate_count != 1, "bt_device_rate_count");
    CHECK_FIELD(strcmp(output.bt_device_rates[0].mac, input.bt_device_rates[0].mac), "bt_device_rates.mac");
    CHECK_FIELD(output.bt_device_rates[0].rate != input.bt_device_rates[0].rate, "bt_device_rates.rate");
    CHECK_FIELD(output.subsonic_saved_count != 1, "subsonic_saved_count");
    CHECK_FIELD(strcmp(output.subsonic_saved[0].url, input.subsonic_saved[0].url), "subsonic_saved.url");
    CHECK_FIELD(strcmp(output.subsonic_saved[0].username, input.subsonic_saved[0].username), "subsonic_saved.username");
    CHECK_FIELD(strcmp(output.subsonic_saved[0].password, input.subsonic_saved[0].password), "subsonic_saved.password");
    CHECK_FIELD(output.subsonic_saved[0].verify_tls, "subsonic_saved.verify_tls");
    CHECK_FIELD(output.ui_customization.music.count != 3 || output.ui_customization.music.ids[0] != 5 ||
                output.ui_customization.music.ids[1] != 1 || output.ui_customization.music.ids[2] != 6,
                "ui_customization.music order");
    CHECK_FIELD(output.ui_customization.drawer.count != 2 || output.ui_customization.drawer.ids[0] != 9 ||
                output.ui_customization.drawer.ids[1] != 2, "ui_customization.drawer order");
    CHECK_FIELD(output.ui_customization.music_toolbar.count != UI_MUSIC_TOOLBAR_COUNT ||
                output.ui_customization.music_toolbar.ids[0] != 0, "derived toolbar order");
    CHECK_FIELD(strcmp(output.setup_plugin_ids[SETTINGS_SETUP_PLUGIN_MAX - 1], "qa.plugin_2"),
                "setup_plugin_id last slot");
    CHECK_FIELD(strcmp(output.setup_layout_plugin_id, "layout.qa-2"), "setup_layout_plugin_id");
#undef CHECK_FIELD

    /* Boundary-sized values exercise the public fixed-buffer and path rules. */
    memset(input.last_track, 'x', sizeof(input.last_track) - 1);
    input.last_track[0] = '/';
    input.last_track[sizeof(input.last_track) - 1] = '\0';
    input.subsonic_download_subfolder[0] = 'a';
    memset(input.subsonic_download_subfolder + 1, 'b', 254);
    input.subsonic_download_subfolder[255] = '\0';
    input.startup_volume_fixed_percent = 0;
    input.car_mode_volume_percent = 100;
    input.max_volume_percent = 1;
    input.brightness_percent = 0;
    input.accent_color = 0xFF123456u;
    input.topbar_hidden_elements = TOPBAR_HIDE_ALL | (1u << 20);
    if (!round_trip(&input, &output) || strcmp(output.last_track, input.last_track) ||
        strcmp(output.subsonic_download_subfolder, input.subsonic_download_subfolder) ||
        output.startup_volume_fixed_percent != 0 || output.car_mode_volume_percent != 100 ||
        output.max_volume_percent != 1 || output.brightness_percent != 0 ||
        output.accent_color != 0x00123456u || output.topbar_hidden_elements != TOPBAR_HIDE_ALL) {
        fputs("settings boundary normalization failed\n", stderr);
        return false;
    }

    input = *defaults;
#define FILL_MAX(field, character) do { \
    memset(input.field, (character), sizeof(input.field) - 1); \
    input.field[sizeof(input.field) - 1] = '\0'; \
} while (0)
    FILL_MAX(subsonic_url, 'u');
    FILL_MAX(subsonic_username, 'n');
    FILL_MAX(subsonic_password, 'p');
    FILL_MAX(last_source_name, 's');
    FILL_MAX(timezone, 't');
    FILL_MAX(hostname, 'h');
    FILL_MAX(custom_font, 'f');
    FILL_MAX(player_layout, 'l');
    FILL_MAX(bt_codec, 'c');
#undef FILL_MAX
    memset(input.language, 'a', sizeof(input.language) - 1);
    input.language[sizeof(input.language) - 1] = '\0';
    memset(input.remote_control_pin, '7', REMOTE_CONTROL_PIN_MAX_LENGTH);
    input.remote_control_pin[REMOTE_CONTROL_PIN_MAX_LENGTH] = '\0';
    input.subsonic_saved_count = 1;
    memset(input.subsonic_saved[0].url, 'v', sizeof(input.subsonic_saved[0].url) - 1);
    input.subsonic_saved[0].url[sizeof(input.subsonic_saved[0].url) - 1] = '\0';
    memset(input.subsonic_saved[0].username, 'w', sizeof(input.subsonic_saved[0].username) - 1);
    input.subsonic_saved[0].username[sizeof(input.subsonic_saved[0].username) - 1] = '\0';
    memset(input.subsonic_saved[0].password, 'z', sizeof(input.subsonic_saved[0].password) - 1);
    input.subsonic_saved[0].password[sizeof(input.subsonic_saved[0].password) - 1] = '\0';
    memset(input.setup_plugin_ids[SETTINGS_SETUP_PLUGIN_MAX - 1], 'q', SETTINGS_SETUP_PLUGIN_ID_MAX - 1);
    input.setup_plugin_ids[SETTINGS_SETUP_PLUGIN_MAX - 1][SETTINGS_SETUP_PLUGIN_ID_MAX - 1] = '\0';
    memset(input.setup_layout_plugin_id, 'r', SETTINGS_SETUP_PLUGIN_ID_MAX - 1);
    input.setup_layout_plugin_id[SETTINGS_SETUP_PLUGIN_ID_MAX - 1] = '\0';
    if (!round_trip(&input, &output) ||
        strcmp(output.subsonic_url, input.subsonic_url) ||
        strcmp(output.subsonic_username, input.subsonic_username) ||
        strcmp(output.subsonic_password, input.subsonic_password) ||
        strcmp(output.last_source_name, input.last_source_name) ||
        strcmp(output.timezone, input.timezone) || strcmp(output.hostname, input.hostname) ||
        strcmp(output.custom_font, input.custom_font) || strcmp(output.player_layout, input.player_layout) ||
        strcmp(output.bt_codec, input.bt_codec) || strcmp(output.language, input.language) ||
        strcmp(output.remote_control_pin, input.remote_control_pin) ||
        output.subsonic_saved_count != 1 ||
        strcmp(output.subsonic_saved[0].url, input.subsonic_saved[0].url) ||
        strcmp(output.subsonic_saved[0].username, input.subsonic_saved[0].username) ||
        strcmp(output.subsonic_saved[0].password, input.subsonic_saved[0].password) ||
        strcmp(output.setup_plugin_ids[SETTINGS_SETUP_PLUGIN_MAX - 1],
               input.setup_plugin_ids[SETTINGS_SETUP_PLUGIN_MAX - 1]) ||
        strcmp(output.setup_layout_plugin_id, input.setup_layout_plugin_id) ||
        strlen(output.subsonic_url) != sizeof(input.subsonic_url) - 1 ||
        strlen(output.subsonic_username) != sizeof(input.subsonic_username) - 1 ||
        strlen(output.subsonic_password) != sizeof(input.subsonic_password) - 1 ||
        strlen(output.last_source_name) != sizeof(input.last_source_name) - 1 ||
        strlen(output.timezone) != sizeof(input.timezone) - 1 ||
        strlen(output.hostname) != sizeof(input.hostname) - 1 ||
        strlen(output.custom_font) != sizeof(input.custom_font) - 1 ||
        strlen(output.player_layout) != sizeof(input.player_layout) - 1 ||
        strlen(output.bt_codec) != sizeof(input.bt_codec) - 1 ||
        strlen(output.language) != sizeof(input.language) - 1 ||
        strlen(output.remote_control_pin) != REMOTE_CONTROL_PIN_MAX_LENGTH ||
        strlen(output.subsonic_saved[0].url) != sizeof(input.subsonic_saved[0].url) - 1 ||
        strlen(output.subsonic_saved[0].username) != sizeof(input.subsonic_saved[0].username) - 1 ||
        strlen(output.subsonic_saved[0].password) != sizeof(input.subsonic_saved[0].password) - 1 ||
        strlen(output.setup_plugin_ids[SETTINGS_SETUP_PLUGIN_MAX - 1]) != SETTINGS_SETUP_PLUGIN_ID_MAX - 1 ||
        strlen(output.setup_layout_plugin_id) != SETTINGS_SETUP_PLUGIN_ID_MAX - 1) {
        fputs("maximum-length settings strings did not round trip\n", stderr);
        return false;
    }

    /* The UI toolbar order is runtime-derived from defaults and not serialized. */
    if (output.ui_customization.music_toolbar.count != UI_MUSIC_TOOLBAR_COUNT ||
        output.ui_customization.music_toolbar.ids[0] != 0 ||
        output.ui_customization.music_toolbar.ids[1] != 1 ||
        output.ui_customization.music_toolbar.ids[2] != 2) {
        fputs("derived toolbar order did not reset to defaults\n", stderr);
        return false;
    }
    return true;
}

static bool check_invalid_strings(const player_settings_t *defaults) {
    player_settings_t input = *defaults;
    player_settings_t output;
    snprintf(input.custom_font, sizeof(input.custom_font), "%s", "../unsafe.ttf");
    snprintf(input.player_layout, sizeof(input.player_layout), "%s", "folder/layout");
    snprintf(input.language, sizeof(input.language), "%s", "bad!");
    snprintf(input.setup_plugin_ids[0], sizeof(input.setup_plugin_ids[0]), "%s", "Bad.Plugin");
    snprintf(input.setup_layout_plugin_id, sizeof(input.setup_layout_plugin_id), "%s", "../layout");
    snprintf(input.subsonic_download_subfolder, sizeof(input.subsonic_download_subfolder), "%s", "../private");
    snprintf(input.remote_control_pin, sizeof(input.remote_control_pin), "%s", "123");
    if (!round_trip(&input, &output) || output.custom_font[0] || output.player_layout[0] ||
        strcmp(output.language, "en") || output.setup_plugin_ids[0][0] ||
        output.setup_layout_plugin_id[0] || output.subsonic_download_subfolder[0] ||
        output.remote_control_pin[0]) {
        fputs("settings invalid-string normalization failed\n", stderr);
        return false;
    }
    return true;
}

static bool check_legacy_migrations(const player_settings_t *defaults) {
    player_settings_t output;
    (void)defaults;
    if (!replace_raw_settings("play_pause_button_mode=2\nreplaygain_enabled=0\n") ||
        !settings_load(&output) || output.play_pause_button_mode != 2 ||
        output.replaygain_mode != 0 ||
        output.button_bindings[BUTTON_MAPPING_PLAY_PAUSE].tap != BUTTON_MAPPING_ACTION_PLAY_PAUSE ||
        output.button_bindings[BUTTON_MAPPING_PLAY_PAUSE].double_tap != BUTTON_MAPPING_ACTION_PREVIOUS) {
        fputs("legacy settings migration failed\n", stderr);
        return false;
    }
    if (!replace_raw_settings("play_pause_button_mode=1\nreplaygain_enabled=1\n"
                              "button_binding_play_pause_tap=0\n") ||
        !settings_load(&output) || output.replaygain_mode != 1 ||
        output.button_bindings[BUTTON_MAPPING_PLAY_PAUSE].tap != BUTTON_MAPPING_ACTION_DEFAULT ||
        output.button_bindings[BUTTON_MAPPING_PLAY_PAUSE].double_tap != BUTTON_MAPPING_ACTION_DEFAULT) {
        fputs("legacy setting migration precedence failed\n", stderr);
        return false;
    }
    return true;
}

static bool check_empty_strings(const player_settings_t *defaults) {
    player_settings_t input = *defaults;
    player_settings_t output;
    input.ui_customization.music.count = 0;
    input.ui_customization.drawer.count = 0;
    if (!round_trip(&input, &output) || output.last_track[0] || output.last_source_name[0] ||
        output.subsonic_url[0] || output.subsonic_username[0] || output.subsonic_password[0] ||
        output.subsonic_download_subfolder[0] || output.remote_control_pin[0] ||
        output.bt_last_output_mac[0] || output.timezone[0] || output.hostname[0] ||
        output.custom_font[0] || output.player_layout[0] || output.setup_plugin_ids[0][0] ||
        output.setup_layout_plugin_id[0] || strcmp(output.language, "en") ||
        output.ui_customization.music.count != 0 || output.ui_customization.drawer.count != 0) {
        fputs("empty string/order settings did not round trip\n", stderr);
        return false;
    }
    return true;
}

static bool check_unpersisted_runtime_state(const player_settings_t *defaults) {
    player_settings_t input = *defaults;
    player_settings_t output;
    input.bt_dac_mode_enabled = true;
    input.ui_customization.music_toolbar.count = 1;
    input.ui_customization.music_toolbar.ids[0] = 2;
    if (!round_trip(&input, &output) || output.bt_dac_mode_enabled ||
        output.ui_customization.music_toolbar.count != UI_MUSIC_TOOLBAR_COUNT ||
        output.ui_customization.music_toolbar.ids[0] != 0 ||
        output.ui_customization.music_toolbar.ids[1] != 1 ||
        output.ui_customization.music_toolbar.ids[2] != 2) {
        fputs("runtime-only or derived settings were not reset on load\n", stderr);
        return false;
    }
    return true;
}

static bool check_bt_device_rate_roundtrips(const player_settings_t *defaults) {
    static const unsigned int rates[] = { 0, 44100, 88200, 192000 };
    player_settings_t input = *defaults;
    player_settings_t output;
    input.bt_device_rate_count = (int)(sizeof(rates) / sizeof(rates[0]));
    for (int i = 0; i < input.bt_device_rate_count; ++i) {
        snprintf(input.bt_device_rates[i].mac, sizeof(input.bt_device_rates[i].mac),
                 "02:00:00:00:00:%02X", i + 1);
        input.bt_device_rates[i].rate = rates[i];
    }
    if (!round_trip(&input, &output) || output.bt_device_rate_count != input.bt_device_rate_count) {
        fputs("valid Bluetooth device rate entries did not round trip\n", stderr);
        return false;
    }
    for (int i = 0; i < input.bt_device_rate_count; ++i) {
        if (strcmp(output.bt_device_rates[i].mac, input.bt_device_rates[i].mac) != 0 ||
            output.bt_device_rates[i].rate != rates[i]) {
            fputs("valid Bluetooth device rate value did not round trip\n", stderr);
            return false;
        }
    }

    /* A ninth valid row is ignored once the public fixed-capacity setting is full. */
    input = *defaults;
    input.bt_device_rate_count = BT_DEVICE_RATE_MAX;
    for (int i = 0; i < BT_DEVICE_RATE_MAX; ++i) {
        snprintf(input.bt_device_rates[i].mac, sizeof(input.bt_device_rates[i].mac),
                 "02:00:00:00:01:%02X", i);
        input.bt_device_rates[i].rate = 44100;
    }
    if (!round_trip(&input, &output) || output.bt_device_rate_count != BT_DEVICE_RATE_MAX ||
        !append_raw_settings_lines("bt_device_rate=02:00:00:00:02:00,88200\n") ||
        !settings_load(&output) || output.bt_device_rate_count != BT_DEVICE_RATE_MAX) {
        fputs("Bluetooth device rate capacity handling failed\n", stderr);
        return false;
    }

    /* Parse a fixture with malformed separators, MAC text, rate syntax and bounds. */
    input = *defaults;
    settings_save(&input);
    const char *malformed[] = {
        "bt_device_rate=02-00-00-00-00-01,44100\n",
        "bt_device_rate=02:00:00:00:GG:02,44100\n",
        "bt_device_rate=02:00:00:00:00:03,192001\n",
        "bt_device_rate=02:00:00:00:00:04,-1\n",
        "bt_device_rate=02:00:00:00:00:05,44100junk\n",
        "bt_device_rate=02:00:00:00:00:06\n",
        "bt_device_rate=0200:00:00:00:07,44100\n"
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        settings_save(defaults);
        if (!append_raw_settings_lines(malformed[i])) {
            fputs("could not append malformed Bluetooth fixture\n", stderr);
            return false;
        }
        if (!settings_load(&output)) {
            fputs("could not reload malformed Bluetooth fixture\n", stderr);
            return false;
        }
        if (output.bt_device_rate_count != 0) {
            fprintf(stderr, "malformed Bluetooth device rate row %zu was accepted\n", i);
            return false;
        }
    }
    return true;
}

int main(void) {
    /* The runner reuses its private cache directory, so clear only this test's
     * host fixture before asking settings_load() for defaults. */
    remove(".compas/settings.txt");
    remove(".compas/settings.txt.tmp");
    player_settings_t defaults;
    (void)settings_load(&defaults); /* Missing fixture file returns defaults. */
    assert(defaults.last_track[0] == '\0' && defaults.last_source_name[0] == '\0');
    assert(defaults.subsonic_url[0] == '\0' && defaults.subsonic_username[0] == '\0' &&
           defaults.subsonic_password[0] == '\0' && defaults.remote_control_pin[0] == '\0');
    assert(defaults.timezone[0] == '\0' && defaults.hostname[0] == '\0' &&
           defaults.custom_font[0] == '\0' && defaults.player_layout[0] == '\0');
    assert(check_bool_roundtrips(&defaults));
    assert(check_enums_and_presets(&defaults));
    assert(check_invalid_values(&defaults));
    for (int i = 0; i <= 1; ++i) {
        player_settings_t input = defaults;
        player_settings_t output;
        input.volume = i ? 1.0f : 0.0f;
        assert(round_trip(&input, &output));
        assert(output.volume == (i ? 1.0f : 0.0f));
    }
    assert(check_strings_and_numeric_precision(&defaults));
    assert(check_invalid_strings(&defaults));
    assert(check_legacy_migrations(&defaults));
    assert(check_empty_strings(&defaults));
    assert(check_bt_device_rate_roundtrips(&defaults));
    assert(check_unpersisted_runtime_state(&defaults));
    printf("settings round-trip coverage passed (%zu persisted bool fields plus saved-profile TLS)\n",
           sizeof(persisted_booleans) / sizeof(persisted_booleans[0]));
    return 0;
}
