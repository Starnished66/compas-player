#ifndef SETTINGS_REGISTRY_H
#define SETTINGS_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Allowlist of the player settings the remote API may show or change. The
 * tables are const data; anything not listed (the Remote Control PIN, music
 * service credentials, paths, setup state) is never serialised or writable.
 * Validation here is pure and thread-safe. The values are read from a
 * snapshot that settings_save*() refreshes, so HTTP threads never touch
 * current_settings. Applying a change happens on the UI thread, see
 * gui_remote_settings.c. */

#define SETTINGS_REGISTRY_SCHEMA_VERSION 1
#define SETTINGS_REGISTRY_STRING_MAX 64
#define SETTINGS_REGISTRY_LOUD_DEFAULT 85 /* used when the stock warning level is unknown */

typedef enum { SET_BOOL, SET_ENUM, SET_INT, SET_PRESET, SET_STRING } set_type_t;
typedef enum { SET_APPLY_LIVE, SET_APPLY_NEXT_TRACK, SET_APPLY_NEXT_USE, SET_APPLY_NEXT_BOOT } set_apply_t;

#define SET_F_CONFIRM  1u /* raising above the loud level needs an acknowledgement */
#define SET_F_READONLY 2u /* visible, never writable over the API */
#define SET_F_RUNTIME  4u /* value comes from the runtime hook, not the struct */
#define SET_F_DAC      8u /* only on boards with the DAC mixer controls */
#define SET_F_DRE      16u /* only where the DRE control exists */

typedef struct {
    int value;
    const char * label; /* English msgid */
} set_option_t;

typedef struct {
    const char * key;   /* stable id, same as the settings.txt key where one exists */
    const char * group;
    const char * label; /* English msgid, translated when served */
    set_type_t type;
    size_t offset;      /* offsetof(player_settings_t, field) */
    size_t size;        /* sizeof the field: 1 for bool, sizeof(int), or the char array */
    int min, max;       /* SET_INT */
    const char * unit;
    const int * presets; /* SET_PRESET: the same tables the device screens use */
    int preset_count;
    const set_option_t * options; /* SET_ENUM */
    int option_count;
    const char * depends; /* key of a bool that must be on for this one to be on */
    const char * reason;  /* why it is read-only */
    set_apply_t apply;
    unsigned flags;
} setting_desc_t;

typedef struct {
    const char * id;
    const char * label; /* English msgid */
} setting_group_t;

typedef enum {
    SETV_OK = 0,
    SETV_UNKNOWN_KEY,
    SETV_INVALID_TYPE,
    SETV_READ_ONLY,
    SETV_UNAVAILABLE,
    SETV_OUT_OF_RANGE,
    SETV_NOT_AN_OPTION,
    SETV_CONFIRMATION_REQUIRED,
    SETV_DEPENDENCY_UNMET
} setv_result_t;

const char * settings_registry_error_code(setv_result_t result);

size_t settings_registry_count(void);
const setting_desc_t * settings_registry_at(size_t index);
const setting_desc_t * settings_registry_find(const char * key);
int settings_registry_index(const setting_desc_t * desc); /* -1 when not in the table */
size_t settings_registry_group_count(void);
const setting_group_t * settings_registry_group_at(size_t index);

/* Hooks installed by the UI at boot; all optional. */
typedef struct {
    bool (*feature_present)(unsigned flag);        /* SET_F_DAC / SET_F_DRE probes */
    bool (*runtime_value)(const setting_desc_t * desc, int * out); /* SET_F_RUNTIME */
    int loud_volume_percent;                        /* 0 = unknown, use the default */
} settings_registry_hooks_t;
void settings_registry_set_hooks(const settings_registry_hooks_t * hooks);

bool settings_registry_available(const setting_desc_t * desc, const char ** why_not);
int settings_registry_loud_threshold(void);

/* Pure range/option/availability check of one value. `acknowledged` is true
 * when the request listed the key in its acknowledge array. */
setv_result_t settings_registry_validate(const setting_desc_t * desc, int value, bool acknowledged);

/* A change after validation. */
typedef struct {
    const setting_desc_t * desc;
    int value;
} setting_change_t;

/* Checks bool dependencies across the whole request (a dependent cannot be
 * turned on while the key it depends on ends up off) and appends the
 * cascade: turning a key off turns its currently-on dependents off too.
 * `changes` must have room for n + settings_registry_count() entries.
 * Returns the new count, or -1 with *bad_index set to the offending entry. */
int settings_registry_resolve(setting_change_t * changes, int n, int * bad_index);

/* Snapshot and revision. */
void settings_registry_note_save(const void * player_settings); /* const player_settings_t * */
uint32_t settings_registry_revision(void);
/* Random per boot. The revision restarts at 0 on every boot, so clients pair it
 * with this id: a cached revision is only meaningful for the epoch it came from. */
uint32_t settings_registry_epoch(void);
bool settings_registry_get_int(const setting_desc_t * desc, int * out);
bool settings_registry_get_string(const setting_desc_t * desc, char * out, size_t out_size);
uint32_t settings_registry_changed_at(const setting_desc_t * desc);

#endif /* SETTINGS_REGISTRY_H */
