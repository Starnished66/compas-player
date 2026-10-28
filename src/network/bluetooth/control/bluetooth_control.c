#include "bluetooth_control.h"
#include "bt_ssc_policy.h"
#include "../../../core/system/logging/debug_log.h"
#include "../../hiby/hiby_sys_server.h"
#include "../../../core/process/subprocess.h"
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

/* Guards bt_control_init_chip(), bt_control_enable(), and bt_control_disable()
 * so chip firmware operations and power toggles are strictly serialized across threads. */
static pthread_mutex_t bt_chip_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Shared by fallback bring-up and explicit profile changes. */
static pthread_mutex_t bt_daemon_respawn_mutex = PTHREAD_MUTEX_INITIALIZER;
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

static atomic_int bt_codec_preference = BT_CODEC_AUTO;
static atomic_bool modern_soft_volume_requested = false;
/* Effective outgoing gain owner: unknown AVRCP capability uses software. */
static atomic_bool bt_source_soft_volume = true;
static atomic_int bt_source_vol_pending_percent = -1;
static pthread_mutex_t bt_soft_volume_mutex = PTHREAD_MUTEX_INITIALIZER;
static char bt_soft_volume_applied_path[256];
#define BT_SOURCE_VOLUME_MAX 127

/* The firmware overlay contains BlueALSA 5 only. Keep the daemon and control
 * client names centralized so every path uses the same current API. */
typedef struct {
    const char * daemon;
    const char * daemon_name;
    const char * ctl;
} bluealsa_backend_t;

static bluealsa_backend_t bluealsa_backend(void) {
    return (bluealsa_backend_t) {
        "/usr/bin/bluealsad", "bluealsad", "/usr/bin/bluealsactl"
    };
}

#define BT_SSC_AUTO_PROFILE_ARG "--ssc-auto-profile"
static pthread_mutex_t bt_daemon_ssc_feature_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_int bt_daemon_ssc_auto_profile_feature = -1;

static bool bt_daemon_supports_ssc_auto_profile(void) {
    int state = atomic_load(&bt_daemon_ssc_auto_profile_feature);
    if (state >= 0) return state != 0;
    pthread_mutex_lock(&bt_daemon_ssc_feature_mutex);
    state = atomic_load(&bt_daemon_ssc_auto_profile_feature);
    if (state < 0) {
        char help[16384] = {0};
        char *argv[] = { (char *)"/usr/bin/bluealsad", (char *)"--help", NULL };
        int exit_code = -1;
        bool queried = subprocess_run_checked(argv, help, sizeof(help), 1000, &exit_code) && exit_code == 0;
        bool supported = false;
        if (queried) {
            char *save = NULL;
            for (char *token = strtok_r(help, " \t\r\n", &save); token;
                 token = strtok_r(NULL, " \t\r\n", &save)) {
                if (strcmp(token, BT_SSC_AUTO_PROFILE_ARG) == 0) {
                    supported = true;
                    break;
                }
            }
        }
        if (queried) {
            atomic_store(&bt_daemon_ssc_auto_profile_feature, supported ? 1 : 0);
            state = supported ? 1 : 0;
        }
    }
    pthread_mutex_unlock(&bt_daemon_ssc_feature_mutex);
    return state > 0;
}

static const char * bluealsa_ctl_name(void) {
    return bluealsa_backend().ctl;
}

/* Pushed in from the UI; see the setter below. */
static atomic_bool bt_speexrate_enabled = false;
/* Requested A2DP transport rate in Hz, 0 for BlueALSA's own choice. */
static atomic_uint bt_sample_rate = 44100;
static void bt_pref_rate_changed(void);

static bool bt_ssc_auto_profile_enabled(void) {
    return atomic_load(&bt_sample_rate) == 0 && bt_daemon_supports_ssc_auto_profile();
}

static bool bt_ssc_auto_profile_for_rate(unsigned int rate) {
    return rate == 0 && bt_daemon_supports_ssc_auto_profile();
}

/* Remembers a rate the accessory REFUSED, not one that was applied: a refusal
 * can tear the transport down, so retrying it every reconnect would become a
 * connect/refuse/drop cycle. A successful apply needs no memory, because the
 * transport then reports the requested rate and the next attempt is a no-op. */
static pthread_mutex_t bt_rate_applied_mutex = PTHREAD_MUTEX_INITIALIZER;
static char bt_rate_refused_path[256];
static char bt_rate_refused_codec[32]; /* a refusal holds for this codec only */
static unsigned int bt_rate_refused_rate;
static pthread_mutex_t bt_ssc_correction_mutex = PTHREAD_MUTEX_INITIALIZER;
static char bt_ssc_correction_mac[18];
static unsigned int bt_ssc_correction_rate;
static char bt_ssc_correction_path[256];
static char bt_ssc_correction_codec[32];
static atomic_uint bt_ssc_correction_generation = 0;
/* Counted while a rate correction owns the link (codec commands wait). */
static atomic_int bt_ssc_correction_running;

void bt_control_ssc_rate_correction_cancel(void) {
    atomic_fetch_add_explicit(&bt_ssc_correction_generation, 1, memory_order_acq_rel);
    pthread_mutex_lock(&bt_ssc_correction_mutex);
    bt_ssc_correction_mac[0] = '\0';
    bt_ssc_correction_rate = 0;
    bt_ssc_correction_path[0] = '\0';
    bt_ssc_correction_codec[0] = '\0';
    pthread_mutex_unlock(&bt_ssc_correction_mutex);
}

bool bt_control_ssc_rate_correction_peek(char * mac, size_t mac_size, unsigned int * rate) {
    if (!mac || mac_size < 18 || !rate) return false;
    pthread_mutex_lock(&bt_ssc_correction_mutex);
    bool pending = bt_ssc_correction_mac[0] != '\0';
    if (pending) {
        snprintf(mac, mac_size, "%s", bt_ssc_correction_mac);
        *rate = bt_ssc_correction_rate;
    }
    pthread_mutex_unlock(&bt_ssc_correction_mutex);
    return pending;
}

bool bt_control_ssc_rate_correction_take(const char * mac, unsigned int rate) {
    bool taken = false;
    pthread_mutex_lock(&bt_ssc_correction_mutex);
    if (mac && bt_ssc_correction_rate == rate &&
            strcmp(bt_ssc_correction_mac, mac) == 0 && atomic_load(&bt_sample_rate) == rate) {
        bt_ssc_correction_mac[0] = '\0';
        bt_ssc_correction_rate = 0;
        pthread_mutex_lock(&bt_rate_applied_mutex);
        snprintf(bt_rate_refused_path, sizeof(bt_rate_refused_path), "%s", bt_ssc_correction_path);
        snprintf(bt_rate_refused_codec, sizeof(bt_rate_refused_codec), "%s", bt_ssc_correction_codec);
        bt_rate_refused_rate = rate;
        bt_ssc_correction_path[0] = '\0';
        bt_ssc_correction_codec[0] = '\0';
        pthread_mutex_unlock(&bt_rate_applied_mutex);
        atomic_fetch_add_explicit(&bt_ssc_correction_generation, 1, memory_order_acq_rel);
        taken = true;
    }
    pthread_mutex_unlock(&bt_ssc_correction_mutex);
    return taken;
}

/* Routine (startup, the connected device changing): the refusal memory is
 * kept, so a refused rate is not retried on every reconnect. */
void bt_control_set_sample_rate(unsigned int rate) {
    if (atomic_exchange(&bt_sample_rate, rate) != rate) bt_pref_rate_changed();
}

unsigned int bt_control_get_sample_rate(void) {
    return atomic_load(&bt_sample_rate);
}

/* The user picked a rate: a different choice deserves a fresh attempt even
 * on a device that refused the previous one. */
void bt_control_choose_sample_rate(unsigned int rate) {
    atomic_store(&bt_sample_rate, rate);
    bt_control_ssc_rate_correction_cancel();
    pthread_mutex_lock(&bt_rate_applied_mutex);
    bt_rate_refused_path[0] = '\0';
    bt_rate_refused_codec[0] = '\0';
    bt_rate_refused_rate = 0;
    pthread_mutex_unlock(&bt_rate_applied_mutex);
}

/* The source daemon's force-audio-cd option keeps non-SSC codecs at the
 * player's 44.1 kHz default. With --ssc-auto-profile, SSC instead negotiates
 * from the accessory's advertised rates while other codecs retain that
 * default. */
/* The explicit transport-rate apply path resolves Automatic to 44.1 kHz;
 * the daemon's SSC auto-profile option is the separate exception that lets
 * an SSC transport use its advertised HiFi/UHQ profile. */
static unsigned int bt_effective_sample_rate(void) {
    unsigned int rate = atomic_load(&bt_sample_rate);
    return rate == 0 ? 44100 : rate;
}

static bool bt_rate_wants_audio_cd(void) {
    return bt_effective_sample_rate() == 44100;
}

static bool bt_rate_wants_audio_cd_for_rate(unsigned int rate) {
    return rate == 0 || rate == 44100;
}

/* Selects the speex resampler for Bluetooth output. On by default: a track
 * whose rate differs from the A2DP transport's is converted either way, and
 * speex measured about a point of CPU more than alsa-lib's built-in linear
 * converter while sounding better. Converting at all is the expensive part,
 * which is why the transport rate matters more than the converter. */
void bt_control_set_speexrate_enabled(bool enabled) {
    atomic_store(&bt_speexrate_enabled, enabled);
}

#ifndef BT_LDAC_DECODER_PATH
#define BT_LDAC_DECODER_PATH "/usr/lib/libldacdec.so.1"
#endif

/* The stock firmware's libldacdec.so.1 segfaults on phone LDAC streams,
 * killing bluealsad and dropping the connection. The rebuilt decoder in the
 * updated base image exports the bounded ldacDecode_type_len entry point, so
 * the sink offers LDAC only when the installed library contains that name. */
static bool bt_ldac_decoder_is_rebuilt(void) {
    static const char marker[] = "ldacDecode_type_len";
    const size_t marker_len = sizeof(marker) - 1;
    FILE * f = fopen(BT_LDAC_DECODER_PATH, "rb");
    if (!f) return false;
    char buf[4096];
    size_t keep = 0, n;
    bool found = false;
    while (!found && (n = fread(buf + keep, 1, sizeof(buf) - keep, f)) > 0) {
        size_t len = keep + n;
        for (size_t i = 0; i + marker_len <= len; i++) {
            if (buf[i] == marker[0] && memcmp(buf + i, marker, marker_len) == 0) {
                found = true;
                break;
            }
        }
        /* Carry the tail over so a name split across reads still matches. */
        keep = len < marker_len - 1 ? len : marker_len - 1;
        memmove(buf, buf + len - keep, keep);
    }
    fclose(f);
    return found;
}

static void bt_buds_reset(const char *path);

void bt_control_restore_codec_preference(const char * codec) {
    bt_codec_preference_t preference = BT_CODEC_AUTO;
    if (codec) {
        if (strcmp(codec, "ldac_hq") == 0) preference = BT_CODEC_LDAC_HQ;
        else if (strcmp(codec, "ldac_sq") == 0 || strcmp(codec, "ldac") == 0) preference = BT_CODEC_LDAC_SQ;
        else if (strcmp(codec, "ssc") == 0) preference = BT_CODEC_SSC;
        else if (strcmp(codec, "aptx") == 0) preference = BT_CODEC_APTX;
        else if (strcmp(codec, "aac") == 0) preference = BT_CODEC_AAC;
        else if (strcmp(codec, "sbc") == 0) preference = BT_CODEC_SBC;
        else if (strcmp(codec, "sbc_xq") == 0) preference = BT_CODEC_SBC_XQ;
    }
    atomic_store(&bt_codec_preference, preference);
    bt_control_ssc_discovery_recovery_cancel();
    bt_buds_reset(NULL);
}

/* ALSA reads $HOME/.asoundrc; main() points HOME at this writable
 * directory because the rootfs is a read-only squashfs. */
/* Negotiate the A2DP transport at 44.1 kHz. Resampling costs ~28% of a core
 * on an R1 whatever converter does it, and it only happens when a track's
 * rate differs from the transport's, so the cheapest transport rate is the
 * one most tracks already use. BlueALSA otherwise picks the highest rate up
 * to 48 kHz, which resamples the 44.1 kHz majority of a typical library.
 * Source only: see bt_control_apply_output_settings(). */
#define BT_A2DP_FORCE_44K1_ARG "--a2dp-force-audio-cd"

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

const char * bt_control_get_playback_pcm(void) {
    switch ((bt_codec_preference_t) atomic_load(&bt_codec_preference)) {
    case BT_CODEC_LDAC_HQ:
    case BT_CODEC_LDAC_SQ:
        return "bluealsa:CODEC=LDAC";
    case BT_CODEC_SSC:
        /* The PCMAdded selector already applied SSC or its supported-device
         * fallback. Leave the plugin free to open that negotiated codec. */
        return "bluealsa";
    case BT_CODEC_APTX:
        return "bluealsa:CODEC=aptX";
    case BT_CODEC_AAC:
        return "bluealsa:CODEC=AAC";
    case BT_CODEC_SBC:
    case BT_CODEC_SBC_XQ:
        return "bluealsa:CODEC=SBC";
    case BT_CODEC_AUTO:
    default:
        return "bluealsa";
    }
}

static bool bt_codec_is_sbc_xq(void) {
    return (bt_codec_preference_t) atomic_load(&bt_codec_preference) == BT_CODEC_SBC_XQ;
}

/* Optional codecs are not all enabled by BlueALSA's default configuration.
 * Enabling the selected codec here keeps the setting effective. SBC is
 * mandatory and needs no -c option. */
static const char * bt_codec_daemon_name(void) {
    switch ((bt_codec_preference_t) atomic_load(&bt_codec_preference)) {
    case BT_CODEC_LDAC_HQ:
    case BT_CODEC_LDAC_SQ: return "LDAC";
    case BT_CODEC_SSC: return "samsung-SC";
    case BT_CODEC_APTX: return "aptX";
    case BT_CODEC_AAC: return "AAC";
    default: return NULL;
    }
}

/* The codec name `bluealsactl codec PCM_PATH CODEC` expects. Deliberately
 * NOT bt_codec_daemon_name() above: that one feeds the daemon's own -c
 * enable flag, where SBC is omitted because it is mandatory and always
 * built in, but selecting a codec on a live PCM has to name SBC explicitly.
 * AUTO returns NULL -- there is nothing to force, BlueALSA negotiating on
 * its own IS the preference. */
static const char * bt_codec_select_name(void) {
    switch ((bt_codec_preference_t) atomic_load(&bt_codec_preference)) {
    case BT_CODEC_LDAC_HQ:
    case BT_CODEC_LDAC_SQ: return "LDAC";
    case BT_CODEC_SSC: return "samsung-SC";
    case BT_CODEC_APTX: return "aptX";
    case BT_CODEC_AAC: return "AAC";
    case BT_CODEC_SBC:
    case BT_CODEC_SBC_XQ: return "SBC";
    default: return NULL;
    }
}

/* Automatic runs LDAC like the stock player (LDAC_ABR): starting at high
 * quality with adaptive bitrate, so a weak link steps down instead of
 * dropping out. */
static const char * bt_codec_daemon_quality(void) {
    switch ((bt_codec_preference_t) atomic_load(&bt_codec_preference)) {
    case BT_CODEC_LDAC_HQ: return "high";
    case BT_CODEC_LDAC_SQ: return "standard";
    case BT_CODEC_AUTO: return "high";
    default: return NULL;
    }
}

static bool bt_codec_auto(void) {
    return (bt_codec_preference_t) atomic_load(&bt_codec_preference) == BT_CODEC_AUTO;
}

/* Build source-daemon codec arguments for BlueALSA 5. */
static int bt_codec_append_daemon_args(char ** argv, int argc) {
    if (bt_codec_auto()) {
        argv[argc++] = (char *) "--all-codecs";
        argv[argc++] = (char *) "--ldac-abr";
    } else if ((bt_codec_preference_t) atomic_load(&bt_codec_preference) == BT_CODEC_SSC) {
        /* Keep every usable advertised fallback enabled. The per-PCM
         * preference worker explicitly selects SSC when the peer supports it. */
        argv[argc++] = (char *) "--all-codecs";
    } else {
        const char * daemon_codec = bt_codec_daemon_name();
        if (daemon_codec) {
            argv[argc++] = (char *) "-c";
            argv[argc++] = (char *) daemon_codec;
        }
    }
    const char * daemon_quality = bt_codec_daemon_quality();
    if (daemon_quality) {
        argv[argc++] = (char *) "--ldac-quality";
        argv[argc++] = (char *) daemon_quality;
    }
    return argc;
}

static bool bluealsa_is_audio_pcm_path(const char * path) {
    return path && (strstr(path, "/a2dpsnk/source") != NULL ||
                    strstr(path, "/a2dpsrc/sink") != NULL);
}

/* BlueALSA 5 has no daemon-level --a2dp-volume option; apply the equivalent
 * control command when each PCM appears. */
/* bt_soft_volume_mutex guards only the applied-path latch and is never held
 * across a subprocess, so event handlers clearing it stay fast. Appliers
 * take bt_soft_volume_serial for the whole command (one at a time); the
 * latch is recorded only after the command succeeded, and not at all if a
 * clear happened meanwhile (bt_soft_volume_clears). */
static pthread_mutex_t bt_soft_volume_serial = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint bt_soft_volume_clears;
static atomic_uint bt_source_readiness_generation;
static unsigned int bt_source_vol_pending_audio_generation;

/* bluealsactl soft-volume PATH prints "SoftVolume: true|false". */
static bool bluealsa_read_soft_volume(const char * path, bool * out) {
    char reply[128];
    char * argv[] = { (char *) bluealsa_backend().ctl, (char *) "soft-volume", (char *) path, NULL };
    if (!subprocess_run(argv, reply, sizeof(reply))) return false;
    const char * v = strstr(reply, "SoftVolume:");
    if (!v) return false;
    v += strlen("SoftVolume:");
    while (*v == ' ') v++;
    *out = strncmp(v, "true", 4) == 0;
    return strncmp(v, "true", 4) == 0 || strncmp(v, "false", 5) == 0;
}

static const char * dbus_prop_value(const char * out, const char * prop);
static bool dbus_prop_string_equals(const char * out, const char * prop, const char * expected);
static uint32_t bt_control_monotonic_ms(void);
static bool bluealsa_read_source_volume(const char * path, int * out);

typedef enum {
    BT_SOURCE_TRANSPORT_ERROR = -1,
    BT_SOURCE_TRANSPORT_MISSING = 0,
    BT_SOURCE_TRANSPORT_READY = 1,
} bt_source_transport_result_t;

static uint32_t bt_source_probe_remaining_ms(uint32_t deadline_ms) {
    int32_t remaining = (int32_t)(deadline_ms - bt_control_monotonic_ms());
    return remaining > 0 ? (uint32_t)remaining : 0;
}

/* Locate this PCM's exact BlueZ MediaTransport1 object and read its current
 * absolute volume. Volume is only present after AVRCP negotiation succeeds. */
static bt_source_transport_result_t bluealsa_find_source_transport_until(
        const char * pcm_path, char * out_transport, size_t transport_size,
        int * out_volume, uint32_t deadline_ms) {
    if (!pcm_path || !out_transport || transport_size == 0) return BT_SOURCE_TRANSPORT_ERROR;
    const char * adapter = strstr(pcm_path, "/hci");
    const char * role = strstr(pcm_path, "/a2dpsrc/sink");
    if (!adapter || !role || role <= adapter) return BT_SOURCE_TRANSPORT_ERROR;
    char device[192];
    int len = snprintf(device, sizeof(device), "/org/bluez%.*s", (int)(role - adapter), adapter);
    if (len <= 0 || (size_t)len >= sizeof(device)) return BT_SOURCE_TRANSPORT_ERROR;
    enum { REPLY_SIZE = 65536 };
    char * reply = malloc(REPLY_SIZE);
    if (!reply) return BT_SOURCE_TRANSPORT_ERROR;
    char * argv[] = { "dbus-send", "--system", "--print-reply", "--dest=org.bluez", "/",
                     "org.freedesktop.DBus.ObjectManager.GetManagedObjects", NULL };
    int status = -1;
    bt_source_transport_result_t result = BT_SOURCE_TRANSPORT_ERROR;
    uint32_t remaining = bt_source_probe_remaining_ms(deadline_ms);
    if (!remaining || !subprocess_run_checked(argv, reply, REPLY_SIZE,
                                              remaining < 1500 ? remaining : 1500, &status) || status != 0 ||
        strlen(reply) >= REPLY_SIZE - 1) goto done;
    result = BT_SOURCE_TRANSPORT_MISSING;
    const char * scan = reply;
    unsigned probes = 0;
    while ((scan = strstr(scan, "object path \"")) != NULL && probes < 4) {
        scan += strlen("object path \"");
        const char * end = strchr(scan, '"');
        if (!end) break;
        const char * path = scan;
        scan = end + 1;
        size_t n = (size_t)(end - path);
        if (n <= (size_t)len || strncmp(path, device, (size_t)len) != 0 || path[len] != '/') continue;
        char transport[256];
        if (n >= sizeof(transport)) continue;
        memcpy(transport, path, n);
        transport[n] = '\0';
        const char * fd = strrchr(transport, '/');
        if (!fd || strncmp(fd, "/fd", 3) != 0 || !isdigit((unsigned char)fd[3]) ||
            strspn(fd + 3, "0123456789") != strlen(fd + 3)) continue;
        probes++;
        char props[4096];
        char * prop_argv[] = { "dbus-send", "--system", "--print-reply", "--dest=org.bluez",
            transport, "org.freedesktop.DBus.Properties.GetAll", "string:org.bluez.MediaTransport1", NULL };
        remaining = bt_source_probe_remaining_ms(deadline_ms);
        status = -1;
        if (!remaining || !subprocess_run_checked(prop_argv, props, sizeof(props),
                                                   remaining < 1000 ? remaining : 1000, &status) || status != 0 ||
            strlen(props) >= sizeof(props) - 1) {
            result = BT_SOURCE_TRANSPORT_ERROR;
            goto done;
        }
        if (!dbus_prop_string_equals(props, "UUID", "0000110a-0000-1000-8000-00805f9b34fb")) continue;
        const char * value = dbus_prop_value(props, "Volume");
        unsigned raw;
        int consumed = 0;
        if (value) {
            if (sscanf(value, "uint16 %u%n", &raw, &consumed) == 1 && raw <= BT_SOURCE_VOLUME_MAX &&
                (value[consumed] == '\0' || isspace((unsigned char)value[consumed]))) {
                snprintf(out_transport, transport_size, "%s", transport);
                if (out_volume) *out_volume = (int) raw;
                result = BT_SOURCE_TRANSPORT_READY;
                break;
            }
            result = BT_SOURCE_TRANSPORT_ERROR;
        }
    }
done:
    free(reply);
    return result;
}

static bool bluealsa_find_source_transport(const char * pcm_path,
                                           char * out_transport, size_t transport_size,
                                           int * out_volume) {
    return bluealsa_find_source_transport_until(pcm_path, out_transport, transport_size,
                                                out_volume, bt_control_monotonic_ms() + 6500) ==
           BT_SOURCE_TRANSPORT_READY;
}

/* BlueALSA can report success after updating its PCM cache even when BlueZ's
 * remote AVRCP Set fails. In hardware-volume mode, check that request on the
 * exact transport before updating BlueALSA PCM state. BlueZ's volume property
 * can settle asynchronously or round the requested value; monitor events adopt
 * the headset's settled value. Software mode keeps the established PCM path. */
static bool bluealsa_write_source_volume(const char * path, int raw, bool software_volume) {
    if (raw < 0) raw = 0;
    if (raw > BT_SOURCE_VOLUME_MAX) raw = BT_SOURCE_VOLUME_MAX;
    if (!software_volume) {
        char transport[256];
        int current_raw = -1;
        if (!bluealsa_find_source_transport(path, transport, sizeof(transport), &current_raw)) return false;
        if (current_raw != raw) {
            char variant[32];
            snprintf(variant, sizeof(variant), "variant:uint16:%d", raw);
            char * set_argv[] = { "dbus-send", "--system", "--print-reply", "--dest=org.bluez",
                transport, "org.freedesktop.DBus.Properties.Set", "string:org.bluez.MediaTransport1",
                "string:Volume", variant, NULL };
            int set_status = -1;
            if (!subprocess_run_checked(set_argv, NULL, 0, 1000, &set_status) || set_status != 0)
                return false;
        }
    }
    char text[8];
    snprintf(text, sizeof(text), "%d", raw);
    char * argv[] = { (char *)bluealsa_ctl_name(), "volume", (char *)path, text, text, NULL };
    int status = -1;
    return subprocess_run_checked(argv, NULL, 0, 1000, &status) && status == 0;
}

/* Enabling SoftVolume resets the PCM level. Seed it before playback opens,
 * using the mode currently being selected rather than the temporary atomic
 * fallback value that remains true until the policy latch commits. */
static bool bluealsa_seed_source_volume(const char * path, bool software_volume) {
    float percent = audio_get_volume();
    int raw = (int)(percent * BT_SOURCE_VOLUME_MAX + 0.5f);
    return bluealsa_write_source_volume(path, raw, software_volume);
}

/* Negative capability is retried, but a live software-controlled PCM is
 * never switched to hardware gain across two subprocess calls mid-playback. */
static uint32_t bt_source_volume_probe_ms;
static char bt_source_readiness_attempted_path[256];
static unsigned int bt_source_readiness_attempted_generation;

static bool bluealsa_source_running_state_until(const char * path, bool * out_running,
                                                uint32_t deadline_ms) {
    char info[2048];
    char * argv[] = { (char *)bluealsa_ctl_name(), (char *)"info", (char *)path, NULL };
    uint32_t remaining = bt_source_probe_remaining_ms(deadline_ms);
    int exit_code = -1;
    if (!remaining || !subprocess_run_checked(argv, info, sizeof(info), remaining, &exit_code) || exit_code != 0)
        return false;
    if (strlen(info) >= sizeof(info) - 1) return false;
    const char * running = strstr(info, "Running:");
    if (!running) return false;
    running += strlen("Running:");
    while (*running == ' ' || *running == '\t') running++;
    if (strncmp(running, "false", 5) == 0 &&
        (running[5] == '\0' || isspace((unsigned char)running[5]))) {
        *out_running = false;
        return true;
    }
    if (strncmp(running, "true", 4) == 0 &&
        (running[4] == '\0' || isspace((unsigned char)running[4]))) {
        *out_running = true;
        return true;
    }
    return false;
}

#define BT_SOURCE_VOLUME_READINESS_MS 1500
#define BT_SOURCE_VOLUME_READINESS_POLL_MS 100

