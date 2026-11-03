#include "bluetooth_control_internal.h"

/* BlueALSA 5 has no daemon-level --a2dp-volume option; apply the equivalent
 * control command when each PCM appears. */
/* bt_soft_volume_mutex guards only the applied-path latch and is never held
 * across a subprocess, so event handlers clearing it stay fast. Appliers
 * take bt_soft_volume_serial for the whole command (one at a time); the
 * latch is recorded only after the command succeeded, and not at all if a
 * clear happened meanwhile (bt_soft_volume_clears). */
pthread_mutex_t bt_soft_volume_serial = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint bt_soft_volume_clears;
static atomic_uint bt_source_readiness_generation;
static unsigned int bt_source_vol_pending_audio_generation;

/* bluealsactl soft-volume PATH prints "SoftVolume: true|false". */
static bool bluealsa_read_soft_volume(const char * path, bool * out) {
    char reply[128];
    char * argv[] = { (char *) BLUEALSA_CTL, (char *) "soft-volume", (char *) path, NULL };
    if (!subprocess_run(argv, reply, sizeof(reply))) return false;
    const char * v = strstr(reply, "SoftVolume:");
    if (!v) return false;
    v += strlen("SoftVolume:");
    while (*v == ' ') v++;
    *out = strncmp(v, "true", 4) == 0;
    return strncmp(v, "true", 4) == 0 || strncmp(v, "false", 5) == 0;
}

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
        if (!dbus_prop_string_equals(props, "UUID", BT_A2DP_SOURCE_UUID)) continue;
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

/* Sets BlueZ's absolute Volume (AVRCP) on a MediaTransport1 object. */
static bool bt_transport_set_volume(const char * transport, int raw, int * status) {
    char variant[32];
    snprintf(variant, sizeof(variant), "variant:uint16:%d", raw);
    char * argv[] = { "dbus-send", "--system", "--print-reply", "--dest=org.bluez",
        (char *) transport, "org.freedesktop.DBus.Properties.Set", "string:org.bluez.MediaTransport1",
        "string:Volume", variant, NULL };
    return subprocess_run_checked(argv, NULL, 0, 1000, status) && *status == 0;
}

/* `bluealsactl volume PATH N N`: BlueALSA's own PCM volume, both channels. */
static bool bluealsa_set_pcm_volume(const char * path, int raw, int * status) {
    char text[8];
    snprintf(text, sizeof(text), "%d", raw);
    char * argv[] = { (char *)BLUEALSA_CTL, "volume", (char *)path, text, text, NULL };
    return subprocess_run_checked(argv, NULL, 0, 1000, status) && *status == 0;
}

/* BlueALSA can report success after updating its PCM cache even when BlueZ's
 * remote AVRCP Set fails. In hardware-volume mode, check that request on the
 * exact transport before updating BlueALSA PCM state. BlueZ's volume property
 * can settle asynchronously or round the requested value; monitor events adopt
 * the headset's settled value. Software mode keeps the established PCM path. */
