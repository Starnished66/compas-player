#include "bluetooth_control_internal.h"

/* Guards bt_control_init_chip(), bt_control_enable(), and bt_control_disable()
 * so chip firmware operations and power toggles are strictly serialized across threads. */
pthread_mutex_t bt_chip_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Shared by fallback bring-up and explicit profile changes. */
pthread_mutex_t bt_daemon_respawn_mutex = PTHREAD_MUTEX_INITIALIZER;

atomic_int bt_codec_preference = BT_CODEC_AUTO;
atomic_bool modern_soft_volume_requested = false;
/* Effective outgoing gain owner: unknown AVRCP capability uses software. */
atomic_bool bt_source_soft_volume = true;
atomic_int bt_source_vol_pending_percent = -1;
pthread_mutex_t bt_soft_volume_mutex = PTHREAD_MUTEX_INITIALIZER;
char bt_soft_volume_applied_path[256];
/* Source PCM whose hardware (AVRCP) volume write failed; it keeps software
 * gain until the PCM goes away. Guarded by bt_soft_volume_mutex. */
char bt_source_hw_failed_path[256];
/* Source PCM whose seed nudge went unanswered; the volume monitor forces one
 * more write once it is ready. Guarded by bt_soft_volume_mutex. */
char bt_source_reconcile_path[256];
static pthread_mutex_t bt_daemon_ssc_feature_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_int bt_daemon_ssc_auto_profile_feature = -1;

