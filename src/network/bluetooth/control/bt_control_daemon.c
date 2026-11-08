#include <fcntl.h>
#ifndef HOST_BUILD
#include "../media/bt_media_player.h"
#endif
#include "bluetooth_control_internal.h"

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
/* Lines of `ps` output containing needle as a substring or, with
 * exact_basename, any whitespace-separated token whose basename is needle.
 * Each line counts once. */
static int count_ps(const char * needle, bool exact_basename) {
    char out[4096];
    char * argv[] = { (char *) "ps", NULL };
    if (!subprocess_run(argv, out, sizeof(out))) return -1;

    int count = 0;
    char * line_save = NULL;
    char * line = strtok_r(out, "\n", &line_save);
    while (line) {
        if (!exact_basename) {
            if (strstr(line, needle)) count++;
        } else {
            char * field_save = NULL;
            char * field = strtok_r(line, " \t", &field_save);
            while (field) {
                const char * base = strrchr(field, '/');
                if (base) base++;
                else base = field;
                if (strcmp(base, needle) == 0) {
                    count++;
                    break;
                }
                field = strtok_r(NULL, " \t", &field_save);
            }
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return count;
}

/* Reconcile only the backend daemon itself.  The stock startup scripts can
 * leave a source daemon running without the requested encoder quality; a
 * sink daemon is deliberately left alone because SBC-XQ is source-only. */
static bool bluealsa_needs_source_restart(bt_codec_preference_t preference,
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
        if (strcmp(name, BLUEALSA_DAEMON_NAME) != 0) continue;
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
    if (count_ps("dbus-daemon", false) <= 1 && dbus_system_bus_reachable()) return;

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
    /* The kernel lists every HCI controller under /sys/class/bluetooth, the registry `hciconfig`
     * prints from, so a directory scan answers without spawning anything. Without that directory
     * ask hciconfig as before. */
    DIR * dir = opendir("/sys/class/bluetooth");
    if (dir) {
        bool found = false;
        struct dirent * entry;
        while (!found && (entry = readdir(dir)) != NULL)
            found = strncmp(entry->d_name, "hci", 3) == 0 && strchr(entry->d_name, ':') == NULL;
        closedir(dir);
        return found;
    }
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

    /* `bluetoothctl show` asks bluetoothd for its managed objects and prints the default
     * controller. bt_adapter_dbus_powered() makes the same request without a process and the result
     * is fed into the code below as the text bluetoothctl would have printed. A timeout or error
     * (-1) runs bluetoothctl as before, so the timeout counter is fed exactly as it always was. */
    char out[2048];
    int dbus_state = bt_adapter_dbus_powered();
    if (dbus_state != BT_ADAPTER_UNKNOWN) {
        snprintf(out, sizeof(out), "%s", dbus_state == BT_ADAPTER_POWERED ? "Powered: yes" :
                 dbus_state == BT_ADAPTER_NONE ? "No default controller available" : "Powered: no");
    } else {
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

/* Side-effect-free accessory probe, answering "maybe" rather than "no" when
 * it cannot tell: a failed or timed-out list-pcms must never authorize
 * killing a daemon that may own a live link, and a timeout is likeliest
 * exactly when the system is already struggling. A daemon that is genuinely
 * gone is still respawned, because the caller finds no process to keep.
 * Deliberately not find_source_pcm_path(), which also applies the
 * SoftVolume preference to whatever it finds. */
bool bluealsa_source_pcm_may_be_connected(void) {
    char list_out[4096];
    char * argv[] = { (char *) BLUEALSA_CTL, (char *) "list-pcms", NULL };
    if (!subprocess_run_low_priority(argv, list_out, sizeof(list_out))) return true;
    return strstr(list_out, "/a2dpsrc/sink") != NULL;
}

/* Recorded so bt_control_reapply_last_output_settings() (the wedge
 * recovery in bt_control_is_powered()) can restore this after a
 * bluetoothd restart -- a fresh bluetoothd/bt_resume-equivalent bring-up
 * always comes back up in the plain a2dp-source default, not whatever
 * profile was actually requested last. */
static bool last_applied_dac_mode_enabled = false;
static bool last_applied_volume_sync_enabled = false;
static bool output_settings_ever_applied = false;
static atomic_uint output_request_generation;
static unsigned int last_applied_generation;

static bool bt_agent_process_running(void) {
    DIR * proc = opendir("/proc");
    if (!proc) return false;
    bool found = false;
    struct dirent * entry;
    while (!found && (entry = readdir(proc))) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        char path[320];
        snprintf(path, sizeof(path), "/proc/%s/stat", entry->d_name);
        FILE * f = fopen(path, "r");
        if (!f) continue;
        /* "pid (comm) state ...": a just-killed agent lingers as a zombie, which is not running. */
        char stat[64] = {0};
        if (fgets(stat, sizeof(stat), f)) {
            const char * name = strchr(stat, '(');
            found = name && strncmp(name, "(bt-agent) ", 11) == 0 && name[11] != 'Z' && name[11] != 'X';
        }
        fclose(f);
    }
    closedir(proc);
    return found;
}

static void spawn_stock_agent(void) {
    char * agent_argv[] = { (char *) "bt-agent", (char *) "-c", (char *) "NoInputNoOutput",
                            (char *) "-p", (char *) "/usr/data/pin.conf", NULL };
    subprocess_spawn_daemon(agent_argv);
}

/* Both audio modes share authenticated pairing for Remote Control. Keep the
 * stock agent only when ours cannot register. Caller holds bt_daemon_respawn_mutex. */
static void ensure_pairing_agent(void) {
    if (bt_pairing_agent_activate()) {
        if (bt_agent_process_running()) subprocess_kill_all_matching("bt-agent");
    } else if (!bt_agent_process_running()) {
        spawn_stock_agent();
    }
}

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
bool ensure_bluealsa_running_rate_ex(unsigned int requested_rate, bool * out_deferred) {
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    if (last_applied_dac_mode_enabled) {
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
        return true;
    }
    ensure_pairing_agent();
    bt_codec_preference_t preference = (bt_codec_preference_t) atomic_load(&bt_codec_preference);
    bool must_restart = bluealsa_needs_source_restart(preference, requested_rate);
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
        subprocess_kill_all_matching(BLUEALSA_DAEMON_NAME);
    /* Killing detached daemons is asynchronous; the old process may still be
     * visible to an immediate ps snapshot. Once a mismatch was established,
     * respawn unconditionally instead of allowing that stale snapshot to
     * suppress the replacement. */
    if (!must_restart && count_ps(BLUEALSA_DAEMON_NAME, true) > 0) {
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
        return true;
    }
    char * argv[11];
    int argc = 0;
    argv[argc++] = (char *) BLUEALSA_DAEMON_PATH;
    argv[argc++] = (char *) "-p";
    argv[argc++] = (char *) "a2dp-source";
    if (bt_rate_wants_audio_cd_for_rate(requested_rate)) argv[argc++] = (char *) BT_A2DP_FORCE_44K1_ARG;
    bool ssc_auto_profile = bt_ssc_auto_profile_for_rate(requested_rate);
    if (ssc_auto_profile) argv[argc++] = (char *) BT_SSC_AUTO_PROFILE_ARG;
    if (bt_codec_is_sbc_xq()) argv[argc++] = (char *) "--sbc-quality=xq";
    argc = bt_codec_append_daemon_args(argv, argc);
    argv[argc] = NULL;
    /* A fresh daemon has no PCMs, and a dead one may not have reported their removal. */
    bluealsa_clear_soft_volume_path(NULL);
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
bool bt_control_init_chip(void) {
    pthread_mutex_lock(&bt_chip_mutex);

    if (bt_control_adapter_present()) {
        ensure_bluealsa_running();
        pthread_mutex_unlock(&bt_chip_mutex);
        return true;
    }

    if (aic_radio_active()) {
        /* AIC8800D40: bt_resume does nothing under the power gate. Take the gate
         * and bring the controller up through bt_enable_aic.sh instead. */
        bool powered = aic_radio_bt_power_on();
        if (powered) ensure_bluealsa_running();
        bool aic_result = powered && bt_control_adapter_present();
        pthread_mutex_unlock(&bt_chip_mutex);
        return aic_result;
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
#ifndef HOST_BUILD
    bt_media_player_set_phone_control_enabled(false);
#endif
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    bool sink = last_applied_dac_mode_enabled;
    bool volume_sync = last_applied_volume_sync_enabled;
    pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    if (sink) {
        if (!bt_control_apply_output_settings(false, volume_sync)) return;
    } else {
        if (!bt_control_dac_route_stop()) return;
        bt_dac_info_monitor_stop();
    }
    pthread_mutex_lock(&bt_chip_mutex);
    char out[256];
    char * argv[] = { (char *) "/usr/bin/bt_disable", NULL };
    if (aic_radio_active()) aic_radio_bt_power_off(); /* also releases the Bluetooth power gate */
    else subprocess_run(argv, out, sizeof(out));
    pthread_mutex_unlock(&bt_chip_mutex);
}

#define BLUEALSA_STARTUP_LOG_PATH "/usr/data/bluealsa_startup.log"

/* Spawns bluealsa daemon, logging startup output if TEST_BUILD_TAG is defined
 * and debug logging is on. */
static bool spawn_bluealsa_and_verify(char * const argv[]) {
#if defined(TEST_BUILD_TAG)
    /* Settings are loaded before bluealsa is first spawned, so
     * db_log_enabled() is authoritative; off means no log file. */
    return subprocess_spawn_daemon_logged(argv, db_log_enabled() ? BLUEALSA_STARTUP_LOG_PATH : NULL);
#else
    return subprocess_spawn_daemon_logged(argv, NULL);
#endif
}

static pid_t dac_route_pid = -1;
static int dac_route_fd = -1;

/* These helpers serialize with profile changes and adapter power-off. */
static bool dac_route_stop_locked(void) {
    if (dac_route_pid <= 0) return true;
    subprocess_terminate(dac_route_pid);
    pid_t result;
    do { result = waitpid(dac_route_pid, NULL, 0); } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != ECHILD) return false;
    close(dac_route_fd);
    dac_route_fd = -1;
    dac_route_pid = -1;
    return true;
}

bool bt_control_dac_route_stop(void) {
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    bool stopped = dac_route_stop_locked();
    pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    return stopped;
}

static bool dac_route_alive_locked(void) {
    if (dac_route_pid <= 0) return false;
    char discard[512];
    for (int i = 0; i < 32 && read(dac_route_fd, discard, sizeof(discard)) > 0; i++) {}
    pid_t result = waitpid(dac_route_pid, NULL, WNOHANG);
    if (result == 0 || (result < 0 && errno == EINTR)) return true;
    if (result == dac_route_pid || (result < 0 && errno == ECHILD)) {
        close(dac_route_fd); dac_route_fd = -1; dac_route_pid = -1;
    }
    return false;
}

bool bt_control_dac_route_alive(void) {
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    bool alive = dac_route_alive_locked();
    pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    return alive;
}

bool bt_control_dac_route_start(const bt_dac_stream_info_t * stream, bt_control_cancel_callback_t cancelled, void * ctx) {
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    bt_dac_stream_info_t now;
    bt_control_get_dac_stream_info(&now);
    bool valid = !cancelled(ctx) && last_applied_dac_mode_enabled && now.running && stream->mac[0] &&
                 now.generation == stream->generation && !strcmp(now.pcm_path, stream->pcm_path);
    if (valid && dac_route_pid <= 0) {
        char * argv[] = { (char *) "bluealsa-aplay", (char *) "--pcm-buffer-time=200000",
                         (char *) "--pcm-period-time=50000", (char *) stream->mac, NULL };
        valid = subprocess_popen(argv, &dac_route_pid, &dac_route_fd);
        if (valid && cancelled(ctx)) { dac_route_stop_locked(); valid = false; }
        if (valid) {
            int flags = fcntl(dac_route_fd, F_GETFL, 0);
            if (flags < 0 || fcntl(dac_route_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                dac_route_stop_locked(); valid = false;
            }
        }
    } else valid = false;
    pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    return valid;
}

static bool wait_bluealsa_profile(bool started, bool sink) {
    uint32_t deadline = bt_control_monotonic_ms() + 2000;
    for (;;) {
        int remaining = (int32_t) (deadline - bt_control_monotonic_ms());
        if (remaining <= 0) return false;
        char output[4096];
        char * ps[] = { (char *) "ps", NULL };
        int status;
        if (!subprocess_run_checked(ps, output, sizeof(output), remaining, &status) || status != 0) return false;
        char * daemon = strstr(output, BLUEALSA_DAEMON_NAME);
        if (!started && !daemon) return true;
        if (started && daemon) {
            daemon[strcspn(daemon, "\n")] = '\0';
            remaining = (int32_t) (deadline - bt_control_monotonic_ms());
            char * pcms[] = { (char *) BLUEALSA_CTL, (char *) "list-pcms", NULL };
            if (remaining > 0 && strstr(daemon, sink ? "a2dp-sink" : "a2dp-source") &&
                    subprocess_run_checked(pcms, NULL, 0, remaining, &status) && status == 0) return true;
        }
        usleep(50000);
    }
}

static bool apply_output_settings_generation(bool dac_mode_enabled, bool volume_sync_enabled, unsigned int generation) {
    /* Serializes bluealsa/bt-agent respawns and updates last-applied settings
     * to protect against concurrent caller races. */
    pthread_mutex_lock(&bt_chip_mutex);
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    if (generation != atomic_load(&output_request_generation)) {
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
        pthread_mutex_unlock(&bt_chip_mutex);
        return false;
    }
    if (!dac_route_stop_locked()) {
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
        pthread_mutex_unlock(&bt_chip_mutex);
        return false;
    }
    last_applied_generation = generation;
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

    subprocess_kill_all_matching(BLUEALSA_DAEMON_NAME);
    if (!wait_bluealsa_profile(false, false)) {
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
        pthread_mutex_unlock(&bt_chip_mutex);
        return false;
    }

    char * argv[14];
    int i = 0;
    argv[i++] = (char *) BLUEALSA_DAEMON_PATH;
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
    if (!spawn_bluealsa_and_verify(argv) || !wait_bluealsa_profile(true, dac_mode_enabled)) {
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
        pthread_mutex_unlock(&bt_chip_mutex);
        return false;
    }

    ensure_pairing_agent();

    if (!dac_mode_enabled) {
        char * disc_argv[] = { (char *) "bluetoothctl", (char *) "discoverable", (char *) "off", NULL };
        subprocess_run(disc_argv, NULL, 0);
    }
    if (dac_mode_enabled) bt_dac_info_monitor_start();
    pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    pthread_mutex_unlock(&bt_chip_mutex);
    return true;
}

bool bt_control_apply_output_settings(bool dac_mode_enabled, bool volume_sync_enabled) {
    unsigned int generation = atomic_fetch_add(&output_request_generation, 1) + 1;
    return apply_output_settings_generation(dac_mode_enabled, volume_sync_enabled, generation);
}

static void bt_control_reapply_last_output_settings(void) {
    /* Read settings under lock to avoid racing concurrent updates,
     * then call bt_control_apply_output_settings outside the lock. */
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    unsigned int generation = last_applied_generation;
    bool ever_applied = output_settings_ever_applied;
    bool dac_mode = last_applied_dac_mode_enabled;
    bool volume_sync = last_applied_volume_sync_enabled;
    pthread_mutex_unlock(&bt_daemon_respawn_mutex);

    if (!ever_applied) {
        /* Like every other ensure caller; apply_output_settings below takes it itself. */
        pthread_mutex_lock(&bt_chip_mutex);
        ensure_bluealsa_running();
        pthread_mutex_unlock(&bt_chip_mutex);
        return;
    }
    apply_output_settings_generation(dac_mode, volume_sync, generation);
}