static bool bluealsa_write_source_volume(const char * path, int raw, bool software_volume,
                                         bool force_remote, bool * out_nudge_unconfirmed) {
    if (out_nudge_unconfirmed) *out_nudge_unconfirmed = false;
    if (raw < 0) raw = 0;
    if (raw > BT_SOURCE_VOLUME_MAX) raw = BT_SOURCE_VOLUME_MAX;
    if (!software_volume) {
        char transport[256];
        int current_raw = -1;
        if (bluealsa_find_source_transport_until(path, transport, sizeof(transport), &current_raw,
                                                 bt_control_monotonic_ms() + 6500) !=
            BT_SOURCE_TRANSPORT_READY) {
            DBG_LOG("bt_control: hardware Volume: no transport for %s\n", path);
            return false;
        }
        const int restore_raw = current_raw;
        bool remote_changed = false;
        bool nudged = false;
        if (force_remote && current_raw == raw) {
            /* BlueZ drops a Set equal to its cached Volume, and that cache can be
             * stale after the transport was recreated (the accessory reset its own
             * level). Nudge first so the real Set below goes out. An answer slower
             * than the wait is reported so the caller can force one more write. */
            int nudge_status = -1;
            if (bt_transport_set_volume(transport, raw > 0 ? raw - 1 : 1, &nudge_status)) {
                remote_changed = nudged = true;
                for (int tries = 0; tries < 20; tries++) {
                    int settled = -1;
                    if (bluealsa_find_source_transport_until(path, transport, sizeof(transport), &settled,
                                                             bt_control_monotonic_ms() + 100) ==
                            BT_SOURCE_TRANSPORT_READY && settled != raw) {
                        current_raw = settled;
                        break;
                    }
                    usleep(100000);
                }
                if (current_raw == raw && out_nudge_unconfirmed) *out_nudge_unconfirmed = true;
            } else {
                DBG_LOG("bt_control: hardware Volume nudge failed on %s (status %d)\n", transport, nudge_status);
                /* Typically AVRCP is not up yet. A seed stays in hardware mode
                 * and leaves it to the reconcile; a reconcile retries. */
                if (!out_nudge_unconfirmed) return false;
                *out_nudge_unconfirmed = true;
            }
        }
        if (current_raw != raw) {
            int set_status = -1;
            if (!bt_transport_set_volume(transport, raw, &set_status)) {
                DBG_LOG("bt_control: hardware Volume Set failed on %s (status %d)\n", transport, set_status);
                if (nudged) {
                    /* Undo the nudge so a software-gain fallback starts from the
                     * accessory's own level. Best effort. */
                    (void) bt_transport_set_volume(transport, restore_raw, &set_status);
                }
                return false;
            }
            remote_changed = remote_changed || restore_raw >= 0;
        }
        int status = -1;
        if (bluealsa_set_pcm_volume(path, raw, &status)) return true;
        DBG_LOG("bt_control: hardware volume: bluealsactl volume failed on %s (status %d)\n", path, status);
        if (remote_changed) {
            /* Leave the accessory at the level it had, so a caller falling back to
             * software gain does not attenuate twice. BlueZ queues the Set and
             * updates its cached Volume only when the accessory answers; until
             * then a restore to the old value matches the cache and is dropped,
             * so wait briefly for the new value to settle first. Best effort: an
             * accessory slower than this can still apply the queued level. */
            for (int tries = 0; tries < 5; tries++) {
                int settled = -1;
                if (bluealsa_find_source_transport_until(path, transport, sizeof(transport), &settled,
                                                         bt_control_monotonic_ms() + 100) ==
                        BT_SOURCE_TRANSPORT_READY && settled != restore_raw)
                    break;
                usleep(100000);
            }
            (void) bt_transport_set_volume(transport, restore_raw, &status);
        }
        return false;
    }
    int status = -1;
    return bluealsa_set_pcm_volume(path, raw, &status);
}

/* Enabling SoftVolume resets the PCM level. Seed it before playback opens,
 * using the mode currently being selected rather than the temporary atomic
 * fallback value that remains true until the policy latch commits. */
static bool bluealsa_seed_source_volume(const char * path, bool software_volume) {
    float percent = audio_get_volume();
    int raw = (int)(percent * BT_SOURCE_VOLUME_MAX + 0.5f);
    bool unconfirmed = false;
    bool ok = bluealsa_write_source_volume(path, raw, software_volume, true, &unconfirmed);
    if (unconfirmed) {
        pthread_mutex_lock(&bt_soft_volume_mutex);
        snprintf(bt_source_reconcile_path, sizeof(bt_source_reconcile_path), "%s", path);
        pthread_mutex_unlock(&bt_soft_volume_mutex);
    }
    return ok;
}

/* Negative capability is retried, but a live software-controlled PCM is
 * never switched to hardware gain across two subprocess calls mid-playback. */
static uint32_t bt_source_volume_probe_ms;
static char bt_source_readiness_attempted_path[256];
static unsigned int bt_source_readiness_attempted_generation;

