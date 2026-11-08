#include "bluetooth_control_internal.h"

void copy_info_field(char * dst, size_t size, const char * text, const char * key) {
    const char * p = strstr(text, key);
    if (!p) return;
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;
    size_t n = strcspn(p, "\r\n");
    if (n >= size) n = size - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
}

/* Synchronizes volume between this player and connected a2dp-source accessories.
 * Maps AVRCP 0-127 linearly to the player's 0-100% volume. */
/* apply_soft_volume: discovery also applies the SoftVolume preference, as a
 * source PCM can predate a monitor subscription. The output watch's worker
 * passes false and applies it in its own cancellable pass instead. */
bool find_source_pcm_path_ex(char * out, size_t out_size, bool apply_soft_volume) {
    char list_out[4096];
    char * argv[] = { (char *) BLUEALSA_CTL, (char *) "list-pcms", NULL };
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

bool find_source_pcm_path(char * out, size_t out_size) {
    return find_source_pcm_path_ex(out, out_size, true);
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

/* The accessory's MAC ("XX:XX:XX:XX:XX:XX") from the "dev_XX_XX_XX_XX_XX_XX"
 * part of a BlueALSA PCM path; mac holds 18 bytes. */
bool bt_pcm_path_mac(const char * path, char * mac) {
    const char * dev = strstr(path, "dev_");
    if (!dev) return false;
    dev += 4; /* skip "dev_" */
    if (strlen(dev) < 17) return false; /* "XX_XX_XX_XX_XX_XX" */
    for (int i = 0; i < 17; i++) mac[i] = (dev[i] == '_') ? ':' : dev[i];
    mac[17] = '\0';
    return true;
}

bool bt_control_get_connected_device_mac(char * out, size_t out_size) {
    char path[256];
    if (!find_source_pcm_path(path, sizeof(path))) return false;

    char mac[18];
    if (!bt_pcm_path_mac(path, mac)) return false;

    snprintf(out, out_size, "%s", mac);
    return true;
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
    char * argv[] = { (char *) BLUEALSA_CTL, (char *) "info", (char *) "-v", path, NULL };
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

/* Value of one dbus-send property, as the tool prints it. */
const char * dbus_prop_value(const char * out, const char * prop) {
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

bool dbus_prop_string_equals(const char * out, const char * prop,
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
    if (bt_codec_name_is_ssc(codec))
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
        } else if (bt_codec_name_is_ssc(codec)) {
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
        char * info_argv[] = { (char *) BLUEALSA_CTL, (char *) "info", path, NULL };
        if (subprocess_run_low_priority(info_argv, info_out, sizeof(info_out))) {
            char codec[32] = {0};
            bluealsa_info_codec_name(info_out, codec, sizeof(codec));

            if (codec[0]) {
                char rate_str[16];
                snprintf(rate_str, sizeof(rate_str), "%u", want);
                char * argv[] = { (char *) BLUEALSA_CTL, (char *) "codec",
                                  (char *) "-r", rate_str, path, codec, NULL };
                (void) subprocess_run_checked(argv, NULL, 0, 5000, NULL);

                /* Believe the transport, not the exit status: a refusal can
                 * still leave the link up at the old rate. */
                unsigned int now = 0;
                if (find_source_pcm_path(path, sizeof(path)) &&
                        subprocess_run_low_priority(info_argv, info_out, sizeof(info_out)))
                    now = bluealsa_info_rate(info_out);
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
        char * argv[] = { (char *) BLUEALSA_CTL, (char *) "info", path, NULL };
        if (!subprocess_run_low_priority(argv, info_out, sizeof(info_out))) continue;
        unsigned int now = bluealsa_info_rate(info_out);
        if (automatic ? now != 0 : now == want) return true;
    }
    return false;
}

/* Rates the accessory itself advertises, read from BlueZ rather than from
 * BlueALSA. BlueALSA reports the negotiated intersection, and
 * --a2dp-force-audio-cd rewrites its own advertised capability to 44.1 kHz
 * alone (a2dp-aac.c and friends), so asking it while the clamp is on would
 * only ever answer "44.1". BlueZ still exposes each remote endpoint under
 * org.bluez.MediaEndpoint1 with the capability bytes the accessory actually
 * sent, which is what this decodes. */
int bt_control_get_available_rates(unsigned int * out, int max_count) {
    if (!out || max_count <= 0) return 0;

    char path[256];
    if (!find_source_pcm_path(path, sizeof(path))) return 0;

    char info_out[2048];
    char * info_argv[] = { (char *) BLUEALSA_CTL, (char *) "info", path, NULL };
    if (!subprocess_run_low_priority(info_argv, info_out, sizeof(info_out))) return 0;

    char device_path[160] = {0};
    copy_info_field(device_path, sizeof(device_path), info_out, "Device:");
    char codec[32] = {0};
    bluealsa_info_codec_name(info_out, codec, sizeof(codec));
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
                                     BT_A2DP_SINK_UUID)) continue;

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
bool bt_control_prepare_playback_pcm(char * out, size_t out_size) {
    if (!out || out_size == 0) return false;

    char path[256];
    if (!find_source_pcm_path(path, sizeof(path))) return false;

    /* Derived from the path already in hand: bt_control_get_connected_device_mac()
     * would repeat the list-pcms this just did. */
    char mac[18];
    if (!bt_pcm_path_mac(path, mac)) return false;

    char info_out[2048];
    char * info_argv[] = { (char *) BLUEALSA_CTL, (char *) "info", path, NULL };
    if (!subprocess_run_low_priority(info_argv, info_out, sizeof(info_out))) return false;

    unsigned int rate = bluealsa_info_rate(info_out);
    unsigned int channels = 0;
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