static void bluealsa_apply_soft_volume_cancellable_ex(const char * path, const atomic_bool * cancel,
                                                       bool force_source_probe) {
    bluealsa_backend_t backend = bluealsa_backend();
    if (!bluealsa_is_audio_pcm_path(path)) return;
    pthread_mutex_lock(&bt_soft_volume_serial);
    pthread_mutex_lock(&bt_soft_volume_mutex);
    bool source = strstr(path, "/a2dpsrc/sink") != NULL;
    uint32_t now = bt_control_monotonic_ms();
    bool prior_source_soft_volume = atomic_load(&bt_source_soft_volume);
    bool prior_ready = strcmp(bt_soft_volume_applied_path, path) == 0;
    bool retry = source && !atomic_load(&modern_soft_volume_requested) &&
                 prior_source_soft_volume &&
                 (force_source_probe || (uint32_t)(now - bt_source_volume_probe_ms) >= 5000);
    bool done = (strcmp(bt_soft_volume_applied_path, path) == 0 && !retry) ||
                (cancel && atomic_load(cancel));
    unsigned int clears = atomic_load_explicit(&bt_soft_volume_clears, memory_order_acquire);
    unsigned int readiness_generation = atomic_load_explicit(&bt_source_readiness_generation,
                                                              memory_order_acquire);
    if (!done) {
        /* Invalidate slow incoming events before changing or reseeding policy.
         * A reapply can change effective gain ownership without a PCM removal. */
        clears = atomic_fetch_add_explicit(&bt_soft_volume_clears, 1, memory_order_acq_rel) + 1;
        /* A failed retry must not leave an old readiness latch authorizing audio
         * after SoftVolume has reset the level to maximum. */
        if (strcmp(bt_soft_volume_applied_path, path) == 0)
            bt_soft_volume_applied_path[0] = '\0';
        if (source) atomic_store(&bt_source_soft_volume, true);
    }
    pthread_mutex_unlock(&bt_soft_volume_mutex);
    if (!done) {
        bool want = atomic_load(&modern_soft_volume_requested);
        bool readiness_cancelled = false;
        bool source_probe_failed = false;
        if (source && !want) {
            bt_source_volume_probe_ms = now;
            bool may_wait = force_source_probe && prior_source_soft_volume &&
                            (strcmp(bt_source_readiness_attempted_path, path) != 0 ||
                             bt_source_readiness_attempted_generation != readiness_generation);
            uint32_t readiness_deadline = now + BT_SOURCE_VOLUME_READINESS_MS;
            bt_source_transport_result_t capability = BT_SOURCE_TRANSPORT_ERROR;
            if (may_wait) {
                want = true;
                snprintf(bt_source_readiness_attempted_path, sizeof(bt_source_readiness_attempted_path), "%s", path);
                bt_source_readiness_attempted_generation = readiness_generation;
                do {
                    if ((cancel && atomic_load(cancel)) ||
                        readiness_generation != atomic_load_explicit(&bt_source_readiness_generation,
                                                                     memory_order_acquire)) {
                        readiness_cancelled = true;
                        break;
                    }
                    char transport[256];
                    capability = bluealsa_find_source_transport_until(path, transport, sizeof(transport), NULL,
                                                                       readiness_deadline);
                    if (capability == BT_SOURCE_TRANSPORT_READY) {
                        bool running = true;
                        if (bluealsa_source_running_state_until(path, &running, readiness_deadline) && !running) {
                            want = false;
                            DBG_LOG("bt_control: source volume readiness: hardware ready before open on %s\n", path);
                        } else {
                            want = true;
                            DBG_LOG("bt_control: source volume readiness: transport appeared while live or state unknown on %s; keeping software gain\n", path);
                        }
                        break;
                    }
                    uint32_t remaining = bt_source_probe_remaining_ms(readiness_deadline);
                    if (!remaining) break;
                    uint32_t sleep_ms = remaining < BT_SOURCE_VOLUME_READINESS_POLL_MS ?
                                        remaining : BT_SOURCE_VOLUME_READINESS_POLL_MS;
                    usleep(sleep_ms * 1000);
                } while (true);
                if (capability != BT_SOURCE_TRANSPORT_READY)
                    DBG_LOG("bt_control: source volume readiness: no hardware Volume within %ums on %s; keeping software gain\n",
                            BT_SOURCE_VOLUME_READINESS_MS, path);
            } else {
                char transport[256];
                uint32_t probe_deadline = force_source_probe ? now + 300 : now + 6500;
                capability = bluealsa_find_source_transport_until(path, transport, sizeof(transport), NULL,
                                                                   probe_deadline);
                want = capability != BT_SOURCE_TRANSPORT_READY;
                if (force_source_probe && !want)
                    DBG_LOG("bt_control: source volume readiness: immediate hardware recheck found Volume on %s\n", path);
            }
            source_probe_failed = want;
            if (!want && prior_source_soft_volume && !may_wait) {
                /* Unknown running state is treated as live. */
                bool running = true;
                uint32_t state_deadline = force_source_probe ? now + 300 : bt_control_monotonic_ms() + 1500;
                if (!bluealsa_source_running_state_until(path, &running, state_deadline) || running)
                    want = true;
            }
        }
        if (readiness_cancelled || (cancel && atomic_load(cancel)) ||
            (source && readiness_generation != atomic_load_explicit(&bt_source_readiness_generation,
                                                                     memory_order_acquire))) {
            pthread_mutex_unlock(&bt_soft_volume_serial);
            return;
        }
        bool current;
        bool ok;
        bool soft_volume_unchanged = bluealsa_read_soft_volume(path, &current) && current == want;
        if (soft_volume_unchanged) {
            ok = true;
        } else {
            char * argv[] = { (char *) backend.ctl, (char *) "soft-volume",
                              (char *) path, (char *) (want ? "on" : "off"), NULL };
            int exit_code = -1;
            ok = subprocess_run_checked(argv, NULL, 0, 5000, &exit_code) && exit_code == 0;
        }
        if (source && ok && !(prior_ready && source_probe_failed && prior_source_soft_volume &&
                              want && soft_volume_unchanged))
            ok = bluealsa_seed_source_volume(path, want);
        pthread_mutex_lock(&bt_soft_volume_mutex);
        if (ok && atomic_load_explicit(&bt_soft_volume_clears, memory_order_acquire) == clears) {
            snprintf(bt_soft_volume_applied_path, sizeof(bt_soft_volume_applied_path), "%s", path);
            if (source) atomic_store(&bt_source_soft_volume, want);
        }
        pthread_mutex_unlock(&bt_soft_volume_mutex);
    }
    pthread_mutex_unlock(&bt_soft_volume_serial);
}

static void bluealsa_apply_soft_volume_cancellable(const char * path, const atomic_bool * cancel) {
    bluealsa_apply_soft_volume_cancellable_ex(path, cancel, false);
}

static void bluealsa_apply_soft_volume(const char * path) {
    bluealsa_apply_soft_volume_cancellable(path, NULL);
}

/* Every clear also invalidates an application still in flight, whatever
 * its path, since that one has no latch yet to compare against; the cost of
 * a spurious invalidation is one idempotent soft-volume command later. */
static void bluealsa_clear_soft_volume_path(const char * path) {
    pthread_mutex_lock(&bt_soft_volume_mutex);
    bool clear = !path || strcmp(bt_soft_volume_applied_path, path) == 0;
    if (clear)
        bt_soft_volume_applied_path[0] = '\0';
    atomic_fetch_add_explicit(&bt_soft_volume_clears, 1, memory_order_acq_rel);
    bool invalidate_source = !path ||
        (strstr(path, "/a2dpsrc/sink") != NULL &&
         (!bt_soft_volume_applied_path[0] || clear));
    if (invalidate_source) {
        atomic_store(&bt_source_soft_volume, true);
        atomic_fetch_add_explicit(&bt_source_readiness_generation, 1, memory_order_acq_rel);
    }
    pthread_mutex_unlock(&bt_soft_volume_mutex);
}

/* Extracts the "Selected codec: X" line from `bluealsactl info` output.
 * Shared by the codec readback below and bt_control_get_connected_device_
 * codec() so the two cannot drift apart on the format. */
/* Display name for a "NAME[:hexblob]" codec field. BlueALSA reports SBC-XQ
 * as plain SBC, so it is labelled from the configuration itself: 44.1 kHz,
 * dual channel, 16 blocks, 8 subbands, loudness allocation -- exactly the
 * configuration sbc_a2dp_get_bitpool() gives the XQ bit-pool (which the
 * accessory's maximum may still cap). */
static void bluealsa_codec_display_name(const char * selected, char * out, size_t out_size) {
    const char * colon = strchr(selected, ':');
    size_t name_len = colon ? (size_t) (colon - selected) : strlen(selected);
    if (name_len >= out_size) name_len = out_size - 1;
    memcpy(out, selected, name_len);
    out[name_len] = '\0';
    if (strcasecmp(out, "samsung-SC") == 0) {
        snprintf(out, out_size, "SSC");
        return;
    }
    if (!colon || strcasecmp(out, "SBC") != 0) return;
    unsigned int b0 = 0, b1 = 0;
    if (sscanf(colon + 1, "%2x%2x", &b0, &b1) != 2) return;
    bool rate_44k1 = (b0 & 0xF0) == 0x20;
    bool dual = (b0 & 0x0F) == 0x04;
    bool blocks16_sb8_loud = b1 == (0x10 | 0x04 | 0x01);
    if (rate_44k1 && dual && blocks16_sb8_loud) snprintf(out, out_size, "SBC-XQ");
}

static bool bluealsa_parse_selected_codec(const char * info_out, char * out, size_t out_size) {
    const char * line = strstr(info_out, "Selected codec:");
    if (!line) return false;
    line += strlen("Selected codec:");
    while (*line == ' ') line++;
    size_t i = 0;
    while (line[i] != '\0' && line[i] != '\n' && line[i] != '\r' && i < out_size - 1) {
        out[i] = line[i];
        i++;
    }
    out[i] = '\0';
    return i != 0;
}

/* Applies the user's codec preference to a live PCM.
 *
 * Without this, the preference only ever reached BlueALSA through the
 * bluealsa:CODEC=... PCM name that audio_output.c opens (see
 * bt_control_get_playback_pcm()), so a freshly connected accessory kept
 * whatever BlueALSA negotiated on its own -- usually AAC -- until the next
 * time playback happened to reopen that PCM, i.e. a track change a couple
 * of songs later. That is GitHub issue #94's remaining half: manual
 * selection appeared to work only because changing it restarts the daemon
 * and the user then presses play.
 *
 * `bluealsactl codec` terminates the PCM if it is currently running (its
 * own documented behavior), which is exactly why this runs at PCMAdded --
 * the PCM exists but nothing has opened it for playback yet. A codec change
 * can itself cycle the PCM; that is already tolerated, since PCMRemoved
 * starts BT_OUTPUT_RECONFIGURE_GRACE_MS rather than reporting a disconnect,
 * and the re-add carries the same path (BlueALSA paths key on device and
 * profile, not codec), so the latch below suppresses a re-apply loop.
 *
 * Purely additive: if this fails, the PCM-name path still applies the codec
 * on the next open exactly as before, so the worst case is today's
 * behavior. */
static char bt_codec_applied_path[256];
static pthread_mutex_t bt_codec_applied_mutex = PTHREAD_MUTEX_INITIALIZER;

/* bt_codec_applied_mutex guards only the latch; see bt_codec_apply_serial. */
static unsigned int bt_codec_clears;

/* Called only on a confirmed disconnect or when the watch ends; like the
 * soft-volume clear it also invalidates a selection still in flight. */
static void bluealsa_clear_codec_path(const char * path) {
    pthread_mutex_lock(&bt_codec_applied_mutex);
    if (!path || strcmp(bt_codec_applied_path, path) == 0)
        bt_codec_applied_path[0] = '\0';
    bt_codec_clears++;
    pthread_mutex_unlock(&bt_codec_applied_mutex);
}

/* Defined further down alongside the volume-sync helpers; needed earlier by
 * bt_control_reconcile_source_settings()'s already-connected case. */
static bool find_source_pcm_path(char * out, size_t out_size);
static uint32_t bt_control_monotonic_ms(void);
/* Posts to the output watch's preference worker; false when none runs. */
static bool bt_pref_request(const char * path);

/* Applies an explicit transport rate other than 44.1 kHz, which the daemon
 * has no argument for. This goes through SelectCodec, and in BlueALSA 5 that
 * recreates the A2DP transport rather than reconfiguring it, so it costs a
 * brief re-handshake and the accessory is free to refuse it outright.
 *
 * Attempted at most once per PCM path per run, and deliberately NOT reset on
 * disconnect the way the codec preference is: a refusal can itself tear the
 * transport down, and retrying on every reconnect would turn that into an
 * endless connect/refuse/drop cycle. */
static void copy_info_field(char * dst, size_t size, const char * text, const char * key);

static bool bt_cancelled(const atomic_bool * cancel);

/* cancel (optional) is checked between its two subprocesses. Returns true
 * when it ran a rate reconfiguration (`codec -r`), successful or not: that
 * can replace the PCM and fall back to another codec, so the caller has to
 * verify the codec again. */
static void bt_auto_forget_switch(const char * path);

static void bt_rate_remember_refusal(const char * path, const char * codec, unsigned int rate) {
    pthread_mutex_lock(&bt_rate_applied_mutex);
    snprintf(bt_rate_refused_path, sizeof(bt_rate_refused_path), "%s", path);
    snprintf(bt_rate_refused_codec, sizeof(bt_rate_refused_codec), "%s", codec);
    bt_rate_refused_rate = rate;
    pthread_mutex_unlock(&bt_rate_applied_mutex);
}

static bool bluealsa_apply_rate_preference_cancellable(const char * path, const atomic_bool * cancel) {
    /* Automatic SSC negotiates from the profile selected by the daemon. An
     * explicit 44.1 kHz choice still needs a live transport check because
     * --ssc-auto-profile can select a higher SSC rate. */
    unsigned int want = atomic_load(&bt_sample_rate);
    if (want == 0) return false;
    if (!bluealsa_is_audio_pcm_path(path)) return false;
    unsigned int correction_generation = atomic_load_explicit(&bt_ssc_correction_generation, memory_order_acquire);

    bluealsa_backend_t backend = bluealsa_backend();
    char info_out[2048];
    char * info_argv[] = { (char *) backend.ctl, (char *) "info", (char *) path, NULL };
    if (!subprocess_run_low_priority(info_argv, info_out, sizeof(info_out))) return false;
    if (bt_cancelled(cancel)) return false;

    unsigned int current = 0;
    const char * rate_field = strstr(info_out, "Rate:");
    if (rate_field) (void) sscanf(rate_field + strlen("Rate:"), "%u", &current);
    if (current == want) return false; /* already negotiated there */

    char codec[32] = {0};
    copy_info_field(codec, sizeof(codec), info_out, "Selected codec:");
    char * codec_end = strchr(codec, ':'); /* v5 appends the configuration blob */
    if (codec_end) *codec_end = '\0';
    if (!codec[0]) return false;

    pthread_mutex_lock(&bt_rate_applied_mutex);
    bool refused = bt_rate_refused_rate == want && strcmp(bt_rate_refused_path, path) == 0 &&
                   strcasecmp(bt_rate_refused_codec, codec) == 0;
    pthread_mutex_unlock(&bt_rate_applied_mutex);
    if (refused) return false;

    if (want == 44100 &&
            (strcasecmp(codec, "samsung-SC") == 0 || strcasecmp(codec, "SSC") == 0)) {
        if (current != 0 && current != want) {
            char mac[18];
            const char * dev = strstr(path, "/dev_");
            if (dev && strlen(dev + 5) >= 17) {
                memcpy(mac, dev + 5, 17);
                mac[17] = '\0';
                for (size_t i = 0; i < 17; i++) if (mac[i] == '_') mac[i] = ':';
                pthread_mutex_lock(&bt_ssc_correction_mutex);
                if (correction_generation == atomic_load_explicit(&bt_ssc_correction_generation, memory_order_acquire) &&
                        atomic_load(&bt_sample_rate) == want) {
                    if (!bt_ssc_correction_mac[0] ||
                            (strcmp(bt_ssc_correction_mac, mac) == 0 && bt_ssc_correction_rate == want)) {
                        snprintf(bt_ssc_correction_mac, sizeof(bt_ssc_correction_mac), "%s", mac);
                        bt_ssc_correction_rate = want;
                        snprintf(bt_ssc_correction_path, sizeof(bt_ssc_correction_path), "%s", path);
                        snprintf(bt_ssc_correction_codec, sizeof(bt_ssc_correction_codec), "%s", codec);
                    }
                }
                pthread_mutex_unlock(&bt_ssc_correction_mutex);
            }
        }
        /* These SSC peers refuse live SelectCodec and disconnect. The GUI
         * claims the target-bound correction in its existing reconnect poll. */
        return false;
    }

    /* The link is about to be re-established for the rate: a drop from here
     * on is not the codec's fault, so Automatic must not blame it. */
    bt_auto_forget_switch(path);
    char rate_str[16];
    snprintf(rate_str, sizeof(rate_str), "%u", want);
    char * argv[] = { (char *) backend.ctl, (char *) "codec", (char *) "-r", rate_str,
                      (char *) path, codec, NULL };
    int exit_code = -1;
    if (!subprocess_run_checked(argv, NULL, 0, 5000, &exit_code) || exit_code != 0) {
        DBG_LOG("bt_control: rate %u refused for %s on %s\n", want, path, codec);
        bt_rate_remember_refusal(path, codec, want);
        return true;
    }
    /* Accepted is not the same as in effect: an accessory can settle on
     * another rate. Remember that as a refusal too, so the rate is not
     * requested (and the codec re-verified) again and again. */
    if (!bt_cancelled(cancel) && subprocess_run_low_priority(info_argv, info_out, sizeof(info_out))) {
        unsigned int after = 0;
        rate_field = strstr(info_out, "Rate:");
        if (rate_field) (void) sscanf(rate_field + strlen("Rate:"), "%u", &after);
        if (after != want) {
            DBG_LOG("bt_control: rate %u not kept for %s on %s (now %u)\n", want, path, codec, after);
            bt_rate_remember_refusal(path, codec, want);
        }
    }
    return true;
}

static void bluealsa_apply_rate_preference(const char * path) {
    (void) bluealsa_apply_rate_preference_cancellable(path, NULL);
}

/* Automatic picks the best codec both sides support, in the order the stock
 * HiBy player documents: LDAC > samsung-SC > aptX HD > aptX > AAC > SBC. Left to itself,
 * BlueZ takes the first endpoint in an order that does not follow quality
 * (and then keeps its LastUsed one), so LDAC headphones could stay on AAC. */
static const char * const BT_AUTO_CODEC_RANKING[] = { "LDAC", "samsung-SC", "aptX-HD", "aptX", "AAC", "SBC" };
static pthread_mutex_t bt_buds_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static char bt_buds_state_path[256];
static bool bt_buds_name_checked, bt_buds_name_match;
static bt_ssc_signal_t bt_buds_signal;
static uint32_t bt_buds_wait_started;
static unsigned int bt_buds_generation;
#define BT_BUDS_SSC_DISCOVERY_GRACE_MS 4000u

static bool bluealsa_available_codec_contains(const char * info_out, const char * codec) {
    if (!info_out || !codec) return false;
    const char * line = strstr(info_out, "Available codecs:");
    if (!line) return false;
    line += strlen("Available codecs:");
    const char * eol = strchr(line, '\n');
    size_t len = eol ? (size_t)(eol - line) : strlen(line);
    char list[512];
    if (len >= sizeof(list)) len = sizeof(list) - 1;
    memcpy(list, line, len);
    list[len] = '\0';
    char * save = NULL;
    for (char * tok = strtok_r(list, " \t\r", &save); tok;
         tok = strtok_r(NULL, " \t\r", &save))
        if (strcasecmp(tok, codec) == 0) return true;
    return false;
}

static bool bluealsa_available_codecs_known(const char * info_out) {
    for (size_t i = 0; i < sizeof(BT_AUTO_CODEC_RANKING) / sizeof(BT_AUTO_CODEC_RANKING[0]); i++)
        if (bluealsa_available_codec_contains(info_out, BT_AUTO_CODEC_RANKING[i])) return true;
    return false;
}

/* SSC selection is serialized by the PCMAdded preference worker. Keep ALSA
 * from issuing a second codec change from a stale asoundrc snapshot. */
static const char * bt_playback_codec_override(void) {
    if ((bt_codec_preference_t) atomic_load(&bt_codec_preference) == BT_CODEC_SSC)
        return NULL;
    return bt_codec_select_name();
}

/* From `bluealsactl info` output: the highest-ranked entry of "Available
 * codecs:" (codecs the accessory advertises that this daemon has enabled).
 * False while the list is unknown, e.g. before the accessory's endpoints
 * have been published. */
static bool bluealsa_best_available_codec(const char * info_out, unsigned int rejected,
                                          char * out, size_t out_size) {
    if (!bluealsa_available_codecs_known(info_out)) return false;
    for (size_t r = 0; r < sizeof(BT_AUTO_CODEC_RANKING) / sizeof(BT_AUTO_CODEC_RANKING[0]); r++) {
        if (rejected & (1u << r)) continue;
        if (bluealsa_available_codec_contains(info_out, BT_AUTO_CODEC_RANKING[r])) {
            snprintf(out, out_size, "%s", BT_AUTO_CODEC_RANKING[r]);
            return true;
        }
    }
    return false;
}

/* Match known Samsung Buds model tokens. Aliases may put an owner's name
 * before Galaxy or after a numbered model. */
static bool bt_is_galaxy_buds_name(const char * name) {
    if (!name) return false;
    char normalized[128];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && n < sizeof(normalized)-1; p++) {
        if (*p == '+' && n + 5 < sizeof(normalized)) {
            memcpy(normalized + n, " plus", 5); n += 5;
        } else normalized[n++] = (char)(isalnum(*p) ? tolower(*p) : ' ');
    }
    normalized[n] = '\0';
    char *tokens[8], *save = NULL;
    size_t count = 0;
    for (char *tok = strtok_r(normalized, " ", &save); tok && count < 8;
         tok = strtok_r(NULL, " ", &save)) tokens[count++] = tok;
    bool galaxy = false;
    size_t i = 0;
    for (; i < count; i++) {
        if (strcmp(tokens[i], "galaxy") == 0) { galaxy = true; i++; break; }
    }
    if (!galaxy) {
        for (i = 0; i < count && strncmp(tokens[i], "buds", 4) != 0; i++) {}
    }
    if (i >= count) return false;
    char *model = tokens[i++];
    if (strncmp(model, "buds", 4) != 0) return false;
    model += 4;
    if (!*model) {
        if (i >= count) return galaxy;
        model = tokens[i++];
    }
    if (!*model) return galaxy; /* Original family name requires Galaxy. */
    if (strcmp(model, "plus") == 0 || strcmp(model, "live") == 0 ||
        strcmp(model, "pro") == 0 || strcmp(model, "fe") == 0 || strcmp(model, "core") == 0)
        return true;
    if (galaxy && (strcmp(model, "de") == 0 || strcmp(model, "of") == 0 || strcmp(model, "della") == 0))
        return true;
    if (strcmp(model, "1") == 0 || strcmp(model, "2") == 0 || strcmp(model, "3") == 0 ||
        strcmp(model, "4") == 0) {
        return true; /* Model number is decisive; remaining tokens are suffix/owner. */
    }
    if (model[0] >= '1' && model[0] <= '4' &&
        (strcmp(model + 1, "pro") == 0 || strcmp(model + 1, "fe") == 0 ||
         strcmp(model + 1, "live") == 0)) return true;
    return false;
}

/* The device's name, Modalias and connection state as `bluetoothctl info`
 * lines, read with one D-Bus call: bluetoothctl loads BlueZ's whole object
 * tree and registers an advertisement monitor on every start, which is
 * heavy next to a Bluetooth stream being set up. */
static bool bt_device_info_text(const char * mac, char * out, size_t size) {
    char object[64], dbus[8192];
    snprintf(object, sizeof(object), "/org/bluez/hci0/dev_%s", mac);
    for (char * c = object; *c; c++) if (*c == ':') *c = '_';
    char * argv[] = { (char *) "dbus-send", (char *) "--system", (char *) "--print-reply",
                      (char *) "--dest=org.bluez", object, (char *) "org.freedesktop.DBus.Properties.GetAll",
                      (char *) "string:org.bluez.Device1", NULL };
    int status = -1;
    if (!subprocess_run_checked(argv, dbus, sizeof(dbus), 1000, &status) || status != 0) return false;
    bt_ssc_policy_dbus_to_info(dbus, out, size);
    return out[0] != '\0';
}

/* Whether the device is probably SSC-capable although its SSC endpoint is
 * not offered (yet): Samsung Modalias, SSC remembered for it, then the name
 * (Galaxy Buds). The endpoint itself is checked by the caller. */
static bool bt_pcm_is_galaxy_buds(const char * path, bool *known, bt_ssc_signal_t *signal_out,
                                  const atomic_bool *cancel) {
    *known = false;
    *signal_out = BT_SSC_SIGNAL_NONE;
    const char * dev = path ? strstr(path, "/dev_") : NULL;
    if (!dev) { *known = true; return false; }
    dev += 5;
    char mac[18];
    if (strlen(dev) < 17 || dev[17] != '/') { *known = true; return false; }
    for (int i = 0; i < 17; i++) mac[i] = dev[i] == '_' ? ':' : dev[i];
    mac[17] = '\0';
    char info[1024];
    if (bt_cancelled(cancel)) return false;
    if (!bt_device_info_text(mac, info, sizeof(info))) return false;
    if (bt_cancelled(cancel)) return false;
    *known = true;
    bool by_modalias = bt_ssc_policy_modalias_is_samsung(info);
    bool by_memory = bt_ssc_memory_worked(mac);
    bool by_name = false;
    const char *keys[] = { "Alias:", "Name:" };
    for (size_t k = 0; k < 2 && !by_name; k++) {
        const char *p = strstr(info, keys[k]);
        if (!p) continue;
        p += strlen(keys[k]);
        while (*p == ' ' || *p == '\t') p++;
        char name[128]; size_t i = 0;
        while (*p && *p != '\n' && *p != '\r' && i + 1 < sizeof(name)) name[i++] = *p++;
        name[i] = '\0';
        by_name = bt_is_galaxy_buds_name(name);
    }
    /* Another vendor's Modalias (Pixel, OnePlus, Redmi Buds) rules the name out. */
    if (by_name && bt_ssc_policy_modalias_excludes_name(info)) by_name = false;
    bt_ssc_signal_t signal = bt_ssc_policy_signal(false, by_modalias, by_memory, by_name);
    *signal_out = signal;
    if (signal != BT_SSC_SIGNAL_NONE)
        DBG_LOG("bt_control: %s looks SSC-capable (%s)\n", mac, bt_ssc_policy_signal_name(signal));
    return signal != BT_SSC_SIGNAL_NONE;
}

static void bt_buds_state(const char *path, bool *is_buds, bool *metadata_known,
                          uint32_t *wait_started, bt_ssc_signal_t *signal, const atomic_bool *cancel) {
    pthread_mutex_lock(&bt_buds_state_mutex);
    if (strcmp(bt_buds_state_path, path) != 0) {
        bt_buds_generation++;
        snprintf(bt_buds_state_path, sizeof(bt_buds_state_path), "%s", path);
        bt_buds_name_checked = false;
        bt_buds_name_match = false;
        bt_buds_signal = BT_SSC_SIGNAL_NONE;
        bt_buds_wait_started = bt_control_monotonic_ms();
    }
    bool need_check = !bt_buds_name_checked;
    unsigned int generation = bt_buds_generation;
    pthread_mutex_unlock(&bt_buds_state_mutex);
    bool matched = false, known = false;
    bt_ssc_signal_t found = BT_SSC_SIGNAL_NONE;
    if (need_check) matched = bt_pcm_is_galaxy_buds(path, &known, &found, cancel);
    pthread_mutex_lock(&bt_buds_state_mutex);
    if (need_check && known && bt_buds_generation == generation &&
        strcmp(bt_buds_state_path, path) == 0) {
        bt_buds_name_match = matched;
        bt_buds_signal = found;
        bt_buds_name_checked = true;
    }
    *is_buds = bt_buds_name_match;
    *signal = bt_buds_signal;
    *metadata_known = bt_buds_name_checked;
    *wait_started = bt_buds_wait_started;
    pthread_mutex_unlock(&bt_buds_state_mutex);
}

static void bt_buds_reset(const char *path) {
    pthread_mutex_lock(&bt_buds_state_mutex);
    if (!path || strcmp(bt_buds_state_path, path) == 0) {
        bt_buds_state_path[0] = '\0';
        bt_buds_generation++;
        bt_buds_name_checked = false;
        bt_buds_name_match = false;
        bt_buds_signal = BT_SSC_SIGNAL_NONE;
        bt_buds_wait_started = 0;
    }
    pthread_mutex_unlock(&bt_buds_state_mutex);
}

/* Automatic remembers, per accessory and for this session, codecs it could
 * not use: a selection that failed or was not in effect afterwards, or one
 * followed by a disconnect within BT_AUTO_REJECT_WINDOW_MS (a device that
 * advertises a codec but drops the link on it). Automatic then settles on
 * the next one down instead of retrying it on every reconnect. SBC, the
 * mandatory floor, is never rejected. */
#define BT_AUTO_REJECT_DEVICES 8
#define BT_AUTO_REJECT_WINDOW_MS 10000u
static struct {
    char device[32];     /* "dev_XX_XX_XX_XX_XX_XX" from the PCM path */
    unsigned int rejected;
    int switched;        /* ranking index of the last switch, or -1 */
    uint32_t switched_at;
    bool ssc_discovery_attempted; /* one automatic recovery per connection */
    bt_ssc_dev_t ssc;    /* switch / reconfigure / backoff state, bt_ssc_policy.h */
    uint8_t cycles[8];   /* switch-then-drop cycles per ranking index, this session */
} bt_auto_devices[BT_AUTO_REJECT_DEVICES];
static int bt_auto_devices_next;
static pthread_mutex_t bt_auto_devices_mutex = PTHREAD_MUTEX_INITIALIZER;

static int bt_auto_rank_index(const char * codec) {
    for (size_t r = 0; r < sizeof(BT_AUTO_CODEC_RANKING) / sizeof(BT_AUTO_CODEC_RANKING[0]); r++)
        if (strcasecmp(codec, BT_AUTO_CODEC_RANKING[r]) == 0) return (int) r;
    return -1;
}

static bool bt_auto_device_key(const char * path, char * out, size_t out_size) {
    const char * dev = path ? strstr(path, "/dev_") : NULL;
    if (!dev) return false;
    dev++;
    size_t len = strcspn(dev, "/");
    if (len == 0 || len >= out_size) return false;
    memcpy(out, dev, len);
    out[len] = '\0';
    return true;
}

static bool bt_auto_mac_from_path(const char * path, char * mac, size_t mac_size) {
    char key[32];
    if (mac_size < 18 || !bt_auto_device_key(path, key, sizeof(key)) || strlen(key) != 21) return false;
    for (int j = 0; j < 17; j++) mac[j] = key[j + 4] == '_' ? ':' : key[j + 4];
    mac[17] = '\0';
    return true;
}

