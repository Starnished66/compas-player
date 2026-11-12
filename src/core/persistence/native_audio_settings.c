#include "native_audio_settings.h"
#include "settings.h"
#include "storage_paths.h"
#include "../../audio/engine/audio.h"
#include "../../audio/effects/peq.h"
#include "../../plugins/storage/plugin_storage.h"
#include "../../plugins/lifecycle/plugin_disabled_list.h"
#include "../../library/browser/paths/library_paths.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The old plugins' EQ snapshots stay where they were: Loudness Boost checks them there. */
#define PLUGINS_DIR MUSIC_ROOT_DIR "/.plugins/"
#define MSEB_MANUAL_BACKUP PLUGINS_DIR ".mseb_pre_backup.peq"
#define MSEB_ACTIVE_PROFILE PLUGINS_DIR ".mseb_active.peq"
#define BOOST_BACKUP PLUGINS_DIR ".playback_extras_backup.peq"

static bool restore_manual_backup(void);

static const char * const axes[MSEB_CONTROL_COUNT] = {
    "temperature", "bass_ext", "bass_texture", "thickness", "vocal_pos",
    "female_vocal", "sibilance_lf", "sibilance_hf", "impulse_tone", "air"
};
static const char * const legacy_axes[] = {"instruments", "bass_bite", "treble_bite"};
static const uint8_t gain_curves[2][HW_VOLUME_CURVE_LEN] = {
    {255,188,178,168,158,154,150,146,142,138,136,134,132,130,128,126,124,122,120,118,
     116,114,112,110,108,106,104,102,100,98,96,94,92,90,88,86,84,82,80,78,76,74,
     72,70,68,67,66,65,64,63,62,61,60,59,58,57,56,55,54,53,52,51,50,49,48,47,
     46,45,44,43,42,41,40,39,38,37,36,35,34,33,32,31,30,29,28,27,26,25,24,23,
     22,21,20,19,18,17,16,15,14,13,0},
    {255,176,166,156,146,142,138,134,130,126,124,122,120,118,116,114,112,110,108,106,
     104,102,100,98,96,94,92,90,88,86,84,82,80,78,76,74,72,70,68,66,64,62,60,58,
     56,55,54,53,52,51,50,49,48,47,46,45,44,43,42,41,40,39,38,37,36,35,34,33,
     32,31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10,9,
     8,7,6,5,4,3,2,1,0}
};

static bool read_file(const char * path, char * out, size_t size) {
    FILE * f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(out, 1, size - 1, f);
    bool ok = !ferror(f) && feof(f);
    out[n] = 0;
    fclose(f);
    return ok;
}

static bool kv(const char * text, const char * key, char * out, size_t size) {
    size_t k = strlen(key);
    for (const char * p = text; p && *p;) {
        const char * end = strpbrk(p, ";\n\r");
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n > k && !strncmp(p, key, k) && p[k] == '=') {
            n -= k + 1;
            if (n >= size) return false;
            memcpy(out, p + k + 1, n); out[n] = 0;
            return true;
        }
        p = end ? end + 1 : NULL;
    }
    return false;
}

static double number(const char * text, const char * key, double fallback, double min, double max) {
    char value[80], * end;
    if (!kv(text, key, value, sizeof(value))) return fallback;
    double n = strtod(value, &end);
    if (end == value || *end || !isfinite(n)) return fallback;
    return fmax(min, fmin(max, n));
}

static void parse_mseb(const char * text, native_mseb_state_t * state) {
    *state = (native_mseb_state_t){ .config.width = 1, .range = 100 };
    char curve[32];
    state->legacy = text[0] && (!kv(text, "curve", curve, sizeof(curve)) || strcmp(curve, "new"));
    state->config.enabled = number(text, "enabled", 0, 0, 1) == 1;
    int range = (int)number(text, "range", 100, 20, 100);
    if (range == 20 || range == 40 || range == 100) state->range = range;
    state->config.width = 1 + round(number(text, "sound_stage", 0, -100, 100)) / 100;
    for (int i = 0; i < MSEB_CONTROL_COUNT; i++)
        state->config.values[i] = (int)round(number(text, axes[i], 0, -100, 100));
    for (int i = 0; i < 3; i++) state->legacy_values[i] = (int)round(number(text, legacy_axes[i], 0, -100, 100));
    if (state->legacy) for (int i = 6; i < 9; i++) state->config.values[i] = 0;
}