static bool bluealsa_source_running_state_until(const char * path, bool * out_running,
                                                uint32_t deadline_ms) {
    char info[2048];
    char * argv[] = { (char *)BLUEALSA_CTL, (char *)"info", (char *)path, NULL };
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

static bool bluealsa_set_soft_volume(const char * path, bool on) {
    char * argv[] = { (char *) BLUEALSA_CTL, (char *) "soft-volume",
                      (char *) path, (char *) (on ? "on" : "off"), NULL };
    int exit_code = -1;
    return subprocess_run_checked(argv, NULL, 0, 5000, &exit_code) && exit_code == 0;
}

void bluealsa_apply_soft_volume_cancellable_ex(const char * path, const atomic_bool * cancel,
                                                       bool force_source_probe) {
    if (!bluealsa_is_audio_pcm_path(path)) return;
    pthread_mutex_lock(&bt_soft_volume_serial);
    pthread_mutex_lock(&bt_soft_volume_mutex);
    bool source = strstr(path, "/a2dpsrc/sink") != NULL;
    uint32_t now = bt_control_monotonic_ms();
    bool prior_source_soft_volume = atomic_load(&bt_source_soft_volume);
    bool prior_ready = strcmp(bt_soft_volume_applied_path, path) == 0;
    bool hw_failed = source && strcmp(bt_source_hw_failed_path, path) == 0;
    bool retry = source && !atomic_load(&modern_soft_volume_requested) &&
                 !hw_failed && prior_source_soft_volume &&
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
        bool want = atomic_load(&modern_soft_volume_requested) || hw_failed;
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
        bool hw_attempt = source && !want;
        bool hw_fallback = false;
        bool current;
        bool ok;
        bool soft_volume_unchanged = bluealsa_read_soft_volume(path, &current) && current == want;
        if (soft_volume_unchanged) {
            ok = true;
        } else {
            ok = bluealsa_set_soft_volume(path, want);
        }
        if (source && ok && !(prior_ready && source_probe_failed && prior_source_soft_volume &&
                              want && soft_volume_unchanged))
            ok = bluealsa_seed_source_volume(path, want);
        if (hw_attempt && !ok) {
            /* The accessory exposes Volume but rejects or ignores the write;
             * fall back to software gain instead of failing the output. */
            DBG_LOG("bt_control: hardware volume failed on %s; falling back to software gain\n", path);
            want = true;
            bool on = bluealsa_read_soft_volume(path, &current) && current;
            if (!on) on = bluealsa_set_soft_volume(path, true);
            ok = on && bluealsa_seed_source_volume(path, true);
            hw_fallback = ok;
        }
        pthread_mutex_lock(&bt_soft_volume_mutex);
        if (ok && atomic_load_explicit(&bt_soft_volume_clears, memory_order_acquire) == clears) {
            snprintf(bt_soft_volume_applied_path, sizeof(bt_soft_volume_applied_path), "%s", path);
            if (hw_fallback)
                snprintf(bt_source_hw_failed_path, sizeof(bt_source_hw_failed_path), "%s", path);
            if (source) atomic_store(&bt_source_soft_volume, want);
        }
        pthread_mutex_unlock(&bt_soft_volume_mutex);
    }
    pthread_mutex_unlock(&bt_soft_volume_serial);
}

void bluealsa_apply_soft_volume(const char * path) {
    bluealsa_apply_soft_volume_cancellable_ex(path, NULL, false);
}

/* Every clear also invalidates an application still in flight, whatever
 * its path, since that one has no latch yet to compare against; the cost of
 * a spurious invalidation is one idempotent soft-volume command later. */