/* Caller holds bt_auto_devices_mutex. */
static int bt_auto_device_slot_locked(const char * path, bool create) {
    char key[32];
    if (!bt_auto_device_key(path, key, sizeof(key))) return -1;
    for (int i = 0; i < BT_AUTO_REJECT_DEVICES; i++)
        if (strcmp(bt_auto_devices[i].device, key) == 0) return i;
    if (!create) return -1;
    int i = bt_auto_devices_next;
    bt_auto_devices_next = (bt_auto_devices_next + 1) % BT_AUTO_REJECT_DEVICES;
    snprintf(bt_auto_devices[i].device, sizeof(bt_auto_devices[i].device), "%s", key);
    bt_auto_devices[i].rejected = 0;
    bt_auto_devices[i].switched = -1;
    bt_auto_devices[i].ssc_discovery_attempted = false;
    memset(&bt_auto_devices[i].ssc, 0, sizeof(bt_auto_devices[i].ssc));
    memset(bt_auto_devices[i].cycles, 0, sizeof(bt_auto_devices[i].cycles));
    return i;
}

/* Counts disconnects asked for through bt_control_disconnect, so a recovery
 * that is cancelled can tell the user's disconnect from other cancels. */
static atomic_uint bt_user_disconnect_generation;
static pthread_mutex_t bt_ssc_discovery_mutex = PTHREAD_MUTEX_INITIALIZER;
static char bt_ssc_discovery_mac[18];
static bool bt_ssc_discovery_claimed;
static uint32_t bt_ssc_discovery_queued_at;

/* Explicit SSC and Automatic both recover a missing SSC endpoint. */
static bool bt_ssc_recovery_preference(void) {
    bt_codec_preference_t p = (bt_codec_preference_t) atomic_load(&bt_codec_preference);
    return p == BT_CODEC_SSC || p == BT_CODEC_AUTO;
}

void bt_control_ssc_discovery_recovery_cancel(void) {
    pthread_mutex_lock(&bt_ssc_discovery_mutex);
    bt_ssc_discovery_mac[0] = '\0';
    bt_ssc_discovery_claimed = false;
    pthread_mutex_unlock(&bt_ssc_discovery_mutex);
}

bool bt_control_ssc_discovery_recovery_peek(char * mac, size_t mac_size) {
    if (!mac || mac_size < sizeof(bt_ssc_discovery_mac)) return false;
    pthread_mutex_lock(&bt_ssc_discovery_mutex);
    bool pending = bt_ssc_discovery_mac[0] && !bt_ssc_discovery_claimed;
    if (pending) snprintf(mac, mac_size, "%s", bt_ssc_discovery_mac);
    pthread_mutex_unlock(&bt_ssc_discovery_mutex);
    return pending;
}

bool bt_control_ssc_discovery_recovery_take(const char * mac) {
    pthread_mutex_lock(&bt_ssc_discovery_mutex);
    bool pending = mac && bt_ssc_discovery_mac[0] && !bt_ssc_discovery_claimed &&
                   strcmp(mac, bt_ssc_discovery_mac) == 0 &&
                   bt_ssc_recovery_preference();
    if (pending) bt_ssc_discovery_claimed = true;
    pthread_mutex_unlock(&bt_ssc_discovery_mutex);
    return pending;
}

void bt_control_ssc_discovery_recovery_finish(const char * mac) {
    pthread_mutex_lock(&bt_ssc_discovery_mutex);
    if (mac && bt_ssc_discovery_claimed && strcmp(mac, bt_ssc_discovery_mac) == 0) {
        bt_ssc_discovery_mac[0] = '\0';
        bt_ssc_discovery_claimed = false;
    }
    pthread_mutex_unlock(&bt_ssc_discovery_mutex);
}

/* The preference worker only posts work. The GUI applies its usual manual
 * operation/power guards before the separate reconnect worker claims it.
 * Consume the attempt when queued, so a cancelled or failed recovery cannot
 * re-arm itself through a PCM removal or another discovery pass. */
static bool bt_ssc_discovery_recovery_queue(const char * path) {
    char mac[18];
    if (!bt_auto_mac_from_path(path, mac, sizeof(mac))) return false;
    pthread_mutex_lock(&bt_ssc_discovery_mutex);
    if (bt_ssc_discovery_mac[0]) {
        bool waiting = strcmp(bt_ssc_discovery_mac, mac) == 0;
        /* A recovery nothing picks up must not hold the worker polling. */
        if (waiting && !bt_ssc_discovery_claimed &&
            bt_control_monotonic_ms() - bt_ssc_discovery_queued_at >= BT_SSC_RECOVERY_WAIT_CAP_MS) {
            bt_ssc_discovery_mac[0] = '\0';
            waiting = false;
            DBG_LOG("bt_control: SSC endpoint recovery for %s not taken up, giving up\n", mac);
        }
        pthread_mutex_unlock(&bt_ssc_discovery_mutex);
        return waiting;
    }
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, true);
    bool queue = i >= 0 && !bt_auto_devices[i].ssc_discovery_attempted;
    if (queue) {
        bt_auto_devices[i].ssc_discovery_attempted = true;
        snprintf(bt_ssc_discovery_mac, sizeof(bt_ssc_discovery_mac), "%s", mac);
        bt_ssc_discovery_claimed = false;
        bt_ssc_discovery_queued_at = bt_control_monotonic_ms();
    }
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    pthread_mutex_unlock(&bt_ssc_discovery_mutex);
    if (queue) DBG_LOG("bt_control: queued one SSC endpoint recovery for %s\n", mac);
    return queue;
}

static unsigned int bt_auto_rejected(const char * path) {
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, false);
    unsigned int rejected = i >= 0 ? bt_auto_devices[i].rejected : 0;
    /* Persisted backoff: SSC is skipped on this connection only. */
    if (i >= 0 && bt_auto_devices[i].ssc.skip_ssc) rejected |= 1u << bt_auto_rank_index("samsung-SC");
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    return rejected;
}

/* Rejections of this session only, without the persisted backoff. */
static unsigned int bt_auto_rejected_session(const char * path) {
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, false);
    unsigned int rejected = i >= 0 ? bt_auto_devices[i].rejected : 0;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    return rejected;
}

static void bt_auto_reject(const char * path, const char * codec) {
    int r = bt_auto_rank_index(codec);
    if (r < 0 || strcasecmp(codec, "SBC") == 0) return;
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, true);
    if (i >= 0) bt_auto_devices[i].rejected |= 1u << r;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    DBG_LOG("bt_control: auto codec: %s rejected for %s\n", codec, path);
}

static void bt_auto_note_switch(const char * path, const char * codec) {
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, true);
    if (i >= 0) {
        bt_auto_devices[i].switched = bt_auto_rank_index(codec);
        bt_auto_devices[i].switched_at = bt_control_monotonic_ms();
    }
    pthread_mutex_unlock(&bt_auto_devices_mutex);
}

/* Link changes that are not the codec's fault: a rate reconfiguration after
 * the switch, or a disconnect the player itself asked for. */
static void bt_auto_forget_switch(const char * path) {
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, false);
    if (i >= 0) {
        bt_auto_devices[i].switched = -1;
        bt_ssc_dev_switch_abort(&bt_auto_devices[i].ssc);
    }
    pthread_mutex_unlock(&bt_auto_devices_mutex);
}

/* A confirmed disconnect shortly after a switch. With BlueZ reporting the
 * device itself gone and audio not yet flowing it blames the codec (an SSC
 * blame is also remembered across restarts). With the link not shown down
 * it only counts a switch-then-drop cycle, and BT_SSC_CYCLE_CAP of them
 * reject the codec for this session, which stops a select/drop loop. */
static void bt_auto_note_disconnect_at(const char * path, uint32_t dropped_at, bt_ssc_link_t link) {
    char codec[16] = "";
    bool persist = false;
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, false);
    if (i >= 0 && bt_auto_devices[i].switched >= 0 &&
        bt_control_monotonic_ms() - bt_auto_devices[i].switched_at < BT_AUTO_REJECT_WINDOW_MS &&
        !bt_ssc_dev_flowing(&bt_auto_devices[i].ssc, dropped_at)) {
        int r = bt_auto_devices[i].switched;
        if (bt_ssc_dev_blames_codec(&bt_auto_devices[i].ssc, dropped_at, link)) {
            snprintf(codec, sizeof(codec), "%s", BT_AUTO_CODEC_RANKING[r]);
            persist = r == bt_auto_rank_index("samsung-SC");
        } else if (++bt_auto_devices[i].cycles[r] >= BT_SSC_CYCLE_CAP) {
            snprintf(codec, sizeof(codec), "%s", BT_AUTO_CODEC_RANKING[r]);
        }
    }
    if (i >= 0) bt_auto_devices[i].switched = -1;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    if (codec[0]) bt_auto_reject(path, codec);
    char mac[18];
    if (persist && bt_auto_mac_from_path(path, mac, sizeof(mac))) bt_ssc_memory_note_failure(mac);
}

/* The link is known to be down (the common case, and what the tests use). */
static void __attribute__((unused)) bt_auto_note_disconnect(const char * path) {
    bt_auto_note_disconnect_at(path, bt_control_monotonic_ms(), BT_SSC_LINK_DISCONNECTED);
}

/* Switch bookkeeping for the reconfigure-versus-disconnect decision. */
static void bt_auto_switch_begin(const char * path) {
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, true);
    if (i >= 0) bt_ssc_dev_switch_begin(&bt_auto_devices[i].ssc, bt_control_monotonic_ms());
    pthread_mutex_unlock(&bt_auto_devices_mutex);
}

static void bt_auto_switch_abort(const char * path) {
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, false);
    if (i >= 0) bt_ssc_dev_switch_abort(&bt_auto_devices[i].ssc);
    pthread_mutex_unlock(&bt_auto_devices_mutex);
}

static void bt_auto_pcm_added(const char * path) {
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, true);
    if (i >= 0) bt_ssc_dev_pcm_added(&bt_auto_devices[i].ssc, bt_control_monotonic_ms());
    pthread_mutex_unlock(&bt_auto_devices_mutex);
}

static void bt_auto_switch_confirmed(const char * path, const char * codec) {
    bool ssc = strcasecmp(codec, "samsung-SC") == 0;
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, true);
    if (i >= 0) bt_ssc_dev_confirmed(&bt_auto_devices[i].ssc, bt_control_monotonic_ms(), ssc);
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    char mac[18];
    if (ssc && bt_auto_mac_from_path(path, mac, sizeof(mac))) bt_ssc_memory_note_worked(mac);
}

static uint32_t bt_output_watch_started_at; /* 0 until a watch has run */

/* 0 when a codec command may run now; otherwise the time to try again: a
 * recovery that claimed this device owns the link, and a PCM younger than
 * BT_SSC_STREAM_OPEN_MS may not have an open stream yet. */
static uint32_t bt_auto_codec_command_ready_at(const char * path) {
    uint32_t now = bt_control_monotonic_ms(), ready = 0;
    char mac[18];
    pthread_mutex_lock(&bt_ssc_discovery_mutex);
    if (bt_auto_mac_from_path(path, mac, sizeof(mac)) && bt_ssc_discovery_claimed &&
        strcmp(bt_ssc_discovery_mac, mac) == 0)
        ready = now + 500;
    pthread_mutex_unlock(&bt_ssc_discovery_mutex);
    /* ... or a rate correction, pending for this device or running. */
    unsigned int rate = 0;
    char pending[18];
    if (atomic_load(&bt_ssc_correction_running) > 0 ||
        (bt_control_ssc_rate_correction_peek(pending, sizeof(pending), &rate) && bt_auto_mac_from_path(path, mac, sizeof(mac)) &&
         strcmp(pending, mac) == 0))
        ready = now + 500;
    if (ready) return ready;
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, false);
    uint32_t open_at = i >= 0 ? bt_ssc_dev_stream_open_at(&bt_auto_devices[i].ssc) : 0;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    /* A PCM that predates the watch (found by the startup scan) is aged from
     * the moment the watch started. */
    if (!open_at && bt_output_watch_started_at) open_at = bt_output_watch_started_at + BT_SSC_STREAM_OPEN_MS;
    return open_at && (int32_t)(open_at - now) > 0 ? open_at : 0;
}

/* Absolute time a PCM removal must be held back until because a codec
 * switch is still in flight, or 0 for the normal grace. */
static uint32_t bt_auto_reconfigure_deadline(const char * path, uint32_t removed_at) {
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, false);
    uint32_t until = i >= 0 ? bt_ssc_dev_reconfigure_deadline(&bt_auto_devices[i].ssc, removed_at) : 0;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    return until;
}

/* Starts a connection for the persisted backoff, once per connection. */
static void bt_auto_connection_begin(const char * path, bool automatic) {
    char mac[18];
    /* The persisted backoff is for Automatic only; explicit SSC is the
     * owner's choice and always tries. */
    if (!automatic || !bt_auto_mac_from_path(path, mac, sizeof(mac))) return;
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, true);
    bool first = i >= 0 && !bt_auto_devices[i].ssc.conn_started;
    if (first) bt_auto_devices[i].ssc.conn_started = true;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    if (!first) return;
    bool skip = bt_ssc_memory_connection_begin(mac);
    if (skip) DBG_LOG("bt_control: SSC backed off for %s on this connection\n", mac);
    pthread_mutex_lock(&bt_auto_devices_mutex);
    i = bt_auto_device_slot_locked(path, false);
    if (i >= 0) bt_auto_devices[i].ssc.skip_ssc = skip;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
}

/* A given-up discovery recovery counts once per connection towards backoff. */
static void bt_auto_endpoint_failure(const char * path) {
    char mac[18];
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, false);
    bool count = i >= 0 && bt_auto_devices[i].ssc_discovery_attempted &&
                 !bt_auto_devices[i].ssc.endpoint_failure_counted;
    if (count) bt_auto_devices[i].ssc.endpoint_failure_counted = true;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    if (count && bt_auto_mac_from_path(path, mac, sizeof(mac))) bt_ssc_memory_note_failure(mac);
}

static bt_ssc_link_t bt_device_link_state(const char * path) {
    char mac[18];
    char info[1024];
    if (!bt_auto_mac_from_path(path, mac, sizeof(mac)) || !bt_device_info_text(mac, info, sizeof(info)))
        return BT_SSC_LINK_UNKNOWN;
    return bt_ssc_policy_link_state(info);
}

/* A disconnect of this PCM is confirmed (the grace, extended while a switch
 * was in flight, ran out). Only an ACL loss reported by BlueZ right after a
 * switch, before audio was flowing, blames the codec; a transport that just
 * did not come back does not. Then the connection's own state ends: a
 * successful SSC session lifts the backoff, a queued recovery is cancelled,
 * and the next connection may use its one recovery. */
static void bt_auto_drop_confirmed(const char * path, uint32_t removed_at) {
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, false);
    bool recent = i >= 0 && bt_auto_devices[i].switched >= 0 &&
                  bt_control_monotonic_ms() - bt_auto_devices[i].switched_at < BT_AUTO_REJECT_WINDOW_MS;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    bt_auto_note_disconnect_at(path, removed_at,
                               recent ? bt_device_link_state(path) : BT_SSC_LINK_UNKNOWN);

    char mac[18];
    bool have_mac = bt_auto_mac_from_path(path, mac, sizeof(mac));
    pthread_mutex_lock(&bt_ssc_discovery_mutex);
    bool recovery_mine = have_mac && bt_ssc_discovery_mac[0] && strcmp(bt_ssc_discovery_mac, mac) == 0;
    bool recovery_running = recovery_mine && bt_ssc_discovery_claimed;
    if (recovery_mine && !recovery_running) bt_ssc_discovery_mac[0] = '\0';
    pthread_mutex_lock(&bt_auto_devices_mutex);
    i = bt_auto_device_slot_locked(path, false);
    bool session_ok = false;
    if (i >= 0) {
        session_ok = bt_auto_devices[i].ssc.confirmed_ssc &&
                     bt_ssc_dev_flowing(&bt_auto_devices[i].ssc, removed_at);
        /* Our own recovery or rate-correction reconnect is not a new
         * connection: it keeps the backoff bookkeeping and its one recovery. */
        uint32_t now = bt_control_monotonic_ms();
        bool own = recovery_running || (bt_auto_devices[i].ssc.keep_until &&
                                        (int32_t)(bt_auto_devices[i].ssc.keep_until - now) > 0);
        bt_ssc_dev_connection_end(&bt_auto_devices[i].ssc, own);
        if (!own) bt_auto_devices[i].ssc_discovery_attempted = false;
    }
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    pthread_mutex_unlock(&bt_ssc_discovery_mutex);
    if (session_ok && have_mac) bt_ssc_memory_note_session_ok(mac);
}

/* The output watch ended with a removal still pending (the lifecycle worker
 * stops it once the route is gone): run the connection's end anyway, so the
 * blame (which still needs BlueZ to show the device gone), the recovery
 * cancel and the per-connection resets are not lost. */
static void bt_output_disconnect_finish_pending(const char * pending_path, uint32_t removed_at) {
    if (!pending_path[0]) return;
    bt_auto_drop_confirmed(pending_path, removed_at);
    bluealsa_clear_codec_path(pending_path);
}

/* Our own reconnect (recovery, rate correction) is about to drop the PCM;
 * a drop in the next 30 s belongs to the same connection. */
static void bt_auto_expect_reconnect(const char * path) {
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, true);
    if (i >= 0) bt_auto_devices[i].ssc.keep_until = bt_control_monotonic_ms() + 30000u;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
}

/* A flowing SSC session lifts the persisted backoff as soon as it has
 * flowed long enough, not only when it ends. */
static void bt_auto_flowing_tick(void) {
    char macs[BT_AUTO_REJECT_DEVICES][18];
    int n = 0;
    uint32_t now = bt_control_monotonic_ms();
    pthread_mutex_lock(&bt_auto_devices_mutex);
    for (int i = 0; i < BT_AUTO_REJECT_DEVICES; i++) {
        bt_ssc_dev_t * d = &bt_auto_devices[i].ssc;
        if (!bt_auto_devices[i].device[0] || !d->confirmed_ssc || d->session_noted ||
            !bt_ssc_dev_flowing(d, now)) continue;
        char path[64];
        snprintf(path, sizeof(path), "/%.31s/", bt_auto_devices[i].device);
        if (!bt_auto_mac_from_path(path, macs[n], sizeof(macs[n]))) continue;
        d->session_noted = true;
        n++;
    }
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    for (int i = 0; i < n; i++) bt_ssc_memory_note_session_ok(macs[i]);
}

/* Milliseconds a removed PCM should still be waited for because a codec
 * switch of ours is in flight; 0 when none is. The GUI's own absence
 * recheck uses this so it does not clear the route mid-reconfigure. */
uint32_t bt_control_reconfigure_hold_ms(void) {
    uint32_t now = bt_control_monotonic_ms(), hold = 0;
    pthread_mutex_lock(&bt_auto_devices_mutex);
    for (int i = 0; i < BT_AUTO_REJECT_DEVICES; i++) {
        uint32_t until = bt_ssc_dev_reconfigure_deadline(&bt_auto_devices[i].ssc, now);
        if (until && (int32_t)(until - now) > 0 && until - now > hold) hold = until - now;
    }
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    return hold;
}

/* One codec selection at a time, across every caller (the output watch's
 * preference worker, reconciliation): taken for the whole sequence, never
 * by event handling. The latch is recorded only once a selection has been
 * verified, so no caller can mistake one still in flight for a result, and
 * not at all if a disconnect cleared it meanwhile (bt_codec_clears). */
static pthread_mutex_t bt_codec_apply_serial = PTHREAD_MUTEX_INITIALIZER;

static bool bluealsa_codec_latch_commit(const char * path, unsigned int clears) {
    pthread_mutex_lock(&bt_codec_applied_mutex);
    if (bt_codec_clears == clears)
        snprintf(bt_codec_applied_path, sizeof(bt_codec_applied_path), "%s", path);
    pthread_mutex_unlock(&bt_codec_applied_mutex);
    return true;
}

/* Returns false only when a selection was attempted and did not take
 * effect (or, for Automatic, the accessory's codecs are not known yet), so
 * the caller can retry; nothing to do counts as success. */
static bool bt_cancelled(const atomic_bool * cancel) {
    return cancel && atomic_load(cancel);
}

static bool bluealsa_apply_codec_preference_serial(const char * path, const atomic_bool * cancel,
                                                   uint32_t * defer_until) {
    if (defer_until) *defer_until = 0;
    bluealsa_backend_t backend = bluealsa_backend();
    pthread_mutex_lock(&bt_codec_applied_mutex);
    bool done = strcmp(bt_codec_applied_path, path) == 0;
    unsigned int clears = bt_codec_clears;
    pthread_mutex_unlock(&bt_codec_applied_mutex);
    if (done) return true;
    if (bt_cancelled(cancel)) return true; /* stopping: not a failure to retry */

    /* Current state first: an accessory already on the wanted codec is
     * recorded without re-selecting it (which would reconfigure the link). */
    char info_out[2048];
    char * info_argv[] = { (char *) backend.ctl, (char *) "info", (char *) path, NULL };
    if (!subprocess_run(info_argv, info_out, sizeof(info_out))) return false;
    bt_ssc_policy_offer_selected(info_out, sizeof(info_out)); /* a stream in use is not "unavailable" */
    const char * codec = bt_codec_select_name();
    bool automatic = codec == NULL;
    bool explicit_ssc = (bt_codec_preference_t) atomic_load(&bt_codec_preference) == BT_CODEC_SSC;
    bool galaxy_buds = false;
    bt_ssc_signal_t ssc_signal = BT_SSC_SIGNAL_NONE;
    bool buds_metadata_known = false;
    uint32_t buds_wait_started = 0;
    /* galaxy_buds below: any hint that SSC is coming although it is not
     * offered yet (bt_pcm_is_galaxy_buds). An offered SSC endpoint is itself
     * the strongest signal and needs no lookup or wait. */
    bool ssc_advertised = bt_ssc_policy_endpoint_offered(info_out);
    bt_auto_connection_begin(path, automatic);
    bool inspect_buds_name = (automatic || explicit_ssc) &&
        !(bt_auto_rejected(path) & (1u << bt_auto_rank_index("samsung-SC"))) && !ssc_advertised;
    if (inspect_buds_name)
        bt_buds_state(path, &galaxy_buds, &buds_metadata_known, &buds_wait_started, &ssc_signal, cancel);
    /* Only a Samsung Modalias or a remembered SSC justifies a recovery
     * reconnect or a recorded failure; a name alone only earns the wait. */
    bool ssc_strong = ssc_signal == BT_SSC_SIGNAL_MODALIAS || ssc_signal == BT_SSC_SIGNAL_REMEMBERED;
    if (bt_cancelled(cancel)) return true;
    if (inspect_buds_name && !buds_metadata_known &&
        bt_control_monotonic_ms() - buds_wait_started < BT_BUDS_SSC_DISCOVERY_GRACE_MS) {
        DBG_LOG("bt_control: waiting for Bluetooth device name on %s\n", path);
        if (defer_until) *defer_until = buds_wait_started + BT_BUDS_SSC_DISCOVERY_GRACE_MS;
        return false;
    }
    char best[32];
    if (automatic) {
        bool ssc_rejected = (bt_auto_rejected(path) & (1u << bt_auto_rank_index("samsung-SC"))) != 0;
        /* The persisted backoff only stops us choosing SSC; when the accessory
         * opened SSC itself it stays ("already"), as reconfiguring a live SSC
         * stream can drop the link. A rejection this session still applies. */
        char live[32];
        if (ssc_rejected && ssc_advertised && bluealsa_parse_selected_codec(info_out, live, sizeof(live)) &&
            (strcasecmp(live, "samsung-SC") == 0 || strcasecmp(live, "SSC") == 0) &&
            !(bt_auto_rejected_session(path) & (1u << bt_auto_rank_index("samsung-SC"))))
            ssc_rejected = false;
        if (ssc_advertised && !ssc_rejected) {
            snprintf(best, sizeof(best), "%s", "samsung-SC");
        } else {
            if (galaxy_buds && !ssc_rejected &&
                bt_control_monotonic_ms() - buds_wait_started < BT_BUDS_SSC_DISCOVERY_GRACE_MS) {
                DBG_LOG("bt_control: waiting for Galaxy Buds SSC endpoint on %s\n", path);
                if (defer_until) *defer_until = buds_wait_started + BT_BUDS_SSC_DISCOVERY_GRACE_MS;
                return false;
            }
            if (ssc_strong && !ssc_rejected && bluealsa_available_codecs_known(info_out)) {
                if (bt_ssc_discovery_recovery_queue(path)) {
                    if (defer_until) *defer_until = bt_control_monotonic_ms() + 500;
                    return false;
                }
                bt_auto_endpoint_failure(path);
            }
            if (!bluealsa_best_available_codec(info_out, bt_auto_rejected(path), best, sizeof(best))) {
                DBG_LOG("bt_control: auto codec for %s: available codecs not known yet\n", path);
                return false;
            }
        }
        codec = best;
    } else if (explicit_ssc) {
        /* SSC is only implemented by Samsung accessories. Keep an explicit
         * SSC preference for a later compatible connection, but use this
         * device's best advertised codec when SSC is unavailable or has
         * already failed configuration for this device. */
        unsigned int rejected = bt_auto_rejected(path);
        int ssc_rank = bt_auto_rank_index("samsung-SC");
        bool ssc_available = ssc_advertised;
        if (!bluealsa_available_codecs_known(info_out)) {
            DBG_LOG("bt_control: SSC codecs for %s are not known yet\n", path);
            return false;
        }
        if (!ssc_available && !(ssc_rank >= 0 && (rejected & (1u << ssc_rank))) && galaxy_buds &&
            bt_control_monotonic_ms() - buds_wait_started < BT_BUDS_SSC_DISCOVERY_GRACE_MS) {
            DBG_LOG("bt_control: waiting for Galaxy Buds SSC endpoint on %s\n", path);
            if (defer_until) *defer_until = buds_wait_started + BT_BUDS_SSC_DISCOVERY_GRACE_MS;
            return false;
        }
        if (!ssc_available && !(rejected & (1u << ssc_rank)) && galaxy_buds &&
                bt_ssc_discovery_recovery_queue(path)) {
            if (defer_until) *defer_until = bt_control_monotonic_ms() + 500;
            return false;
        }
        if (!ssc_available && !(rejected & (1u << ssc_rank)) && ssc_strong)
            bt_auto_endpoint_failure(path);
        if (!ssc_available || (ssc_rank >= 0 && (rejected & (1u << ssc_rank)))) {
            if (!bluealsa_best_available_codec(info_out, rejected, best, sizeof(best))) {
                DBG_LOG("bt_control: SSC unavailable for %s and no fallback codec is known yet\n", path);
                return false;
            }
            codec = best;
        }
    }
    char active[32];
    if (bluealsa_parse_selected_codec(info_out, active, sizeof(active)) &&
        strcasecmp(active, codec) == 0) {
        DBG_LOG("bt_control: codec for %s already %s\n", path, active);
        if (automatic || explicit_ssc) bt_auto_switch_confirmed(path, active);
        return bluealsa_codec_latch_commit(path, clears);
    }
    if (bt_cancelled(cancel)) return true;
    /* Never reconfigure while a recovery owns the link, or before the stream
     * is open: BlueZ aborts a stream that is not OPEN yet, and the accessory
     * may drop the ACL on that. */
    uint32_t ready = bt_auto_codec_command_ready_at(path);
    if (ready) {
        DBG_LOG("bt_control: codec command for %s waits for an open stream\n", path);
        if (defer_until) *defer_until = ready;
        return false;
    }

    char * argv[] = { (char *) backend.ctl, (char *) "codec",
                      (char *) path, (char *) codec, NULL };
    int exit_code = -1;
    bool noted = automatic || explicit_ssc;
    /* The command itself recreates the PCM: from here the removal watch
     * holds a disconnect back until the PCM returns (bt_ssc_policy.h). */
    if (noted) bt_auto_switch_begin(path);
    if (!subprocess_run_checked(argv, NULL, 0, 5000, &exit_code) || exit_code != 0) {
        DBG_LOG("bt_control: codec select %s -> %s FAILED (exit %d)\n", path, codec, exit_code);
        if (noted) bt_auto_switch_abort(path);
        /* A transient D-Bus error or disappearing PCM is not evidence that
         * explicit SSC is unsupported. The worker owns the bounded retry. */
        if (automatic) bt_auto_reject(path, codec);
        return false;
    }
    /* Noted as soon as the command succeeded, before the readback: a codec
     * that kills the link also kills the readback, and must still be blamed
     * for the drop that follows (a disconnect confirmed within
     * BT_AUTO_REJECT_WINDOW_MS, see bt_auto_drop_confirmed). */
    if (noted) bt_auto_note_switch(path, codec);
    if (bt_cancelled(cancel)) return true;
    /* Read back rather than trusting the exit status: the accessory can
     * refuse a codec it advertised, and the top bar reports the real
     * transport, so a silent mismatch would look like the original bug.
     * Only an observed match counts; an unavailable readback is retried,
     * and the next attempt's state check records it if it did take. */
    if (!subprocess_run(info_argv, info_out, sizeof(info_out)) ||
        !bluealsa_parse_selected_codec(info_out, active, sizeof(active))) {
        DBG_LOG("bt_control: codec select %s -> %s: readback unavailable\n", path, codec);
        return false;
    }
    DBG_LOG("bt_control: codec select %s -> %s, now reports %s\n", path, codec, active);
    if (strcasecmp(active, codec) != 0) {
        /* A definite mismatch: Automatic and explicit SSC both give this
         * codec up for the session and settle on the next ranked one rather
         * than re-selecting it. */
        if (automatic || explicit_ssc) bt_auto_reject(path, codec);
        return false;
    }
    if (noted) bt_auto_switch_confirmed(path, codec);
    return bluealsa_codec_latch_commit(path, clears);
}

