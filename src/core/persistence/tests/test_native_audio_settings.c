/* Real native persistence/parser and DSP configuration APIs, with only the
 * hardware, plugin storage and settings disk coordinator replaced. */
#include "src/core/persistence/native_audio_settings.h"
#include "src/core/persistence/settings.h"
#include "src/audio/engine/audio.h"
#include "src/audio/effects/peq.h"
#include "src/plugins/storage/plugin_storage.h"
#include "src/plugins/lifecycle/plugin_disabled_list.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

player_settings_t current_settings;
static player_settings_t saved_settings;
static int saves, async_saves, staged, applied;
static uint8_t staged_curve[HW_VOLUME_CURVE_LEN], applied_curve[HW_VOLUME_CURVE_LEN];
static const char * stage_storage;
static const char * disabled_plugin;
bool plugin_disabled_list_contains(const char * file) { return disabled_plugin && !strcmp(file, disabled_plugin); }
static const char * preset_storage;
static bool peq_load_ok = true;
static int peq_loads, peq_saves;
static bool live_mono;
static int mono_sets;
static int live_manual_eq;
static double crossfeed = .37;
static int crossfeed_angle = 45;

void settings_save(const player_settings_t * s) { saved_settings = *s; saves++; }
void settings_save_async(const player_settings_t * s) { saved_settings = *s; async_saves++; }
void audio_stage_custom_hw_volume_curve(bool active, const uint8_t * curve) {
    assert(active); staged++; memcpy(staged_curve, curve, sizeof(staged_curve));
}
void audio_set_custom_hw_volume_curve(const uint8_t * curve) {
    applied++; memcpy(applied_curve, curve, sizeof(applied_curve));
}
bool plugin_storage_get(const char * id, const char * key, char ** value, size_t * len) {
    assert(!strcmp(id, "compas.soundstage"));
    const char * source = !strcmp(key, "state") ? stage_storage : preset_storage;
    if (!source) return false;
    *value = strdup(source); *len = strlen(source); return true;
}
double peq_get_crossfeed(void) { return crossfeed; }
int peq_get_crossfeed_angle(void) { return crossfeed_angle; }
void peq_set_crossfeed(double value) { crossfeed = value; }
void peq_set_crossfeed_angle(int value) { crossfeed_angle = value; }
void peq_set_mono(bool enabled) { live_mono = enabled; mono_sets++; }
bool peq_load_from_path(const char * path) {
    assert(strstr(path, "/.mseb_pre_backup.peq") || strstr(path, "/.mseb_active.peq")); peq_loads++;
    if (!peq_load_ok || access(path, F_OK)) return false;
    crossfeed = .9; crossfeed_angle = 75;
    live_manual_eq = strstr(path, "_active.peq") ? 200 : 100;
    return true;
}
void peq_save(void) { peq_saves++; }

static const char * modern =
    "curve=new;enabled=1;range=40;sound_stage=37;temperature=-17;bass_ext=22;"
    "bass_texture=-33;thickness=44;vocal_pos=-55;female_vocal=66;"
    "sibilance_lf=-77;sibilance_hf=88;impulse_tone=-99;air=100;";
static const char * legacy =
    "enabled=1\nrange=20\nsound_stage=-25\ntemperature=12\nbass_ext=13\n"
    "bass_texture=14\nthickness=15\nvocal_pos=16\nfemale_vocal=17\n"
    "instruments=83\nbass_bite=-72\ntreble_bite=61\nair=18\nfuture_control=39\n";