static void encode_mseb(const native_mseb_state_t * state, char * out, size_t size) {
    size_t used = (size_t)snprintf(out, size, "curve=%s;enabled=%d;range=%d;sound_stage=%d;",
        state->legacy ? "legacy" : "new", state->config.enabled, state->range,
        (int)round((state->config.width - 1) * 100));
    for (int i = 0; i < MSEB_CONTROL_COUNT && used < size; i++) {
        bool legacy = state->legacy && i >= 6 && i <= 8;
        used += (size_t)snprintf(out + used, size - used, "%s=%d;", legacy ? legacy_axes[i - 6] : axes[i],
                                legacy ? state->legacy_values[i - 6] : state->config.values[i]);
    }
}

void native_audio_soundstage_preset(soundstage_config_t * c, int preset) {
    soundstage_defaults(c);
    static const double values[6][10] = {
        {30,.6,0,0,150,0,5000,.15,.3,.6}, {30,1,0,0,150,0,5000,.25,.35,.5},
        {30,.8,0,0,150,0,5000,.5,.3,.6}, {35,.8,0,0,150,0,5000,.8,1,.4},
        {20,1,0,-6,150,0,5000,.1,.5,.5}, {50,1,3,0,150,2,5000,.2,.5,.5}
    };
    if (preset < 1 || preset > 6) return;
    for (size_t i = 0; i < soundstage_field_count; i++)
        *(double *)((char *)c + soundstage_fields[i].offset) = values[preset - 1][i];
}

void native_audio_get_mseb(native_mseb_state_t * state) { parse_mseb(current_settings.native_mseb, state); }
void native_audio_get_soundstage(soundstage_config_t * c, int * preset) {
    const char * text = current_settings.native_soundstage;
    native_audio_soundstage_preset(c, 1);
    c->enabled = number(text, "enabled", 0, 0, 1) == 1;
    double stored = number(text, "preset", 0, -1, 7);
    *preset = stored >= 0 && stored <= 6 ? (int)stored : 0; /* missing or unknown: Custom, like the plugin */
    for (size_t i = 0; i < soundstage_field_count; i++) {
        const soundstage_field_t * field = &soundstage_fields[i];
        double * v = (double *)((char *)c + field->offset);
        *v = number(text, field->name, *v, field->min, field->max);
    }
}

bool native_audio_set_mseb(const native_mseb_state_t * state) {
    if (state->range != 20 && state->range != 40 && state->range != 100) return false;
    if (state->legacy && state->config.enabled) return false;
    if (state->legacy && !restore_manual_backup()) return false;
    if (state->config.enabled && native_audio_boost_active()) return false;
    if (!mseb_set_config(&state->config)) return false;
    encode_mseb(state, current_settings.native_mseb, sizeof(current_settings.native_mseb));
    if (state->legacy) {
        settings_save(&current_settings);
        unlink(MSEB_MANUAL_BACKUP);
    } else settings_save_async(&current_settings);
    return true;
}

static void encode_soundstage(const soundstage_config_t * c, int preset) {
    char * out = current_settings.native_soundstage;
    size_t size = sizeof(current_settings.native_soundstage);
    size_t used = (size_t)snprintf(out, size, "enabled=%d;preset=%d;", c->enabled, preset);
    for (size_t i = 0; i < soundstage_field_count && used < size; i++) {
        double v = *(const double *)((const char *)c + soundstage_fields[i].offset);
        used += (size_t)snprintf(out + used, size - used, "%s=%.4f;", soundstage_fields[i].name, v);
    }
}

bool native_audio_set_soundstage(const soundstage_config_t * c, int preset) {
    if (preset < 0 || preset > 6 || !soundstage_set_config(c)) return false;
    encode_soundstage(c, preset);
    settings_save_async(&current_settings);
    return true;
}

bool native_audio_boost_active(void) { return access(BOOST_BACKUP, F_OK) == 0; }

int native_audio_gain_mode(void) { return current_settings.native_gain_mode; }
void native_audio_set_gain_mode(int high) {
    current_settings.native_gain_mode = !!high;
    audio_set_custom_hw_volume_curve(gain_curves[!!high]);
    settings_save_async(&current_settings);
}