static bool bluealsa_apply_codec_preference_deferred(const char *path, const atomic_bool *cancel,
                                                     uint32_t *defer_until) {
    if (defer_until) *defer_until = 0;
    if (!bluealsa_is_audio_pcm_path(path)) return true;
    pthread_mutex_lock(&bt_codec_apply_serial);
    bool ok = bluealsa_apply_codec_preference_serial(path, cancel, defer_until);
    pthread_mutex_unlock(&bt_codec_apply_serial);
    return ok;
}

/* cancel (optional) is checked between subprocesses; a cancelled pass
 * reports success so it is not scheduled for a retry. */
static bool bluealsa_apply_codec_preference_cancellable(const char *path, const atomic_bool *cancel) {
    return bluealsa_apply_codec_preference_deferred(path, cancel, NULL);
}

static bool bluealsa_apply_codec_preference(const char * path) {
    return bluealsa_apply_codec_preference_cancellable(path, NULL);
}

/* Whether this PCM's codec has been verified since it last connected: the
 * latch is recorded only after an observed match. */
static bool bluealsa_codec_verified(const char * path) {
    pthread_mutex_lock(&bt_codec_applied_mutex);
    bool verified = strcmp(bt_codec_applied_path, path) == 0;
    pthread_mutex_unlock(&bt_codec_applied_mutex);
    return verified;
}

static bool parse_monitor_volume(const char * text, int * out) {
    if (!text || !out) return false;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        char * end;
        unsigned long packed = strtoul(text, &end, 16);
        if (end == text || *end != '\0' || packed > UINT32_MAX) return false;
        *out = (int) ((packed >> 8) & 0xFF); /* legacy left channel byte */
        return true;
    }
    char * end;
    long value = strtol(text, &end, 10);
    if (end == text || (strcmp(end, "[M]") != 0 && *end != '\0') ||
            value < 0 || value > BT_SOURCE_VOLUME_MAX) return false;
    *out = (int) value; /* v5 prints one value per channel */
    return true;
}

static bool bluealsa_parse_source_volume_reply(const char * reply, int * out) {
    if (!reply || !out) return false;
    const char * value = strstr(reply, "variant");
    if (!value) return false;
    value += strlen("variant");
    if (!isspace((unsigned char)*value)) return false;
    while (isspace((unsigned char)*value)) value++;

    if (strncmp(value, "uint16", 6) == 0 && isspace((unsigned char)value[6])) {
        value += 6;
        while (isspace((unsigned char)*value)) value++;
        if (!isdigit((unsigned char)*value)) return false;
        char * end;
        unsigned long packed = strtoul(value, &end, 10);
        if (end == value || (packed & ~0x7f7fUL) != 0) return false;
        while (isspace((unsigned char)*end)) end++;
        if (*end != '\0') return false;
        unsigned left = (unsigned) packed & 0x7f;
        unsigned right = ((unsigned) packed >> 8) & 0x7f;
        *out = (int) ((left + right + 1) / 2);
        return true;
    }

    static const char array_type[] = "array of bytes";
    if (strncmp(value, array_type, sizeof(array_type) - 1) != 0) return false;
    value += sizeof(array_type) - 1;
    while (isspace((unsigned char)*value)) value++;
    if (*value++ != '[') return false;
    uint8_t channels[2];
    for (size_t i = 0; i < 2; i++) {
        while (isspace((unsigned char)*value)) value++;
        unsigned byte = 0;
        int digits = 0;
        while (isxdigit((unsigned char)*value) && digits < 2) {
            unsigned char c = (unsigned char)*value++;
            byte = (byte << 4) | (unsigned) (isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
            digits++;
        }
        if (digits != 2 || isxdigit((unsigned char)*value) || byte > BT_SOURCE_VOLUME_MAX)
            return false;
        channels[i] = (uint8_t) byte;
    }
    while (isspace((unsigned char)*value)) value++;
    if (*value++ != ']') return false;
    while (isspace((unsigned char)*value)) value++;
    if (*value != '\0') return false;
    *out = (int) ((channels[0] + channels[1] + 1) / 2);
    return true;
}

/* Read the live PCM property before adopting a monitor event. A queued event
 * from a removed/recreated PCM can have the same path as the new PCM; its
 * reported value must still match the current BlueALSA state. */
static bool bluealsa_read_source_volume(const char * path, int * out) {
    char reply[256];
    char * argv[] = { "dbus-send", "--system", "--print-reply", "--dest=org.bluealsa",
        (char *) path, "org.freedesktop.DBus.Properties.Get",
        "string:org.bluealsa.PCM1", "string:Volume", NULL };
    int status = -1;
    if (!subprocess_run_checked(argv, reply, sizeof(reply), 1000, &status) || status != 0)
        return false;
    return bluealsa_parse_source_volume_reply(reply, out);
}

static pthread_mutex_t bt_dac_info_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t bt_dac_monitor_mutex = PTHREAD_MUTEX_INITIALIZER;
static bt_dac_stream_info_t bt_dac_info;
static pthread_t bt_dac_info_thread;
static bool bt_dac_info_active;
static pid_t bt_dac_info_monitor_pid = -1;

void bt_control_get_dac_stream_info(bt_dac_stream_info_t * out) {
    if (!out) return;
    pthread_mutex_lock(&bt_dac_info_mutex);
    *out = bt_dac_info;
    pthread_mutex_unlock(&bt_dac_info_mutex);
}

static void bt_dac_info_store(const bt_dac_stream_info_t * info) {
    pthread_mutex_lock(&bt_dac_info_mutex);
    bt_dac_info = *info;
    pthread_mutex_unlock(&bt_dac_info_mutex);
}

static unsigned int pcm_format_bit_depth(const char * format) {
    if (!format) return 0;
    if (strstr(format, "S16") || strstr(format, "U16")) return 16;
    if (strstr(format, "S24") || strstr(format, "U24")) return 24;
    if (strstr(format, "S32") || strstr(format, "U32") || strstr(format, "FLOAT")) return 32;
    if (strstr(format, "S8") || strstr(format, "U8")) return 8;
    return 0;
}

static void copy_info_field(char * dst, size_t size, const char * text, const char * key) {
    const char * p = strstr(text, key);
    if (!p) return;
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;
    size_t n = strcspn(p, "\r\n");
    if (n >= size) n = size - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
}

static void bt_dac_info_refresh(void) {
    bt_dac_stream_info_t info = {0};
    const char * ctl = bluealsa_ctl_name();
    char list_out[4096];
    char * list_argv[] = { (char *) ctl, (char *) "list-pcms", NULL };
    if (!subprocess_run(list_argv, list_out, sizeof(list_out))) {
        bt_dac_info_store(&info);
        return;
    }
    char path[256] = {0};
    char * save = NULL;
    for (char * line = strtok_r(list_out, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (strstr(line, "/a2dpsnk/source")) {
            snprintf(path, sizeof(path), "%s", line);
            break;
        }
    }
    if (!path[0]) {
        bluealsa_clear_soft_volume_path(NULL);
        bt_dac_info_store(&info); /* PCM disappeared: clear stale data now. */
        return;
    }
    bluealsa_apply_soft_volume(path);
    info.available = true;
    char info_out[2048];
    char * info_argv[] = { (char *) ctl, (char *) "info", path, NULL };
    if (subprocess_run(info_argv, info_out, sizeof(info_out))) {
        copy_info_field(info.codec, sizeof(info.codec), info_out, "Selected codec:");
        copy_info_field(info.pcm_format, sizeof(info.pcm_format), info_out, "Format:");
        const char * sampling = strstr(info_out, "Sampling:");
        if (!sampling) sampling = strstr(info_out, "Rate:");
        const char * channels = strstr(info_out, "Channels:");
        const char * running = strstr(info_out, "Running:");
        if (sampling) {
            const char * value = strchr(sampling, ':');
            if (value) (void) sscanf(value + 1, "%u", &info.sample_rate);
        }
        if (channels) (void) sscanf(channels + strlen("Channels:"), "%u", &info.channels);
        if (running) {
            running += strlen("Running:");
            while (*running == ' ' || *running == '\t') running++;
            info.running = !strncmp(running, "true", 4) || !strncmp(running, "yes", 3) || *running == '1';
        }
        info.bit_depth = pcm_format_bit_depth(info.pcm_format);
    }
    bt_dac_info_store(&info);
}

/* One `bluealsactl monitor` session: returns once that child has exited and
 * been reaped, or right away if it could not be started or a stop landed. */
static void bt_dac_info_monitor_run_once(void) {
    /* Codec and Running are property changes, which the monitor omits unless
     * asked for: a PCM appears before playback starts, so without these the
     * idle-to-running transition never arrives. The set is narrowed because
     * Delay/ClientDelay change continuously during playback and each event
     * costs two subprocesses in bt_dac_info_refresh(). */
    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "monitor",
                      (char *) "--properties=Codec,Running", NULL };
    pid_t pid;
    int fd;
    if (!subprocess_popen(argv, &pid, &fd)) return;

    /* Publish the child only while holding the same lock stop() uses. If a
     * stop landed in the small fork-to-publish window, terminate our own
     * child instead of entering fgets() after stop() already looked for it. */
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    if (!bt_dac_info_active) {
        pthread_mutex_unlock(&bt_dac_monitor_mutex);
        subprocess_terminate(pid);
        close(fd);
        return;
    }
    bt_dac_info_monitor_pid = pid;
    pthread_mutex_unlock(&bt_dac_monitor_mutex);

    FILE * f = fdopen(fd, "r");
    if (!f) {
        pthread_mutex_lock(&bt_dac_monitor_mutex);
        if (bt_dac_info_monitor_pid == pid) bt_dac_info_monitor_pid = -1;
        pthread_mutex_unlock(&bt_dac_monitor_mutex);
        subprocess_terminate(pid);
        close(fd);
        return;
    }

    /* Subscribe first so changes occurring during this initial blocking
     * query remain queued in the monitor pipe and cannot be missed. */
    bt_dac_info_refresh();
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if ((strstr(line, "PCMRemoved") && strstr(line, "/a2dpsnk/source")) ||
                strstr(line, "ServiceStopped")) {
            /* A PCM that comes back under the same path (reconnect, daemon
             * restart) starts with its own SoftVolume, so apply it again. */
            bluealsa_clear_soft_volume_path(NULL);
            bt_dac_stream_info_t empty = {0};
            bt_dac_info_store(&empty);
        } else if (strstr(line, "PCM") || strstr(line, "/a2dpsnk/source") || strstr(line, "Codec") || strstr(line, "Running")) {
            bt_dac_info_refresh();
        }
    }
    fclose(f);
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    if (bt_dac_info_monitor_pid == pid) bt_dac_info_monitor_pid = -1;
    pthread_mutex_unlock(&bt_dac_monitor_mutex);
    /* This thread owns reaping its monitor child. stop() only signals and
     * joins, avoiding two threads racing waitpid() on the same PID. */
    (void) waitpid(pid, NULL, 0);
    bt_dac_stream_info_t empty = {0};
    bt_dac_info_store(&empty);
}

/* Supervises the monitor for as long as DAC mode is on: if the child exits on
 * its own, a PCM seen by the next session would otherwise keep a stale
 * "already applied" soft-volume entry, so the cache is cleared and a new
 * monitor started (after a short pause, so a child that cannot start does
 * not spin). stop() clears bt_dac_info_active and ends the loop. */
static void * bt_dac_info_thread_func(void * arg) {
    (void) arg;
    for (;;) {
        bt_dac_info_monitor_run_once();
        bluealsa_clear_soft_volume_path(NULL);
        bool active = true;
        for (int waited_ms = 0; active && waited_ms < 1000; waited_ms += 50) {
            pthread_mutex_lock(&bt_dac_monitor_mutex);
            active = bt_dac_info_active;
            pthread_mutex_unlock(&bt_dac_monitor_mutex);
            if (active) usleep(50000);
        }
        if (!active) break;
    }
    return NULL;
}


static void bt_dac_info_monitor_stop(void) {
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    if (!bt_dac_info_active) {
        pthread_mutex_unlock(&bt_dac_monitor_mutex);
        bt_dac_stream_info_t empty = {0}; bt_dac_info_store(&empty); return;
    }
    bt_dac_info_active = false;
    pid_t pid = bt_dac_info_monitor_pid;
    pthread_mutex_unlock(&bt_dac_monitor_mutex);
    if (pid > 0) {
        kill(pid, SIGTERM);
        /* fgets() normally unblocks as soon as SIGTERM closes the pipe.
         * Keep the same one-second grace period as subprocess_terminate(),
         * but let the monitor thread itself reap the child. */
        for (int waited_ms = 0; waited_ms < 1000; waited_ms += 50) {
            pthread_mutex_lock(&bt_dac_monitor_mutex);
            bool still_same_child = bt_dac_info_monitor_pid == pid;
            pthread_mutex_unlock(&bt_dac_monitor_mutex);
            if (!still_same_child) break;
            usleep(50000);
        }
        pthread_mutex_lock(&bt_dac_monitor_mutex);
        bool still_same_child = bt_dac_info_monitor_pid == pid;
        pthread_mutex_unlock(&bt_dac_monitor_mutex);
        if (still_same_child) kill(pid, SIGKILL);
    }
    pthread_join(bt_dac_info_thread, NULL);
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    bt_dac_info_monitor_pid = -1;
    pthread_mutex_unlock(&bt_dac_monitor_mutex);
    bt_dac_stream_info_t empty = {0};
    bt_dac_info_store(&empty);
}

static void bt_dac_info_monitor_start(void) {
    bt_dac_info_monitor_stop();
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    bt_dac_info_active = true;
    if (pthread_create(&bt_dac_info_thread, NULL, bt_dac_info_thread_func, NULL) != 0) {
        bt_dac_info_active = false;
    }
    pthread_mutex_unlock(&bt_dac_monitor_mutex);
}

/* /etc/init.d/S80_bt_init always starts a second, independent dbus-daemon
 * (`--config-file=/usr/share/dbus-1/system.conf`) alongside the one
 * S30dbus already started at boot (`--system`), regardless of anything
 * this app does. Depending on which of the two buses a given client
 * (bluetoothd, bluealsa, bt-agent, or this file's own bluetoothctl calls)
 * lands on, they can lose visibility of each other -- e.g. an incoming
 * AVDTP connect gets rejected ("Authentication attempt without agent")
 * because bt-agent registered its pairing agent on the bus bluetoothd
 * wasn't listening on. A bus-reachability check alone doesn't catch this,
 * since either daemon individually still answers fine even with the
 * split-brain intact; counting dbus-daemon processes directly is what
 * actually detects it, and is cheap enough (a single `ps`) to call
 * unconditionally. */
static int count_matching(const char * needle) {
    char out[4096];
    char * argv[] = { (char *) "ps", NULL };
    if (!subprocess_run(argv, out, sizeof(out))) return 0;

    int count = 0;
    char * line_save = NULL;
    char * line = strtok_r(out, "\n", &line_save);
    while (line) {
        if (strstr(line, needle)) count++;
        line = strtok_r(NULL, "\n", &line_save);
    }
    return count;
}

static int count_process_exact(const char * name) {
    char out[4096];
    char * argv[] = { (char *) "ps", NULL };
    if (!subprocess_run(argv, out, sizeof(out))) return 0;

    int count = 0;
    char * line_save = NULL;
    char * line = strtok_r(out, "\n", &line_save);
    while (line) {
        char * field_save = NULL;
        char * field = strtok_r(line, " \t", &field_save);
        while (field) {
            const char * base = strrchr(field, '/');
            if (base) base++;
            else base = field;
            if (strcmp(base, name) == 0) {
                count++;
                break;
            }
            field = strtok_r(NULL, " \t", &field_save);
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return count;
}

/* Reconcile only the backend daemon itself.  The stock startup scripts can
 * leave a source daemon running without the requested encoder quality; a
 * sink daemon is deliberately left alone because SBC-XQ is source-only. */
static bool bluealsa_needs_source_restart(const bluealsa_backend_t * backend,
                                          bt_codec_preference_t preference,
                                          unsigned int requested_rate) {
    DIR * proc = opendir("/proc");
    if (!proc) return false;
    bool restart = false;
    struct dirent * entry;
    while ((entry = readdir(proc))) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        char path[320];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
        FILE * f = fopen(path, "r");
        if (!f) continue;
        char args[2048] = {0};
        size_t len = fread(args, 1, sizeof(args) - 1, f);
        fclose(f);
        if (!len) continue;
        const char * name = strrchr(args, '/');
        name = name ? name + 1 : args;
        if (strcmp(name, backend->daemon_name) != 0) continue;
        bool sink = false, active_xq = false, active_all_codecs = false, active_force_cd = false;
        bool active_ssc_auto_profile = false;
        bool active_ldac_abr = false;
        const char * active_codec = NULL;
        const char * active_ldac_quality = NULL;
        for (size_t pos = strlen(args) + 1; pos < len; ) {
            size_t arg_len = strlen(args + pos);
            const char * arg = args + pos;
            if (strcmp(arg, "a2dp-sink") == 0 ||
                    strcmp(arg, "--profile=a2dp-sink") == 0) sink = true;
            if (strcmp(arg, "--profile") == 0 && pos + arg_len + 1 < len &&
                    strcmp(args + pos + arg_len + 1, "a2dp-sink") == 0) sink = true;
            if (strcmp(arg, "--sbc-quality=xq") == 0 ||
                    (strcmp(arg, "--sbc-quality") == 0 &&
                     pos + arg_len + 1 < len &&
                     strcmp(args + pos + arg_len + 1, "xq") == 0)) active_xq = true;
            if (strcmp(arg, "--all-codecs") == 0) active_all_codecs = true;
            if (strcmp(arg, "--ldac-abr") == 0) active_ldac_abr = true;
            if (strcmp(arg, BT_A2DP_FORCE_44K1_ARG) == 0) active_force_cd = true;
            if (strcmp(arg, BT_SSC_AUTO_PROFILE_ARG) == 0) active_ssc_auto_profile = true;
            if (strcmp(arg, "-c") == 0 || strcmp(arg, "--codec") == 0) {
                if (pos + arg_len + 1 < len) active_codec = args + pos + arg_len + 1;
            } else if (strncmp(arg, "--codec=", 8) == 0) {
                active_codec = arg + 8;
            }
            if (strncmp(arg, "--ldac-quality=", 15) == 0) active_ldac_quality = arg + 15;
            else if (strcmp(arg, "--ldac-quality") == 0 && pos + arg_len + 1 < len)
                active_ldac_quality = args + pos + arg_len + 1;
            if (!arg_len) break;
            pos += arg_len + 1;
        }
        if (!sink) {
            const char * expected_codec = NULL;
            if (preference == BT_CODEC_LDAC_HQ || preference == BT_CODEC_LDAC_SQ) expected_codec = "LDAC";
            else if (preference == BT_CODEC_APTX) expected_codec = "aptX";
            else if (preference == BT_CODEC_AAC) expected_codec = "AAC";
            bool expected_xq = preference == BT_CODEC_SBC_XQ;
            const char * expected_ldac_quality = preference == BT_CODEC_LDAC_HQ ? "high" :
                                                  preference == BT_CODEC_LDAC_SQ ? "standard" :
                                                  preference == BT_CODEC_AUTO ? "high" : NULL;
            bool expected_all_codecs = preference == BT_CODEC_AUTO || preference == BT_CODEC_SSC;
            bool expected_ldac_abr = preference == BT_CODEC_AUTO;
            bool expected_ssc_auto_profile = bt_ssc_auto_profile_for_rate(requested_rate);
            bool expected_force_cd = bt_rate_wants_audio_cd_for_rate(requested_rate);
            if (active_xq != expected_xq || active_all_codecs != expected_all_codecs ||
                    active_ldac_abr != expected_ldac_abr ||
                    active_force_cd != expected_force_cd ||
                    active_ssc_auto_profile != expected_ssc_auto_profile ||
                    (expected_codec && (!active_codec || strcasecmp(active_codec, expected_codec) != 0)) ||
                    (!expected_codec && active_codec) ||
                    (expected_ldac_quality && (!active_ldac_quality ||
                                               strcasecmp(active_ldac_quality, expected_ldac_quality) != 0)) ||
                    (!expected_ldac_quality && active_ldac_quality))
                restart = true;
        }
    }
    closedir(proc);
    return restart;
}

static bool dbus_system_bus_reachable(void) {
    char out[256];
    char * argv[] = { (char *) "dbus-send", (char *) "--system", (char *) "--print-reply",
                       (char *) "--dest=org.freedesktop.DBus", (char *) "/org/freedesktop/DBus",
                       (char *) "org.freedesktop.DBus.ListNames", NULL };
    return subprocess_run(argv, out, sizeof(out)) && strstr(out, "method return") != NULL;
}

static void ensure_single_dbus_daemon(void) {
    if (count_matching("dbus-daemon") <= 1 && dbus_system_bus_reachable()) return;

    subprocess_kill_all_matching("dbus-daemon");
    remove("/var/run/messagebus.pid"); /* stale pidfile blocks a fresh start otherwise */
    usleep(300000);

    /* Launch dbus-daemon in the background with --fork. */
    char * argv[] = { (char *) "dbus-daemon", (char *) "--system", (char *) "--fork", NULL };
    subprocess_spawn_daemon(argv);
    usleep(300000);
}

/* Defined with the rest of the output-settings section, below -- restores
 * whatever bt_control_apply_output_settings() was last actually asked for
 * (DAC/a2dp-sink mode in particular). Needed by the wedge recovery further
 * down, which is defined earlier in the file than that section. */
static void bt_control_reapply_last_output_settings(void);

/* Cheap pre-check for whether a Bluetooth adapter exists before asking
 * bluetoothctl about it. With no hci0, `bluetoothctl show` doesn't fail
 * fast -- it can take close to subprocess_run()'s full 15s timeout to give
 * up, which would stall the UI thread since this is called every
 * update_timer_cb tick. `hciconfig` returns almost instantly (empty output
 * when no adapter exists), so the bluetoothctl-over-D-Bus round trip is
 * only attempted when there's actually an adapter to ask about. */
static bool bt_control_adapter_present(void) {
    char out[64];
    char * argv[] = { (char *) "hciconfig", NULL };
    if (!subprocess_run(argv, out, sizeof(out))) return false;
    return out[0] != '\0';
}

/* Once real A2DP audio starts flowing during Bluetooth DAC mode,
 * bluetoothd can end up in a state where `bluetoothctl show` reports "No
 * default controller available" even though hci0 is still genuinely up
 * (bt_control_adapter_present() already true) and bluealsa/bluealsa-aplay
 * may still be actively streaming through it -- bluetoothd itself doesn't
 * crash (same PID, still sleeping), its D-Bus interface just stops
 * answering correctly. The only recovery that works is restarting
 * bluetoothd, mirroring /usr/bin/bt_resume's own remediation: stop
 * bluetoothd, `hciconfig hci0 reset`, start bluetoothd again.
 * Rate-limited via last_recovery_attempt so a wedge that recovery doesn't
 * fix can't trigger a restart on every ~5s status poll indefinitely. */
#define WEDGED_RECOVERY_COOLDOWN_SECONDS 20

static void bt_control_recover_wedged_daemon(void) {
    static time_t last_recovery_attempt = 0;
    time_t now = time(NULL);
    if (now - last_recovery_attempt < WEDGED_RECOVERY_COOLDOWN_SECONDS) {
        DBG_LOG("bt_control: wedged daemon detected, but recovery on cooldown (%lds left)\n",
                (long) (WEDGED_RECOVERY_COOLDOWN_SECONDS - (now - last_recovery_attempt)));
        return;
    }
    last_recovery_attempt = now;

    DBG_LOG("bt_control: wedged daemon detected, attempting recovery\n");

    /* Ensure a single system D-Bus daemon is running before restarting bluetoothd. */
    ensure_single_dbus_daemon();

    subprocess_kill_all_matching("bluetoothd");
    usleep(500000);

    char * reset_argv[] = { (char *) "hciconfig", (char *) "hci0", (char *) "reset", NULL };
    bool reset_ok = subprocess_run(reset_argv, NULL, 0);
    usleep(500000);

    char * bluetoothd_argv[] = { (char *) "/usr/libexec/bluetooth/bluetoothd", (char *) "-E", (char *) "-C", NULL };
    bool spawn_ok = subprocess_spawn_daemon(bluetoothd_argv);
    usleep(500000); /* give it a moment to register the adapter before the next step touches it */
    /* DBG_LOG() compiles out of production builds, which would otherwise
     * leave these results unused. Recovery still continues either way. */
    (void) reset_ok;
    (void) spawn_ok;

    /* A freshly-restarted bluetoothd comes up with the adapter powered off
     * by default; this recovery only ever runs because Bluetooth was
     * expected to be on, so leaving it off after "fixing" it would just be
     * a different flavor of the same failure. */
    bt_control_enable();

    /* Even with power restored, bluealsa is left at whatever bluetoothd's
     * restart left it as -- the stock a2dp-source default, not the
     * a2dp-sink profile Bluetooth DAC mode needs to receive audio. Without
     * this, the phone would reconnect fine but drop the instant it tried
     * to stream, since no sink profile was registered to receive it. */
    bt_control_reapply_last_output_settings();

    DBG_LOG("bt_control: recovery attempt done (hci0 reset=%d, bluetoothd spawn=%d)\n", reset_ok, spawn_ok);
}

/* A single subprocess_run() timeout on `bluetoothctl show` isn't proof of
 * a genuine bluetoothd wedge: bluealsa can legitimately spend ~15s in a
 * normal futex wait for the remote device to send Start after Open (some
 * phones open the transport, never start, then close it), during which
 * bluetoothd can be slow to answer unrelated D-Bus queries. Requiring
 * several CONSECUTIVE timeouts (polled every ~5s, so spanning well over
 * 15s) before triggering recovery filters out that normal window while
 * still catching a real wedge, which by definition doesn't self-resolve.
 * "No default controller available" (a fast, clean response, not a
 * timeout) is a different, unambiguous signal that there's no adapter at
 * all, so that one still recovers immediately without a threshold. */
#define TIMEOUT_RECOVERY_THRESHOLD 4

/* Grace period after startup to avoid falsely triggering daemon recovery while
 * Bluetooth chip initialization or bring-up is still in progress. */
#define BT_BOOT_RACE_GRACE_MS 15000

static uint32_t bt_control_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Guards bt_control_is_powered() state and timeout counters against concurrent
 * polling from the GUI timer and toggle background threads. */
static pthread_mutex_t bt_status_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool bt_control_is_powered_impl(void) {
    static int consecutive_timeouts = 0;
    static bool first_poll_seen = false;
    static uint32_t first_poll_tick = 0;

    if (!first_poll_seen) {
        first_poll_seen = true;
        first_poll_tick = bt_control_monotonic_ms();
    }

    if (!bt_control_adapter_present()) {
        DBG_LOG("bt_control: bt_control_is_powered: no adapter present\n");
        return false;
    }

    char out[2048];
    char * argv[] = { (char *) "bluetoothctl", (char *) "show", NULL };
    if (!subprocess_run(argv, out, sizeof(out))) {
        consecutive_timeouts++;
        DBG_LOG("bt_control: bt_control_is_powered: bluetoothctl show subprocess failed/timed out (%d/%d)\n",
                consecutive_timeouts, TIMEOUT_RECOVERY_THRESHOLD);
        if (consecutive_timeouts >= TIMEOUT_RECOVERY_THRESHOLD) {
            bt_control_recover_wedged_daemon();
            consecutive_timeouts = 0;
        }
        return false;
    }
    consecutive_timeouts = 0; /* any actual reply, even "not powered", means it isn't stuck */

    if (strstr(out, "Powered: yes") != NULL) return true;

    if (strstr(out, "No default controller available") != NULL) {
        uint32_t since_first_poll = bt_control_monotonic_ms() - first_poll_tick;
        if (since_first_poll < BT_BOOT_RACE_GRACE_MS) {
            DBG_LOG("bt_control: bt_control_is_powered: 'No default controller' %ums after first poll -- "
                    "within boot grace window, treating as still starting up, not a wedge\n",
                    (unsigned) since_first_poll);
        } else {
            bt_control_recover_wedged_daemon();
        }
    } else {
        DBG_LOG("bt_control: bt_control_is_powered: not powered (output: %.200s)\n", out);
    }
    return false;
}

bool bt_control_is_powered(void) {
    pthread_mutex_lock(&bt_status_mutex);
    bool result = bt_control_is_powered_impl();
    pthread_mutex_unlock(&bt_status_mutex);
    return result;
}

#define BT_INIT_TIMEOUT_MS 30000

/* Uses /usr/bin/bt_resume, not /usr/bin/bt_init -- the stock hiby_player
 * binary references bt_resume, bt_enable, bt_suspend, and `bt-device -l`
 * by exact path, never bt_init/bt_done/bluealsa_profile. bt_init reliably
 * creates a duplicate dbus-daemon; bt_resume avoids that since its own
 * dbus-daemon-startup lines are commented out in the script, so it just
 * uses whatever system dbus-daemon is already running. Otherwise
 * near-identical to bt_init: same chip detection/firmware flash, plus
 * pgrep guards around starting bluetoothd/bt-agent/bluealsa so it won't
 * double-start any of those if called again while they're still up. */
/* hci0 can come up already-present at the kernel level (module auto-load,
 * bluetoothd restoring a persisted "Powered" state) without bt_resume ever
 * having run this boot; in that case bluealsa never gets started, so
 * bluetoothd has no local A2DP source SDP record to offer and every
 * connect attempt fails with `a2dp-sink profile connect failed: Protocol
 * not available` regardless of the remote device. Since
 * bt_control_init_chip()'s early return is keyed purely on hci0 presence,
 * it was silently skipping bt_resume's own bluealsa-start step. This
 * ensures the BlueALSA 5 source daemon specifically, every time, decoupled
 * from whether hci0 needed a fresh bring-up; DAC mode and the app's volume
 * synchronization are handled by their own call sites. */
/* Side-effect-free accessory probe, answering "maybe" rather than "no" when
 * it cannot tell: a failed or timed-out list-pcms must never authorize
 * killing a daemon that may own a live link, and a timeout is likeliest
 * exactly when the system is already struggling. A daemon that is genuinely
 * gone is still respawned, because the caller finds no process to keep.
 * Deliberately not find_source_pcm_path(), which also applies the
 * SoftVolume preference to whatever it finds. */
static bool bluealsa_source_pcm_may_be_connected(void) {
    char list_out[4096];
    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "list-pcms", NULL };
    if (!subprocess_run_low_priority(argv, list_out, sizeof(list_out))) return true;
    return strstr(list_out, "/a2dpsrc/sink") != NULL;
}

