#include "bluetooth_control_internal.h"
#include "bt_rc_trust.h"

/* Parses "Paired: yes"/"Connected: yes" out of `bluetoothctl info <mac>`'s
 * output (see bluetooth_control.h -- confirmed exact field names/format
 * against a real device). */
static void query_device_state(const char * mac, bool * out_paired, bool * out_connected,
                               bool * out_has_class) {
    *out_paired = false;
    *out_connected = false;
    if (out_has_class) *out_has_class = true; /* a failed query must not hide the device */
    char out[2048];
    char * argv[] = { (char *) "bluetoothctl", (char *) "info", (char *) mac, NULL };
    if (!subprocess_run_low_priority(argv, out, sizeof(out))) return;
    *out_paired = strstr(out, "Paired: yes") != NULL;
    *out_connected = strstr(out, "Connected: yes") != NULL;
    if (out_has_class) *out_has_class = strstr(out, "\n\tClass:") != NULL;
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

/* A "Device XX:XX:XX:XX:XX:XX Some Name Here" line of `bluetoothctl devices`:
 * copies the MAC into mac (18 bytes) and points name at the rest of the line. */
static bool bt_device_line_parse(const char * line, char * mac, const char ** name) {
    if (strncmp(line, "Device ", 7) != 0) return false;
    const char * mac_start = line + 7;
    const char * name_start = strchr(mac_start, ' ');
    if (!name_start || (size_t) (name_start - mac_start) != 17) return false;
    memcpy(mac, mac_start, 17);
    mac[17] = '\0';
    *name = name_start + 1;
    return true;
}

/* Fills out[] from `bluetoothctl devices` output, querying each device's
 * paired/connected state, and returns how many entries were written. */
static int bt_collect_device_lines(char * devices_buf, bt_device_t * out, int max_count,
                                   bool skip_unusable) {
    int count = 0;
    char * line_save = NULL;
    char * line = strtok_r(devices_buf, "\n", &line_save);
    while (line && count < max_count) {
        const char * name;
        if (bt_device_line_parse(line, out[count].mac, &name)) {
            snprintf(out[count].name, sizeof(out[count].name), "%s", name);
            bool has_class;
            query_device_state(out[count].mac, &out[count].paired, &out[count].connected, &has_class);
            /* LE-only identities of dual-mode headsets cannot carry A2DP and fail to pair. */
            if (skip_unusable && !out[count].paired && !has_class) {
                line = strtok_r(NULL, "\n", &line_save);
                continue;
            }
            count++;
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return count;
}

static bool bt_control_is_connected(void) {
    char devices_buf[4096];
    char * devices_argv[4];
    if (!bt_control_paired_devices_argv(devices_argv) ||
            !subprocess_run(devices_argv, devices_buf, sizeof(devices_buf))) return false;

    char * line_save = NULL;
    char * line = strtok_r(devices_buf, "\n", &line_save);
    while (line) {
        char mac[18];
        const char * name;
        if (bt_device_line_parse(line, mac, &name)) {
            bool paired, connected;
            query_device_state(mac, &paired, &connected, NULL);
            if (connected) return true;
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
 * determine either way. -1 means capability is MODERN but this specific
 * `devices Connected` call failed; that deliberately does NOT fall back to
 * the O(N) per-paired-device path -- that fallback is exactly the fork storm
 * this function exists to avoid, and a transient bluetoothctl hiccup is most
 * likely during the same radio power-on window that storm is worst in.
 * Callers should keep the last known state on -1, not treat it as "nothing
 * connected". The LEGACY/UNKNOWN fallback reports 0 when its own query
 * fails. */
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

    return bt_collect_device_lines(devices_buf, out, max_count, false);
}

/* True when BlueZ currently knows the device. */
static bool bt_control_device_known(const char * mac) {
    char out[8192] = {0}; /* a paired device's info is ~2 KB; truncation SIGPIPEs it */
    char * argv[] = { (char *) "bluetoothctl", (char *) "info", (char *) mac, NULL };
    int exit_code = -1;
    return subprocess_run_checked(argv, out, sizeof(out), 1500, &exit_code) && exit_code == 0 &&
           strstr(out, "not available") == NULL;
}

/* Runs discovery restricted to the classic BR/EDR bearer using `scan.transport bredr`.
 * Dual-mode devices paired via classic BR/EDR reliably support A2DP/AVRCP audio.
 * With `until_mac`, polls for that device and stops early once BlueZ knows it.
 * The scan runs in this bluetoothctl client, so it must stay alive meanwhile. */
static void run_bredr_scan_until(int seconds, const char * until_mac, bool * found) {
    if (found) *found = false;
    pid_t pid;
    int write_fd;
    char * argv[] = { (char *) "bluetoothctl", NULL };
    if (!subprocess_popen_stdin(argv, &pid, &write_fd)) return;

    const char * start_cmds = "scan.transport bredr\nscan on\n";
    ssize_t ignored = write(write_fd, start_cmds, strlen(start_cmds));
    (void) ignored;

    if (until_mac) {
        uint32_t deadline = bt_control_monotonic_ms() + (uint32_t) seconds * 1000;
        while ((int32_t) (deadline - bt_control_monotonic_ms()) > 0) {
            usleep(500000);
            if (bt_control_device_known(until_mac)) {
                if (found) *found = true;
                break;
            }
        }
    } else {
        sleep(seconds > 0 ? (unsigned int) seconds : 1);
    }

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
    if (seconds > 0) run_bredr_scan_until(seconds, NULL, NULL); /* blocks for `seconds` -- that's the point */

    char devices_buf[8192];
    char * devices_argv[] = { (char *) "bluetoothctl", (char *) "devices", NULL };
    if (!subprocess_run(devices_argv, devices_buf, sizeof(devices_buf))) return 0;

    return bt_collect_device_lines(devices_buf, out, max_count, true);
}

/* `bluetoothctl VERB MAC`, successful unless it fails to run or reports
 * "Failed". The caller takes bt_chip_mutex. */
static bool bt_ctl_verb_ok(const char * verb, const char * mac) {
    char out[512];
    char * argv[] = { (char *) "bluetoothctl", (char *) verb, (char *) mac, NULL };
    return subprocess_run(argv, out, sizeof(out)) && strstr(out, "Failed") == NULL;
}

#define BT_PAIR_TIMEOUT_MS 65000

/* Caller holds bt_chip_mutex. Prepares the daemon only after a successful query confirms that no
 * source PCM is live. A busy or unreadable state keeps the old manual pair/trust/connect behavior
 * and never authorizes a daemon restart. Respawns only on a mismatch, so calling it twice is cheap. */
static void prepare_daemon_for_connect(unsigned int rate) {
    if (!bluealsa_source_pcm_may_be_connected()) (void) ensure_bluealsa_running_rate_ex(rate, NULL);
}

bool bt_control_connect_with_rate(const char * mac, unsigned int rate) {
    char out[512];
    pthread_mutex_lock(&bt_chip_mutex);
    /* Before the pair: many headsets open A2DP right after bonding. */
    prepare_daemon_for_connect(rate);
    /* BlueZ drops non-paired discovered devices shortly after a scan ends, and
     * then pair/trust/connect all fail; rediscover it first. */
    if (!bt_control_device_known(mac)) {
        bool found = false;
        DBG_LOG("bt_control: %s unknown to BlueZ, rediscovering before connect\n", mac);
        run_bredr_scan_until(12, mac, &found);
        DBG_LOG("bt_control: rediscovery of %s %s\n", mac, found ? "found it" : "did not find it");
    }

    /* Pairing waits for the user to answer the popup, so it runs without bt_chip_mutex: it touches
     * neither the chip nor the bluealsa daemon, and holding the mutex that long would block every
     * other Bluetooth operation. The daemon is checked again afterwards, in case another holder
     * changed it meanwhile. */
    pthread_mutex_unlock(&bt_chip_mutex);
    char * pair_argv[] = { (char *) "bluetoothctl", (char *) "pair", (char *) mac, NULL };
    /* The pairing prompt waits up to 30 s (a shown code 60 s) for the user, so allow longer than that. */
    bool pair_finished = subprocess_run_timeout(pair_argv, out, sizeof(out), BT_PAIR_TIMEOUT_MS);
    pthread_mutex_lock(&bt_chip_mutex);
    bool ok = false;
    /* True means bluetoothctl ran to completion, whatever it reported: an already paired device
     * is a success too, and a failed pair is caught by connect. A timeout skips trust and connect. */
    if (pair_finished) {
        prepare_daemon_for_connect(rate);
        char * trust_argv[] = { (char *) "bluetoothctl", (char *) "trust", (char *) mac, NULL };
        subprocess_run(trust_argv, out, sizeof(out));
        ok = bt_ctl_verb_ok("connect", mac);
    }
    pthread_mutex_unlock(&bt_chip_mutex);
    return ok;
}

bool bt_control_disconnect(const char * mac) {
    atomic_fetch_add(&bt_user_disconnect_generation, 1);
    if (mac) {
        /* Asked for here, so not the codec's fault (see bt_auto_note_disconnect_at). */
        char path[64];
        bt_mac_to_path(path, sizeof(path), "/dev_", mac, "/");
        bt_auto_forget_switch(path);
    }
    pthread_mutex_lock(&bt_chip_mutex);
    bool ok = bt_ctl_verb_ok("disconnect", mac);
    pthread_mutex_unlock(&bt_chip_mutex);
    return ok;
}

bool bt_control_forget(const char * mac) {
    pthread_mutex_lock(&bt_chip_mutex);
    bool ok = bt_ctl_verb_ok("remove", mac);
    pthread_mutex_unlock(&bt_chip_mutex);
    bt_rc_trust_remove(mac); /* also when BlueZ no longer knows the device */
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
    char * argv[] = { (char *) BLUEALSA_CTL, (char *) "list-pcms", NULL };
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
            !bt_reconnect_info_has_uuid(info, BT_A2DP_SINK_UUID)) return false;
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
            char candidate[18];
            const char * name;
            if (!bt_device_line_parse(line, candidate, &name)) continue;
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

typedef struct {
    bt_control_cancel_callback_t callback;
    void * context;
} bt_ssc_discovery_cancel_t;

static bool bt_ssc_discovery_cancelled(void * opaque) {
    bt_ssc_discovery_cancel_t * cancel = opaque;
    return !bt_ssc_recovery_preference() ||
           bt_reconnect_cancelled(cancel->callback, cancel->context);
}

/* BlueZ Device1.DisconnectProfile / ConnectProfile for the A2DP sink: only
 * the profile is cycled, so the ACL, HFP and AVRCP stay up and the player's
 * own AVDTP session re-discovers the remote endpoints. The caller holds
 * bt_chip_mutex. */
static bool bt_a2dp_profile_call_locked(const char * mac, const char * method) {
    char object[64], member[48], arg[64];
    bt_mac_to_path(object, sizeof(object), "/org/bluez/hci0/dev_", mac, "");
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
    bt_mac_to_path(expected, sizeof(expected), "/dev_", mac, "/");

    /* Revalidate the target and discovery result under the same lifecycle
     * lock as manual operations. Never disturb another connected output or
     * a peer whose SSC endpoint appeared while the GUI was busy. */
    pthread_mutex_lock(&bt_chip_mutex);
    char list[4096], path[256] = {0}, info[4096];
    char * list_argv[] = { (char *) BLUEALSA_CTL, "list-pcms", NULL };
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
    char * info_argv[] = { (char *) BLUEALSA_CTL, "info", path, NULL };
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
            !bt_auto_ssc_rejected(path) &&
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
    if (!mac || strlen(mac) != 17 || rate != 44100 || bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;
    bool pcm_query_ok = false;
    if (!bt_reconnect_audio_output_connected(cancel_cb, cancel_ctx, &pcm_query_ok) ||
            !pcm_query_ok || bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;
    char path[256];
    if (!find_source_pcm_path_ex(path, sizeof(path), false) ||
            bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;
    char expected[64];
    bt_mac_to_path(expected, sizeof(expected), "/dev_", mac, "/");
    if (!strstr(path, expected)) return false;

    char info[2048];
    char * info_argv[] = { (char *) BLUEALSA_CTL, (char *) "info", path, NULL };
    if (!subprocess_run_low_priority(info_argv, info, sizeof(info)) ||
            bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;
    char codec[32] = {0};
    bluealsa_info_codec_name(info, codec, sizeof(codec));
    unsigned int current = bluealsa_info_rate(info);
    if (!bt_codec_name_is_ssc(codec) || current == 0 || current == rate)
        return false;

    bt_auto_forget_switch(path);
    bt_auto_expect_reconnect(path);
    if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;
    char * disconnect_argv[] = { (char *) "bluetoothctl", (char *) "--timeout", (char *) "5",
                                 (char *) "disconnect", (char *) mac, NULL };
    pthread_mutex_lock(&bt_chip_mutex);
    int exit_code = -1;
    bool disconnected = !bt_reconnect_cancelled(cancel_cb, cancel_ctx) &&
            subprocess_run_checked(disconnect_argv, NULL, 0, 6500, &exit_code) && exit_code == 0;
    pthread_mutex_unlock(&bt_chip_mutex);
    if (!disconnected || bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;

    uint32_t deadline = bt_control_monotonic_ms() + 5000;
    bool pcm_gone = false;
    while ((int32_t)(deadline - bt_control_monotonic_ms()) > 0) {
        if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;
        bool still_connected = bt_reconnect_audio_output_connected(cancel_cb, cancel_ctx, &pcm_query_ok);
        if (!pcm_query_ok) goto reconnect_fallback;
        if (!still_connected) {
            pcm_gone = true;
            break;
        }
        usleep(250000);
    }
    if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;
    if (!pcm_gone) goto reconnect_fallback;
    /* The UI may refresh an empty cached MAC and change the global rate while
     * this worker is disconnecting. Use the request captured by this job for
     * both daemon reconciliation and launch flags. */
    bool deferred = false;
    pthread_mutex_lock(&bt_chip_mutex);
    bool ensured = ensure_bluealsa_running_rate_ex(rate, &deferred);
    pthread_mutex_unlock(&bt_chip_mutex);
    if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;
    if (!ensured || deferred) goto reconnect_fallback;
    if (!bt_control_reconnect_paired_core(mac, rate, false, cancel_cb, cancel_ctx)) {
        if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;
        goto reconnect_fallback;
    }

    deadline = bt_control_monotonic_ms() + 20000;
    while ((int32_t)(deadline - bt_control_monotonic_ms()) > 0) {
        if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;
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
        char * current_argv[] = { (char *) BLUEALSA_CTL, (char *) "info", active, NULL };
        if (subprocess_run_low_priority(current_argv, info, sizeof(info))) {
            current = bluealsa_info_rate(info);
            if (current == rate) return true;
        }
        usleep(250000);
    }
reconnect_fallback:
    /* Once this attempt has dropped the link, make one safe target-bound
     * reconnect attempt unless the owning job was cancelled. The core checks
     * pairing, current PCM state and cancellation before connecting. */
    if (!bt_reconnect_cancelled(cancel_cb, cancel_ctx))
        return bt_control_reconnect_paired_core(mac, rate, true, cancel_cb, cancel_ctx);
    return false;
}