static bool bt_daemon_supports_ssc_auto_profile(void) {
    int state = atomic_load(&bt_daemon_ssc_auto_profile_feature);
    if (state >= 0) return state != 0;
    pthread_mutex_lock(&bt_daemon_ssc_feature_mutex);
    state = atomic_load(&bt_daemon_ssc_auto_profile_feature);
    if (state < 0) {
        char help[16384] = {0};
        char *argv[] = { (char *)BLUEALSA_DAEMON_PATH, (char *)"--help", NULL };
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

/* Pushed in from the UI; see the setter below. */
atomic_bool bt_speexrate_enabled = false;
/* Requested A2DP transport rate in Hz, 0 for BlueALSA's own choice. */
atomic_uint bt_sample_rate = 44100;

bool bt_ssc_auto_profile_enabled(void) {
    return atomic_load(&bt_sample_rate) == 0 && bt_daemon_supports_ssc_auto_profile();
}

bool bt_ssc_auto_profile_for_rate(unsigned int rate) {
    return rate == 0 && bt_daemon_supports_ssc_auto_profile();
}

/* Remembers a rate the accessory REFUSED, not one that was applied: a refusal
 * can tear the transport down, so retrying it every reconnect would become a
 * connect/refuse/drop cycle. A successful apply needs no memory, because the
 * transport then reports the requested rate and the next attempt is a no-op. */
pthread_mutex_t bt_rate_applied_mutex = PTHREAD_MUTEX_INITIALIZER;
char bt_rate_refused_path[256];
char bt_rate_refused_codec[32]; /* a refusal holds for this codec only */
unsigned int bt_rate_refused_rate;
pthread_mutex_t bt_ssc_correction_mutex = PTHREAD_MUTEX_INITIALIZER;
char bt_ssc_correction_mac[18];
unsigned int bt_ssc_correction_rate;
char bt_ssc_correction_path[256];
char bt_ssc_correction_codec[32];
atomic_uint bt_ssc_correction_generation = 0;
/* Counted while a rate correction owns the link (codec commands wait). */
atomic_int bt_ssc_correction_running;

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

bool bt_rate_wants_audio_cd(void) {
    return bt_effective_sample_rate() == 44100;
}

bool bt_rate_wants_audio_cd_for_rate(unsigned int rate) {
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
bool bt_ldac_decoder_is_rebuilt(void) {
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

/* Persisted codec setting names; "ldac" is the older alias of ldac_sq. */
static bool bt_codec_preference_from_name(const char * codec, bt_codec_preference_t * out) {
    static const struct { const char * name; bt_codec_preference_t pref; } names[] = {
        { "auto", BT_CODEC_AUTO },
        { "ldac", BT_CODEC_LDAC_SQ },
        { "ldac_hq", BT_CODEC_LDAC_HQ },
        { "ldac_sq", BT_CODEC_LDAC_SQ },
        { "ssc", BT_CODEC_SSC },
        { "aptx", BT_CODEC_APTX },
        { "aac", BT_CODEC_AAC },
        { "sbc", BT_CODEC_SBC },
        { "sbc_xq", BT_CODEC_SBC_XQ },
    };
    if (!codec) return false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(codec, names[i].name) == 0) {
            *out = names[i].pref;
            return true;
        }
    }
    return false;
}

void bt_control_restore_codec_preference(const char * codec) {
    bt_codec_preference_t preference = BT_CODEC_AUTO; /* NULL or unknown stays Automatic */
    (void) bt_codec_preference_from_name(codec, &preference);
    atomic_store(&bt_codec_preference, preference);
    bt_control_ssc_discovery_recovery_cancel();
    bt_buds_reset(NULL);
}

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

bool bt_codec_is_sbc_xq(void) {
    return (bt_codec_preference_t) atomic_load(&bt_codec_preference) == BT_CODEC_SBC_XQ;
}

/* Optional codecs are not all enabled by BlueALSA's default configuration.
 * Enabling the selected codec here keeps the setting effective. SBC is
 * mandatory and needs no -c option. */
static const char * bt_codec_daemon_name(bt_codec_preference_t preference) {
    switch (preference) {
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
const char * bt_codec_select_name(void) {
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
static const char * bt_codec_daemon_quality(bt_codec_preference_t preference) {
    switch (preference) {
    case BT_CODEC_LDAC_HQ: return "high";
    case BT_CODEC_LDAC_SQ: return "standard";
    case BT_CODEC_AUTO: return "high";
    default: return NULL;
    }
}

/* Build source-daemon codec arguments for BlueALSA 5 from one read of the
 * preference, so a concurrent change cannot mix two preferences. */
int bt_codec_append_daemon_args(char ** argv, int argc) {
    const bt_codec_preference_t preference = (bt_codec_preference_t) atomic_load(&bt_codec_preference);
    if (preference == BT_CODEC_AUTO) {
        argv[argc++] = (char *) "--all-codecs";
        argv[argc++] = (char *) "--ldac-abr";
    } else if (preference == BT_CODEC_SSC) {
        /* Keep every usable advertised fallback enabled. The per-PCM
         * preference worker explicitly selects SSC when the peer supports it. */
        argv[argc++] = (char *) "--all-codecs";
    } else {
        const char * daemon_codec = bt_codec_daemon_name(preference);
        if (daemon_codec) {
            argv[argc++] = (char *) "-c";
            argv[argc++] = (char *) daemon_codec;
        }
    }
    const char * daemon_quality = bt_codec_daemon_quality(preference);
    if (daemon_quality) {
        argv[argc++] = (char *) "--ldac-quality";
        argv[argc++] = (char *) daemon_quality;
    }
    return argc;
}

bool bluealsa_is_audio_pcm_path(const char * path) {
    return path && (strstr(path, "/a2dpsnk/source") != NULL ||
                    strstr(path, "/a2dpsrc/sink") != NULL);
}

uint32_t bt_control_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

bool bt_control_set_codec(const char * codec) {
    bt_codec_preference_t preference;
    if (!bt_codec_preference_from_name(codec, &preference)) return false;
    /* BlueALSA 5 receives the preference through its daemon arguments and
     * the bluealsa:CODEC=... PCM name. There is no per-user ALSA config file
     * to generate, and the old ldac_eqmid stanza is not a BlueALSA 5 API. */
    bt_control_restore_codec_preference(codec);
    return true;
}