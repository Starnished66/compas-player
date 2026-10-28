#include "settings_registry.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern const setting_desc_t settings_reg_audio[];
extern const size_t settings_reg_audio_count;
extern const setting_desc_t settings_reg_device[];
extern const size_t settings_reg_device_count;

#define REGISTRY_MAX 96

static const setting_group_t groups[] = {
    { "sound", "Sound" },
    { "display", "Display" },
    { "power", "Power" },
    { "playback", "Playback" },
    { "connections", "Connections" },
    { "library", "Library" },
};

static settings_registry_hooks_t hooks;
static pthread_mutex_t snapshot_mutex = PTHREAD_MUTEX_INITIALIZER;
static int snapshot_int[REGISTRY_MAX];
static char snapshot_str[REGISTRY_MAX][SETTINGS_REGISTRY_STRING_MAX];
static uint32_t snapshot_changed_at[REGISTRY_MAX];
static uint32_t snapshot_revision;
static bool snapshot_ready;
static uint32_t snapshot_epoch;

size_t settings_registry_count(void) {
    return settings_reg_audio_count + settings_reg_device_count;
}

const setting_desc_t * settings_registry_at(size_t index) {
    if (index < settings_reg_audio_count) return &settings_reg_audio[index];
    index -= settings_reg_audio_count;
    return index < settings_reg_device_count ? &settings_reg_device[index] : NULL;
}

const setting_desc_t * settings_registry_find(const char * key) {
    if (!key) return NULL;
    for (size_t i = 0; i < settings_registry_count(); i++) {
        const setting_desc_t * d = settings_registry_at(i);
        if (strcmp(d->key, key) == 0) return d;
    }
    return NULL;
}

int settings_registry_index(const setting_desc_t * desc) {
    for (size_t i = 0; desc && i < settings_registry_count(); i++)
        if (settings_registry_at(i) == desc) return (int) i;
    return -1;
}

size_t settings_registry_group_count(void) { return sizeof(groups) / sizeof(groups[0]); }

const setting_group_t * settings_registry_group_at(size_t index) {
    return index < settings_registry_group_count() ? &groups[index] : NULL;
}

void settings_registry_set_hooks(const settings_registry_hooks_t * h) {
    pthread_mutex_lock(&snapshot_mutex);
    if (h) hooks = *h;
    else memset(&hooks, 0, sizeof(hooks));
    pthread_mutex_unlock(&snapshot_mutex);
}

const char * settings_registry_error_code(setv_result_t result) {
    switch (result) {
        case SETV_OK: return "ok";
        case SETV_UNKNOWN_KEY: return "unknown_key";
        case SETV_INVALID_TYPE: return "invalid_type";
        case SETV_READ_ONLY: return "read_only";
        case SETV_UNAVAILABLE: return "unavailable";
        case SETV_OUT_OF_RANGE: return "out_of_range";
        case SETV_NOT_AN_OPTION: return "not_an_option";
        case SETV_CONFIRMATION_REQUIRED: return "confirmation_required";
        case SETV_DEPENDENCY_UNMET: return "dependency_unmet";
    }
    return "invalid_request";
}

bool settings_registry_available(const setting_desc_t * desc, const char ** why_not) {
    unsigned need = desc->flags & (SET_F_DAC | SET_F_DRE);
    for (unsigned bit = SET_F_DAC; need && bit <= SET_F_DRE; bit <<= 1) {
        if (!(need & bit)) continue;
        pthread_mutex_lock(&snapshot_mutex);
        bool (*probe)(unsigned) = hooks.feature_present;
        pthread_mutex_unlock(&snapshot_mutex);
        if (!probe || !probe(bit)) {
            if (why_not) *why_not = "not_on_this_model";
            return false;
        }
    }
    return true;
}

int settings_registry_loud_threshold(void) {
    pthread_mutex_lock(&snapshot_mutex);
    int level = hooks.loud_volume_percent;
    pthread_mutex_unlock(&snapshot_mutex);
    return level > 0 ? level : SETTINGS_REGISTRY_LOUD_DEFAULT;
}

setv_result_t settings_registry_validate(const setting_desc_t * desc, int value, bool acknowledged) {
    if (!desc) return SETV_UNKNOWN_KEY;
    if ((desc->flags & SET_F_READONLY) || desc->type == SET_STRING) return SETV_READ_ONLY;
    if (!settings_registry_available(desc, NULL)) return SETV_UNAVAILABLE;
    switch (desc->type) {
        case SET_BOOL:
            if (value != 0 && value != 1) return SETV_INVALID_TYPE;
            break;
        case SET_INT:
            if (value < desc->min || value > desc->max) return SETV_OUT_OF_RANGE;
            if ((desc->flags & SET_F_CONFIRM) && value > settings_registry_loud_threshold() && !acknowledged) {
                int current = 0; /* lowering a cap that is already high needs no acknowledgement */
                if (!settings_registry_get_int(desc, &current) || value > current) return SETV_CONFIRMATION_REQUIRED;
            }
            break;
        case SET_PRESET: {
            bool found = false;
            for (int i = 0; i < desc->preset_count; i++) found |= desc->presets[i] == value;
            if (!found) return SETV_NOT_AN_OPTION;
            break;
        }
        case SET_ENUM: {
            bool found = false;
            for (int i = 0; i < desc->option_count; i++) found |= desc->options[i].value == value;
            if (!found) return SETV_NOT_AN_OPTION;
            break;
        }
        case SET_STRING:
            return SETV_READ_ONLY;
    }
    return SETV_OK;
}

