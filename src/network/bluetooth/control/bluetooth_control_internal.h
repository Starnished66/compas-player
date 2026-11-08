#ifndef BLUETOOTH_CONTROL_INTERNAL_H
#define BLUETOOTH_CONTROL_INTERNAL_H

/* Shared by the bluetooth_control translation units (bluetooth_control.c and the bt_*.c
 * files split from it): the common includes, shared macros and types, and the
 * declarations of the state and helpers they use across files. Each declared
 * object has exactly one definition, in the file named in its section. */
#include "bluetooth_control.h"
#include "bt_ssc_policy.h"
#include "bt_adapter_query.h"
#include "bt_pairing_agent.h"
#include "../../../core/system/logging/db_log.h"
#include "../../../core/system/logging/debug_log.h"
#include "../../hiby/hiby_sys_server.h"
#include "../../../core/process/subprocess.h"
#include "../../../library/metadata/scan/library_scan_storage.h"
#include "../../radio/aic_radio.h"
#include "../../../audio/engine/audio.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#undef DBG_LOG
#define DBG_LOG(...) DB_LOG_DBG("BT", __VA_ARGS__)

typedef enum {
    BT_CODEC_AUTO = 0,
    BT_CODEC_LDAC_HQ,
    BT_CODEC_LDAC_SQ,
    BT_CODEC_SSC,
    BT_CODEC_APTX,
    BT_CODEC_AAC,
    BT_CODEC_SBC,
    BT_CODEC_SBC_XQ,
} bt_codec_preference_t;
#define BT_SOURCE_VOLUME_MAX 127

/* The firmware overlay contains BlueALSA 5 only. Keep the daemon and control
 * client names centralized so every path uses the same current API. */
#define BLUEALSA_DAEMON_PATH "/usr/bin/bluealsad"
#define BLUEALSA_DAEMON_NAME "bluealsad"
#define BLUEALSA_CTL "/usr/bin/bluealsactl"

#define BT_SSC_AUTO_PROFILE_ARG "--ssc-auto-profile"

/* Negotiate the A2DP transport at 44.1 kHz. Resampling costs ~28% of a core
 * on an R1 whatever converter does it, and it only happens when a track's
 * rate differs from the transport's, so the cheapest transport rate is the
 * one most tracks already use. BlueALSA otherwise picks the highest rate up
 * to 48 kHz, which resamples the 44.1 kHz majority of a typical library.
 * Source only: see bt_control_apply_output_settings(). */
#define BT_A2DP_FORCE_44K1_ARG "--a2dp-force-audio-cd"

/* ALSA reads $HOME/.asoundrc; main() points HOME at this writable
 * directory because the rootfs is a read-only squashfs. */
#define BT_ALSA_CONFIG_DIR "/usr/data/alsa"
#define BT_ALSA_PCM_NAME "bt_out"
/* Quality-3 speex converter, shipped in the base image. Measured on an R1
 * resampling 44.1k to a 48k transport, it costs the same as alsa-lib's
 * built-in linear converter while being a real sinc resampler; quality 5
 * costs ~14 points more CPU and quality 10 cannot keep up at all. alsa-lib
 * fails the open outright when a named converter is missing, so its
 * presence is checked rather than assumed: a player binary must keep
 * working on a base image that predates it. */
#define BT_ALSA_RATE_CONVERTER "speexrate"
#define BT_ALSA_RATE_CONVERTER_PLUGIN \
    "/usr/lib/alsa-lib/libasound_module_rate_" BT_ALSA_RATE_CONVERTER ".so"

/* Both spellings of Samsung's codec: BlueALSA's name and the display name. */
static inline bool bt_codec_name_is_ssc(const char * codec) {
    return strcasecmp(codec, "samsung-SC") == 0 || strcasecmp(codec, "SSC") == 0;
}