static void write_text(const char * path, const char * text) {
    FILE * f = fopen(path, "w"); assert(f); assert(fputs(text, f) >= 0); assert(!fclose(f));
}
static void scenario(const char * name) {
    assert(!mkdir(name, 0700)); assert(!chdir(name));
    assert(!mkdir("music", 0700)); assert(!mkdir("music/.plugins", 0700)); assert(!mkdir(".compas", 0700));
    const char * plugins[] = {"GainMode", "MSEB", "Soundstage", "PlayThrough"};
    for (int i = 0; i < 4; ++i) {
        char path[64]; snprintf(path, sizeof(path), "music/.plugins/%s.lua", plugins[i]); write_text(path, "-- old copy");
    }
    disabled_plugin = NULL;
    memset(&current_settings, 0, sizeof(current_settings));
    memset(&saved_settings, 0, sizeof(saved_settings));
    current_settings.native_gain_mode = -1;
    saves = async_saves = staged = applied = peq_loads = peq_saves = 0;
    live_mono = false; mono_sets = 0;
    stage_storage = preset_storage = NULL;
    peq_load_ok = true; crossfeed = .37; crossfeed_angle = 45; live_manual_eq = 7;
}
static void done(void) { assert(!chdir("..")); }
static void close_to(double a, double b) { assert(fabs(a - b) < .00001); }
static void mono_restore(void) {
    for (int enabled = 0; enabled < 2; ++enabled) {
        scenario(enabled ? "mono-on" : "mono-off");
        current_settings.mono_audio = enabled;
        live_mono = !enabled;
        native_audio_init();
        assert(mono_sets == 1 && live_mono == !!enabled);
        assert(current_settings.mono_audio == !!enabled && saved_settings.mono_audio == !!enabled);
        current_settings = saved_settings;
        live_mono = !enabled;
        native_audio_init();
        assert(mono_sets == 2 && live_mono == !!enabled);
        live_mono = !enabled;
        native_audio_restore();
        assert(mono_sets == 3 && live_mono == !!enabled);
        assert(current_settings.mono_audio == !!enabled);
        done();
    }
}
static void check_modern(const native_mseb_state_t * state) {
    const int expected[] = {-17,22,-33,44,-55,66,-77,88,-99,100};
    assert(!state->legacy && state->config.enabled && state->range == 40);
    close_to(state->config.width, 1.37);
    for (int i = 0; i < MSEB_CONTROL_COUNT; ++i) assert(state->config.values[i] == expected[i]);
}
static void gain_migrations(void) {
    const char * states[] = {NULL, "low\n", "high\n"};
    const char * names[] = {"gain-absent", "gain-low", "gain-high"};
    for (int i = 0; i < 3; ++i) {
        scenario(names[i]);
        if (states[i]) write_text("music/.plugins/.gain_mode_state", states[i]);
        native_audio_init();
        assert(native_audio_gain_mode() == i - 1);
        assert(current_settings.native_audio_migrated && saved_settings.native_audio_migrated && saves == 1);
        assert(staged == (i > 0));
        if (i) { assert(staged_curve[0] == 255); assert(staged_curve[1] == (i == 1 ? 188 : 176)); assert(staged_curve[100] == 0); }
        write_text("music/.plugins/.gain_mode_state", i == 2 ? "low" : "high");
        current_settings = saved_settings; /* The persisted latch is loaded on the next startup. */
        native_audio_init();
        assert(native_audio_gain_mode() == i - 1);
        native_audio_set_gain_mode(0); assert(applied_curve[1] == 188 && native_audio_gain_mode() == 0);
        native_audio_set_gain_mode(1); assert(applied_curve[1] == 176 && native_audio_gain_mode() == 1);
        assert(applied == 2 && async_saves == 2);
        done();
    }
}
static void modern_migration(void) {
    scenario("modern");
    write_text("music/.plugins/.mseb_state", modern);
    write_text("music/.plugins/.play_through_state", "folders=1\nalbums=0\n");
    stage_storage = "enabled=1;speaker_angle_deg=48;binaural_amount=0.7;side_db=2;side_low_db=-3;side_low_hz=180;side_high_db=1;side_high_hz=7200;reflections=0.4;room_size=0.6;damping=0.8";
    preset_storage = "6";
    native_audio_init();
    native_mseb_state_t state; native_audio_get_mseb(&state); check_modern(&state);
    mseb_config_t actual; mseb_get_config(&actual); assert(actual.enabled); close_to(actual.width, 1.37);
    assert(current_settings.native_play_through_folders && !current_settings.native_play_through_albums);
    soundstage_config_t stage; int preset;
    native_audio_get_soundstage(&stage, &preset);
    assert(stage.enabled && preset == 6); close_to(stage.speaker_angle_deg, 48); close_to(stage.binaural_amount, .7);
    close_to(stage.side_db, 2); close_to(stage.side_low_db, -3); close_to(stage.side_low_hz, 180);
    close_to(stage.side_high_db, 1); close_to(stage.side_high_hz, 7200); close_to(stage.reflections, .4);
    close_to(stage.room_size, .6); close_to(stage.damping, .8);
    soundstage_get_config(&stage); assert(stage.enabled); close_to(stage.speaker_angle_deg, 48);
    /* Simulate another plugin's transient overrides and owner restoration. */
    actual.enabled = false; actual.width = .2; assert(mseb_set_config(&actual));
    soundstage_defaults(&stage); assert(soundstage_set_config(&stage));
    native_audio_restore_mseb(); mseb_get_config(&actual); assert(actual.enabled); close_to(actual.width, 1.37);
    native_audio_restore_soundstage(); soundstage_get_config(&stage); assert(stage.enabled); close_to(stage.speaker_angle_deg, 48);
    write_text("music/.plugins/.mseb_state", "curve=new;enabled=0;temperature=99");
    write_text("music/.plugins/.play_through_state", "folders=0\nalbums=1\n");
    stage_storage = "enabled=0;speaker_angle_deg=10"; preset_storage = "1";
    current_settings = saved_settings; native_audio_init();
    native_audio_get_mseb(&state); check_modern(&state);
    assert(current_settings.native_play_through_folders && !current_settings.native_play_through_albums);
    native_audio_get_soundstage(&stage, &preset); assert(stage.enabled && preset == 6); close_to(stage.speaker_angle_deg, 48);
    done();
}
static void legacy_migration(void) {
    scenario("legacy"); write_text("music/.plugins/.mseb_state", legacy);
    write_text("music/.plugins/.mseb_active.peq", "legacy active profile");
    native_audio_init();
    assert(strstr(current_settings.native_mseb, "future_control=39"));
    assert(strstr(current_settings.native_mseb, "instruments=83"));
    native_mseb_state_t state; native_audio_get_mseb(&state);
    assert(state.legacy && state.range == 20 && state.config.values[0] == 12);
    assert(state.legacy_values[0] == 83 && state.legacy_values[1] == -72 && state.legacy_values[2] == 61);
    close_to(state.config.width, .75);
    mseb_config_t actual; mseb_get_config(&actual); assert(!actual.enabled);
    char raw[sizeof(current_settings.native_mseb)]; strcpy(raw, current_settings.native_mseb);
    assert(!native_audio_set_mseb(&state)); assert(!strcmp(raw, current_settings.native_mseb));
    write_text("music/.plugins/.mseb_state", modern); current_settings = saved_settings; native_audio_init();
    assert(!strcmp(raw, current_settings.native_mseb));
    done();
}
static void preset_migration(void) {
    scenario("presets");
    for (int slot = 1; slot <= 3; ++slot) {
        char path[80]; snprintf(path, sizeof(path), "music/.plugins/.mseb_slot%d", slot); write_text(path, modern);
    }
    char named[2048]; snprintf(named, sizeof(named), "%sformat=1;name_hex=4D7920707265736574;", modern);
    write_text("music/.plugins/.mseb_preset1", named);
    write_text("music/.plugins/.mseb_preset20", named);
    write_text("music/.plugins/.mseb_preset2", "format=1;curve=new;range=20;name_hex=426164;temperature=NaN;");
    write_text("music/.plugins/.mseb_preset3", "format=1;curve=new;range=20;name_hex=00;");
    native_audio_init();
    native_mseb_state_t state; char name[NATIVE_MSEB_NAME_MAX + 1];
    for (int slot = 1; slot <= 3; ++slot) {
        assert(native_mseb_preset_exists(slot, false)); assert(native_mseb_preset_read(slot, false, &state, name)); check_modern(&state);
    }
    assert(native_mseb_preset_read(1, true, &state, name)); assert(!strcmp(name, "My preset")); check_modern(&state);
    assert(native_mseb_preset_read(20, true, &state, name)); assert(!strcmp(name, "My preset"));
    assert(native_mseb_preset_exists(2, true)); assert(!native_mseb_preset_read(2, true, &state, name));
    assert(native_mseb_preset_exists(3, true)); assert(!native_mseb_preset_read(3, true, &state, name));
    native_audio_get_mseb(&state); state.range = 20; state.config.width = 1.62;
    assert(native_mseb_preset_write(20, true, &state, "Renamed preset"));
    assert(native_mseb_preset_read(20, true, &state, name)); assert(!strcmp(name, "Renamed preset")); close_to(state.config.width, 1.62);
    assert(!native_mseb_preset_write(21, true, &state, "Out of range"));
    assert(native_mseb_preset_delete(20)); assert(!native_mseb_preset_exists(20, true));
    /* Once migration is persisted, editing the old SD copy must not overwrite the native slot. */
    write_text("music/.plugins/.mseb_slot1", "curve=new;enabled=0;temperature=99");
    current_settings = saved_settings; native_audio_init();
    assert(native_mseb_preset_read(1, false, &state, name)); check_modern(&state);
    done();
}
static void preset_validation(void) {
    scenario("preset-validation"); native_audio_init();
    char fixture[2048], name[NATIVE_MSEB_NAME_MAX + 1];
    native_mseb_state_t state;
    snprintf(fixture, sizeof(fixture), "curve=legacy;%sformat=1;name_hex=4C6567616379;", legacy);
    for (char * p = fixture; *p; ++p) if (*p == '\n') *p = ';';
    write_text(".compas/mseb/preset1", fixture);
    assert(native_mseb_preset_read(1, true, &state, name));
    assert(state.legacy && !strcmp(name, "Legacy") && state.legacy_values[0] == 83);
    const char * bad_fields[] = {"format=2;name_hex=4E616D65;", "format=1;name_hex=00;",
                                "format=1;name_hex=7F;", "format=1;name_hex=GG;", "format=1;name_hex=1;"};
    for (size_t i = 0; i < sizeof(bad_fields) / sizeof(bad_fields[0]); ++i) {
        snprintf(fixture, sizeof(fixture), "%s%s", modern, bad_fields[i]);
        write_text(".compas/mseb/preset2", fixture);
        assert(!native_mseb_preset_read(2, true, &state, name));
    }
    const char * invalid_axes[] = {"NaN", "101", "1.5", "1junk"};
    for (size_t i = 0; i < sizeof(invalid_axes) / sizeof(invalid_axes[0]); ++i) {
        const char * after = strstr(modern, ";bass_ext="); assert(after);
        snprintf(fixture, sizeof(fixture), "curve=new;enabled=1;range=40;sound_stage=37;temperature=%s%sformat=1;name_hex=4E616D65;", invalid_axes[i], after);
        write_text(".compas/mseb/preset2", fixture);
        assert(!native_mseb_preset_read(2, true, &state, name));
    }
    done();
}
static void play_through_flags(void) {
    for (int bits = 0; bits < 4; ++bits) {
        char name[32], text[40]; snprintf(name, sizeof(name), "flags-%d", bits); scenario(name);
        snprintf(text, sizeof(text), "folders=%d\nalbums=%d\n", bits & 1, (bits >> 1) & 1);
        write_text("music/.plugins/.play_through_state", text); native_audio_init();
        assert(current_settings.native_play_through_folders == !!(bits & 1));
        assert(current_settings.native_play_through_albums == !!(bits & 2));
        done();
    }
}
static void retry_migration(void) {
    scenario("retry");
    write_text("music/.plugins/.gain_mode_state", "high"); write_text("music/.plugins/.mseb_slot1", modern);
    write_text(".compas/mseb", "blocks destination directory");
    native_audio_init(); assert(current_settings.native_audio_migrated && saves == 1 && native_audio_gain_mode() == 1);
    assert(!access("music/.plugins/.mseb_slot1", F_OK));
    assert(!unlink(".compas/mseb")); native_audio_init(); assert(saves == 1);
    native_mseb_state_t state; char name[65]; assert(native_mseb_preset_read(1, false, &state, name)); check_modern(&state);
    assert(access("music/.plugins/.mseb_slot1", F_OK) && errno == ENOENT);
    done();
}
static void unused_plugins_stay_off(void) {
    scenario("unused");
    assert(!unlink("music/.plugins/GainMode.lua")); assert(!unlink("music/.plugins/Soundstage.lua"));
    disabled_plugin = "MSEB.lua";
    write_text("music/.plugins/.gain_mode_state", "high"); write_text("music/.plugins/.mseb_state", modern);
    stage_storage = "enabled=1;speaker_angle_deg=48"; preset_storage = "9";
    native_audio_init();
    native_mseb_state_t state; native_audio_get_mseb(&state); assert(!state.config.enabled && state.range == 40);
    soundstage_config_t stage; int preset; native_audio_get_soundstage(&stage, &preset);
    assert(!stage.enabled && preset == 0); close_to(stage.speaker_angle_deg, 48);
    assert(native_audio_gain_mode() == -1 && !staged);
    assert(!strcmp(current_settings.ui_customization.drawer_order_text,
                   "!plugin:example.gain_mode:0,!plugin:example.mseb:0,!plugin:compas.soundstage:0"));
    done();
}
static void unused_legacy_and_saved_drawer(void) {
    for (int absent = 0; absent < 2; ++absent) {
        scenario(absent ? "legacy-absent" : "legacy-disabled");
        if (absent) assert(!unlink("music/.plugins/MSEB.lua")); else disabled_plugin = "MSEB.lua";
        write_text("music/.plugins/.mseb_state", legacy);
        write_text("music/.plugins/.mseb_active.peq", "must stay off");
        strcpy(current_settings.ui_customization.drawer_order_text, "wifi,plugin:example.mseb:0,plugin:example.gain_mode:0x,!plugin:compas.soundstage:0");
        native_audio_init();
        native_mseb_state_t state; native_audio_get_mseb(&state);
        assert(state.legacy && !state.config.enabled && strstr(current_settings.native_mseb, "future_control=39"));
        assert(!peq_loads && live_manual_eq == 7);
        assert(!strcmp(current_settings.ui_customization.drawer_order_text,
                       "wifi,!plugin:example.mseb:0,plugin:example.gain_mode:0x,!plugin:compas.soundstage:0"));
        done();
    }
}
static void waits_for_card(void) {
    scenario("no-card"); assert(!system("rm -rf music"));
    native_audio_init(); assert(!current_settings.native_audio_migrated && !saves);
    done();
}
static void backup_restore(void) {
    scenario("backup"); write_text("music/.plugins/.mseb_state", modern); write_text("music/.plugins/.mseb_pre_backup.peq", "backup");
    native_audio_init(); assert(peq_loads == 1 && peq_saves == 1); close_to(crossfeed, .37); assert(crossfeed_angle == 45);
    assert(access("music/.plugins/.mseb_pre_backup.peq", F_OK) && errno == ENOENT);
    mseb_config_t actual; mseb_get_config(&actual); assert(actual.enabled);
    done();
}
static void backup_failures_and_conversion(void) {
    scenario("backup-failure"); write_text("music/.plugins/.mseb_state", modern); write_text("music/.plugins/.mseb_pre_backup.peq", "backup");
    peq_load_ok = false; native_audio_init();
    mseb_config_t actual; mseb_get_config(&actual); assert(!actual.enabled);
    assert(!access("music/.plugins/.mseb_pre_backup.peq", F_OK) && !peq_saves);
    done();
    scenario("loudness-conflict"); write_text("music/.plugins/.mseb_state", modern);
    write_text("music/.plugins/.playback_extras_backup.peq", "other owner backup"); native_audio_init();
    mseb_get_config(&actual); assert(!actual.enabled);
    native_mseb_state_t state; native_audio_get_mseb(&state); state.config.enabled = true;
    assert(!native_audio_set_mseb(&state));
    done();
    scenario("legacy-conversion"); write_text("music/.plugins/.mseb_state", legacy);
    write_text("music/.plugins/.mseb_active.peq", "legacy active profile");
    write_text("music/.plugins/.mseb_pre_backup.peq", "backup"); native_audio_init();
    assert(peq_loads == 1 && !access("music/.plugins/.mseb_pre_backup.peq", F_OK));
    peq_load_ok = false; assert(!native_audio_convert_mseb(true));
    native_audio_get_mseb(&state); assert(state.legacy && !access("music/.plugins/.mseb_pre_backup.peq", F_OK));
    peq_load_ok = true; assert(native_audio_convert_mseb(true));
    native_audio_get_mseb(&state); assert(!state.legacy && state.config.enabled && state.range == 20);
    assert(state.config.values[0] == 12 && state.config.values[6] == 0 && state.config.values[7] == 0 && state.config.values[8] == 0);
    close_to(state.config.width, .75); close_to(crossfeed, .37); assert(crossfeed_angle == 45);
    assert(access("music/.plugins/.mseb_pre_backup.peq", F_OK) && errno == ENOENT);
    mseb_get_config(&actual); assert(actual.enabled);
    done();
}
static void conversion_conflict_does_not_touch_eq(void) {
    scenario("conversion-conflict");
    write_text("music/.plugins/.mseb_state", legacy);
    write_text("music/.plugins/.mseb_active.peq", "active legacy profile");
    write_text("music/.plugins/.mseb_pre_backup.peq", "old manual profile");
    native_audio_init(); assert(live_manual_eq == 200);
    write_text("music/.plugins/.playback_extras_backup.peq", "Loudness owns EQ");
    live_manual_eq = 777;
    int loads_before = peq_loads, saves_before = peq_saves;
    char raw[sizeof(current_settings.native_mseb)]; strcpy(raw, current_settings.native_mseb);
    assert(!native_audio_convert_mseb(true));
    assert(live_manual_eq == 777 && peq_loads == loads_before && peq_saves == saves_before);
    assert(!strcmp(raw, current_settings.native_mseb));
    assert(!access("music/.plugins/.mseb_pre_backup.peq", F_OK));
    done();
}
static void legacy_disable_retires_backup(void) {
    scenario("legacy-disable-edit-convert");
    write_text("music/.plugins/.mseb_state", legacy);
    write_text("music/.plugins/.mseb_active.peq", "active legacy profile");
    write_text("music/.plugins/.mseb_pre_backup.peq", "old manual profile");
    native_audio_init(); assert(live_manual_eq == 200);
    native_mseb_state_t state; native_audio_get_mseb(&state); state.config.enabled = false;
    assert(native_audio_set_mseb(&state)); assert(live_manual_eq == 100);
    assert(access("music/.plugins/.mseb_pre_backup.peq", F_OK) && errno == ENOENT);
    assert(!strstr(saved_settings.native_mseb, "enabled=1"));
    live_manual_eq = 777; /* User edits the restored manual EQ while MSEB is off. */
    int loads_before = peq_loads, saves_before = peq_saves;
    assert(native_audio_convert_mseb(true));
    assert(live_manual_eq == 777 && peq_loads == loads_before && peq_saves == saves_before);
    native_audio_get_mseb(&state); assert(!state.legacy && state.config.enabled);
    done();
}
static void startup_conflict_legacy_and_modern(void) {
    for (int kind = 0; kind < 2; ++kind) for (int backup = 0; backup < 2; ++backup) {
        char name[48]; snprintf(name, sizeof(name), "startup-conflict-%d-%d", kind, backup); scenario(name);
        write_text("music/.plugins/.mseb_state", kind ? legacy : modern);
        write_text("music/.plugins/.mseb_active.peq", "must not apply active profile during conflict");
        if (backup) write_text("music/.plugins/.mseb_pre_backup.peq", "manual profile");
        write_text("music/.plugins/.playback_extras_backup.peq", "Loudness owns EQ");
        live_manual_eq = 777; native_audio_init();
        native_mseb_state_t state; native_audio_get_mseb(&state); assert(!state.config.enabled);
        assert(peq_loads == backup && peq_saves == backup);
        assert(live_manual_eq == (backup ? 100 : 777));
        assert(access("music/.plugins/.mseb_pre_backup.peq", F_OK) && errno == ENOENT);
        done();
    }
}
static void startup_conflict_failed_backup(void) {
    scenario("startup-conflict-failed-backup");
    write_text("music/.plugins/.mseb_state", modern);
    write_text("music/.plugins/.mseb_pre_backup.peq", "temporarily unreadable backup");
    write_text("music/.plugins/.playback_extras_backup.peq", "Loudness owns EQ");
    peq_load_ok = false; live_manual_eq = 777; native_audio_init();
    native_mseb_state_t state; native_audio_get_mseb(&state); assert(!state.config.enabled);
    mseb_config_t actual; mseb_get_config(&actual); assert(!actual.enabled);
    assert(peq_loads == 1 && !peq_saves && live_manual_eq == 777);
    assert(!access("music/.plugins/.mseb_pre_backup.peq", F_OK));
    /* The disabled state is durable; restoring the retained backup retries. */
    current_settings = saved_settings; peq_load_ok = true; native_audio_init();
    native_audio_get_mseb(&state); assert(!state.config.enabled);
    assert(live_manual_eq == 100 && peq_saves == 1);
    assert(access("music/.plugins/.mseb_pre_backup.peq", F_OK) && errno == ENOENT);
    done();
}
int main(int argc, char ** argv) {
    assert(argc == 2 && !chdir(argv[1]));
    mono_restore();
    gain_migrations(); modern_migration(); legacy_migration(); preset_migration(); preset_validation(); play_through_flags(); retry_migration(); unused_plugins_stay_off(); unused_legacy_and_saved_drawer(); waits_for_card(); backup_restore(); backup_failures_and_conversion();
    conversion_conflict_does_not_touch_eq(); legacy_disable_retires_backup(); startup_conflict_legacy_and_modern(); startup_conflict_failed_backup();
    puts("Native audio migration, persistence latch/retry, presets, legacy retention and DSP restore passed");
    return 0;
}