static void preset_path(char * out, size_t size, int slot, bool named) {
    snprintf(out, size, INTERNAL_COMPAS_DIR "/mseb/%s%d", named ? "preset" : "slot", slot);
}

bool native_mseb_preset_exists(int slot, bool named) {
    char path[512]; preset_path(path, sizeof(path), slot, named);
    return access(path, F_OK) == 0;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool native_mseb_preset_read(int slot, bool named, native_mseb_state_t * state, char * name) {
    char path[512], text[2048], encoded[NATIVE_MSEB_NAME_MAX * 2 + 1];
    preset_path(path, sizeof(path), slot, named);
    if (!read_file(path, text, sizeof(text))) return false;
    parse_mseb(text, state);
    if (!named) return !state->legacy;
    char field[80];
    if (!kv(text, "format", field, sizeof(field)) || strcmp(field, "1")) return false;
    if (!kv(text, "curve", field, sizeof(field)) || (strcmp(field, "new") && strcmp(field, "legacy"))) return false;
    if (!kv(text, "range", field, sizeof(field)) || (strcmp(field, "20") && strcmp(field, "40") && strcmp(field, "100"))) return false;
    const char * legacy[] = {"instruments", "bass_bite", "treble_bite"};
    for (int i = -1; i < MSEB_CONTROL_COUNT; i++) {
        const char * key = i < 0 ? "sound_stage" : state->legacy && i >= 6 && i <= 8 ? legacy[i - 6] : axes[i];
        if (!kv(text, key, field, sizeof(field))) return false;
        char * end; double value = strtod(field, &end);
        if (end == field || *end || !isfinite(value) || value < -100 || value > 100 || value != round(value)) return false;
    }
    if (!kv(text, "name_hex", encoded, sizeof(encoded))) return false;
    size_t n = strlen(encoded);
    if (!n || n % 2 || n > NATIVE_MSEB_NAME_MAX * 2) return false;
    for (size_t i = 0; i < n; i += 2) {
        int a = hex_digit(encoded[i]), b = hex_digit(encoded[i + 1]);
        if (a < 0 || b < 0 || a * 16 + b < 32 || a * 16 + b == 127) return false;
        name[i / 2] = (char)(a * 16 + b);
    }
    name[n / 2] = 0;
    return true;
}

static bool write_file(const char * path, const char * text) {
    char temp[600]; snprintf(temp, sizeof(temp), "%s.tmp", path);
    FILE * f = fopen(temp, "w");
    if (!f) return false;
    bool ok = fputs(text, f) >= 0;
    if (ok) ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f)) ok = false;
    if (ok) ok = rename(temp, path) == 0;
    if (!ok) unlink(temp);
    return ok;
}

bool native_mseb_preset_write(int slot, bool named, const native_mseb_state_t * state, const char * name) {
    char path[512], text[2048], encoded[NATIVE_MSEB_NAME_MAX * 2 + 1];
    if (slot < 1 || slot > (named ? NATIVE_MSEB_PRESET_COUNT : 3)) return false;
    if (named && (!name || !*name || strlen(name) > NATIVE_MSEB_NAME_MAX)) return false;
    if (mkdir(INTERNAL_COMPAS_DIR "/mseb", 0700) && errno != EEXIST) return false;
    encode_mseb(state, text, sizeof(text));
    if (named) {
        size_t n = strlen(name);
        for (size_t i = 0; i < n; i++) snprintf(encoded + i * 2, 3, "%02X", (unsigned char)name[i]);
        size_t used = strlen(text);
        snprintf(text + used, sizeof(text) - used, "format=1;name_hex=%s;", encoded);
    }
    preset_path(path, sizeof(path), slot, named);
    return write_file(path, text);
}

bool native_mseb_preset_delete(int slot) {
    char path[512]; preset_path(path, sizeof(path), slot, true);
    return unlink(path) == 0;
}

void native_audio_restore_mseb(void) {
    native_mseb_state_t state; native_audio_get_mseb(&state);
    if (state.legacy) state.config.enabled = false;
    (void)mseb_set_config(&state.config);
}

void native_audio_restore_soundstage(void) {
    soundstage_config_t stage; int preset;
    native_audio_get_soundstage(&stage, &preset);
    (void)soundstage_set_config(&stage);
}