static bool ensure_bluealsa_running_rate_ex(unsigned int requested_rate, bool * out_deferred) {
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    bluealsa_backend_t backend = bluealsa_backend();
    bt_codec_preference_t preference = (bt_codec_preference_t) atomic_load(&bt_codec_preference);
    bool must_restart = bluealsa_needs_source_restart(&backend, preference, requested_rate);
    /* Killing the daemon tears down every A2DP link it owns, and encoder
     * arguments only affect codec negotiation for new connections, so a
     * connected accessory is never dropped to correct them; the mismatch is
     * reconciled on a later call once nothing is connected. That session
     * keeps whatever it already negotiated: bluealsa_apply_codec_preference()
     * can retarget a live PCM only for an explicit codec choice, never for
     * "auto", and LDAC quality is a daemon-level argument either way.
     * bt_resume/bt_init start the daemon without the arguments an "auto"
     * preference expects, so this mismatch is present on every boot. */
    if (must_restart && bluealsa_source_pcm_may_be_connected()) {
        must_restart = false;
        /* Reported so the caller can retry once nothing is connected, rather
         * than recording a reconciliation that never actually happened. */
        if (out_deferred) *out_deferred = true;
    }
    if (must_restart)
        subprocess_kill_all_matching(backend.daemon_name);
    /* Killing detached daemons is asynchronous; the old process may still be
     * visible to an immediate ps snapshot. Once a mismatch was established,
     * respawn unconditionally instead of allowing that stale snapshot to
     * suppress the replacement. */
    if (!must_restart && count_process_exact(backend.daemon_name) > 0) {
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
        return true;
    }
    char * argv[11];
    int argc = 0;
    argv[argc++] = (char *) backend.daemon;
    argv[argc++] = (char *) "-p";
    argv[argc++] = (char *) "a2dp-source";
    if (bt_rate_wants_audio_cd_for_rate(requested_rate)) argv[argc++] = (char *) BT_A2DP_FORCE_44K1_ARG;
    bool ssc_auto_profile = bt_ssc_auto_profile_for_rate(requested_rate);
    if (ssc_auto_profile) argv[argc++] = (char *) BT_SSC_AUTO_PROFILE_ARG;
    if (bt_codec_is_sbc_xq()) argv[argc++] = (char *) "--sbc-quality=xq";
    argc = bt_codec_append_daemon_args(argv, argc);
    argv[argc] = NULL;
    bool ok = subprocess_spawn_daemon(argv);
    pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    return ok;
}

static bool ensure_bluealsa_running_ex(bool * out_deferred) {
    return ensure_bluealsa_running_rate_ex(atomic_load(&bt_sample_rate), out_deferred);
}

static bool ensure_bluealsa_running(void) {
    return ensure_bluealsa_running_ex(NULL);
}

bool bt_control_init_chip(void) {
    pthread_mutex_lock(&bt_chip_mutex);

    if (bt_control_adapter_present()) {
        ensure_bluealsa_running();
        pthread_mutex_unlock(&bt_chip_mutex);
        return true;
    }

    char out[512];
    char * bt_resume_argv[] = { (char *) "/usr/bin/bt_resume", NULL };
    if (access("/sys/devices/platform/bcm_wlbt_power/identity_state", F_OK) == 0) {
        int status;
        if (!subprocess_run_checked_group(bt_resume_argv, out, sizeof(out), SUBPROCESS_RADIO_TIMEOUT_MS, &status) || status != 0) {
            pthread_mutex_unlock(&bt_chip_mutex);
            return false;
        }
    } else {
        subprocess_run_timeout(bt_resume_argv, out, sizeof(out), BT_INIT_TIMEOUT_MS);
    }

    ensure_bluealsa_running(); /* belt-and-suspenders: bt_resume already starts it, but confirm rather than assume */
    bool result = bt_control_adapter_present();
    pthread_mutex_unlock(&bt_chip_mutex);
    return result;
}

bool bt_control_reconcile_source_settings(void) {
    bool deferred = false;
    pthread_mutex_lock(&bt_chip_mutex);
    bool ensured = ensure_bluealsa_running_ex(&deferred);
    pthread_mutex_unlock(&bt_chip_mutex);
    if (!ensured) return false;
    /* An accessory already connected before the PCMAdded watch started
     * never produces that event, so the codec preference would sit
     * unapplied until the next reconnect. Same reasoning (and same pairing
     * with discovery) find_source_pcm_path() already documents for the v5
     * SoftVolume preference, which it applies on discovery itself. */
    char path[256];
    if (find_source_pcm_path(path, sizeof(path)) && !bt_pref_request(path)) {
        bluealsa_apply_codec_preference(path);
        bluealsa_apply_rate_preference(path);
    }
    /* Not reconciled while an accessory held the daemon's old arguments. */
    return !deferred;
}

/* /usr/bin/bt_enable -- matches hiby_player's own confirmed strings rather
 * than the bluetoothctl power on this function used before. It's
 * `bt-adapter --set Powered On` + `--set Discoverable On` together --
 * turning Bluetooth on for real also makes this device discoverable/
 * pairable by default, it isn't something that only happens during
 * Bluetooth DAC mode (see bt_control_apply_output_settings()'s own separate
 * discoverable toggle for that -- this and that are just two different call
 * sites setting the same underlying state). Confirmed live: on a device
 * where the chip is already flashed but the radio is administratively down
 * (the state the stock boot sequence itself leaves it in -- see
 * bt_control_init_chip()'s comment), bt_enable brings hci0 fully up in
 * about a second, no re-flash needed. */
void bt_control_enable(void) {
    pthread_mutex_lock(&bt_chip_mutex);
    char out[256];
    char * argv[] = { (char *) "/usr/bin/bt_enable", NULL };
    subprocess_run(argv, out, sizeof(out));
    pthread_mutex_unlock(&bt_chip_mutex);
}

/* /usr/bin/bt_disable, NOT /usr/bin/bt_suspend, despite bt_suspend being
 * the one actually referenced in hiby_player's strings (bt_disable isn't
 * referenced by the binary or anything else in the squashfs). Tried
 * bt_suspend first to match that evidence exactly, and confirmed live that
 * it's unsafe for a simple in-app toggle: it fully tears down the chip's
 * UART firmware link (kills brcm_patchram_plus/hciattach, rfkill block),
 * and re-flashing it back with bt_resume afterward reliably failed with
 * "Can't get device info: No such device" -- the same unrecoverable-
 * without-a-reboot failure this project already knew about from re-running
 * bt_init, this time confirmed even through the "correct" suspend-then-
 * resume pair. bt_suspend is presumably tied to the whole device's own
 * sleep/wake cycle, not a user-facing Bluetooth on/off switch. bt_disable
 * (`bt-adapter --set Discoverable Off` + `--set Powered Off`) only touches
 * the D-Bus adapter state, the same layer bt_enable operates at, so a
 * later bt_enable can bring it back in ~1s with no re-flash -- this is the
 * safe pairing. */
void bt_control_disable(void) {
    pthread_mutex_lock(&bt_chip_mutex);
    char out[256];
    char * argv[] = { (char *) "/usr/bin/bt_disable", NULL };
    subprocess_run(argv, out, sizeof(out));
    pthread_mutex_unlock(&bt_chip_mutex);
}

/* Parses "Paired: yes"/"Connected: yes" out of `bluetoothctl info <mac>`'s
 * output (see bluetooth_control.h -- confirmed exact field names/format
 * against a real device). */
static void query_device_state(const char * mac, bool * out_paired, bool * out_connected) {
    *out_paired = false;
    *out_connected = false;
    char out[2048];
    char * argv[] = { (char *) "bluetoothctl", (char *) "info", (char *) mac, NULL };
    if (!subprocess_run_low_priority(argv, out, sizeof(out))) return;
    *out_paired = strstr(out, "Paired: yes") != NULL;
    *out_connected = strstr(out, "Connected: yes") != NULL;
}

/* BlueZ 5.87 removed the legacy `paired-devices` command and added the
 * `Paired` property filter to `devices`.  Probe the CLI's own command help,
 * rather than coupling this to the BlueZ daemon version or filesystem paths.
 * Keep the mutex held while probing so concurrent callers cannot spawn two
 * capability probes.  A failed, empty, truncated, or unrecognized probe
 * deliberately leaves the cache empty, allowing a later poll to retry. */
typedef enum {
    BT_BLUETOOTHCTL_DEVICES_UNKNOWN,
    BT_BLUETOOTHCTL_DEVICES_MODERN,
    BT_BLUETOOTHCTL_DEVICES_LEGACY
} bt_bluetoothctl_devices_capability_t;

static pthread_mutex_t bt_bluetoothctl_devices_mutex = PTHREAD_MUTEX_INITIALIZER;
static bt_bluetoothctl_devices_capability_t bt_bluetoothctl_devices_capability =
    BT_BLUETOOTHCTL_DEVICES_UNKNOWN;

/* `bluetoothctl --help` prints first-level commands as tab-indented lines,
 * followed by a tab-separated description.  It intentionally does not print
 * the command argument specification, so detect command names only. */
static bool bt_bluetoothctl_help_has_command(const char * help, const char * command) {
    size_t command_len = strlen(command);
    const char * line = help;
    while (line && *line) {
        const char * end = strchr(line, '\n');
        const char * token = line;
        if (*token == '\t' && token[1] != '\t') {
            const char * token_end = token + 1;
            token++;
            while (*token_end && *token_end != '\t' && *token_end != ' ' &&
                    *token_end != '\r' && *token_end != '\n') token_end++;
            if ((size_t) (token_end - token) == command_len &&
                    strncmp(token, command, command_len) == 0) return true;
        }
        if (!end) break;
        line = end + 1;
    }
    return false;
}

static bool bt_control_paired_devices_argv_checked(char * argv[4], int timeout_ms,
                                                   bt_control_cancel_callback_t cancel_cb,
                                                   void * cancel_ctx) {
    pthread_mutex_lock(&bt_bluetoothctl_devices_mutex);

    if (cancel_cb && cancel_cb(cancel_ctx)) {
        pthread_mutex_unlock(&bt_bluetoothctl_devices_mutex);
        return false;
    }

    if (bt_bluetoothctl_devices_capability == BT_BLUETOOTHCTL_DEVICES_UNKNOWN) {
        /* 5.87 prints over 9 KiB including submenu commands. A 4 KiB
         * capture truncates it and would leave this probe retrying forever. */
        char help[16384] = {0};
        char * help_argv[] = { (char *) "bluetoothctl", (char *) "--help", NULL };
        int exit_code = -1;
        bool ran = timeout_ms > 0 ? subprocess_run_checked(help_argv, help, sizeof(help),
                                                            timeout_ms, &exit_code) && exit_code == 0 :
                                   subprocess_run(help_argv, help, sizeof(help));
        if (ran) {
            size_t help_len = strnlen(help, sizeof(help));
            bool complete = help_len > 0 && help_len < sizeof(help) - 1 &&
                            help[help_len - 1] == '\n';
            /* Prefer the legacy command if both names ever appear in a
             * vendor-customized help listing. */
            if (complete && bt_bluetoothctl_help_has_command(help, "paired-devices"))
                bt_bluetoothctl_devices_capability = BT_BLUETOOTHCTL_DEVICES_LEGACY;
            else if (complete && bt_bluetoothctl_help_has_command(help, "devices"))
                bt_bluetoothctl_devices_capability = BT_BLUETOOTHCTL_DEVICES_MODERN;
        }
    }

    switch (bt_bluetoothctl_devices_capability) {
    case BT_BLUETOOTHCTL_DEVICES_MODERN:
        argv[0] = (char *) "bluetoothctl";
        argv[1] = (char *) "devices";
        argv[2] = (char *) "Paired";
        argv[3] = NULL;
        break;
    case BT_BLUETOOTHCTL_DEVICES_LEGACY:
        argv[0] = (char *) "bluetoothctl";
        argv[1] = (char *) "paired-devices";
        argv[2] = NULL;
        break;
    default:
        pthread_mutex_unlock(&bt_bluetoothctl_devices_mutex);
        return false;
    }

    pthread_mutex_unlock(&bt_bluetoothctl_devices_mutex);
    return !cancel_cb || !cancel_cb(cancel_ctx);
}

static bool bt_control_paired_devices_argv(char * argv[4]) {
    return bt_control_paired_devices_argv_checked(argv, 0, NULL, NULL);
}