static int find_change(const setting_change_t * changes, int n, const char * key) {
    for (int i = 0; i < n; i++)
        if (strcmp(changes[i].desc->key, key) == 0) return i;
    return -1;
}

int settings_registry_resolve(setting_change_t * changes, int n, int * bad_index) {
    for (int i = 0; i < n; i++) {
        const setting_desc_t * d = changes[i].desc;
        if (!d->depends || !changes[i].value) continue;
        int j = find_change(changes, n, d->depends);
        int final_value = 0;
        if (j >= 0) final_value = changes[j].value;
        else {
            const setting_desc_t * parent = settings_registry_find(d->depends);
            if (parent) (void) settings_registry_get_int(parent, &final_value);
        }
        if (!final_value) {
            if (bad_index) *bad_index = i;
            return -1;
        }
    }
    int total = n;
    for (int i = 0; i < total; i++) {
        if (changes[i].value) continue;
        for (size_t k = 0; k < settings_registry_count(); k++) {
            const setting_desc_t * dep = settings_registry_at(k);
            int current = 0;
            if (!dep->depends || strcmp(dep->depends, changes[i].desc->key) != 0 ||
                find_change(changes, total, dep->key) >= 0) continue;
            if (settings_registry_get_int(dep, &current) && current) {
                changes[total].desc = dep;
                changes[total].value = 0;
                total++;
            }
        }
    }
    return total;
}

void settings_registry_note_save(const void * player_settings) {
    if (!player_settings) return;
    const unsigned char * base = player_settings;
    size_t count = settings_registry_count();
    if (count > REGISTRY_MAX) count = REGISTRY_MAX;
    pthread_mutex_lock(&snapshot_mutex);
    bool changed = false;
    for (size_t i = 0; i < count; i++) {
        const setting_desc_t * d = settings_registry_at(i);
        if (d->flags & SET_F_RUNTIME) continue;
        if (d->type == SET_STRING) {
            char text[SETTINGS_REGISTRY_STRING_MAX];
            size_t n = d->size < sizeof(text) ? d->size : sizeof(text);
            memcpy(text, base + d->offset, n);
            text[n - 1] = '\0';
            if (strcmp(text, snapshot_str[i]) != 0) {
                memcpy(snapshot_str[i], text, sizeof(text));
                if (snapshot_ready) { changed = true; snapshot_changed_at[i] = snapshot_revision + 1; }
            }
            continue;
        }
        int value = 0;
        if (d->size == 1) value = *(const bool *) (base + d->offset) ? 1 : 0;
        else memcpy(&value, base + d->offset, sizeof(int));
        if (value != snapshot_int[i]) {
            snapshot_int[i] = value;
            if (snapshot_ready) { changed = true; snapshot_changed_at[i] = snapshot_revision + 1; }
        }
    }
    if (changed) snapshot_revision++;
    snapshot_ready = true;
    pthread_mutex_unlock(&snapshot_mutex);
}

uint32_t settings_registry_epoch(void) {
    pthread_mutex_lock(&snapshot_mutex);
    if (!snapshot_epoch) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint32_t mix = (uint32_t) ts.tv_sec * 2654435761u ^ (uint32_t) ts.tv_nsec ^ ((uint32_t) getpid() << 12);
        snapshot_epoch = (mix & 0x1fffffffu) | 1u; /* stays below 1e9 for whole-number JSON, never 0 */
    }
    uint32_t epoch = snapshot_epoch;
    pthread_mutex_unlock(&snapshot_mutex);
    return epoch;
}

uint32_t settings_registry_revision(void) {
    pthread_mutex_lock(&snapshot_mutex);
    uint32_t revision = snapshot_revision;
    pthread_mutex_unlock(&snapshot_mutex);
    return revision;
}

uint32_t settings_registry_changed_at(const setting_desc_t * desc) {
    int index = settings_registry_index(desc);
    if (index < 0 || index >= REGISTRY_MAX) return 0;
    pthread_mutex_lock(&snapshot_mutex);
    uint32_t at = snapshot_changed_at[index];
    pthread_mutex_unlock(&snapshot_mutex);
    return at;
}

bool settings_registry_get_int(const setting_desc_t * desc, int * out) {
    int index = settings_registry_index(desc);
    if (index < 0 || index >= REGISTRY_MAX || desc->type == SET_STRING) return false;
    pthread_mutex_lock(&snapshot_mutex);
    bool (*runtime)(const setting_desc_t *, int *) = hooks.runtime_value;
    int value = snapshot_int[index];
    pthread_mutex_unlock(&snapshot_mutex);
    if (desc->flags & SET_F_RUNTIME) return runtime && runtime(desc, out);
    *out = value;
    return true;
}

bool settings_registry_get_string(const setting_desc_t * desc, char * out, size_t out_size) {
    int index = settings_registry_index(desc);
    if (index < 0 || index >= REGISTRY_MAX || desc->type != SET_STRING || !out_size) return false;
    pthread_mutex_lock(&snapshot_mutex);
    snprintf(out, out_size, "%s", snapshot_str[index]);
    pthread_mutex_unlock(&snapshot_mutex);
    return true;
}