void native_audio_set_mono(bool enabled) {
    current_settings.mono_audio = enabled;
    peq_set_mono(enabled);
}

void native_audio_restore(void) {
    native_audio_restore_mseb();
    native_audio_restore_soundstage();
    native_audio_set_mono(current_settings.mono_audio);
}

static bool restore_manual_backup(void) {
    const char * path = MSEB_MANUAL_BACKUP;
    if (access(path, F_OK)) return true;
    double crossfeed = peq_get_crossfeed(); int angle = peq_get_crossfeed_angle();
    if (!peq_load_from_path(path)) return false;
    peq_set_crossfeed(crossfeed); peq_set_crossfeed_angle(angle); peq_save();
    return true;
}

bool native_audio_convert_mseb(bool enable) {
    native_mseb_state_t state; native_audio_get_mseb(&state);
    if ((enable || state.config.enabled) && native_audio_boost_active())
        return false;
    if (!restore_manual_backup()) return false;
    state.legacy = false;
    if (enable) state.config.enabled = true;
    if (!native_audio_set_mseb(&state)) return false;
    settings_save(&current_settings);
    unlink(MSEB_MANUAL_BACKUP);
    return true;
}

static bool plugin_was_on(const char * file) {
    char path[512]; snprintf(path, sizeof(path), PLUGINS_DIR "%s", file);
    return access(path, F_OK) == 0 && !plugin_disabled_list_contains(file);
}

/* A tile whose plugin was not in use starts hidden, so the drawer looks as it did before. */
static void hide_drawer_tile(const char * owner) {
    char key[96], * text = current_settings.ui_customization.drawer_order_text;
    size_t used = strlen(text), cap = sizeof(current_settings.ui_customization.drawer_order_text);
    size_t k = (size_t)snprintf(key, sizeof(key), "plugin:%s:0", owner);
    for (char * p = text; (p = strstr(p, key)); p += k) {
        bool whole = (p == text || p[-1] == ',' || p[-1] == '!') && (p[k] == ',' || !p[k]);
        if (!whole) continue;
        if (p > text && p[-1] == '!') return;
        if (used + 1 >= cap) return;
        memmove(p + 1, p, used - (size_t)(p - text) + 1);
        *p = '!';
        return;
    }
    snprintf(text + used, cap - used, "%s!%s", used ? "," : "", key);
}

/* Keeps every other field of a raw legacy MSEB state; only switches it off. */
static void legacy_mseb_off(char * text, size_t size) {
    char out[sizeof(current_settings.native_mseb)] = "";
    size_t used = 0;
    for (char * p = text; *p && used < sizeof(out);) {
        size_t n = strcspn(p, ";");
        if (n && strncmp(p, "enabled=", 8))
            used += (size_t)snprintf(out + used, sizeof(out) - used, "%.*s;", (int)n, p);
        p += n + (p[n] == ';');
    }
    snprintf(text, size, "%senabled=0;", out);
}