bool bt_control_is_connected(void) {
    char devices_buf[4096];
    char * devices_argv[4];
    if (!bt_control_paired_devices_argv(devices_argv) ||
            !subprocess_run(devices_argv, devices_buf, sizeof(devices_buf))) return false;

    char * line_save = NULL;
    char * line = strtok_r(devices_buf, "\n", &line_save);
    while (line) {
        if (strncmp(line, "Device ", 7) == 0) {
            const char * mac_start = line + 7;
            const char * name_start = strchr(mac_start, ' ');
            if (name_start && (size_t) (name_start - mac_start) == 17) {
                char mac[18];
                memcpy(mac, mac_start, 17);
                mac[17] = '\0';
                bool paired, connected;
                query_device_state(mac, &paired, &connected);
                if (connected) return true;
            }
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return false;
}

/* See the header comment. bt_bluetoothctl_devices_capability is written
 * under bt_bluetoothctl_devices_mutex by bt_control_paired_devices_argv_checked()'s
 * one-time probe, so it must be read under that same mutex too -- copy it out
 * and unlock before doing anything else, both because the mutex is plain
 * (non-recursive) and the LEGACY/UNKNOWN fallback below (bt_control_is_connected())
 * re-enters that exact probe and would self-deadlock if this still held it.
 *
 * Returns 1 (connected), 0 (not connected), or -1 if this cycle couldn't
 * determine either way. -1 covers two cases: capability still UNKNOWN and
 * bt_control_is_connected()'s own query failed, or capability is MODERN but
 * this specific `devices Connected` call failed. The latter deliberately
 * does NOT fall back to the O(N) per-paired-device path -- that fallback is
 * exactly the fork storm this function exists to avoid, and a transient
 * bluetoothctl hiccup is most likely during the same radio power-on window
 * that storm is worst in. Callers should keep the last known state on -1,
 * not treat it as "nothing connected". */
int bt_control_any_paired_connected(void) {
    pthread_mutex_lock(&bt_bluetoothctl_devices_mutex);
    bt_bluetoothctl_devices_capability_t capability = bt_bluetoothctl_devices_capability;
    pthread_mutex_unlock(&bt_bluetoothctl_devices_mutex);

    if (capability == BT_BLUETOOTHCTL_DEVICES_MODERN) {
        char out[512];
        char * argv[] = { (char *) "bluetoothctl", (char *) "devices", (char *) "Connected", NULL };
        if (subprocess_run_low_priority(argv, out, sizeof(out))) return strstr(out, "Device ") != NULL ? 1 : 0;
        return -1;
    }
    return bt_control_is_connected() ? 1 : 0;
}

/* Returns paired and connected state for all paired devices.
 * Returns device count, or -1 if the underlying bluetoothctl call fails. */
int bt_control_list_paired_states(bt_device_t * out, int max_count) {
    char devices_buf[4096];
    char * devices_argv[4];
    if (!bt_control_paired_devices_argv(devices_argv) ||
            !subprocess_run_low_priority(devices_argv, devices_buf, sizeof(devices_buf))) return -1;

    int count = 0;
    char * line_save = NULL;
    char * line = strtok_r(devices_buf, "\n", &line_save);
    while (line && count < max_count) {
        if (strncmp(line, "Device ", 7) == 0) {
            const char * mac_start = line + 7;
            const char * name_start = strchr(mac_start, ' ');
            if (name_start && (size_t) (name_start - mac_start) == 17) {
                memcpy(out[count].mac, mac_start, 17);
                out[count].mac[17] = '\0';
                snprintf(out[count].name, sizeof(out[count].name), "%s", name_start + 1);
                query_device_state(out[count].mac, &out[count].paired, &out[count].connected);
                count++;
            }
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return count;
}

/* Runs discovery restricted to the classic BR/EDR bearer using `scan.transport bredr`.
 * Dual-mode devices paired via classic BR/EDR reliably support A2DP/AVRCP audio. */
static void run_bredr_scan(int seconds) {
    pid_t pid;
    int write_fd;
    char * argv[] = { (char *) "bluetoothctl", NULL };
    if (!subprocess_popen_stdin(argv, &pid, &write_fd)) return;

    const char * start_cmds = "scan.transport bredr\nscan on\n";
    ssize_t ignored = write(write_fd, start_cmds, strlen(start_cmds));
    (void) ignored;

    sleep(seconds > 0 ? (unsigned int) seconds : 1);

    const char * stop_cmds = "scan off\nquit\n";
    ignored = write(write_fd, stop_cmds, strlen(stop_cmds));
    (void) ignored;

    close(write_fd);
    subprocess_terminate(pid); /* reaps it either way -- `quit` alone isn't guaranteed to have taken effect yet */
}

/* Lists devices BlueZ already knows (paired, and anything seen since boot)
 * without running an inquiry. An inquiry makes the controller interleave
 * discovery with an active A2DP link, which is audible as the stream cutting
 * in and out, so the caller decides when that cost is worth paying. */
int bt_control_list_devices(bt_device_t * out, int max_count) {
    return bt_control_scan(0, out, max_count);
}

int bt_control_scan(int seconds, bt_device_t * out, int max_count) {
    if (seconds > 0) run_bredr_scan(seconds); /* blocks for `seconds` -- that's the point */

    char devices_buf[8192];
    char * devices_argv[] = { (char *) "bluetoothctl", (char *) "devices", NULL };
    if (!subprocess_run(devices_argv, devices_buf, sizeof(devices_buf))) return 0;

    int count = 0;
    char * line_save = NULL;
    char * line = strtok_r(devices_buf, "\n", &line_save);
    while (line && count < max_count) {
        /* "Device XX:XX:XX:XX:XX:XX Some Name Here" */
        if (strncmp(line, "Device ", 7) == 0) {
            const char * mac_start = line + 7;
            const char * name_start = strchr(mac_start, ' ');
            if (name_start && (size_t) (name_start - mac_start) == 17) {
                memcpy(out[count].mac, mac_start, 17);
                out[count].mac[17] = '\0';
                snprintf(out[count].name, sizeof(out[count].name), "%s", name_start + 1);
                query_device_state(out[count].mac, &out[count].paired, &out[count].connected);
                count++;
            }
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return count;
}

bool bt_control_connect_with_rate(const char * mac, unsigned int rate) {
    char out[512];
    pthread_mutex_lock(&bt_chip_mutex);
    /* Prepare the daemon only after a successful query confirms that no
     * source PCM is live. A busy or unreadable state keeps the old manual
     * pair/trust/connect behavior and never authorizes a daemon restart. */
    if (!bluealsa_source_pcm_may_be_connected())
        (void) ensure_bluealsa_running_rate_ex(rate, NULL);

    char * pair_argv[] = { (char *) "bluetoothctl", (char *) "pair", (char *) mac, NULL };
    subprocess_run(pair_argv, out, sizeof(out)); /* no-op if already paired -- not fatal either way */

    char * trust_argv[] = { (char *) "bluetoothctl", (char *) "trust", (char *) mac, NULL };
    subprocess_run(trust_argv, out, sizeof(out));

    char * connect_argv[] = { (char *) "bluetoothctl", (char *) "connect", (char *) mac, NULL };
    bool ok = subprocess_run(connect_argv, out, sizeof(out)) && strstr(out, "Failed") == NULL;
    pthread_mutex_unlock(&bt_chip_mutex);
    return ok;
}

bool bt_control_connect(const char * mac) {
    return bt_control_connect_with_rate(mac, bt_control_get_sample_rate());
}

bool bt_control_disconnect(const char * mac) {
    char out[512];
    atomic_fetch_add(&bt_user_disconnect_generation, 1);
    if (mac) {
        /* Asked for here, so not the codec's fault (see bt_auto_note_disconnect). */
        char path[64];
        snprintf(path, sizeof(path), "/dev_%s/", mac);
        for (char * c = path; *c; c++) if (*c == ':') *c = '_';
        bt_auto_forget_switch(path);
    }
    pthread_mutex_lock(&bt_chip_mutex);
    char * argv[] = { (char *) "bluetoothctl", (char *) "disconnect", (char *) mac, NULL };
    bool ok = subprocess_run(argv, out, sizeof(out)) && strstr(out, "Failed") == NULL;
    pthread_mutex_unlock(&bt_chip_mutex);
    return ok;
}

bool bt_control_forget(const char * mac) {
    char out[512];
    pthread_mutex_lock(&bt_chip_mutex);
    char * argv[] = { (char *) "bluetoothctl", (char *) "remove", (char *) mac, NULL };
    bool ok = subprocess_run(argv, out, sizeof(out)) && strstr(out, "Failed") == NULL;
    pthread_mutex_unlock(&bt_chip_mutex);
    return ok;
}

#define BT_RECONNECT_QUERY_TIMEOUT_MS 1500
#define BT_RECONNECT_CONNECT_TIMEOUT_MS 6500

static bool bt_reconnect_cancelled(bt_control_cancel_callback_t cancel_cb, void * cancel_ctx) {
    return cancel_cb && cancel_cb(cancel_ctx);
}

static bool bt_reconnect_valid_mac(const char * mac) {
    if (!mac || strlen(mac) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (mac[i] != ':') return false;
        } else if (!((mac[i] >= '0' && mac[i] <= '9') ||
                     (mac[i] >= 'A' && mac[i] <= 'F') ||
                     (mac[i] >= 'a' && mac[i] <= 'f'))) return false;
    }
    return true;
}

static bool bt_reconnect_run(char * const argv[], char * output, size_t output_size,
                             int timeout_ms, bt_control_cancel_callback_t cancel_cb,
                             void * cancel_ctx, int * exit_code) {
    if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;
    bool ran = subprocess_run_checked(argv, output, output_size, timeout_ms, exit_code);
    if (!ran || (exit_code && *exit_code != 0)) return false;
    /* An incomplete snapshot cannot establish that another output is absent. */
    if (output && output_size && strnlen(output, output_size) >= output_size - 1) return false;
    return !bt_reconnect_cancelled(cancel_cb, cancel_ctx);
}

static bool bt_reconnect_info(const char * mac, char * output, size_t output_size,
                              bt_control_cancel_callback_t cancel_cb, void * cancel_ctx) {
    char * argv[] = { (char *) "bluetoothctl", (char *) "info", (char *) mac, NULL };
    int exit_code = -1;
    return bt_reconnect_run(argv, output, output_size, BT_RECONNECT_QUERY_TIMEOUT_MS,
                            cancel_cb, cancel_ctx, &exit_code);
}

static bool bt_reconnect_audio_output_connected(bt_control_cancel_callback_t cancel_cb,
                                                void * cancel_ctx, bool * query_ok) {
    char output[4096] = {0};
    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "list-pcms", NULL };
    int exit_code = -1;
    bool ok = bt_reconnect_run(argv, output, sizeof(output), BT_RECONNECT_QUERY_TIMEOUT_MS,
                               cancel_cb, cancel_ctx, &exit_code);
    if (query_ok) *query_ok = ok;
    if (!ok) return false;
    return strstr(output, "/a2dpsrc/sink") != NULL;
}

/* Do not call bt_control_is_powered() from the reconnect critical section:
 * that public status path has its own recovery behavior and can re-enter
 * chip/daemon lifecycle code. This is the private, bounded read-only query
 * needed by the worker and deliberately never powers the adapter on. */
static bool bt_reconnect_adapter_powered(bt_control_cancel_callback_t cancel_cb,
                                         void * cancel_ctx) {
    char output[2048] = {0};
    char * argv[] = { (char *) "bluetoothctl", (char *) "show", NULL };
    int exit_code = -1;
    return bt_reconnect_run(argv, output, sizeof(output), BT_RECONNECT_QUERY_TIMEOUT_MS,
                            cancel_cb, cancel_ctx, &exit_code) &&
           strstr(output, "Powered: yes") != NULL;
}

static bool bt_reconnect_info_line_is(const char * info, const char * key, const char * value) {
    size_t key_len = strlen(key);
    const char * line = info;
    while (line && *line) {
        while (*line == ' ' || *line == '\t') line++;
        if (strncmp(line, key, key_len) == 0) {
            const char * p = line + key_len;
            while (*p == ' ' || *p == '\t') p++;
            size_t value_len = strcspn(p, "\r\n");
            while (value_len && (p[value_len - 1] == ' ' || p[value_len - 1] == '\t')) value_len--;
            if (strlen(value) == value_len && strncmp(p, value, value_len) == 0) return true;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return false;
}

static bool bt_reconnect_info_has_uuid(const char * info, const char * uuid) {
    size_t key_len = strlen("UUID:");
    const char * line = info;
    while (line && *line) {
        while (*line == ' ' || *line == '\t') line++;
        if (strncmp(line, "UUID:", key_len) == 0) {
            const char * end = strchr(line, '\n');
            size_t line_len = end ? (size_t)(end - line) : strlen(line);
            const char * match = strstr(line + key_len, uuid);
            if (match && (size_t)(match - line) + strlen(uuid) <= line_len) return true;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return false;
}

static bool bt_reconnect_info_eligible(const char * info, bool * connected) {
    if (connected) *connected = false;
    if (!info || !bt_reconnect_info_line_is(info, "Paired:", "yes") ||
            !bt_reconnect_info_line_is(info, "Trusted:", "yes") ||
            bt_reconnect_info_line_is(info, "Blocked:", "yes") ||
            !bt_reconnect_info_has_uuid(info, "0000110b-0000-1000-8000-00805f9b34fb")) return false;
    if (connected) *connected = bt_reconnect_info_line_is(info, "Connected:", "yes");
    return true;
}

static bool bt_reconnect_disconnect_locked(const char * mac) {
    char output[512] = {0};
    char * argv[] = { (char *) "bluetoothctl", (char *) "disconnect", (char *) mac, NULL };
    int exit_code = -1;
    return subprocess_run_checked(argv, output, sizeof(output),
                                  BT_RECONNECT_QUERY_TIMEOUT_MS, &exit_code) &&
           exit_code == 0 && strstr(output, "Failed") == NULL;
}

static bool bt_control_reconnect_paired_core(const char * preferred_mac, unsigned int rate,
                                            bool prepare_daemon,
                                            bt_control_cancel_callback_t cancel_cb,
                                            void * cancel_ctx) {
    if (preferred_mac && (!bt_reconnect_valid_mac(preferred_mac) ||
                          bt_reconnect_cancelled(cancel_cb, cancel_ctx))) return false;
    if (!bt_reconnect_adapter_powered(cancel_cb, cancel_ctx)) return false;
    bool pcm_query_ok = false;
    if (bt_reconnect_audio_output_connected(cancel_cb, cancel_ctx, &pcm_query_ok)) return true;
    if (!pcm_query_ok) return false;
    if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;

    char target[18] = {0};
    char info[4096] = {0};
    bool was_connected = false;
    if (preferred_mac) {
        snprintf(target, sizeof(target), "%s", preferred_mac);
        if (!bt_reconnect_info(target, info, sizeof(info), cancel_cb, cancel_ctx) ||
                !bt_reconnect_info_eligible(info, &was_connected)) return false;
    } else {
        char devices[4096] = {0};
        char * devices_argv[4];
        if (!bt_control_paired_devices_argv_checked(devices_argv, BT_RECONNECT_QUERY_TIMEOUT_MS,
                                                     cancel_cb, cancel_ctx)) return false;
        int exit_code = -1;
        if (!bt_reconnect_run(devices_argv, devices, sizeof(devices),
                              BT_RECONNECT_QUERY_TIMEOUT_MS, cancel_cb, cancel_ctx, &exit_code)) return false;
        size_t devices_len = strnlen(devices, sizeof(devices));
        if (devices_len == 0 || devices_len >= sizeof(devices) - 1 || devices[devices_len - 1] != '\n') return false;
        unsigned int eligible = 0;
        char * save = NULL;
        for (char * line = strtok_r(devices, "\r\n", &save); line;
             line = strtok_r(NULL, "\r\n", &save)) {
            if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;
            if (strncmp(line, "Device ", 7) != 0) continue;
            const char * mac = line + 7;
            const char * name = strchr(mac, ' ');
            char candidate[18];
            if (!name || (size_t)(name - mac) != 17) continue;
            memcpy(candidate, mac, 17);
            candidate[17] = '\0';
            if (!bt_reconnect_valid_mac(candidate)) continue;
            char candidate_info[4096] = {0};
            bool candidate_connected = false;
            if (!bt_reconnect_info(candidate, candidate_info, sizeof(candidate_info),
                                   cancel_cb, cancel_ctx)) return false;
            if (bt_reconnect_info_eligible(candidate_info, &candidate_connected)) {
                eligible++;
                snprintf(target, sizeof(target), "%s", candidate);
                snprintf(info, sizeof(info), "%s", candidate_info);
                was_connected = candidate_connected;
                if (eligible > 1) return false;
            }
        }
        if (eligible != 1) return false;
    }
    if (was_connected) return true;
    if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;

    pthread_mutex_lock(&bt_chip_mutex);
    bool result = false;
    bool connected_after = false;
    if (!bt_reconnect_cancelled(cancel_cb, cancel_ctx) &&
            bt_reconnect_adapter_powered(cancel_cb, cancel_ctx) &&
            bt_reconnect_audio_output_connected(cancel_cb, cancel_ctx, &pcm_query_ok) == false &&
            pcm_query_ok) {
        /* Refresh under the chip lock so a connection made by another path
         * before lock acquisition is treated as preexisting. */
        char locked_info[4096] = {0};
        bool locked_connected = false;
        if (!bt_reconnect_info(target, locked_info, sizeof(locked_info), NULL, NULL) ||
                !bt_reconnect_info_eligible(locked_info, &locked_connected)) {
            pthread_mutex_unlock(&bt_chip_mutex);
            return false;
        }
        if (locked_connected) {
            pthread_mutex_unlock(&bt_chip_mutex);
            return true;
        }
        if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) {
            pthread_mutex_unlock(&bt_chip_mutex);
            return false;
        }
        if (prepare_daemon) {
            bool deferred = false;
            if (!ensure_bluealsa_running_rate_ex(rate, &deferred) || deferred ||
                    bt_reconnect_cancelled(cancel_cb, cancel_ctx)) {
                pthread_mutex_unlock(&bt_chip_mutex);
                return false;
            }
        }
        char * connect_argv[] = { (char *) "bluetoothctl", (char *) "--timeout", (char *) "5",
                                  (char *) "connect", target, NULL };
        char connect_output[1024] = {0};
        int exit_code = -1;
        bool connect_ran = subprocess_run_checked(connect_argv, connect_output, sizeof(connect_output),
                                                  BT_RECONNECT_CONNECT_TIMEOUT_MS, &exit_code);
        bool connect_ok = connect_ran && exit_code == 0 && strstr(connect_output, "Failed") == NULL;
        /* Verify and, when cancellation raced the command, clean up using an
         * uncancelled bounded read. The callback is intentionally not used
         * for this cleanup observation. */
        if (bt_reconnect_info(target, info, sizeof(info), NULL, NULL))
            connected_after = bt_reconnect_info_eligible(info, &was_connected) && was_connected;
        if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) {
            (void) bt_reconnect_disconnect_locked(target);
        } else {
            result = connect_ok && connected_after;
        }
    }
    pthread_mutex_unlock(&bt_chip_mutex);
    return result;
}

bool bt_control_reconnect_paired_with_rate(const char * preferred_mac, unsigned int rate,
                                           bt_control_cancel_callback_t cancel_cb,
                                           void * cancel_ctx) {
    return bt_control_reconnect_paired_core(preferred_mac, rate, true, cancel_cb, cancel_ctx);
}

bool bt_control_reconnect_paired(const char * preferred_mac,
                                 bt_control_cancel_callback_t cancel_cb,
                                 void * cancel_ctx) {
    return bt_control_reconnect_paired_with_rate(preferred_mac, bt_control_get_sample_rate(),
                                                  cancel_cb, cancel_ctx);
}

/* Synchronizes volume between this player and connected a2dp-source accessories.
 * Maps AVRCP 0-127 linearly to the player's 0-100% volume. */
/* apply_soft_volume: discovery also applies the SoftVolume preference, as a
 * source PCM can predate a monitor subscription. The output watch's worker
 * passes false and applies it in its own cancellable pass instead. */
static bool find_source_pcm_path_ex(char * out, size_t out_size, bool apply_soft_volume) {
    char list_out[4096];
    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "list-pcms", NULL };
    if (!subprocess_run_low_priority(argv, list_out, sizeof(list_out))) {
        /* Log failure if bluealsactl list-pcms fails or times out. */
        DBG_LOG("bt_control: find_source_pcm_path: bluealsactl list-pcms failed/timed out\n");
        return false;
    }

    char * line_save = NULL;
    char * line = strtok_r(list_out, "\n", &line_save);
    while (line) {
        /* "a2dpsrc" (not "a2dpsnk") distinguishes this from a DAC-mode PCM
         * if one ever coexisted; "/sink" is bluealsa's own role name for
         * the PCM WE write into (confirmed live: `bluealsactl list-pcms`
         * on a real connected device returned exactly
         * "/org/bluealsa/hci0/dev_XX_XX_XX_XX_XX_XX/a2dpsrc/sink"). */
        if (strstr(line, "/a2dpsrc/sink") != NULL) {
            snprintf(out, out_size, "%s", line);
            /* A source PCM can predate this monitor subscription, so apply
             * the v5 SoftVolume preference during discovery as well as on
             * PCMAdded. */
            if (apply_soft_volume) bluealsa_apply_soft_volume(out);
            return true;
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return false;
}

static bool find_source_pcm_path(char * out, size_t out_size) {
    return find_source_pcm_path_ex(out, out_size, true);
}

static bool bt_correction_cancelled(bt_control_cancel_callback_t cb, void * ctx) {
    return cb && cb(ctx);
}

typedef struct {
    bt_control_cancel_callback_t callback;
    void * context;
} bt_ssc_discovery_cancel_t;

static bool bt_ssc_discovery_cancelled(void * opaque) {
    bt_ssc_discovery_cancel_t * cancel = opaque;
    return !bt_ssc_recovery_preference() ||
           bt_correction_cancelled(cancel->callback, cancel->context);
}

/* BlueZ Device1.DisconnectProfile / ConnectProfile for the A2DP sink: only
 * the profile is cycled, so the ACL, HFP and AVRCP stay up and the player's
 * own AVDTP session re-discovers the remote endpoints. The caller holds
 * bt_chip_mutex. */
static bool bt_a2dp_profile_call_locked(const char * mac, const char * method) {
    char object[64], member[48], arg[64];
    snprintf(object, sizeof(object), "/org/bluez/hci0/dev_%s", mac);
    for (char * c = object; *c; c++) if (*c == ':') *c = '_';
    snprintf(member, sizeof(member), "org.bluez.Device1.%s", method);
    snprintf(arg, sizeof(arg), "string:%s", BT_A2DP_SINK_UUID);
    char * argv[] = { (char *) "dbus-send", (char *) "--system", (char *) "--print-reply",
                      (char *) "--dest=org.bluez", object, member, arg, NULL };
    int status = -1;
    return subprocess_run_checked(argv, NULL, 0, BT_RECONNECT_CONNECT_TIMEOUT_MS, &status) && status == 0;
}

/* After DisconnectProfile: wait for the output to go, then ConnectProfile.
 * A cancel stops the wait but ConnectProfile is still issued once, so A2DP
 * is not left down under a live ACL; only the user's own disconnect skips
 * it. False when the profile did not come back, leaving the full reconnect
 * to the caller. */
static bool bt_ssc_profile_reconnect(const char * mac, bt_ssc_discovery_cancel_t * cancel,
                                     unsigned int user_generation) {
    uint32_t deadline = bt_control_monotonic_ms() + 5000;
    while (!bt_ssc_discovery_cancelled(cancel)) {
        bool query_ok = false;
        bool live = bt_reconnect_audio_output_connected(bt_ssc_discovery_cancelled, cancel, &query_ok);
        if (!query_ok && !bt_ssc_discovery_cancelled(cancel)) return false;
        if (!live) break;
        if ((int32_t)(deadline - bt_control_monotonic_ms()) <= 0) return false;
        usleep(250000);
    }
    if (atomic_load(&bt_user_disconnect_generation) != user_generation) return false;
    pthread_mutex_lock(&bt_chip_mutex);
    bool ok = bt_a2dp_profile_call_locked(mac, "ConnectProfile");
    pthread_mutex_unlock(&bt_chip_mutex);
    return ok;
}

bool bt_control_reconnect_for_ssc_discovery(const char * mac, unsigned int rate,
                                            bt_control_cancel_callback_t cancel_cb,
                                            void * cancel_ctx) {
    bt_ssc_discovery_cancel_t cancel = { cancel_cb, cancel_ctx };
    unsigned int user_generation = atomic_load(&bt_user_disconnect_generation);
    if (!bt_reconnect_valid_mac(mac) || bt_ssc_discovery_cancelled(&cancel)) return false;
    char expected[64];
    snprintf(expected, sizeof(expected), "/dev_%s/", mac);
    for (char * c = expected; *c; c++) if (*c == ':') *c = '_';

    /* Revalidate the target and discovery result under the same lifecycle
     * lock as manual operations. Never disturb another connected output or
     * a peer whose SSC endpoint appeared while the GUI was busy. */
    pthread_mutex_lock(&bt_chip_mutex);
    char list[4096], path[256] = {0}, info[4096];
    char * list_argv[] = { (char *) bluealsa_ctl_name(), "list-pcms", NULL };
    int status = -1;
    bool valid = bt_reconnect_run(list_argv, list, sizeof(list), BT_RECONNECT_QUERY_TIMEOUT_MS,
                                  bt_ssc_discovery_cancelled, &cancel, &status);
    if (valid) {
        char * save = NULL;
        for (char * line = strtok_r(list, "\r\n", &save); line;
             line = strtok_r(NULL, "\r\n", &save)) {
            if (!strstr(line, "/a2dpsrc/sink")) continue;
            if (path[0] || !strstr(line, expected) || strlen(line) >= sizeof(path)) {
                valid = false;
                break;
            }
            snprintf(path, sizeof(path), "%s", line);
        }
    }
    char * info_argv[] = { (char *) bluealsa_ctl_name(), "info", path, NULL };
    valid = valid && path[0] &&
            bt_reconnect_run(info_argv, info, sizeof(info), BT_RECONNECT_QUERY_TIMEOUT_MS,
                              bt_ssc_discovery_cancelled, &cancel, &status);
    if (valid) bt_ssc_policy_offer_selected(info, sizeof(info)); /* in use counts as offered */
    if (valid && bt_ssc_policy_endpoint_offered(info)) {
        pthread_mutex_unlock(&bt_chip_mutex);
        return true;
    }
    bool connected = false;
    valid = valid && bluealsa_available_codecs_known(info) &&
            !(bt_auto_rejected(path) & (1u << bt_auto_rank_index("samsung-SC"))) &&
            bt_reconnect_info(mac, info, sizeof(info), bt_ssc_discovery_cancelled, &cancel) &&
            bt_reconnect_info_eligible(info, &connected) && connected &&
            !bt_ssc_discovery_cancelled(&cancel);
    bool disconnect_issued = false;
    bool disconnected = false;
    bool profile_issued = false;
    char * disconnect_argv[] = { "bluetoothctl", "--timeout", "5", "disconnect", (char *) mac, NULL };
    if (valid) {
        bt_auto_forget_switch(path);
        bt_auto_expect_reconnect(path);
        DBG_LOG("bt_control: reconnecting %s to rediscover SSC endpoints\n", mac);
        /* Prefer cycling only the A2DP profile; the whole device is the
         * fallback when that cannot be done. */
        profile_issued = bt_a2dp_profile_call_locked(mac, "DisconnectProfile");
        disconnect_issued = true;
        if (!profile_issued)
            disconnected = subprocess_run_checked(disconnect_argv, NULL, 0,
                                                  BT_RECONNECT_CONNECT_TIMEOUT_MS, &status) && status == 0;
    }
    pthread_mutex_unlock(&bt_chip_mutex);
    if (profile_issued) {
        if (bt_ssc_profile_reconnect(mac, &cancel, user_generation)) return true;
        if (bt_ssc_discovery_cancelled(&cancel)) return false;
        DBG_LOG("bt_control: A2DP profile reconnect of %s failed, reconnecting the device\n", mac);
        pthread_mutex_lock(&bt_chip_mutex);
        disconnected = subprocess_run_checked(disconnect_argv, NULL, 0,
                                              BT_RECONNECT_CONNECT_TIMEOUT_MS, &status) && status == 0;
        pthread_mutex_unlock(&bt_chip_mutex);
    }
    if (!disconnect_issued || bt_ssc_discovery_cancelled(&cancel)) return false;
    if (!disconnected) goto reconnect_fallback;

    uint32_t deadline = bt_control_monotonic_ms() + 5000;
    while ((int32_t)(deadline - bt_control_monotonic_ms()) > 0) {
        if (bt_ssc_discovery_cancelled(&cancel)) return false;
        bool query_ok = false;
        bool live = bt_reconnect_audio_output_connected(bt_ssc_discovery_cancelled, &cancel, &query_ok);
        if (!query_ok || !live) goto reconnect_fallback;
        usleep(250000);
    }
reconnect_fallback:
    /* Disconnect may have taken effect even if its CLI timed out. Once we
     * issued it, always offer one target-bound reconnect unless cancelled;
     * the core independently refuses unknown state or another live output. */
    if (bt_ssc_discovery_cancelled(&cancel)) return false;
    return bt_control_reconnect_paired_core(mac, rate, true,
                                             bt_ssc_discovery_cancelled, &cancel);
}

static bool bt_ssc_rate_correction_run(const char * mac, unsigned int rate,
                                       bt_control_cancel_callback_t cancel_cb, void * cancel_ctx);

bool bt_control_reconnect_for_ssc_rate_correction(const char * mac, unsigned int rate,
                                                   bt_control_cancel_callback_t cancel_cb,
                                                   void * cancel_ctx) {
    atomic_fetch_add(&bt_ssc_correction_running, 1);
    bool ok = bt_ssc_rate_correction_run(mac, rate, cancel_cb, cancel_ctx);
    atomic_fetch_sub(&bt_ssc_correction_running, 1);
    return ok;
}

static bool bt_ssc_rate_correction_run(const char * mac, unsigned int rate,
                                       bt_control_cancel_callback_t cancel_cb, void * cancel_ctx) {
    if (!mac || strlen(mac) != 17 || rate != 44100 || bt_correction_cancelled(cancel_cb, cancel_ctx)) return false;
    bool pcm_query_ok = false;
    if (!bt_reconnect_audio_output_connected(cancel_cb, cancel_ctx, &pcm_query_ok) ||
            !pcm_query_ok || bt_correction_cancelled(cancel_cb, cancel_ctx)) return false;
    char path[256];
    if (!find_source_pcm_path_ex(path, sizeof(path), false) ||
            bt_correction_cancelled(cancel_cb, cancel_ctx)) return false;
    char expected[64];
    snprintf(expected, sizeof(expected), "/dev_%s/", mac);
    for (char * c = expected; *c; c++) if (*c == ':') *c = '_';
    if (!strstr(path, expected)) return false;

    char info[2048];
    char * info_argv[] = { (char *) bluealsa_ctl_name(), (char *) "info", path, NULL };
    if (!subprocess_run_low_priority(info_argv, info, sizeof(info)) ||
            bt_correction_cancelled(cancel_cb, cancel_ctx)) return false;
    char codec[32] = {0};
    copy_info_field(codec, sizeof(codec), info, "Selected codec:");
    char * codec_end = strchr(codec, ':');
    if (codec_end) *codec_end = '\0';
    unsigned int current = 0;
    const char * rate_field = strstr(info, "Rate:");
    if (rate_field) (void) sscanf(rate_field + strlen("Rate:"), "%u", &current);
    if ((strcasecmp(codec, "samsung-SC") != 0 && strcasecmp(codec, "SSC") != 0) ||
            current == 0 || current == rate)
        return false;

    bt_auto_forget_switch(path);
    bt_auto_expect_reconnect(path);
    if (bt_correction_cancelled(cancel_cb, cancel_ctx)) return false;
    char * disconnect_argv[] = { (char *) "bluetoothctl", (char *) "--timeout", (char *) "5",
                                 (char *) "disconnect", (char *) mac, NULL };
    pthread_mutex_lock(&bt_chip_mutex);
    int exit_code = -1;
    bool disconnected = !bt_correction_cancelled(cancel_cb, cancel_ctx) &&
            subprocess_run_checked(disconnect_argv, NULL, 0, 6500, &exit_code) && exit_code == 0;
    pthread_mutex_unlock(&bt_chip_mutex);
    if (!disconnected || bt_correction_cancelled(cancel_cb, cancel_ctx)) return false;

    uint32_t deadline = bt_control_monotonic_ms() + 5000;
    bool pcm_gone = false;
    while ((int32_t)(deadline - bt_control_monotonic_ms()) > 0) {
        if (bt_correction_cancelled(cancel_cb, cancel_ctx)) return false;
        bool still_connected = bt_reconnect_audio_output_connected(cancel_cb, cancel_ctx, &pcm_query_ok);
        if (!pcm_query_ok) goto reconnect_fallback;
        if (!still_connected) {
            pcm_gone = true;
            break;
        }
        usleep(250000);
    }
    if (bt_correction_cancelled(cancel_cb, cancel_ctx)) return false;
    if (!pcm_gone) goto reconnect_fallback;
    /* The UI may refresh an empty cached MAC and change the global rate while
     * this worker is disconnecting. Use the request captured by this job for
     * both daemon reconciliation and launch flags. */
    bool deferred = false;
    pthread_mutex_lock(&bt_chip_mutex);
    bool ensured = ensure_bluealsa_running_rate_ex(rate, &deferred);
    pthread_mutex_unlock(&bt_chip_mutex);
    if (bt_correction_cancelled(cancel_cb, cancel_ctx)) return false;
    if (!ensured || deferred) goto reconnect_fallback;
    if (!bt_control_reconnect_paired_core(mac, rate, false, cancel_cb, cancel_ctx)) {
        if (bt_correction_cancelled(cancel_cb, cancel_ctx)) return false;
        goto reconnect_fallback;
    }

    deadline = bt_control_monotonic_ms() + 20000;
    while ((int32_t)(deadline - bt_control_monotonic_ms()) > 0) {
        if (bt_correction_cancelled(cancel_cb, cancel_ctx)) return false;
        bool connected = bt_reconnect_audio_output_connected(cancel_cb, cancel_ctx, &pcm_query_ok);
        if (!pcm_query_ok) goto reconnect_fallback;
        if (!connected) {
            usleep(250000);
            continue;
        }
        char active[256];
        if (!find_source_pcm_path_ex(active, sizeof(active), false) || !strstr(active, expected)) {
            usleep(250000);
            continue;
        }
        char * current_argv[] = { (char *) bluealsa_ctl_name(), (char *) "info", active, NULL };
        if (subprocess_run_low_priority(current_argv, info, sizeof(info))) {
            current = 0;
            rate_field = strstr(info, "Rate:");
            if (rate_field) (void) sscanf(rate_field + strlen("Rate:"), "%u", &current);
            if (current == rate) return true;
        }
        usleep(250000);
    }
reconnect_fallback:
    /* Once this attempt has dropped the link, make one safe target-bound
     * reconnect attempt unless the owning job was cancelled. The core checks
     * pairing, current PCM state and cancellation before connecting. */
    if (!bt_correction_cancelled(cancel_cb, cancel_ctx))
        return bt_control_reconnect_paired_core(mac, rate, true, cancel_cb, cancel_ctx);
    return false;
}

/* Require a successfully selected and seeded gain mode before opening audio.
 * Discovery used for connection status may succeed even when control writes fail. */
bool bt_control_prepare_source_volume(void) {
    char path[256];
    if (!find_source_pcm_path_ex(path, sizeof(path), false)) return false;
    /* A PCM can be opened before AVRCP finishes negotiating. Recheck once at
     * the last safe point before audio opens so a capability that arrived
     * since initial discovery can take hardware gain immediately. */
    bluealsa_apply_soft_volume_cancellable_ex(path, NULL, true);
    pthread_mutex_lock(&bt_soft_volume_mutex);
    bool ready = strcmp(bt_soft_volume_applied_path, path) == 0;
    pthread_mutex_unlock(&bt_soft_volume_mutex);
    return ready;
}

/* Public wrapper around the same check find_source_pcm_path() above already
 * does for the AVRCP volume-sync feature -- a "/a2dpsrc/sink" PCM only
 * exists in bluealsa's own list once a real audio-capable accessory has
 * actually negotiated the A2DP sink role with this device acting as
 * a2dp-source, which is a stronger signal than bt_control_is_connected()/
 * bt_control_list_paired_states() (those report ANY paired device with an
 * active connection, which could be a non-audio BLE peripheral with no A2DP
 * profile at all). Used for the topbar's "BT headphone connected" icon
 * (gui.c) -- same subprocess cost as everything else here, call off the UI
 * thread. */
bool bt_control_is_a2dp_source_connected(void) {
    char path[256];
    return find_source_pcm_path(path, sizeof(path));
}

bool bt_control_get_connected_device_mac(char * out, size_t out_size) {
    char path[256];
    if (!find_source_pcm_path(path, sizeof(path))) return false;

    const char * dev = strstr(path, "dev_");
    if (!dev) return false;
    dev += 4; /* skip "dev_" */

    char mac[18];
    if (strlen(dev) < 17) return false; /* "XX_XX_XX_XX_XX_XX" */
    for (int i = 0; i < 17; i++) mac[i] = (dev[i] == '_') ? ':' : dev[i];
    mac[17] = '\0';

    snprintf(out, out_size, "%s", mac);
    return true;
}

bool bt_control_get_connected_device_codec(char * out, size_t out_size) {
    return bt_control_get_connected_device_stream(out, out_size, NULL);
}

/* One `bluealsactl info` for both the codec and the rate: the same call
 * already reported the sampling frequency, it was just being discarded.
 * out_sample_rate may be NULL when only the codec is wanted, and is left
 * untouched when the output reports no frequency. */
bool bt_control_get_connected_device_stream(char * out, size_t out_size, unsigned int * out_sample_rate) {
    char path[256];
    if (!find_source_pcm_path(path, sizeof(path))) return false;

    /* -v appends each codec's configuration blob ("SBC:24150223"), needed
     * to tell SBC-XQ from plain SBC; other fields are unchanged. */
    char info_out[2048];
    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "info", (char *) "-v", path, NULL };
    if (!subprocess_run_low_priority(argv, info_out, sizeof(info_out))) return false;

    /* "Selected codec: AAC" -- confirmed live via `bluealsactl info
     * <pcm-path>` (also reports "Available codecs: SBC AAC", but that's
     * every codec the accessory advertised support for, not what's
     * actually in use right now). */
    char selected[96];
    if (!bluealsa_parse_selected_codec(info_out, selected, sizeof(selected))) return false;
    char codec[32];
    bluealsa_codec_display_name(selected, codec, sizeof(codec));

    if (out_sample_rate) {
        /* Same two spellings the sink-side parse accepts. */
        const char * sampling = strstr(info_out, "Sampling:");
        if (!sampling) sampling = strstr(info_out, "Rate:");
        const char * value = sampling ? strchr(sampling, ':') : NULL;
        if (value) (void) sscanf(value + 1, "%u", out_sample_rate);
    }

    snprintf(out, out_size, "%s", codec);
    return true;
}

/* Writes an ALSA PCM definition whose plug slave is pinned to the rate the
 * A2DP transport already negotiated, and reports the PCM name to open.
 *
 * Opening the BlueALSA PCM at the track's own rate instead is destructive:
 * on a mismatch the plugin calls SelectCodec, and in BlueALSA 5 that is a
 * transport recreation, not a reconfiguration ("A2DP codec selection is in
 * fact a transport recreation", ba-transport.c), so the accessory drops --
 * and the call fails outright on devices that refuse the new configuration.
 * Pinning the slave rate makes ALSA convert the track instead, which is what
 * PipeWire, PulseAudio and Android all do with an A2DP transport.
 *
 * The definition names the accessory explicitly rather than relying on
 * BlueALSA's "most recently connected" default, so it cannot follow a
 * different device than the one just queried. Written atomically: a
 * half-written file in HOME would break every ALSA open, not just this one.
 *
 * Returns false when the transport cannot be read, leaving the caller to
 * open bt_control_get_playback_pcm() directly as before. */
/* Rates the accessory itself advertises, read from BlueZ rather than from
 * BlueALSA. BlueALSA reports the negotiated intersection, and
 * --a2dp-force-audio-cd rewrites its own advertised capability to 44.1 kHz
 * alone (a2dp-aac.c and friends), so asking it while the clamp is on would
 * only ever answer "44.1". BlueZ still exposes each remote endpoint under
 * org.bluez.MediaEndpoint1 with the capability bytes the accessory actually
 * sent, which is what this decodes. */

/* Value of one dbus-send property, as the tool prints it. */
static const char * dbus_prop_value(const char * out, const char * prop) {
    char needle[64];
    snprintf(needle, sizeof(needle), "string \"%s\"", prop);
    const size_t needle_len = strlen(needle);
    const char * p = out;
    while ((p = strstr(p, needle)) != NULL) {
        const char * value = p + needle_len;
        while (isspace((unsigned char) *value)) value++;
        /* A property key is followed immediately by its variant value. This
         * avoids treating the same text inside an unrelated string value as
         * a property. */
        if (strncmp(value, "variant", 7) == 0 &&
                (isspace((unsigned char) value[7]) || value[7] == '\0')) {
            value += 7;
            while (isspace((unsigned char) *value)) value++;
            return value;
        }
        p += needle_len;
    }
    return NULL;
}

static bool dbus_prop_string_equals(const char * out, const char * prop,
                                    const char * expected) {
    const char * value = dbus_prop_value(out, prop);
    if (!value) return false;
    while (isspace((unsigned char) *value)) value++;
    if (strncmp(value, "string", 6) != 0 ||
            !isspace((unsigned char) value[6])) return false;
    value += 6;
    while (isspace((unsigned char) *value)) value++;
    if (*value++ != '"') return false;
    const char * end = strchr(value, '"');
    if (!end) return false;
    size_t len = (size_t) (end - value);
    return len == strlen(expected) && strncasecmp(value, expected, len) == 0;
}

/* dbus-send prints byte arrays differently across versions: this device
 * emits bare "80 01 8c", others "byte 0x80, byte 0x01". Both the "byte"
 * token and the "0x" prefix are therefore optional, because requiring
 * either one silently yields an empty capability list on the other format. */
static bool dbus_parse_hex_byte(const char ** cursor, uint8_t * out) {
    const char * p = *cursor;
    while (isspace((unsigned char) *p)) p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;

    unsigned int value = 0;
    int digits = 0;
    while (isxdigit((unsigned char) *p) && digits < 2) {
        value <<= 4;
        if (*p >= '0' && *p <= '9') value += (unsigned int) (*p - '0');
        else if (*p >= 'a' && *p <= 'f') value += (unsigned int) (*p - 'a' + 10);
        else value += (unsigned int) (*p - 'A' + 10);
        p++;
        digits++;
    }
    if (digits == 0 || isxdigit((unsigned char) *p)) return false;
    *out = (uint8_t) value;
    *cursor = p;
    return true;
}

static bool dbus_prop_byte(const char * out, const char * prop, uint8_t * value) {
    const char * p = dbus_prop_value(out, prop);
    if (!p) return false;
    while (isspace((unsigned char) *p)) p++;
    if (strncmp(p, "byte", 4) != 0 || !isspace((unsigned char) p[4])) return false;
    p += 4;
    while (isspace((unsigned char) *p)) p++;

    /* Scalar D-Bus bytes are decimal in current dbus-send output. Older
     * captures used an explicit 0x prefix, so accept that form as hex while
     * keeping bare array bytes on dbus_parse_hex_byte() above. */
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
        if (!isxdigit((unsigned char) *p)) return false;
    } else if (!isdigit((unsigned char) *p)) return false;

    errno = 0;
    char * end;
    unsigned long parsed = strtoul(p, &end, base);
    if (end == p || errno == ERANGE || parsed > UINT8_MAX) return false;
    if (*end && !isspace((unsigned char) *end) && *end != ',' &&
        *end != ')' && *end != ']' && *end != '}') return false;
    *value = (uint8_t) parsed;
    return true;
}

/* dbus-send prints a byte array as e.g. "array [ byte 0x80 byte 0x01 ]";
 * line wrapping and optional commas are presentation details, not separators
 * that can be ignored while starting at the first character after '['. */
static bool dbus_prop_byte_array(const char * out, const char * prop,
                                 uint8_t * bytes, size_t bytes_size,
                                 size_t * out_len) {
    const char * p = dbus_prop_value(out, prop);
    if (!p) return false;
    p = strchr(p, '[');
    if (!p) return false;
    p++;

    size_t len = 0;
    for (;;) {
        while (isspace((unsigned char) *p) || *p == ',') p++;
        if (*p == ']') {
            *out_len = len;
            return len != 0;
        }
        if (*p == '\0' || len >= bytes_size) return false;
        if (strncmp(p, "byte", 4) == 0 && isspace((unsigned char) p[4])) p += 4;
        if (!dbus_parse_hex_byte(&p, &bytes[len])) return false;
        len++;
    }
}

/* BlueALSA exposes the standard codec byte separately and vendor codec IDs
 * in the first six bytes of a vendor capability blob. Match both, because
 * the selected codec name alone cannot distinguish two remote vendor SEPs. */
static bool a2dp_caps_match_codec(const uint8_t * caps, size_t len,
                                  uint8_t codec_id, const char * codec) {
    if (strcasecmp(codec, "SBC") == 0) return codec_id == 0x00;
    if (strcasecmp(codec, "AAC") == 0) return codec_id == 0x02;
    if (len < 6 || codec_id != 0xff) return false;

    uint32_t vendor_id = (uint32_t) caps[0] |
                         ((uint32_t) caps[1] << 8) |
                         ((uint32_t) caps[2] << 16) |
                         ((uint32_t) caps[3] << 24);
    uint16_t vendor_codec_id = (uint16_t) caps[4] | ((uint16_t) caps[5] << 8);
    if (strcasecmp(codec, "aptX") == 0)
        return vendor_id == 0x004f && vendor_codec_id == 0x0001;
    if (strcasecmp(codec, "aptX-HD") == 0)
        return vendor_id == 0x00d7 && vendor_codec_id == 0x0024;
    if (strcasecmp(codec, "LDAC") == 0)
        return vendor_id == 0x012d && vendor_codec_id == 0x00aa;
    if (strcasecmp(codec, "samsung-SC") == 0 || strcasecmp(codec, "SSC") == 0)
        return vendor_id == 0x0075 && vendor_codec_id == 0x0103;
    return false;
}

/* A2DP capability bytes are transmitted most-significant-bit first, so each
 * codec's rate bitfield is read straight off the wire layout in
 * bluetooth-a2dp.h rather than through its packed struct. */
static int a2dp_caps_rates(const uint8_t * caps, size_t len, const char * codec,
                           unsigned int * out, int max_count) {
    typedef struct { unsigned int mask, rate; } rate_bit_t;
    static const rate_bit_t sbc_aptx[] = {
        { 1u << 3, 16000 }, { 1u << 2, 32000 }, { 1u << 1, 44100 }, { 1u << 0, 48000 },
    };
    static const rate_bit_t aac[] = {
        { 1u << 11, 8000 }, { 1u << 10, 11025 }, { 1u << 9, 12000 }, { 1u << 8, 16000 },
        { 1u << 7, 22050 }, { 1u << 6, 24000 }, { 1u << 5, 32000 }, { 1u << 4, 44100 },
        { 1u << 3, 48000 }, { 1u << 2, 64000 }, { 1u << 1, 88200 }, { 1u << 0, 96000 },
    };
    static const rate_bit_t ldac[] = {
        { 1u << 5, 44100 }, { 1u << 4, 48000 }, { 1u << 3, 88200 },
        { 1u << 2, 96000 }, { 1u << 1, 176400 }, { 1u << 0, 192000 },
    };
    static const rate_bit_t ssc[] = {
        { 0x20, 44100 }, { 0x14, 48000 }, { 0x82, 96000 },
    };

    const rate_bit_t * table = NULL;
    size_t table_n = 0;
    unsigned int field = 0;

    if (strcasecmp(codec, "SBC") == 0) {
        if (len < 1) return 0;
        field = caps[0] >> 4;
        table = sbc_aptx; table_n = sizeof(sbc_aptx) / sizeof(sbc_aptx[0]);
    } else if (strcasecmp(codec, "AAC") == 0) {
        if (len < 3) return 0;
        field = ((unsigned int) caps[1] << 4) | (caps[2] >> 4);
        table = aac; table_n = sizeof(aac) / sizeof(aac[0]);
    } else if (len >= 7) { /* vendor codecs carry 4-byte vendor + 2-byte codec first */
        if (strcasecmp(codec, "LDAC") == 0) {
            field = caps[6] & 0x3f;
            table = ldac; table_n = sizeof(ldac) / sizeof(ldac[0]);
        } else if (strcasecmp(codec, "aptX") == 0 ||
                   strcasecmp(codec, "aptX-HD") == 0) {
            /* a2dp_aptx_hd_t embeds a2dp_aptx_t, so both rate fields are at
             * byte 6 despite aptX-HD's additional trailing reserved bytes. */
            field = caps[6] >> 4;
            table = sbc_aptx; table_n = sizeof(sbc_aptx) / sizeof(sbc_aptx[0]);
        } else if (strcasecmp(codec, "samsung-SC") == 0 ||
                   strcasecmp(codec, "SSC") == 0) {
            field = caps[6];
            table = ssc; table_n = sizeof(ssc) / sizeof(ssc[0]);
        }
    }
    if (!table) return 0;

    /* Tables run low to high, so a forward walk lists rates in order. */
    int count = 0;
    for (size_t i = 0; i < table_n && count < max_count; i++)
        if (field & table[i].mask) out[count++] = table[i].rate;
    return count;
}

/* Re-establishes the accessory's link so a new transport rate takes effect.
 *
 * The rate is fixed when the A2DP configuration is negotiated, and these
 * accessories refuse to renegotiate a live transport: SelectCodec on a
 * connected PCM returns EIO and takes the link down with it (measured on a
 * real headset, in both clamped and unclamped daemons). Cycling the device
 * link is therefore the only thing that actually applies a rate change --
 * but only the link needs cycling, not the radio, so this does it instead
 * of asking the user to turn Bluetooth off and on.
 *
 * Blocks for several seconds. Callers must run it off the UI thread. */
bool bt_control_reconnect_for_rate_change(unsigned int want) {
    char mac[18];
    if (!bt_control_get_connected_device_mac(mac, sizeof(mac))) return false;
    bool automatic = want == 0;
    /* Re-asserted rather than read from the atomic: disconnecting below makes
     * the status refresh publish "nothing connected", which would otherwise
     * overwrite the requested rate with the global default before the daemon
     * is respawned with it. Keep Automatic's zero intact so the daemon can
     * enable --ssc-auto-profile on the replacement connection. */
    atomic_store(&bt_sample_rate, want);

    /* Try reconfiguring the live transport first. A Galaxy Buds2 Pro refuses
     * this with EIO and drops the link whether or not it is streaming
     * (measured), but accessories that accept it change rate with nothing
     * dropping at all, so it is worth one attempt before cycling the link. */
    char path[256];
    if (!automatic && find_source_pcm_path(path, sizeof(path))) {
        char info_out[2048];
        char * info_argv[] = { (char *) bluealsa_ctl_name(), (char *) "info", path, NULL };
        if (subprocess_run_low_priority(info_argv, info_out, sizeof(info_out))) {
            char codec[32] = {0};
            copy_info_field(codec, sizeof(codec), info_out, "Selected codec:");
            char * codec_end = strchr(codec, ':');
            if (codec_end) *codec_end = '\0';

            if (codec[0]) {
                char rate_str[16];
                snprintf(rate_str, sizeof(rate_str), "%u", want);
                char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "codec",
                                  (char *) "-r", rate_str, path, codec, NULL };
                (void) subprocess_run_checked(argv, NULL, 0, 5000, NULL);

                /* Believe the transport, not the exit status: a refusal can
                 * still leave the link up at the old rate. */
                unsigned int now = 0;
                if (find_source_pcm_path(path, sizeof(path)) &&
                        subprocess_run_low_priority(info_argv, info_out, sizeof(info_out))) {
                    const char * field = strstr(info_out, "Rate:");
                    if (field) (void) sscanf(field + strlen("Rate:"), "%u", &now);
                }
                if (now == want) return true; /* accepted in place, nothing dropped */
            }
        }
    }

    bt_control_disconnect(mac);
    /* The PCM outlives the disconnect briefly, and ensure_bluealsa_running()
     * refuses to respawn while it still sees one, so the respawn would be
     * skipped and the old arguments kept. Wait for it to go. */
    for (int waited_ms = 0; waited_ms < 5000; waited_ms += 250) {
        char gone[256];
        if (!find_source_pcm_path(gone, sizeof(gone))) break;
        usleep(250000);
    }
    /* A disconnect can refresh settings while the link is down. Reassert
     * the captured request immediately before building daemon arguments. */
    atomic_store(&bt_sample_rate, automatic ? 0 : want);
    bt_control_connect_with_rate(mac, want);

    /* Believe the transport, not bluetoothctl's exit: it reports failure on a
     * slow connect that then completes on its own a few seconds later, which
     * otherwise told the user it had failed right before it worked. */
    /* Bounded on the clock, not on iterations: each probe forks a subprocess
     * that can itself take up to the subprocess timeout, so counting rounds
     * would let this run for minutes and stall a teardown that joins it. */
    uint32_t deadline = bt_control_monotonic_ms() + 20000;
    while ((int32_t) (deadline - bt_control_monotonic_ms()) > 0) {
        usleep(500000);
        char path[256];
        if (!find_source_pcm_path(path, sizeof(path))) continue;
        char info_out[2048];
        char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "info", path, NULL };
        if (!subprocess_run_low_priority(argv, info_out, sizeof(info_out))) continue;
        unsigned int now = 0;
        const char * field = strstr(info_out, "Rate:");
        if (field) (void) sscanf(field + strlen("Rate:"), "%u", &now);
        if (automatic ? now != 0 : now == want) return true;
    }
    return false;
}

int bt_control_get_available_rates(unsigned int * out, int max_count) {
    if (!out || max_count <= 0) return 0;

    char path[256];
    if (!find_source_pcm_path(path, sizeof(path))) return 0;

    char info_out[2048];
    char * info_argv[] = { (char *) bluealsa_ctl_name(), (char *) "info", path, NULL };
    if (!subprocess_run_low_priority(info_argv, info_out, sizeof(info_out))) return 0;

    char device_path[160] = {0};
    copy_info_field(device_path, sizeof(device_path), info_out, "Device:");
    char codec[32] = {0};
    copy_info_field(codec, sizeof(codec), info_out, "Selected codec:");
    char * codec_end = strchr(codec, ':');
    if (codec_end) *codec_end = '\0';
    if (!device_path[0] || !codec[0]) return 0;

    /* The accessory's endpoints hang off its BlueZ object as sepN children,
     * listed at the very END of the introspection XML after every interface
     * and method. That document runs past 5 KiB on a real headset, so this
     * buffer is generous and heap-allocated rather than sitting on the
     * worker's stack -- undersizing it silently drops the sep nodes and the
     * rate list comes back empty. */
    enum { BT_INTROSPECT_MAX = 16384 };
    char * introspect = malloc(BT_INTROSPECT_MAX);
    if (!introspect) return 0;
    char * intro_argv[] = { (char *) "dbus-send", (char *) "--system", (char *) "--print-reply",
                            (char *) "--dest=org.bluez", device_path,
                            (char *) "org.freedesktop.DBus.Introspectable.Introspect", NULL };
    if (!subprocess_run_low_priority(intro_argv, introspect, BT_INTROSPECT_MAX)) {
        free(introspect);
        return 0;
    }

    const char * scan = introspect;
    const char * node;
    while ((node = strstr(scan, "<node name=\"sep")) != NULL) {
        scan = node + 1;
        const char * name = node + strlen("<node name=\"");
        size_t name_len = strcspn(name, "\"");
        if (name_len == 0 || name_len >= 16) continue;

        char sep_path[192];
        snprintf(sep_path, sizeof(sep_path), "%s/%.*s", device_path, (int) name_len, name);

        char props[2048];
        char * prop_argv[] = { (char *) "dbus-send", (char *) "--system", (char *) "--print-reply",
                               (char *) "--dest=org.bluez", sep_path,
                               (char *) "org.freedesktop.DBus.Properties.GetAll",
                               (char *) "string:org.bluez.MediaEndpoint1", NULL };
        if (!subprocess_run_low_priority(prop_argv, props, sizeof(props))) continue;

        /* Only the accessory's SINK endpoint matters: that is what this
         * device streams into. */
        if (!dbus_prop_string_equals(props, "UUID",
                                     "0000110b-0000-1000-8000-00805f9b34fb")) continue;

        uint8_t codec_id;
        if (!dbus_prop_byte(props, "Codec", &codec_id)) continue;

        uint8_t caps[32];
        size_t caps_len = 0;
        if (!dbus_prop_byte_array(props, "Capabilities", caps, sizeof(caps), &caps_len)) continue;
        if (!a2dp_caps_match_codec(caps, caps_len, codec_id, codec)) continue;

        int n = a2dp_caps_rates(caps, caps_len, codec, out, max_count);
        if (n > 0) {
            free(introspect);
            return n;
        }
    }
    free(introspect);
    return 0;
}

bool bt_control_prepare_playback_pcm(char * out, size_t out_size) {
    if (!out || out_size == 0) return false;

    char path[256];
    if (!find_source_pcm_path(path, sizeof(path))) return false;

    /* Derived from the path already in hand: bt_control_get_connected_device_mac()
     * would repeat the list-pcms this just did. */
    const char * dev = strstr(path, "dev_");
    if (!dev || strlen(dev + 4) < 17) return false;
    dev += 4;
    char mac[18];
    for (int i = 0; i < 17; i++) mac[i] = (dev[i] == '_') ? ':' : dev[i];
    mac[17] = '\0';

    char info_out[2048];
    char * info_argv[] = { (char *) bluealsa_ctl_name(), (char *) "info", path, NULL };
    if (!subprocess_run_low_priority(info_argv, info_out, sizeof(info_out))) return false;

    unsigned int rate = 0;
    unsigned int channels = 0;
    const char * rate_field = strstr(info_out, "Rate:");
    if (rate_field) (void) sscanf(rate_field + strlen("Rate:"), "%u", &rate);
    const char * channels_field = strstr(info_out, "Channels:");
    if (channels_field) (void) sscanf(channels_field + strlen("Channels:"), "%u", &channels);
    if (rate == 0) return false;
    if (channels == 0) channels = 2;

    if (mkdir(BT_ALSA_CONFIG_DIR, 0755) != 0 && errno != EEXIST) return false;

    char tmp_path[128];
    char final_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.asoundrc.tmp", BT_ALSA_CONFIG_DIR);
    snprintf(final_path, sizeof(final_path), "%s/.asoundrc", BT_ALSA_CONFIG_DIR);

    FILE * f = fopen(tmp_path, "w");
    if (!f) return false;
    fprintf(f, "pcm.%s {\n", BT_ALSA_PCM_NAME);
    fprintf(f, "    type plug\n");
    fprintf(f, "    slave {\n");
    fprintf(f, "        pcm {\n");
    fprintf(f, "            type bluealsa\n");
    fprintf(f, "            device \"%s\"\n", mac);
    fprintf(f, "            profile \"a2dp\"\n");
    const char * codec = bt_playback_codec_override();
    if (codec) fprintf(f, "            codec \"%s\"\n", codec);
    fprintf(f, "        }\n");
    fprintf(f, "        rate %u\n", rate);
    fprintf(f, "        channels %u\n", channels);
    fprintf(f, "    }\n");
    if (atomic_load(&bt_speexrate_enabled) && access(BT_ALSA_RATE_CONVERTER_PLUGIN, F_OK) == 0)
        fprintf(f, "    rate_converter \"%s\"\n", BT_ALSA_RATE_CONVERTER);
    fprintf(f, "}\n");
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp_path); return false; }
    if (rename(tmp_path, final_path) != 0) { remove(tmp_path); return false; }

    snprintf(out, out_size, "%s", BT_ALSA_PCM_NAME);
    DBG_LOG("bt_control: BT output pinned to %u Hz %u ch for %s\n", rate, channels, mac);
    return true;
}

static pthread_t bt_source_vol_sync_thread;
static atomic_bool bt_source_vol_sync_active = false;
static bool bt_source_vol_sync_joinable;
/* Set by whichever direction writes/observes a value most recently, so the
 * other direction recognizes it as already in sync instead of re-writing
 * it right back, just shared between two directions here instead of one
 * direction echoing itself. */
static int bt_source_vol_last_synced_raw = -1;
/* Owned by the source monitor worker, like last_synced_raw. */
static char bt_source_vol_pcm_path[256];

static void bt_source_push_app_volume_if_changed(float * last_synced_app_percent) {
    char path[256];
    if (!find_source_pcm_path(path, sizeof(path))) {
        bt_source_vol_pcm_path[0] = '\0';
        return;
    }
    bool changed_path = strcmp(bt_source_vol_pcm_path, path) != 0;
    snprintf(bt_source_vol_pcm_path, sizeof(bt_source_vol_pcm_path), "%s", path);
    if (changed_path) {
        *last_synced_app_percent = -1.0f;
        bt_source_vol_last_synced_raw = -1;
    }

    /* Discovery reseeds a recreated PCM even at the same percentage. All
     * writes share the mode-change lock; read app volume after slow discovery. */
    pthread_mutex_lock(&bt_soft_volume_serial);
    pthread_mutex_lock(&bt_soft_volume_mutex);
    bool ready = strcmp(bt_soft_volume_applied_path, path) == 0;
    unsigned int generation = atomic_load_explicit(&bt_soft_volume_clears, memory_order_acquire);
    bool software_volume = atomic_load(&bt_source_soft_volume) ||
                           atomic_load(&modern_soft_volume_requested);
    pthread_mutex_unlock(&bt_soft_volume_mutex);
    if (!ready) goto done;
    float current_percent = audio_get_volume();
    if (current_percent == *last_synced_app_percent) goto done;
    int raw = (int) (current_percent * (float) BT_SOURCE_VOLUME_MAX + 0.5f);
    if (raw < 0) raw = 0;
    if (raw > BT_SOURCE_VOLUME_MAX) raw = BT_SOURCE_VOLUME_MAX;
    if (raw == bt_source_vol_last_synced_raw) {
        *last_synced_app_percent = current_percent;
        goto done;
    }

    if (!bluealsa_write_source_volume(path, raw, software_volume)) goto done;
    pthread_mutex_lock(&bt_soft_volume_mutex);
    bool current_generation = generation == atomic_load_explicit(&bt_soft_volume_clears, memory_order_acquire) &&
                              strcmp(bt_soft_volume_applied_path, path) == 0 &&
                              software_volume == (atomic_load(&bt_source_soft_volume) ||
                                                  atomic_load(&modern_soft_volume_requested));
    if (current_generation) {
        bt_source_vol_last_synced_raw = raw;
        *last_synced_app_percent = current_percent;
    }
    pthread_mutex_unlock(&bt_soft_volume_mutex);
    if (current_generation)
        hiby_sys_server_report_volume((int) (current_percent * 100.0f + 0.5f));
done:
    pthread_mutex_unlock(&bt_soft_volume_serial);
}

static void * bt_source_vol_sync_thread_func(void * arg) {
    (void) arg;

    /* Establish the already-visible player value before subscribing to
     * bluealsa property changes. Starting the monitor first can queue the
     * accessory's remembered pre-sync value, which would then overwrite
     * the app even if we push immediately afterward. */
    float last_synced_app_percent = -1.0f;
    bt_source_push_app_volume_if_changed(&last_synced_app_percent);
    if (!atomic_load(&bt_source_vol_sync_active)) return NULL;

    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "monitor", (char *) "-p", NULL };
    pid_t pid;
    int read_fd;
    if (!subprocess_popen(argv, &pid, &read_fd)) {
        atomic_store(&bt_source_vol_sync_active, false);
        return NULL;
    }

    char buf[512];
    size_t buf_len = 0;
    char reset_path[256] = "";

    while (bt_source_vol_sync_active) {
        /* Push app-driven state BEFORE reading monitor events. In
         * particular, the first iteration must make the already-visible
         * player percentage authoritative before bluealsa's monitor can
         * report the accessory's remembered value; doing this afterward
         * let that startup event silently change audio_get_volume() while
         * the slider/topbar still showed the old app value. */
        bt_source_push_app_volume_if_changed(&last_synced_app_percent);

        /* Bounded, not a blocking fgets() -- this loop also needs to notice
         * this app's OWN volume changing (UI slider, hardware buttons), which
         * has no fd to select() on, so it polls this monitor's pipe with a
         * short timeout instead of blocking on it indefinitely. 500ms is
         * imprecise for a "keep two volume controls in sync" feature (not
         * a latency-sensitive control path), matched on both sides of this
         * loop -- see the push check below. */
        struct pollfd pfd = { .fd = read_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 500);
        if (!atomic_load(&bt_source_vol_sync_active)) break;
        if (pr < 0 && errno != EINTR) break;
        if (pr > 0 && (pfd.revents & (POLLERR | POLLNVAL))) break;
        if (pr > 0 && (pfd.revents & POLLHUP) && !(pfd.revents & POLLIN)) break;

        if (pr > 0 && (pfd.revents & POLLIN)) {
            if (buf_len >= sizeof(buf) - 1) buf_len = 0; /* defensive -- a line this long can't be a real PropertyChanged message, drop and resync */
            ssize_t n = read(read_fd, buf + buf_len, sizeof(buf) - 1 - buf_len);
            if (n <= 0) break; /* monitor subprocess died or its pipe closed */
            buf_len += (size_t) n;
            buf[buf_len] = '\0';

            char * line_start = buf;
            char * newline;
            while ((newline = memchr(line_start, '\n', buf_len - (size_t) (line_start - buf))) != NULL) {
                *newline = '\0';
                char path[256];
                int raw;
                char volume_text[64];
                char soft_text[16];
                if (sscanf(line_start, "PropertyChanged %255s SoftVolume %15s", path, soft_text) == 2 &&
                    strcmp(path, bt_source_vol_pcm_path) == 0) {
                    /* BlueALSA resets the level on every SoftVolume write and
                     * reports it right after (max, or the transport's value,
                     * 127 on a fresh one). That is not a user change: skip it
                     * and push the player's own volume again instead. */
                    snprintf(reset_path, sizeof(reset_path), "%s", path);
                    last_synced_app_percent = -1.0f;
                    bt_source_vol_last_synced_raw = -1;
                } else if (sscanf(line_start, "PropertyChanged %255s Volume %63s", path, volume_text) == 2 &&
                    strcmp(path, bt_source_vol_pcm_path) == 0 &&
                    parse_monitor_volume(volume_text, &raw)) {
                    if (reset_path[0] && strcmp(reset_path, path) == 0) {
                        reset_path[0] = '\0';
                    } else {
                        /* Capture both cancellation domains before the slow
                         * live-property read. A local volume move while D-Bus
                         * is in flight must win over this queued remote event. */
                        unsigned int audio_generation = audio_get_applied_volume_generation();
                        pthread_mutex_lock(&bt_soft_volume_mutex);
                        unsigned int generation = atomic_load_explicit(&bt_soft_volume_clears,
                                                                       memory_order_acquire);
                        bool ready = strcmp(bt_soft_volume_applied_path, path) == 0;
                        bool hardware_volume = !atomic_load(&bt_source_soft_volume) &&
                                               !atomic_load(&modern_soft_volume_requested);
                        pthread_mutex_unlock(&bt_soft_volume_mutex);
                        int live_raw = -1;
                        bool live_match = ready && hardware_volume &&
                                          bluealsa_read_source_volume(path, &live_raw) &&
                                          live_raw == raw;
                        pthread_mutex_lock(&bt_soft_volume_mutex);
                        bool still_ready = generation == atomic_load_explicit(&bt_soft_volume_clears,
                                                                               memory_order_acquire) &&
                                           strcmp(bt_soft_volume_applied_path, path) == 0 &&
                                           !atomic_load(&bt_source_soft_volume) &&
                                           !atomic_load(&modern_soft_volume_requested);
                        bool adopted = live_match && still_ready && raw != bt_source_vol_last_synced_raw;
                        float accessory_percent = (float) raw / (float) BT_SOURCE_VOLUME_MAX;
                        pthread_mutex_unlock(&bt_soft_volume_mutex);
                        if (adopted) {
                            unsigned int committed_generation = 0;
                            bool accepted = audio_try_set_external_volume(
                                accessory_percent, audio_generation, &bt_soft_volume_clears,
                                generation, &committed_generation);
                            if (accepted) {
                                int percent = (int) (audio_get_volume() * 100.0f + 0.5f);
                                pthread_mutex_lock(&bt_soft_volume_mutex);
                                /* Publish the accepted audio generation even
                                 * if disconnect cleared the PCM during apply;
                                 * the UI drops it only if a newer local change
                                 * has already superseded it. */
                                bt_source_vol_pending_audio_generation = committed_generation;
                                atomic_store_explicit(&bt_source_vol_pending_percent, percent,
                                                      memory_order_release);
                                bool cache_current = generation == atomic_load_explicit(
                                                         &bt_soft_volume_clears, memory_order_acquire) &&
                                                     strcmp(bt_soft_volume_applied_path, path) == 0 &&
                                                     audio_get_volume_generation() == committed_generation;
                                if (cache_current) {
                                    bt_source_vol_last_synced_raw = raw;
                                    last_synced_app_percent = accessory_percent;
                                }
                                pthread_mutex_unlock(&bt_soft_volume_mutex);
                            }
                        }
                    }
                }
                line_start = newline + 1;
            }
            size_t remaining = buf_len - (size_t) (line_start - buf);
            memmove(buf, line_start, remaining);
            buf_len = remaining;
        }

    }

    close(read_fd);
    subprocess_terminate(pid); /* worker owns and reaps its own child */
    atomic_store(&bt_source_vol_sync_active, false);
    return NULL;
}

