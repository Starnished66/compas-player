#ifndef NATIVE_AUDIO_SETTINGS_H
#define NATIVE_AUDIO_SETTINGS_H
#include "../../audio/effects/mseb.h"
#include "../../audio/effects/soundstage.h"

#define NATIVE_MSEB_PRESET_COUNT 20
#define NATIVE_MSEB_NAME_MAX 64

typedef struct {
    mseb_config_t config;
    int range;
    bool legacy;
    int legacy_values[3];
} native_mseb_state_t;

/* Called on the UI thread at plugin startup/reload. Plugin overrides remain
 * transient; unloading their owner restores these persisted native settings. */
void native_audio_init(void);
void native_audio_restore(void);
void native_audio_restore_mseb(void);
void native_audio_restore_soundstage(void);
void native_audio_set_mono(bool enabled);
int native_audio_gain_mode(void);
/* Loudness Boost holds the EQ (its backup exists), so MSEB cannot turn on. */
bool native_audio_boost_active(void);
void native_audio_set_gain_mode(int high);
void native_audio_get_mseb(native_mseb_state_t * state);
bool native_audio_set_mseb(const native_mseb_state_t * state);
bool native_audio_convert_mseb(bool enable);
void native_audio_get_soundstage(soundstage_config_t * config, int * preset);
bool native_audio_set_soundstage(const soundstage_config_t * config, int preset);
void native_audio_soundstage_preset(soundstage_config_t * config, int preset);
bool native_mseb_preset_read(int slot, bool named, native_mseb_state_t * state, char * name);
bool native_mseb_preset_write(int slot, bool named, const native_mseb_state_t * state, const char * name);
bool native_mseb_preset_exists(int slot, bool named);
bool native_mseb_preset_delete(int slot);
#endif
