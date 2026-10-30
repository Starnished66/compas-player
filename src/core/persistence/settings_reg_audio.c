#include "settings_registry.h"
#include "settings.h"
#include "../../ui/text/i18n/i18n.h"

/* Sound and Playback entries. The key is the settings.txt key. */

_Static_assert(sizeof(usb_dsd_mode_t) == sizeof(int), "registry reads enum fields as int");

#define FIELD(f) offsetof(player_settings_t, f), sizeof(((player_settings_t *) 0)->f)

static const set_option_t replaygain_options[] = {
    { 0, N_("Off") }, { 1, N_("Per Track") }, { 2, N_("Per Album") },
};
static const set_option_t dac_filter_options[] = {
    { 0, "Fast roll-off, low latency" }, { 1, "Fast roll-off, phase compensated" },
    { 2, "Slow roll-off, low latency" }, { 3, "Slow roll-off, phase compensated" },
};
static const set_option_t usb_dsd_options[] = {
    { 0, "PCM" }, { 1, "Native DSD" }, { 2, "DoP" },
};
static const set_option_t resume_options[] = {
    { 0, N_("Off") }, { 1, N_("Resume and Play") }, { 2, N_("Resume, but Paused") },
};
static const set_option_t play_mode_options[] = {
    { 0, "Sequential" }, { 1, "Repeat All" }, { 2, "Repeat One" },
    { 3, "Shuffle with repeat" }, { 4, "Shuffle once" },
};

#define BOOL_ENTRY(k, grp, lbl, field, ap, ...) \
    { .key = k, .group = grp, .label = lbl, .type = SET_BOOL, FIELD(field), .apply = ap, ##__VA_ARGS__ }

const setting_desc_t settings_reg_audio[] = {
    { .key = "replaygain_mode", .group = "sound", .label = "ReplayGain", .type = SET_ENUM,
      FIELD(replaygain_mode), .options = replaygain_options, .option_count = 3, .apply = SET_APPLY_NEXT_TRACK },
    BOOL_ENTRY("gapless_enabled", "sound", "Gapless", gapless_enabled, SET_APPLY_LIVE),
    BOOL_ENTRY("crossfade_enabled", "sound", "Crossfade", crossfade_enabled, SET_APPLY_LIVE,
               .depends = "gapless_enabled"),
    { .key = "max_volume_percent", .group = "sound", .label = "Max Volume", .type = SET_INT,
      FIELD(max_volume_percent), .min = 1, .max = 100, .unit = "%", .apply = SET_APPLY_LIVE,
      .flags = SET_F_CONFIRM },
    BOOL_ENTRY("startup_volume_fixed_enabled", "sound", "Startup Volume", startup_volume_fixed_enabled,
               SET_APPLY_NEXT_BOOT),
    { .key = "startup_volume_fixed_percent", .group = "sound", .label = "Startup Volume", .type = SET_INT,
      FIELD(startup_volume_fixed_percent), .min = 0, .max = 100, .unit = "%", .apply = SET_APPLY_NEXT_BOOT },
    { .key = "dac_filter", .group = "sound", .label = "Filter type", .type = SET_ENUM, FIELD(dac_filter),
      .options = dac_filter_options, .option_count = 4, .apply = SET_APPLY_NEXT_TRACK, .flags = SET_F_DAC },
    BOOL_ENTRY("dac_nos", "sound", "NOS", dac_nos, SET_APPLY_NEXT_TRACK, .flags = SET_F_DAC),
    BOOL_ENTRY("dac_dre", "sound", "DRE", dac_dre, SET_APPLY_NEXT_TRACK, .flags = SET_F_DAC | SET_F_DRE),
    { .key = "usb_dsd_output", .group = "sound", .label = "USB DSD Output", .type = SET_ENUM,
      FIELD(usb_dsd_output), .options = usb_dsd_options, .option_count = 3, .apply = SET_APPLY_NEXT_TRACK },
    { .key = "resume_mode", .group = "playback", .label = "Resume Last Track", .type = SET_ENUM,
      FIELD(resume_mode), .options = resume_options, .option_count = 3, .apply = SET_APPLY_NEXT_BOOT },
    { .key = "play_mode", .group = "playback", .label = "Play Mode", .type = SET_ENUM, FIELD(play_mode),
      .options = play_mode_options, .option_count = 5, .apply = SET_APPLY_LIVE },
    BOOL_ENTRY("volume_lock_screen_off", "playback", "Volume Lock (Screen Off)", volume_lock_screen_off,
               SET_APPLY_LIVE),
    BOOL_ENTRY("car_mode_enabled", "playback", "Car Mode", car_mode_enabled, SET_APPLY_LIVE),
    { .key = "car_mode_volume_percent", .group = "playback", .label = "Car Mode Volume", .type = SET_INT,
      FIELD(car_mode_volume_percent), .min = 0, .max = 100, .unit = "%", .apply = SET_APPLY_NEXT_USE },
    BOOL_ENTRY("lyrics_enabled", "playback", "Lyrics", lyrics_enabled, SET_APPLY_LIVE),
};
const size_t settings_reg_audio_count = sizeof(settings_reg_audio) / sizeof(settings_reg_audio[0]);
