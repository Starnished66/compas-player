#include "bluetooth_control_internal.h"

/* Display name for a "NAME[:hexblob]" codec field. BlueALSA reports SBC-XQ
 * as plain SBC, so it is labelled from the configuration itself: 44.1 kHz,
 * dual channel, 16 blocks, 8 subbands, loudness allocation -- exactly the
 * configuration sbc_a2dp_get_bitpool() gives the XQ bit-pool (which the
 * accessory's maximum may still cap). */
void bluealsa_codec_display_name(const char * selected, char * out, size_t out_size) {
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

/* Extracts the "Selected codec: X" line from `bluealsactl info` output.
 * Shared by the codec readback below and bt_control_get_connected_device_
 * stream() so the two cannot drift apart on the format. */
bool bluealsa_parse_selected_codec(const char * info_out, char * out, size_t out_size) {
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
void bluealsa_clear_codec_path(const char * path) {
    pthread_mutex_lock(&bt_codec_applied_mutex);
    if (!path || strcmp(bt_codec_applied_path, path) == 0)
        bt_codec_applied_path[0] = '\0';
    bt_codec_clears++;
    pthread_mutex_unlock(&bt_codec_applied_mutex);
}
static bool bt_cancelled(const atomic_bool * cancel);

/* The "Rate:" value of `bluealsactl info` output; 0 when missing or invalid. */
unsigned int bluealsa_info_rate(const char * info) {
    unsigned int rate = 0;
    const char * field = strstr(info, "Rate:");
    if (field) (void) sscanf(field + strlen("Rate:"), "%u", &rate);
    return rate;
}

/* The "Selected codec:" name of `bluealsactl info` output, without the
 * ":configuration blob" that only `info -v` appends. Leaves out untouched
 * when the field is missing, like copy_info_field(). */
void bluealsa_info_codec_name(const char * info, char * out, size_t out_size) {
    copy_info_field(out, out_size, info, "Selected codec:");
    char * colon = strchr(out, ':');
    if (colon) *colon = '\0';
}

static void bt_rate_remember_refusal(const char * path, const char * codec, unsigned int rate) {
    pthread_mutex_lock(&bt_rate_applied_mutex);
    snprintf(bt_rate_refused_path, sizeof(bt_rate_refused_path), "%s", path);
    snprintf(bt_rate_refused_codec, sizeof(bt_rate_refused_codec), "%s", codec);
    bt_rate_refused_rate = rate;
    pthread_mutex_unlock(&bt_rate_applied_mutex);
}

/* Applies an explicit transport rate other than 44.1 kHz, which the daemon
 * has no argument for. This goes through SelectCodec, and in BlueALSA 5 that
 * recreates the A2DP transport rather than reconfiguring it, so it costs a
 * brief re-handshake and the accessory is free to refuse it outright.
 *
 * Attempted at most once per PCM path per run, and deliberately NOT reset on
 * disconnect the way the codec preference is: a refusal can itself tear the
 * transport down, and retrying on every reconnect would turn that into an
 * endless connect/refuse/drop cycle.
 *
 * cancel (optional) is checked between its two subprocesses. Returns true
 * when it ran a rate reconfiguration (`codec -r`), successful or not: that
 * can replace the PCM and fall back to another codec, so the caller has to
 * verify the codec again. */
bool bluealsa_apply_rate_preference_cancellable(const char * path, const atomic_bool * cancel) {
    /* Automatic SSC negotiates from the profile selected by the daemon. An
     * explicit 44.1 kHz choice still needs a live transport check because
     * --ssc-auto-profile can select a higher SSC rate. */
    unsigned int want = atomic_load(&bt_sample_rate);
    if (want == 0) return false;
    if (!bluealsa_is_audio_pcm_path(path)) return false;
    unsigned int correction_generation = atomic_load_explicit(&bt_ssc_correction_generation, memory_order_acquire);

    char info_out[2048];
    char * info_argv[] = { (char *) BLUEALSA_CTL, (char *) "info", (char *) path, NULL };
    if (!subprocess_run_low_priority(info_argv, info_out, sizeof(info_out))) return false;
    if (bt_cancelled(cancel)) return false;

    unsigned int current = bluealsa_info_rate(info_out);
    if (current == want) return false; /* already negotiated there */

    char codec[32] = {0};
    bluealsa_info_codec_name(info_out, codec, sizeof(codec));
    if (!codec[0]) return false;

    pthread_mutex_lock(&bt_rate_applied_mutex);
    bool refused = bt_rate_refused_rate == want && strcmp(bt_rate_refused_path, path) == 0 &&
                   strcasecmp(bt_rate_refused_codec, codec) == 0;
    pthread_mutex_unlock(&bt_rate_applied_mutex);
    if (refused) return false;

    if (want == 44100 && bt_codec_name_is_ssc(codec)) {
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
    char * argv[] = { (char *) BLUEALSA_CTL, (char *) "codec", (char *) "-r", rate_str,
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
        unsigned int after = bluealsa_info_rate(info_out);
        if (after != want) {
            DBG_LOG("bt_control: rate %u not kept for %s on %s (now %u)\n", want, path, codec, after);
            bt_rate_remember_refusal(path, codec, want);
        }
    }
    return true;
}

void bluealsa_apply_rate_preference(const char * path) {
    (void) bluealsa_apply_rate_preference_cancellable(path, NULL);
}

/* Automatic picks the best codec both sides support, in the order the stock
 * HiBy player documents: LDAC > samsung-SC > aptX HD > aptX > AAC > SBC. Left to itself,
 * BlueZ takes the first endpoint in an order that does not follow quality
 * (and then keeps its LastUsed one), so LDAC headphones could stay on AAC. */
enum { BT_AUTO_SSC_RANK = 1, BT_AUTO_SSC_REJECT_BIT = 1u << BT_AUTO_SSC_RANK };
/* Keep the SSC array entry and BT_AUTO_SSC_RANK together when changing the ranking. */
static const char * const BT_AUTO_CODEC_RANKING[] = { "LDAC", [BT_AUTO_SSC_RANK] = "samsung-SC", "aptX-HD", "aptX", "AAC", "SBC" };
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

bool bluealsa_available_codecs_known(const char * info_out) {
    for (size_t i = 0; i < sizeof(BT_AUTO_CODEC_RANKING) / sizeof(BT_AUTO_CODEC_RANKING[0]); i++)
        if (bluealsa_available_codec_contains(info_out, BT_AUTO_CODEC_RANKING[i])) return true;
    return false;
}

/* SSC selection is serialized by the PCMAdded preference worker. Keep ALSA
 * from issuing a second codec change from a stale asoundrc snapshot. */
const char * bt_playback_codec_override(void) {
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

/* prefix + mac + suffix, with the MAC's colons as the underscores BlueZ and
 * BlueALSA use in object paths. Truncates like snprintf. */
void bt_mac_to_path(char * out, size_t out_size, const char * prefix, const char * mac,
                           const char * suffix) {
    snprintf(out, out_size, "%s%s%s", prefix, mac, suffix);
    for (char * c = out; *c; c++) if (*c == ':') *c = '_';
}

/* The device's name, Modalias and connection state as `bluetoothctl info`
 * lines, read with one D-Bus call: bluetoothctl loads BlueZ's whole object
 * tree and registers an advertisement monitor on every start, which is
 * heavy next to a Bluetooth stream being set up. */
bool bt_device_info_text(const char * mac, char * out, size_t size) {
    char object[64], dbus[8192];
    bt_mac_to_path(object, sizeof(object), "/org/bluez/hci0/dev_", mac, "");
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

void bt_buds_reset(const char *path) {
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
atomic_uint bt_user_disconnect_generation;
static pthread_mutex_t bt_ssc_discovery_mutex = PTHREAD_MUTEX_INITIALIZER;
static char bt_ssc_discovery_mac[18];
static bool bt_ssc_discovery_claimed;
static uint32_t bt_ssc_discovery_queued_at;

/* Explicit SSC and Automatic both recover a missing SSC endpoint. */
bool bt_ssc_recovery_preference(void) {
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
    if (i >= 0 && bt_auto_devices[i].ssc.skip_ssc) rejected |= BT_AUTO_SSC_REJECT_BIT;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    return rejected;
}

/* SSC is rejected for this accessory, including by the persisted backoff. */
bool bt_auto_ssc_rejected(const char * path) {
    return (bt_auto_rejected(path) & BT_AUTO_SSC_REJECT_BIT) != 0;
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
void bt_auto_forget_switch(const char * path) {
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
            persist = r == BT_AUTO_SSC_RANK;
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

void bt_auto_pcm_added(const char * path) {
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

/* 0 until a watch has run. Written by the output watch, read by the
 * preference worker; a stand-alone timestamp that publishes no other data, so
 * relaxed ordering is enough. */
atomic_uint bt_output_watch_started_at;

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
    uint32_t watch_started_at = atomic_load_explicit(&bt_output_watch_started_at, memory_order_relaxed);
    if (!open_at && watch_started_at) open_at = watch_started_at + BT_SSC_STREAM_OPEN_MS;
    return open_at && (int32_t)(open_at - now) > 0 ? open_at : 0;
}

/* Absolute time a PCM removal must be held back until because a codec
 * switch is still in flight, or 0 for the normal grace. */
uint32_t bt_auto_reconfigure_deadline(const char * path, uint32_t removed_at) {
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
void bt_auto_drop_confirmed(const char * path, uint32_t removed_at) {
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
void bt_output_disconnect_finish_pending(const char * pending_path, uint32_t removed_at) {
    if (!pending_path[0]) return;
    bt_auto_drop_confirmed(pending_path, removed_at);
    bluealsa_clear_codec_path(pending_path);
}

/* Our own reconnect (recovery, rate correction) is about to drop the PCM;
 * a drop in the next 30 s belongs to the same connection. */
void bt_auto_expect_reconnect(const char * path) {
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, true);
    if (i >= 0) bt_auto_devices[i].ssc.keep_until = bt_control_monotonic_ms() + 30000u;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
}

/* A flowing SSC session lifts the persisted backoff as soon as it has
 * flowed long enough, not only when it ends. */
void bt_auto_flowing_tick(void) {
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

static bool bt_cancelled(const atomic_bool * cancel) {
    return cancel && atomic_load(cancel);
}

/* True while a Galaxy Buds SSC endpoint may still appear, with the retry time
 * set; the caller then reports "not done yet". */
static bool bt_defer_buds_endpoint_wait(const char * path, uint32_t wait_started,
                                        uint32_t * defer_until) {
    if (bt_control_monotonic_ms() - wait_started >= BT_BUDS_SSC_DISCOVERY_GRACE_MS) return false;
    DBG_LOG("bt_control: waiting for Galaxy Buds SSC endpoint on %s\n", path);
    *defer_until = wait_started + BT_BUDS_SSC_DISCOVERY_GRACE_MS;
    return true;
}

/* False means not done yet (*defer_until may be set); true means a codec was chosen. */
static bool bt_choose_auto_codec(const char * path, const char * info_out, bool ssc_advertised,
                                  bool galaxy_buds, bool ssc_strong, uint32_t buds_wait_started,
                                  char * best, size_t best_size, uint32_t * defer_until) {
    bool ssc_rejected = bt_auto_ssc_rejected(path);
    /* The persisted backoff only stops us choosing SSC; when the accessory
     * opened SSC itself it stays ("already"), as reconfiguring a live SSC
     * stream can drop the link. A rejection this session still applies. */
    char live[32];
    if (ssc_rejected && ssc_advertised && bluealsa_parse_selected_codec(info_out, live, sizeof(live)) &&
        bt_codec_name_is_ssc(live) &&
        !(bt_auto_rejected_session(path) & BT_AUTO_SSC_REJECT_BIT))
        ssc_rejected = false;
    if (ssc_advertised && !ssc_rejected) {
        snprintf(best, best_size, "%s", "samsung-SC");
    } else {
        if (galaxy_buds && !ssc_rejected &&
            bt_defer_buds_endpoint_wait(path, buds_wait_started, defer_until))
            return false;
        if (ssc_strong && !ssc_rejected && bluealsa_available_codecs_known(info_out)) {
            if (bt_ssc_discovery_recovery_queue(path)) {
                *defer_until = bt_control_monotonic_ms() + 500; /* a queued SSC endpoint recovery goes first */
                return false;
            }
            bt_auto_endpoint_failure(path);
        }
        if (!bluealsa_best_available_codec(info_out, bt_auto_rejected(path), best, best_size)) {
            DBG_LOG("bt_control: auto codec for %s: available codecs not known yet\n", path);
            return false;
        }
    }
    return true;
}

/* False means not done yet (*defer_until may be set); true means a codec was chosen. */
static bool bt_choose_explicit_ssc_codec(const char * path, const char * info_out, bool ssc_advertised,
                                          bool galaxy_buds, bool ssc_strong, uint32_t buds_wait_started,
                                          char * best, size_t best_size, const char ** codec,
                                          uint32_t * defer_until) {
    /* SSC is only implemented by Samsung accessories. Keep an explicit
     * SSC preference for a later compatible connection, but use this
     * device's best advertised codec when SSC is unavailable or has
     * already failed configuration for this device. */
    unsigned int rejected = bt_auto_rejected(path);
    bool ssc_rejected = (rejected & BT_AUTO_SSC_REJECT_BIT) != 0;
    bool may_wait_for_ssc = !ssc_advertised && !ssc_rejected;
    if (!bluealsa_available_codecs_known(info_out)) {
        DBG_LOG("bt_control: SSC codecs for %s are not known yet\n", path);
        return false;
    }
    if (may_wait_for_ssc && galaxy_buds &&
        bt_defer_buds_endpoint_wait(path, buds_wait_started, defer_until))
        return false;
    if (may_wait_for_ssc && galaxy_buds &&
            bt_ssc_discovery_recovery_queue(path)) {
        *defer_until = bt_control_monotonic_ms() + 500; /* a queued SSC endpoint recovery goes first */
        return false;
    }
    if (may_wait_for_ssc && ssc_strong)
        bt_auto_endpoint_failure(path);
    if (!ssc_advertised || ssc_rejected) {
        if (!bluealsa_best_available_codec(info_out, rejected, best, best_size)) {
            DBG_LOG("bt_control: SSC unavailable for %s and no fallback codec is known yet\n", path);
            return false;
        }
        *codec = best;
    }
    return true;
}

/* Returns false only when a selection was attempted and did not take
 * effect (or, for Automatic, the accessory's codecs are not known yet), so
 * the caller can retry; nothing to do counts as success. */
static bool bluealsa_apply_codec_preference_serial(const char * path, const atomic_bool * cancel,
                                                   uint32_t * defer_until) {
    uint32_t unused_defer;
    if (!defer_until) defer_until = &unused_defer;
    *defer_until = 0;
    pthread_mutex_lock(&bt_codec_applied_mutex);
    bool done = strcmp(bt_codec_applied_path, path) == 0;
    unsigned int clears = bt_codec_clears;
    pthread_mutex_unlock(&bt_codec_applied_mutex);
    if (done) return true;
    if (bt_cancelled(cancel)) return true; /* stopping: not a failure to retry */

    /* Current state first: an accessory already on the wanted codec is
     * recorded without re-selecting it (which would reconfigure the link). */
    char info_out[2048];
    char * info_argv[] = { (char *) BLUEALSA_CTL, (char *) "info", (char *) path, NULL };
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
        !bt_auto_ssc_rejected(path) && !ssc_advertised;
    if (inspect_buds_name)
        bt_buds_state(path, &galaxy_buds, &buds_metadata_known, &buds_wait_started, &ssc_signal, cancel);
    /* Only a Samsung Modalias or a remembered SSC justifies a recovery
     * reconnect or a recorded failure; a name alone only earns the wait. */
    bool ssc_strong = ssc_signal == BT_SSC_SIGNAL_MODALIAS || ssc_signal == BT_SSC_SIGNAL_REMEMBERED;
    if (bt_cancelled(cancel)) return true;
    if (inspect_buds_name && !buds_metadata_known &&
        bt_control_monotonic_ms() - buds_wait_started < BT_BUDS_SSC_DISCOVERY_GRACE_MS) {
        DBG_LOG("bt_control: waiting for Bluetooth device name on %s\n", path);
        *defer_until = buds_wait_started + BT_BUDS_SSC_DISCOVERY_GRACE_MS;
        return false;
    }
    char best[32];
    if (automatic) {
        if (!bt_choose_auto_codec(path, info_out, ssc_advertised, galaxy_buds, ssc_strong,
                                  buds_wait_started, best, sizeof(best), defer_until))
            return false;
        codec = best;
    } else if (explicit_ssc) {
        if (!bt_choose_explicit_ssc_codec(path, info_out, ssc_advertised, galaxy_buds, ssc_strong,
                                          buds_wait_started, best, sizeof(best), &codec, defer_until))
            return false;
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
        *defer_until = ready;
        return false;
    }

    char * argv[] = { (char *) BLUEALSA_CTL, (char *) "codec",
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

/* cancel (optional) is checked between subprocesses; a cancelled pass
 * reports success so it is not scheduled for a retry. */
bool bluealsa_apply_codec_preference_deferred(const char *path, const atomic_bool *cancel,
                                                     uint32_t *defer_until) {
    if (defer_until) *defer_until = 0;
    if (!bluealsa_is_audio_pcm_path(path)) return true;
    pthread_mutex_lock(&bt_codec_apply_serial);
    bool ok = bluealsa_apply_codec_preference_serial(path, cancel, defer_until);
    pthread_mutex_unlock(&bt_codec_apply_serial);
    return ok;
}

bool bluealsa_apply_codec_preference(const char * path) {
    return bluealsa_apply_codec_preference_deferred(path, NULL, NULL);
}

/* Whether this PCM's codec has been verified since it last connected: the
 * latch is recorded only after an observed match. */
bool bluealsa_codec_verified(const char * path) {
    pthread_mutex_lock(&bt_codec_applied_mutex);
    bool verified = strcmp(bt_codec_applied_path, path) == 0;
    pthread_mutex_unlock(&bt_codec_applied_mutex);
    return verified;
}