void bluealsa_clear_soft_volume_path(const char * path) {
    pthread_mutex_lock(&bt_soft_volume_mutex);
    bool clear = !path || strcmp(bt_soft_volume_applied_path, path) == 0;
    if (!path || strcmp(bt_source_hw_failed_path, path) == 0)
        bt_source_hw_failed_path[0] = '\0';
    if (!path || strcmp(bt_source_reconcile_path, path) == 0)
        bt_source_reconcile_path[0] = '\0';
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
    uint8_t channels[2];
    if (*value == '"') {
        /* dbus-send prints all-printable byte arrays as a quoted string, with
         * " + \0" when the last byte is NUL. Raw bytes may include quotes. */
        const char * start = ++value;
        const char * close = strrchr(value, '"');
        if (!close) return false;
        const char * tail = close + 1;
        size_t len = (size_t) (close - start);
        size_t extra = 0;
        while (*tail == ' ') tail++;
        if (*tail == '+') {
            tail++;
            while (*tail == ' ') tail++;
            if (strncmp(tail, "\\0", 2) != 0) return false;
            tail += 2;
            extra = 1;
        }
        while (isspace((unsigned char)*tail)) tail++;
        if (*tail != '\0' || len + extra != 2) return false;
        for (size_t i = 0; i < len; i++) channels[i] = (uint8_t) start[i];
        if (extra) channels[1] = 0;
        if (channels[0] > BT_SOURCE_VOLUME_MAX || channels[1] > BT_SOURCE_VOLUME_MAX) return false;
    } else {
        if (*value++ != '[') return false;
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
    }
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

/* monitor_live: the monitor is subscribed, so a late accessory answer to a
 * reconcile write is seen; only then is the one-shot reconcile spent. */
static void bt_source_push_app_volume_if_changed(float * last_synced_app_percent, bool monitor_live) {
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
    /* An unanswered seed nudge can land after the monitor dropped its event;
     * write once more with force even at an unchanged app volume. */
    bool reconcile = monitor_live && ready && !software_volume &&
                     strcmp(bt_source_reconcile_path, path) == 0;
    pthread_mutex_unlock(&bt_soft_volume_mutex);
    if (!ready) goto done;
    float current_percent = audio_get_volume();
    if (!reconcile && current_percent == *last_synced_app_percent) goto done;
    int raw = (int) (current_percent * (float) BT_SOURCE_VOLUME_MAX + 0.5f);
    if (raw < 0) raw = 0;
    if (raw > BT_SOURCE_VOLUME_MAX) raw = BT_SOURCE_VOLUME_MAX;
    if (!reconcile && raw == bt_source_vol_last_synced_raw) {
        *last_synced_app_percent = current_percent;
        goto done;
    }

    if (!bluealsa_write_source_volume(path, raw, software_volume, reconcile, NULL)) goto done;
    pthread_mutex_lock(&bt_soft_volume_mutex);
    bool current_generation = generation == atomic_load_explicit(&bt_soft_volume_clears, memory_order_acquire) &&
                              strcmp(bt_soft_volume_applied_path, path) == 0 &&
                              software_volume == (atomic_load(&bt_source_soft_volume) ||
                                                  atomic_load(&modern_soft_volume_requested));
    if (current_generation) {
        bt_source_vol_last_synced_raw = raw;
        *last_synced_app_percent = current_percent;
        if (reconcile && strcmp(bt_source_reconcile_path, path) == 0) bt_source_reconcile_path[0] = '\0';
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
    bt_source_push_app_volume_if_changed(&last_synced_app_percent, false);
    if (!atomic_load(&bt_source_vol_sync_active)) return NULL;

    char * argv[] = { (char *) BLUEALSA_CTL, (char *) "monitor", (char *) "-p", NULL };
    pid_t pid;
    int read_fd;
    if (!subprocess_popen(argv, &pid, &read_fd)) {
        atomic_store(&bt_source_vol_sync_active, false);
        return NULL;
    }

    char buf[512];
    size_t buf_len = 0;
    char reset_path[256] = "";
    /* The child subscribes after it starts; allow it a moment before relying
     * on it to deliver an accessory answer. */
    uint32_t monitor_started_ms = bt_control_monotonic_ms();

    while (bt_source_vol_sync_active) {
        /* Push app-driven state BEFORE reading monitor events. In
         * particular, the first iteration must make the already-visible
         * player percentage authoritative before bluealsa's monitor can
         * report the accessory's remembered value; doing this afterward
         * let that startup event silently change audio_get_volume() while
         * the slider/topbar still showed the old app value. */
        bt_source_push_app_volume_if_changed(&last_synced_app_percent,
            (uint32_t) (bt_control_monotonic_ms() - monitor_started_ms) >= 1000);

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