static void import_plugin_settings(void) {
    char text[2048];
    bool gain_on = plugin_was_on("GainMode.lua"), mseb_on = plugin_was_on("MSEB.lua");
    bool stage_on = plugin_was_on("Soundstage.lua"), through_on = plugin_was_on("PlayThrough.lua");
    if (through_on && read_file(PLUGINS_DIR ".play_through_state", text, sizeof(text))) {
        current_settings.native_play_through_folders = number(text, "folders", 0, 0, 1) == 1;
        current_settings.native_play_through_albums = number(text, "albums", 0, 0, 1) == 1;
    }
    if (gain_on && read_file(PLUGINS_DIR ".gain_mode_state", text, sizeof(text))) {
        text[strcspn(text, "\r\n")] = 0;
        if (!strcmp(text, "high")) current_settings.native_gain_mode = 1;
        else if (!strcmp(text, "low")) current_settings.native_gain_mode = 0;
    }
    if (read_file(PLUGINS_DIR ".mseb_state", text, sizeof(text))) {
        native_mseb_state_t state; parse_mseb(text, &state);
        state.config.enabled = state.config.enabled && mseb_on;
        if (state.legacy) {
            for (char * p = text; *p; p++) if (*p == '\n' || *p == '\r') *p = ';';
            snprintf(current_settings.native_mseb, sizeof(current_settings.native_mseb), "%.*s",
                     (int)sizeof(current_settings.native_mseb) - 1, text);
            if (!mseb_on) legacy_mseb_off(current_settings.native_mseb, sizeof(current_settings.native_mseb));
        } else encode_mseb(&state, current_settings.native_mseb, sizeof(current_settings.native_mseb));
    }
    char * stored = NULL; size_t len;
    if (plugin_storage_get("compas.soundstage", "state", &stored, &len)) {
        snprintf(current_settings.native_soundstage, sizeof(current_settings.native_soundstage), "%.*s",
                 (int)sizeof(current_settings.native_soundstage) - 1, stored);
        free(stored); stored = NULL;
        if (plugin_storage_get("compas.soundstage", "preset", &stored, &len)) {
            size_t used = strlen(current_settings.native_soundstage);
            snprintf(current_settings.native_soundstage + used, sizeof(current_settings.native_soundstage) - used,
                     ";preset=%s;", stored);
            free(stored);
        }
        soundstage_config_t stage; int preset;
        native_audio_get_soundstage(&stage, &preset);
        stage.enabled = stage.enabled && stage_on;
        encode_soundstage(&stage, preset);
    }
    if (!gain_on) hide_drawer_tile("example.gain_mode");
    if (!mseb_on) hide_drawer_tile("example.mseb");
    if (!stage_on) hide_drawer_tile("compas.soundstage");
}

/* Presets move off the card one by one; one that fails to copy stays there for the next start.
 * A native preset is never overwritten. */
static void move_mseb_presets(void) {
    if (mkdir(INTERNAL_COMPAS_DIR, 0755) && errno != EEXIST) return;
    if (mkdir(INTERNAL_COMPAS_DIR "/mseb", 0700) && errno != EEXIST) return;
    for (int named = 0; named < 2; named++) for (int i = 1; i <= (named ? NATIVE_MSEB_PRESET_COUNT : 3); i++) {
        char source[512], dest[512], text[2048];
        snprintf(source, sizeof(source), PLUGINS_DIR ".mseb_%s%d", named ? "preset" : "slot", i);
        preset_path(dest, sizeof(dest), i, named);
        if (access(dest, F_OK) && read_file(source, text, sizeof(text)) && write_file(dest, text)) unlink(source);
    }
}

/* Loudness Boost and MSEB cannot both own the EQ; files left by older versions can say both are on. */
static void resolve_eq_owner(void) {
    native_mseb_state_t state; native_audio_get_mseb(&state);
    bool conflict = state.config.enabled && native_audio_boost_active();
    if (conflict) {
        state.config.enabled = false;
        if (state.legacy) (void)native_audio_set_mseb(&state);
        else {
            bool restored = restore_manual_backup();
            if (native_audio_set_mseb(&state)) {
                settings_save(&current_settings);
                if (restored) unlink(MSEB_MANUAL_BACKUP);
            }
        }
    }
    if (!conflict && state.legacy && state.config.enabled) {
        double crossfeed = peq_get_crossfeed(); int angle = peq_get_crossfeed_angle();
        if (!peq_load_from_path(MSEB_ACTIVE_PROFILE)) {
            state.config.enabled = false;
            (void)native_audio_set_mseb(&state);
        }
        peq_set_crossfeed(crossfeed); peq_set_crossfeed_angle(angle);
    }
    if (!conflict && !state.legacy && access(MSEB_MANUAL_BACKUP, F_OK) == 0) {
        if (restore_manual_backup()) {
            settings_save(&current_settings);
            unlink(MSEB_MANUAL_BACKUP);
        } else {
            state.config.enabled = false;
            (void)native_audio_set_mseb(&state);
        }
    }
}

void native_audio_init(void) {
    /* Without the card (not mounted yet, or none) there is nothing to read, so wait for a later start. */
    bool card = access(PLUGINS_DIR, F_OK) == 0;
    if (card && !current_settings.native_audio_migrated) {
        import_plugin_settings();
        current_settings.native_audio_migrated = true;
        settings_save(&current_settings);
    }
    if (card) move_mseb_presets();
    resolve_eq_owner();
    native_audio_restore();
    int gain = native_audio_gain_mode();
    if (gain == 0 || gain == 1) audio_stage_custom_hw_volume_curve(true, gain_curves[gain]);
}