bool bt_control_source_volume_sync_is_running(void) {
    return atomic_load(&bt_source_vol_sync_active);
}

void bt_control_source_volume_sync_start(void) {
    if (bt_source_vol_sync_joinable) {
        if (atomic_load(&bt_source_vol_sync_active)) return;
        pthread_join(bt_source_vol_sync_thread, NULL);
        bt_source_vol_sync_joinable = false;
    }
    bt_source_vol_sync_active = true;
    bt_source_vol_last_synced_raw = -1;
    bt_source_vol_pcm_path[0] = '\0';
    if (pthread_create(&bt_source_vol_sync_thread, NULL, bt_source_vol_sync_thread_func, NULL) == 0)
        bt_source_vol_sync_joinable = true;
    else bt_source_vol_sync_active = false;
}

void bt_control_source_volume_sync_stop(void) {
    if (!bt_source_vol_sync_joinable) return;
    /* Cancellation is observed after any in-flight command and the bounded
     * pipe poll. This join belongs on the lifecycle worker, never LVGL. */
    bt_source_vol_sync_active = false;
    pthread_join(bt_source_vol_sync_thread, NULL);
    bt_source_vol_sync_joinable = false;
}

bool bt_control_source_volume_sync_consume_percent(int * out_percent) {
    pthread_mutex_lock(&bt_soft_volume_mutex);
    int percent = atomic_exchange_explicit(&bt_source_vol_pending_percent, -1,
                                           memory_order_acq_rel);
    unsigned int generation = bt_source_vol_pending_audio_generation;
    pthread_mutex_unlock(&bt_soft_volume_mutex);
    if (percent < 0 || audio_get_volume_generation() != generation) return false;
    if (out_percent) *out_percent = percent;
    return true;
}

/* See bt_control_output_disconnect_watch_start()'s own doc comment
 * (bluetooth_control.h) for what this is and why. Plain `monitor`, no `-p`
 * needed -- confirmed via `strings` on the real bluealsactl binary that
 * PCMAdded/PCMRemoved come from its base InterfacesAdded/InterfacesRemoved
 * subscription (path_namespace='/org/bluealsa'), which is always active;
 * `-p` only adds the separate PropertyChanged stream (Volume/Codec/Running/
 * SoftVolume) the two volume-sync threads above already use -- this doesn't
 * need any of that. "/a2dpsrc/sink" matches find_source_pcm_path()'s own
 * naming exactly: the PCM this app itself writes local playback into
 * (audio_output.c's `aplay -D bluealsa`), not any other PCM bluealsa might
 * have (e.g. one from DAC mode). */
static pthread_t bt_output_disconnect_thread;
static atomic_bool bt_output_disconnect_active = false;
static bool bt_output_disconnect_joinable;
static atomic_bool bt_output_disconnect_flag = false;
static pthread_mutex_t bt_output_disconnect_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static char bt_output_disconnect_confirmed_path[256];