/* Defined in bluetooth_control.c. */
extern pthread_mutex_t bt_chip_mutex;
extern pthread_mutex_t bt_daemon_respawn_mutex;
extern atomic_int bt_codec_preference;
extern atomic_bool modern_soft_volume_requested;
extern atomic_bool bt_source_soft_volume;
extern atomic_int bt_source_vol_pending_percent;
extern pthread_mutex_t bt_soft_volume_mutex;
extern char bt_soft_volume_applied_path[256];
extern char bt_source_hw_failed_path[256];
extern char bt_source_reconcile_path[256];
extern atomic_bool bt_speexrate_enabled;
extern atomic_uint bt_sample_rate;
bool bt_ssc_auto_profile_enabled(void);
bool bt_ssc_auto_profile_for_rate(unsigned int rate);
extern pthread_mutex_t bt_rate_applied_mutex;
extern char bt_rate_refused_path[256];
extern char bt_rate_refused_codec[32];
extern unsigned int bt_rate_refused_rate;
extern pthread_mutex_t bt_ssc_correction_mutex;
extern char bt_ssc_correction_mac[18];
extern unsigned int bt_ssc_correction_rate;
extern char bt_ssc_correction_path[256];
extern char bt_ssc_correction_codec[32];
extern atomic_uint bt_ssc_correction_generation;
extern atomic_int bt_ssc_correction_running;
bool bt_rate_wants_audio_cd(void);
bool bt_rate_wants_audio_cd_for_rate(unsigned int rate);
bool bt_ldac_decoder_is_rebuilt(void);
bool bt_codec_is_sbc_xq(void);
const char * bt_codec_select_name(void);
int bt_codec_append_daemon_args(char ** argv, int argc);
bool bluealsa_is_audio_pcm_path(const char * path);
uint32_t bt_control_monotonic_ms(void);

/* Defined in bt_source_volume.c. */
extern pthread_mutex_t bt_soft_volume_serial;
void bluealsa_apply_soft_volume_cancellable_ex(const char * path, const atomic_bool * cancel,
                                               bool force_source_probe);
void bluealsa_apply_soft_volume(const char * path);
void bluealsa_clear_soft_volume_path(const char * path);

/* Defined in bt_codec_policy.c. */
void bluealsa_codec_display_name(const char * selected, char * out, size_t out_size);
bool bluealsa_parse_selected_codec(const char * info_out, char * out, size_t out_size);
void bluealsa_clear_codec_path(const char * path);
unsigned int bluealsa_info_rate(const char * info);
void bluealsa_info_codec_name(const char * info, char * out, size_t out_size);
bool bluealsa_apply_rate_preference_cancellable(const char * path, const atomic_bool * cancel);
void bluealsa_apply_rate_preference(const char * path);
bool bluealsa_available_codecs_known(const char * info_out);
const char * bt_playback_codec_override(void);
void bt_mac_to_path(char * out, size_t out_size, const char * prefix, const char * mac,
                    const char * suffix);
void bt_buds_reset(const char *path);
extern atomic_uint bt_user_disconnect_generation;
bool bt_ssc_recovery_preference(void);
bool bt_auto_ssc_rejected(const char * path);
void bt_auto_forget_switch(const char * path);
void bt_auto_pcm_added(const char * path);
extern atomic_uint bt_output_watch_started_at;
uint32_t bt_auto_reconfigure_deadline(const char * path, uint32_t removed_at);
void bt_auto_drop_confirmed(const char * path, uint32_t removed_at);
void bt_output_disconnect_finish_pending(const char * pending_path, uint32_t removed_at);
void bt_auto_expect_reconnect(const char * path);
void bt_auto_flowing_tick(void);
bool bluealsa_apply_codec_preference_deferred(const char *path, const atomic_bool *cancel,
                                              uint32_t *defer_until);
bool bluealsa_apply_codec_preference(const char * path);
bool bluealsa_codec_verified(const char * path);

/* Defined in bt_dac_info.c. */
void bt_dac_info_monitor_stop(void);
void bt_dac_info_monitor_start(void);

/* Defined in bt_control_daemon.c. */
bool bluealsa_source_pcm_may_be_connected(void);
bool ensure_bluealsa_running_rate_ex(unsigned int requested_rate, bool * out_deferred);

/* Defined in bt_devices.c. */
bool bt_control_disconnect(const char * mac);

/* Defined in bt_transport_info.c. */
void copy_info_field(char * dst, size_t size, const char * text, const char * key);
bool find_source_pcm_path_ex(char * out, size_t out_size, bool apply_soft_volume);
bool find_source_pcm_path(char * out, size_t out_size);
const char * dbus_prop_value(const char * out, const char * prop);
bool dbus_prop_string_equals(const char * out, const char * prop, const char * expected);

/* Defined in bt_output_watch.c. */
void bt_pref_rate_changed(void);
/* Posts to the output watch's preference worker; false when none runs. */
bool bt_pref_request(const char * path);

/* Device identity of the BlueALSA PCM owner. */
bool bt_pcm_path_mac(const char * path, char * mac);
bool bt_device_info_text(const char * mac, char * out, size_t size);

#endif