#define BT_OUTPUT_DISCONNECT_MAX_ADDED_PATHS 16
/* Codec re-selection after a failed attempt: 2 s, 4 s, 6 s, 8 s apart. */
#define BT_CODEC_RETRY_BASE_MS 2000u
#define BT_CODEC_RETRY_MAX_ATTEMPTS 4

/* Preference worker, owned by the output watch thread. Codec, rate and
 * soft-volume selection run bluealsactl subprocesses bounded by their own
 * timeouts; doing that here keeps the watch thread reading PCMRemoved/
 * PCMAdded, so a disconnect is noticed on time. The watch thread and
 * reconciliation only post requests (short lock, no I/O).
 *
 * One PCM is tracked at a time. A failed codec selection is retried
 * BT_CODEC_RETRY_MAX_ATTEMPTS times, spaced out, and never while a
 * disconnect is pending. The budget belongs to the PCM path: a PCMAdded for
 * the same path (a replacement, which a codec selection itself can cause)
 * asks only for rate and soft volume to be re-applied, so a device that
 * keeps falling back is not switched back and forth without end; only a
 * different path or a confirmed disconnect starts over. */
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    bool running;        /* read/written under mutex */
    atomic_bool stop;
    bool paused;         /* a removal is pending: no new selection */
    bool in_flight;      /* a selection pass is running */
    bool scan_requested; /* look for a PCM that predates the subscription */
    bool codec_due;      /* select the codec for path now */
    bool refresh_due;    /* re-apply rate and soft volume only */
    char path[256];
    int attempts;        /* failed codec selections for path */
    uint32_t retry_at;
    bool discovery_pending; /* waiting for SEP/name discovery; no attempt charged */
} bt_pref = { .mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static void bt_pref_rate_changed(void) {
    pthread_mutex_lock(&bt_pref.mutex);
    if (bt_pref.running && !atomic_load(&bt_pref.stop)) {
        if (bt_pref.path[0]) bt_pref.refresh_due = true;
        else bt_pref.scan_requested = true;
        pthread_cond_signal(&bt_pref.cond);
    }
    pthread_mutex_unlock(&bt_pref.mutex);
}

static bool bt_pref_retry_scheduled_locked(void) {
    return bt_pref.path[0] && (bt_pref.discovery_pending ||
           (bt_pref.attempts > 0 && bt_pref.attempts < BT_CODEC_RETRY_MAX_ATTEMPTS));
}

/* Called with bt_pref.mutex held. Discovery deferrals do not consume the
 * finite selection-failure budget. */
static void bt_pref_record_codec_result_locked(const char *path, bool applied, uint32_t defer_until) {
    if (strcmp(bt_pref.path, path) != 0) return;
    if (defer_until) {
        bt_pref.discovery_pending = true;
        bt_pref.retry_at = defer_until;
    } else {
        bt_pref.discovery_pending = false;
        if (applied) {
            bt_pref.attempts = 0;
        } else if (++bt_pref.attempts < BT_CODEC_RETRY_MAX_ATTEMPTS) {
            bt_pref.retry_at = bt_control_monotonic_ms() + BT_CODEC_RETRY_BASE_MS * (uint32_t) bt_pref.attempts;
        } else {
            DBG_LOG("bt_control: codec selection for %s gave up after %d attempts\n",
                    path, bt_pref.attempts);
        }
    }
}

static void bt_pref_begin_work_locked(bool codec_work) {
    bt_pref.codec_due = false;
    if (codec_work) bt_pref.discovery_pending = false;
    bt_pref.refresh_due = false;
}

static void * bt_pref_worker_func(void * arg) {
    (void) arg;
    pthread_mutex_lock(&bt_pref.mutex);
    while (!atomic_load(&bt_pref.stop)) {
        uint32_t now = bt_control_monotonic_ms();
        bool retry_due = !bt_pref.paused && bt_pref_retry_scheduled_locked() &&
                         (int32_t) (now - bt_pref.retry_at) >= 0;
        bool codec_work = !bt_pref.paused && bt_pref.path[0] && (bt_pref.codec_due || retry_due);
        bool refresh_work = !bt_pref.paused && bt_pref.path[0] && bt_pref.refresh_due;
        if (bt_pref.scan_requested) {
            bt_pref.scan_requested = false;
            pthread_mutex_unlock(&bt_pref.mutex);
            char found[256];
            bool present = !atomic_load(&bt_pref.stop) &&
                           find_source_pcm_path_ex(found, sizeof(found), false);
            pthread_mutex_lock(&bt_pref.mutex);
            if (present && !bt_pref.path[0]) {
                snprintf(bt_pref.path, sizeof(bt_pref.path), "%s", found);
                bt_pref.attempts = 0;
                bt_pref.discovery_pending = false;
                bt_pref.codec_due = true;
            }
            continue;
        }
        if (!codec_work && !refresh_work) {
            uint32_t wait_ms = 1000;
            if (!bt_pref.paused && bt_pref_retry_scheduled_locked()) {
                int32_t left = (int32_t) (bt_pref.retry_at - now);
                wait_ms = left <= 0 ? 0 : (uint32_t) left < wait_ms ? (uint32_t) left : wait_ms;
            }
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += wait_ms / 1000;
            ts.tv_nsec += (long) (wait_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&bt_pref.cond, &bt_pref.mutex, &ts);
            continue;
        }
        char path[256];
        snprintf(path, sizeof(path), "%s", bt_pref.path);
        bt_pref_begin_work_locked(codec_work);
        bt_pref.in_flight = true;
        pthread_mutex_unlock(&bt_pref.mutex);

        /* Codec first: selecting one can cycle the PCM, which would
         * otherwise drop a soft-volume setting applied just before. Stop is
         * checked between steps, so it waits for at most one of them. */
        uint32_t defer_until = 0;
        bool applied = codec_work ? bluealsa_apply_codec_preference_deferred(path, &bt_pref.stop,
                                                                               &defer_until) : true;
        /* Refresh-only passes too: the rate step waits for a verified codec,
         * so it never reconfigures a fallback codec or pre-empts a retry. */
        bool codec_verified = bluealsa_codec_verified(path);
        /* The rate belongs to the codec just selected: skipped while that
         * selection has not succeeded, so a fallback codec's refusal is not
         * recorded (and later applied) in its place. */
        bool rate_reconfigured = applied && codec_verified && !atomic_load(&bt_pref.stop) &&
                                 bluealsa_apply_rate_preference_cancellable(path, &bt_pref.stop);
        if (rate_reconfigured) {
            /* The link was re-established for the rate: the codec it ended
             * up on must be checked again (cheap when unchanged). */
            bluealsa_clear_codec_path(path);
        }
        bluealsa_apply_soft_volume_cancellable(path, &bt_pref.stop);

        pthread_mutex_lock(&bt_pref.mutex);
        bt_pref.in_flight = false;
        if (rate_reconfigured && applied && strcmp(bt_pref.path, path) == 0 &&
            bt_pref.attempts < BT_CODEC_RETRY_MAX_ATTEMPTS)
            bt_pref.codec_due = true;
        if (codec_work && strcmp(bt_pref.path, path) == 0) {
            bt_pref_record_codec_result_locked(path, applied, defer_until);
        }
    }
    pthread_mutex_unlock(&bt_pref.mutex);
    return NULL;
}

static void bt_pref_start(void) {
    pthread_mutex_lock(&bt_pref.mutex);
    atomic_store(&bt_pref.stop, false);
    bt_pref.paused = false;
    bt_pref.in_flight = false;
    bt_pref.scan_requested = false;
    bt_pref.codec_due = false;
    bt_pref.refresh_due = false;
    bt_pref.path[0] = '\0';
    bt_pref.attempts = 0;
    bt_pref.discovery_pending = false;
    bt_pref.running = pthread_create(&bt_pref.thread, NULL, bt_pref_worker_func, NULL) == 0;
    pthread_mutex_unlock(&bt_pref.mutex);
}

/* Joins the worker; it finishes at most the step it is running. */
static void bt_pref_stop(void) {
    pthread_mutex_lock(&bt_pref.mutex);
    bool running = bt_pref.running;
    atomic_store(&bt_pref.stop, true);
    pthread_cond_signal(&bt_pref.cond);
    pthread_mutex_unlock(&bt_pref.mutex);
    if (running) pthread_join(bt_pref.thread, NULL);
    pthread_mutex_lock(&bt_pref.mutex);
    bt_pref.running = false;
    bt_pref.discovery_pending = false;
    bt_pref.attempts = 0;
    bt_pref.path[0] = '\0';
    pthread_mutex_unlock(&bt_pref.mutex);
}

static void bt_pref_request_scan(void) {
    pthread_mutex_lock(&bt_pref.mutex);
    bt_pref.scan_requested = true;
    pthread_cond_signal(&bt_pref.cond);
    pthread_mutex_unlock(&bt_pref.mutex);
}

/* A PCMAdded (or reconciliation). Returns false when no worker is running,
 * leaving the caller to apply inline. */
static bool bt_pref_request(const char * path) {
    pthread_mutex_lock(&bt_pref.mutex);
    bool running = bt_pref.running && !atomic_load(&bt_pref.stop);
    if (running) {
        if (strcmp(bt_pref.path, path) != 0) {
            snprintf(bt_pref.path, sizeof(bt_pref.path), "%.*s", (int) sizeof(bt_pref.path) - 1, path);
            bt_pref.attempts = 0;
            bt_pref.discovery_pending = false;
            bt_pref.codec_due = true;
            bt_pref.refresh_due = false;
        } else {
            /* Same PCM again. Its soft volume must be re-applied; the codec
             * only when no selection is in flight or scheduled (a settled
             * success is then a cheap latch check). */
            bt_pref.refresh_due = true;
            if (!bt_pref.in_flight && bt_pref.attempts == 0 && !bt_pref.discovery_pending)
                bt_pref.codec_due = true;
        }
        pthread_cond_signal(&bt_pref.cond);
    }
    pthread_mutex_unlock(&bt_pref.mutex);
    return running;
}

/* Mirrors the watch's pending-removal state: no new selection meanwhile. */
static void bt_pref_set_paused(bool paused) {
    pthread_mutex_lock(&bt_pref.mutex);
    if (bt_pref.paused != paused) {
        bt_pref.paused = paused;
        pthread_cond_signal(&bt_pref.cond);
    }
    pthread_mutex_unlock(&bt_pref.mutex);
}

/* A confirmed disconnect: the next connection starts with a fresh budget. */
static void bt_pref_forget(const char * path) {
    pthread_mutex_lock(&bt_pref.mutex);
    if (!path || strcmp(bt_pref.path, path) == 0) {
        bt_pref.path[0] = '\0';
        bt_pref.attempts = 0;
        bt_pref.discovery_pending = false;
        bt_pref.codec_due = false;
        bt_pref.refresh_due = false;
    }
    pthread_mutex_unlock(&bt_pref.mutex);
}

static uint32_t bt_output_removed_at; /* when the pending removal was seen; watch thread only */

static void bt_output_disconnect_process_line(const char * line, char * pending_path,
                                              uint32_t * pending_deadline,
                                              char added_paths[][256], int * added_count) {
    char path[256];
    if (sscanf(line, "PCMRemoved %255s", path) == 1) {
        if (strstr(path, "/a2dpsrc/sink") != NULL) {
            snprintf(pending_path, 256, "%s", path);
            bt_output_removed_at = bt_control_monotonic_ms();
            *pending_deadline = bt_output_removed_at + BT_OUTPUT_RECONFIGURE_GRACE_MS;
            /* Soft volume only: the PCM is recreated by a reconfigure, so
             * that setting must be re-applied. The codec latch deliberately
             * survives here and is cleared only once a disconnect is
             * actually confirmed -- selecting a codec can itself cycle the
             * PCM, and clearing on every PCMRemoved would let the resulting
             * PCMAdded re-select, cycling it again without end. */
            bluealsa_clear_soft_volume_path(path);
            DBG_LOG("bt_control: output_disconnect_watch: PCMRemoved %s -- pending disconnect\n", path);
        }
        return;
    }
    if (strncmp(line, "PCMAdded ", 9) != 0 || sscanf(line, "PCMAdded %255s", path) != 1) return;
    bt_auto_pcm_added(path);
    if (strcmp(pending_path, path) == 0) {
        pending_path[0] = '\0';
        *pending_deadline = 0;
        DBG_LOG("bt_control: output_disconnect_watch: PCMAdded %s -- cancelled pending disconnect\n", path);
    }
    /* A replacement can arrive after the deadline but before the UI consumes
     * the event. Cancel only the event for this exact PCM, under the same
     * short lock used by the UI consumer. Never hold it during control I/O. */
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    if (strcmp(bt_output_disconnect_confirmed_path, path) == 0) {
        atomic_store(&bt_output_disconnect_flag, false);
        bt_output_disconnect_confirmed_path[0] = '\0';
    }
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
    if (*added_count < BT_OUTPUT_DISCONNECT_MAX_ADDED_PATHS)
        snprintf(added_paths[(*added_count)++], 256, "%s", path);
    DBG_LOG("bt_control: output_disconnect_watch: %s\n", line);
}

static bool bt_output_disconnect_read_batch(int fd, char * buf, size_t * used,
                                            char * pending_path, uint32_t * pending_deadline,
                                            char added_paths[][256], int * added_count) {
    if (*used == 511) *used = 0;
    ssize_t n = read(fd, buf + *used, 511 - *used);
    if (n <= 0) return false;
    *used += (size_t)n;
    buf[*used] = '\0';
    char * line = buf;
    char * end;
    while ((end = strchr(line, '\n')) != NULL) {
        *end = '\0';
        bt_output_disconnect_process_line(line, pending_path, pending_deadline,
                                          added_paths, added_count);
        line = end + 1;
    }
    size_t remaining = *used - (size_t)(line - buf);
    memmove(buf, line, remaining);
    *used = remaining;
    return true;
}

static void bt_output_disconnect_publish_if_due(char * pending_path, uint32_t * pending_deadline) {
    if (!pending_path[0] || (int32_t)(bt_control_monotonic_ms() - *pending_deadline) < 0) return;
    /* A codec switch of ours is recreating the transport: keep waiting for
     * the PCM (up to BT_SSC_SWITCH_CAP_MS) instead of calling it a drop. */
    uint32_t hold = bt_auto_reconfigure_deadline(pending_path, bt_output_removed_at);
    if (hold && (int32_t)(hold - bt_control_monotonic_ms()) > 0) {
        *pending_deadline = hold;
        DBG_LOG("bt_control: output_disconnect_watch: %s -- codec switch in flight, waiting\n", pending_path);
        return;
    }
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    if (atomic_load(&bt_output_disconnect_active) && !atomic_load(&bt_output_disconnect_flag)) {
        snprintf(bt_output_disconnect_confirmed_path, sizeof(bt_output_disconnect_confirmed_path), "%s", pending_path);
        atomic_store(&bt_output_disconnect_flag, true);
    }
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
    DBG_LOG("bt_control: output_disconnect_watch: PCMRemoved %s -- flagging disconnect\n", pending_path);
    bt_auto_drop_confirmed(pending_path, bt_output_removed_at);
    bluealsa_clear_codec_path(pending_path); /* real disconnect: next connect must re-select */
    bt_buds_reset(pending_path);
    bt_pref_forget(pending_path);
    pending_path[0] = '\0';
    *pending_deadline = 0;
}

static void * bt_output_disconnect_thread_func(void * arg) {
    (void) arg;

    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "monitor", NULL };
    pid_t pid;
    int read_fd;
    if (!subprocess_popen(argv, &pid, &read_fd)) {
        DBG_LOG("bt_control: output_disconnect_watch: failed to spawn bluealsactl monitor\n");
        atomic_store(&bt_output_disconnect_active, false);
        return NULL;
    }
    DBG_LOG("bt_control: output_disconnect_watch: monitor started (pid %d)\n", (int) pid);
    bt_output_watch_started_at = bt_control_monotonic_ms() ? bt_control_monotonic_ms() : 1;

    /* The GUI starts this watch only after it has seen the A2DP link, so the
     * PCM of that connection usually predates the subscription and never
     * produces PCMAdded here. That left a first connection (no BlueZ
     * LastUsed endpoint yet, e.g. right after pairing) on whatever codec
     * BlueZ happened to pick, such as AAC instead of a chosen LDAC, until a
     * reconnect. The worker scans for such a PCM once the monitor has
     * printed its first line: bluealsactl installs its D-Bus matches before
     * printing ServiceRunning/ServiceStopped, so from then on any new PCM is
     * reported below; if nothing arrives the scan still runs after 2 s. */
    bt_pref_start();
    bool scanned_on_ready = false, scanned_on_fallback = false;
    uint32_t scan_fallback_at = bt_control_monotonic_ms() + 2000;

    char buf[512];
    size_t used = 0;
    char pending_path[256] = "";
    uint32_t pending_deadline = 0;
    char added_paths[BT_OUTPUT_DISCONNECT_MAX_ADDED_PATHS][256];
    int added_count = 0;
    while (atomic_load(&bt_output_disconnect_active)) {
        uint32_t now = bt_control_monotonic_ms();
        int timeout_ms = 100;
        if (pending_path[0]) {
            int32_t remaining = (int32_t)(pending_deadline - now);
            timeout_ms = remaining <= 0 ? 0 : (remaining < timeout_ms ? remaining : timeout_ms);
        }
        struct pollfd pfd = { .fd = read_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (!atomic_load(&bt_output_disconnect_active)) break;
        if (pr < 0) { if (errno == EINTR) continue; break; }
        bool terminal = false;
        if (pr > 0 && (pfd.revents & POLLIN))
            terminal = !bt_output_disconnect_read_batch(read_fd, buf, &used, pending_path,
                                                         &pending_deadline, added_paths, &added_count);
        /* Scan on readiness; a fallback scan (monitor slow to print) does
         * not replace it, since a PCM could appear between the two. */
        if (!scanned_on_ready && pr > 0 && (pfd.revents & POLLIN) && !terminal) {
            scanned_on_ready = true;
            bt_pref_request_scan();
        } else if (!scanned_on_ready && !scanned_on_fallback &&
                   (int32_t) (bt_control_monotonic_ms() - scan_fallback_at) >= 0) {
            scanned_on_fallback = true;
            bt_pref_request_scan();
        }
        if (!terminal) {
            struct pollfd drain = { .fd = read_fd, .events = POLLIN };
            while (atomic_load(&bt_output_disconnect_active) && poll(&drain, 1, 0) > 0) {
                if (!(drain.revents & POLLIN)) {
                    terminal = (drain.revents & (POLLERR | POLLNVAL | POLLHUP)) != 0;
                    break;
                }
                if (!bt_output_disconnect_read_batch(read_fd, buf, &used, pending_path,
                                                     &pending_deadline, added_paths, &added_count)) {
                    terminal = true;
                    break;
                }
                drain.revents = 0;
            }
        }
        if (!atomic_load(&bt_output_disconnect_active)) break;
        if (terminal || (pr > 0 && (pfd.revents & (POLLERR | POLLNVAL | POLLHUP)))) {
            /* A failed monitor may have missed the replacement. Leave an
             * unconfirmed removal to periodic polling rather than inventing
             * a disconnect after its event stream has ended. */
            break;
        }
        bt_output_disconnect_publish_if_due(pending_path, &pending_deadline);
        if (!pending_path[0]) bt_auto_flowing_tick();
        bt_pref_set_paused(pending_path[0] != '\0');
        /* Do not let volume subprocesses delay reading a replacement while
         * a removal is pending. Keep the bounded queue until it settles. */
        if (!pending_path[0]) {
            for (int i = 0; i < added_count; i++) {
                if (!bt_pref_request(added_paths[i])) {
                    /* No worker (thread creation failed): apply inline. */
                    bluealsa_apply_codec_preference(added_paths[i]);
                    bluealsa_apply_rate_preference(added_paths[i]);
                    bluealsa_apply_soft_volume(added_paths[i]);
                }
            }
            added_count = 0;
        }
    }

    DBG_LOG("bt_control: output_disconnect_watch: monitor exited (pid %d)\n", (int) pid);
    bt_pref_stop();
    bt_output_disconnect_finish_pending(pending_path, bt_output_removed_at);
    /* The monitor can exit with a removal still unconfirmed, which would
     * leave the latch set and suppress the re-select on the next connect. */
    bluealsa_clear_codec_path(NULL);
    bt_buds_reset(NULL);
    close(read_fd);
    subprocess_terminate(pid);
    atomic_store(&bt_output_disconnect_active, false);
    return NULL;
}

bool bt_control_output_disconnect_watch_is_running(void) {
    return atomic_load(&bt_output_disconnect_active);
}

void bt_control_output_disconnect_watch_start(void) {
    if (bt_output_disconnect_joinable) {
        if (atomic_load(&bt_output_disconnect_active)) return;
        pthread_join(bt_output_disconnect_thread, NULL);
        bt_output_disconnect_joinable = false;
    }
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    bt_output_disconnect_active = true;
    bt_output_disconnect_flag = false;
    bt_output_disconnect_confirmed_path[0] = '\0';
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
    if (pthread_create(&bt_output_disconnect_thread, NULL, bt_output_disconnect_thread_func, NULL) == 0)
        bt_output_disconnect_joinable = true;
    else bt_output_disconnect_active = false;
}

void bt_control_output_disconnect_watch_stop(void) {
    bt_output_disconnect_active = false;
    if (bt_output_disconnect_joinable) {
        pthread_join(bt_output_disconnect_thread, NULL);
        bt_output_disconnect_joinable = false;
    }
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    atomic_store(&bt_output_disconnect_flag, false);
    bt_output_disconnect_confirmed_path[0] = '\0';
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
}

bool bt_control_output_disconnect_consume(void) {
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    bool disconnected = atomic_exchange(&bt_output_disconnect_flag, false);
    bt_output_disconnect_confirmed_path[0] = '\0';
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
    return disconnected;
}

/* Recorded so bt_control_reapply_last_output_settings() (the wedge
 * recovery in bt_control_is_powered(), above) can restore this after a
 * bluetoothd restart -- a fresh bluetoothd/bt_resume-equivalent bring-up
 * always comes back up in the plain a2dp-source default, not whatever
 * profile was actually requested last. */
static bool last_applied_dac_mode_enabled = false;
static bool last_applied_volume_sync_enabled = false;
static bool output_settings_ever_applied = false;

#define BLUEALSA_STARTUP_LOG_PATH "/usr/data/bluealsa_startup.log"

/* Spawns bluealsa daemon, logging startup output if TEST_BUILD_TAG is defined. */
static bool spawn_bluealsa_and_verify(char * const argv[]) {
#if defined(TEST_BUILD_TAG)
    return subprocess_spawn_daemon_logged(argv, BLUEALSA_STARTUP_LOG_PATH);
#else
    return subprocess_spawn_daemon_logged(argv, NULL);
#endif
}

bool bt_control_apply_output_settings(bool dac_mode_enabled, bool volume_sync_enabled) {
    /* Serializes bluealsa/bt-agent respawns and updates last-applied settings
     * to protect against concurrent caller races. */
    pthread_mutex_lock(&bt_chip_mutex);
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    last_applied_dac_mode_enabled = dac_mode_enabled;
    last_applied_volume_sync_enabled = volume_sync_enabled;
    output_settings_ever_applied = true;
    /* DAC mode always uses BlueALSA's software volume: bluealsa-aplay would
     * otherwise apply the phone's AVRCP volume to an ALSA "Master" control,
     * which this hardware does not have, so phone volume changes were lost. */
    pthread_mutex_lock(&bt_soft_volume_serial);
    atomic_store(&modern_soft_volume_requested, dac_mode_enabled || !volume_sync_enabled);
    atomic_store(&bt_source_soft_volume, true);
    bluealsa_clear_soft_volume_path(NULL);
    pthread_mutex_unlock(&bt_soft_volume_serial);

    /* Single profile mode: runs a2dp-sink or a2dp-source. */
    /* Clean up existing instances before respawning daemons. The cache is
     * cleared only after the old monitor thread is joined, so it cannot
     * record a path again in between. */
    bt_dac_info_monitor_stop();
    bluealsa_clear_soft_volume_path(NULL);

    bluealsa_backend_t backend = bluealsa_backend();
    subprocess_kill_all_matching(backend.daemon_name);
    subprocess_kill_all_matching("aplay");
    subprocess_kill_all_matching("bt-agent");
    /* usleep(500000) delay TEMPORARILY REMOVED for the same live A/B test
     * as the codec restriction and bluealsa-aplay below -- the stock
     * player's own bluealsa_profile script has zero delay between the
     * kills and respawning bluealsa/bt-agent (both backgrounded with a
     * bare `&`, script just ends), and was just confirmed working
     * flawlessly. */

    char * argv[14];
    int i = 0;
    argv[i++] = (char *) backend.daemon;
    argv[i++] = (char *) "-p";
    argv[i++] = dac_mode_enabled ? (char *) "a2dp-sink" : (char *) "a2dp-source";
    /* Source only. In DAC mode the rate is the phone's choice and the local
     * DAC takes it as-is, so forcing 44.1 there would resample for nothing. */
    if (!dac_mode_enabled && bt_rate_wants_audio_cd()) argv[i++] = (char *) BT_A2DP_FORCE_44K1_ARG;
    if (!dac_mode_enabled && bt_ssc_auto_profile_enabled())
        argv[i++] = (char *) BT_SSC_AUTO_PROFILE_ARG;
    if (!dac_mode_enabled && bt_codec_is_sbc_xq())
        argv[i++] = (char *) "--sbc-quality=xq";
    if (dac_mode_enabled) {
        /* aptX and aptX-HD decode reliably and are always offered, on top of
         * BlueALSA's defaults (SBC, AAC). The stock firmware's LDAC decoder
         * crashes on phone streams, so offer LDAC only when the rebuilt
         * decoder is installed. */
        argv[i++] = (char *) "-c";
        argv[i++] = (char *) "aptX";
        argv[i++] = (char *) "-c";
        argv[i++] = (char *) "aptX-HD";
        if (bt_ldac_decoder_is_rebuilt()) {
            argv[i++] = (char *) "-c";
            argv[i++] = (char *) "LDAC";
        }
    } else {
        i = bt_codec_append_daemon_args(argv, i);
    }
    argv[i] = NULL;
    if (!spawn_bluealsa_and_verify(argv)) {
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
        pthread_mutex_unlock(&bt_chip_mutex);
        return false;
    }

    if (dac_mode_enabled) {
        char * agent_argv[] = { (char *) "bt-agent", (char *) "-c", (char *) "NoInputNoOutput", NULL };
        subprocess_spawn_daemon(agent_argv);
    } else {
        char * agent_argv[] = { (char *) "bt-agent", (char *) "-c", (char *) "NoInputNoOutput",
                                 (char *) "-p", (char *) "/usr/data/pin.conf", NULL };
        subprocess_spawn_daemon(agent_argv);
    }

    if (dac_mode_enabled) {
        /* Runs bluealsa-aplay with 200ms buffer (4 periods of 50ms) to ensure
         * exact frame boundaries at both 44.1kHz and 48kHz and reduce latency
         * while buffering against clock drift. */
        char * bluealsa_aplay_argv[] = { (char *) "bluealsa-aplay",
                                          (char *) "--pcm-buffer-time=200000",
                                          (char *) "--pcm-period-time=50000",
                                          NULL };
        subprocess_spawn_daemon(bluealsa_aplay_argv);
    } else {
        char * disc_argv[] = { (char *) "bluetoothctl", (char *) "discoverable", (char *) "off", NULL };
        subprocess_run(disc_argv, NULL, 0);
    }
    if (dac_mode_enabled) bt_dac_info_monitor_start();
    pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    pthread_mutex_unlock(&bt_chip_mutex);
    return true;
}

static void bt_control_reapply_last_output_settings(void) {
    /* Read settings under lock to avoid racing concurrent updates,
     * then call bt_control_apply_output_settings outside the lock. */
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    bool ever_applied = output_settings_ever_applied;
    bool dac_mode = last_applied_dac_mode_enabled;
    bool volume_sync = last_applied_volume_sync_enabled;
    pthread_mutex_unlock(&bt_daemon_respawn_mutex);

    if (!ever_applied) {
        ensure_bluealsa_running();
        return;
    }
    bt_control_apply_output_settings(dac_mode, volume_sync);
}

bool bt_control_set_codec(const char * codec) {
    if (!codec || (strcmp(codec, "auto") != 0 && strcmp(codec, "ldac") != 0 && strcmp(codec, "ldac_hq") != 0 &&
                   strcmp(codec, "ldac_sq") != 0 && strcmp(codec, "ssc") != 0 &&
                   strcmp(codec, "aptx") != 0 &&
                   strcmp(codec, "aac") != 0 && strcmp(codec, "sbc") != 0 &&
                   strcmp(codec, "sbc_xq") != 0)) return false;
    /* BlueALSA 5 receives the preference through its daemon arguments and
     * the bluealsa:CODEC=... PCM name. There is no per-user ALSA config file
     * to generate, and the old ldac_eqmid stanza is not a BlueALSA 5 API. */
    bt_control_restore_codec_preference(codec);
    return true;
}
