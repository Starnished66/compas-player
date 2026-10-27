/* Host regression harness for the Bluetooth source-volume policy.
 * Include the production translation unit so tests exercise its real policy
 * functions; subprocesses and the audio-volume boundary are deterministic. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/network/bluetooth/control/bluetooth_control.c"

static const char *pcm_path = "/org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/a2dpsrc/sink";
static const char *soft_reply = "SoftVolume: false\n";
static const char *codec_info_reply = "Running: false\nAvailable codecs: AAC SBC\nSelected codec: AAC\n";
static const char *bluetooth_info_reply = "Alias: Generic Headphones\n";
static bool transport_has_volume;
static int transport_available_after;
static int transport_probes;
static int transport_getall_calls;
static bool volume_property_present;
static bool fail_transport_getall_once;
static bool reconnect_backend;
static const char *backend_pcm_list;
static int fail_list_queries;
static int list_pcms_calls;
static bool fail_disconnect_command;
static const char *pcm_after_disconnect;
static bool backend_device_connected;
static int backend_disconnect_calls;
static int backend_connect_calls;
static char backend_connect_target[18];
static bool fail_soft_volume;
static int fail_codec_attempts;
static bool hide_codec_readback_once;
static bool hide_next_codec_readback;
static int codec_selects;
static int soft_volume_sets;
static int pcm_volume_sets;
static int dbus_volume_sets;
static int last_pcm_raw = -1;
static int max_command_timeout_ms;
static float player_volume = 0.42f;

static void reply(char *out, size_t size, const char *value) {
    if (out && size) snprintf(out, size, "%s", value);
}

static bool fake_run(char *const argv[], char *out, size_t size, int *status) {
    const char *cmd = argv[0];
    const char *sub = argv[1] ? argv[1] : "";
    if (strcmp(cmd, "ps") == 0) {
        reply(out, size, "  PID USER COMMAND\n  100 root /usr/bin/bluealsad\n");
    } else if (strcmp(cmd, "bluetoothctl") == 0 && strcmp(sub, "show") == 0) {
        reply(out, size, "Controller AA:BB:CC:DD:EE:FF\n Powered: yes\n");
    } else if (strcmp(cmd, "bluetoothctl") == 0 && argv[3] && strcmp(argv[3], "disconnect") == 0) {
        backend_disconnect_calls++;
        backend_device_connected = false;
        if (pcm_after_disconnect) backend_pcm_list = pcm_after_disconnect;
        if (status) *status = fail_disconnect_command ? 1 : 0;
    } else if (strcmp(cmd, "bluetoothctl") == 0 && argv[3] && strcmp(argv[3], "connect") == 0) {
        backend_connect_calls++;
        snprintf(backend_connect_target, sizeof(backend_connect_target), "%s", argv[4]);
        backend_device_connected = true;
        reply(out, size, "Connection successful\n");
        if (status) *status = 0;
    } else if (strcmp(cmd, "bluetoothctl") == 0 && strcmp(sub, "info") == 0 && reconnect_backend) {
        reply(out, size, backend_device_connected
            ? "Device AA:BB:CC:DD:EE:FF\n Paired: yes\n Trusted: yes\n Blocked: no\n Connected: yes\n UUID: Audio Sink (0000110b-0000-1000-8000-00805f9b34fb)\n"
            : "Device AA:BB:CC:DD:EE:FF\n Paired: yes\n Trusted: yes\n Blocked: no\n Connected: no\n UUID: Audio Sink (0000110b-0000-1000-8000-00805f9b34fb)\n");
    } else if (strstr(cmd, "bluealsactl") && strcmp(sub, "list-pcms") == 0) {
        list_pcms_calls++;
        if (fail_list_queries > 0) {
            fail_list_queries--;
            reply(out, size, "");
            return false;
        }
        reply(out, size, reconnect_backend ? backend_pcm_list : pcm_path);
    } else if (strstr(cmd, "bluealsactl") && strcmp(sub, "soft-volume") == 0 && argv[3] == NULL) {
        reply(out, size, soft_reply);
    } else if (strstr(cmd, "bluealsactl") && strcmp(sub, "soft-volume") == 0) {
        soft_volume_sets++;
        if (status) *status = fail_soft_volume ? 1 : 0;
    } else if (strstr(cmd, "bluealsactl") && strcmp(sub, "info") == 0) {
        if (hide_codec_readback_once) {
            hide_codec_readback_once = false;
            reply(out, size, "Running: false\nAvailable codecs: samsung-SC AAC\n");
        } else reply(out, size, codec_info_reply);
    } else if (strstr(cmd, "bluealsactl") && strcmp(sub, "volume") == 0) {
        pcm_volume_sets++;
        last_pcm_raw = atoi(argv[3]);
        if (status) *status = 0;
    } else if (strcmp(cmd, "dbus-send") == 0) {
        if (strstr(argv[5] ? argv[5] : "", "GetManagedObjects")) {
            transport_probes++;
            if (transport_has_volume || (transport_available_after > 0 &&
                                         transport_probes >= transport_available_after))
                reply(out, size, "object path \"/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/sep1/fd0\"\n");
            else reply(out, size, "");
        } else if (strstr(argv[5] ? argv[5] : "", "Properties.GetAll")) {
            transport_getall_calls++;
            if (fail_transport_getall_once) {
                fail_transport_getall_once = false;
                reply(out, size, "");
                if (status) *status = 1;
            } else if (volume_property_present) {
                reply(out, size,
                      "string \"UUID\"\n variant string \"0000110a-0000-1000-8000-00805f9b34fb\"\n"
                      "string \"Volume\"\n variant uint16 77\n");
            } else reply(out, size, "string \"UUID\"\n variant string \"0000110a-0000-1000-8000-00805f9b34fb\"\n");
        } else if (strstr(argv[5] ? argv[5] : "", "Properties.Set")) {
            dbus_volume_sets++;
            if (status) *status = 0;
        } else {
            reply(out, size, "");
            if (status) *status = 1;
        }
    } else if (strcmp(cmd, "bluetoothctl") == 0 && strcmp(sub, "info") == 0) {
        reply(out, size, bluetooth_info_reply);
    } else if (strstr(cmd, "bluealsactl") && strcmp(sub, "codec") == 0) {
        codec_selects++;
        if (fail_codec_attempts > 0) {
            fail_codec_attempts--;
            if (status) *status = 1;
        } else {
            if (status) *status = 0;
            if (hide_next_codec_readback) {
                hide_next_codec_readback = false;
                hide_codec_readback_once = true;
                return true;
            }
            if (hide_codec_readback_once) {
                hide_codec_readback_once = false;
                return true;
            }
            static char selected[160];
            const char *available = strstr(codec_info_reply, "Available codecs:");
            const char *line_end = available ? strchr(available, '\n') : NULL;
            char codecs[96] = "AAC SBC";
            if (available && line_end) {
                size_t n = (size_t)(line_end - available);
                if (n >= sizeof(codecs)) n = sizeof(codecs) - 1;
                memcpy(codecs, available, n);
                codecs[n] = '\0';
                char *colon = strchr(codecs, ':');
                if (colon) memmove(codecs, colon + 1, strlen(colon + 1) + 1);
            }
            snprintf(selected, sizeof(selected), "Running: false\nAvailable codecs:%s\nSelected codec: %s\n",
                     codecs, argv[3] ? argv[3] : "AAC");
            codec_info_reply = selected;
        }
    } else {
        fprintf(stderr, "unexpected subprocess in host test: %s %s\n", cmd, sub);
        abort();
    }
    if (status && *status < 0) *status = 0;
    return true;
}

bool subprocess_run(char *const argv[], char *out, size_t size) {
    int status = 0;
    return fake_run(argv, out, size, &status);
}

bool subprocess_run_low_priority(char *const argv[], char *out, size_t size) {
    return subprocess_run(argv, out, size);
}

bool subprocess_run_checked(char *const argv[], char *out, size_t size, int timeout_ms, int *status) {
    if (timeout_ms > max_command_timeout_ms) max_command_timeout_ms = timeout_ms;
    if (status) *status = -1;
    return fake_run(argv, out, size, status);
}

bool subprocess_spawn_daemon(char *const argv[]) {
    (void)argv;
    return true;
}

void subprocess_kill_all_matching(const char *needle) { (void)needle; }

float audio_get_volume(void) { return player_volume; }

static void reset_policy(void) {
    pthread_mutex_lock(&bt_soft_volume_mutex);
    bt_soft_volume_applied_path[0] = '\0';
    pthread_mutex_unlock(&bt_soft_volume_mutex);
    atomic_store(&bt_source_soft_volume, true);
    atomic_store(&modern_soft_volume_requested, false);
    atomic_store(&bt_soft_volume_clears, 0);
    atomic_store(&bt_source_readiness_generation, 0);
    bt_source_readiness_attempted_path[0] = '\0';
    bt_source_readiness_attempted_generation = 0;
    atomic_store(&bt_source_vol_pending_percent, -1);
    soft_volume_sets = pcm_volume_sets = dbus_volume_sets = 0;
    fail_soft_volume = false;
    transport_has_volume = false;
    transport_available_after = 0;
    transport_probes = 0;
    transport_getall_calls = 0;
    volume_property_present = true;
    fail_transport_getall_once = false;
    reconnect_backend = false;
    backend_pcm_list = "";
    fail_list_queries = 0;
    list_pcms_calls = 0;
    fail_disconnect_command = false;
    pcm_after_disconnect = NULL;
    backend_device_connected = true;
    backend_disconnect_calls = backend_connect_calls = 0;
    backend_connect_target[0] = '\0';
    soft_reply = "SoftVolume: false\n";
    codec_info_reply = "Running: false\nAvailable codecs: AAC SBC\nSelected codec: AAC\n";
    bluetooth_info_reply = "Alias: Generic Headphones\n";
    fail_codec_attempts = 0;
    hide_codec_readback_once = false;
    hide_next_codec_readback = false;
    codec_selects = 0;
    last_pcm_raw = -1;
    max_command_timeout_ms = 0;
    bt_control_ssc_discovery_recovery_cancel();
    pthread_mutex_lock(&bt_auto_devices_mutex);
    memset(bt_auto_devices, 0, sizeof(bt_auto_devices));
    bt_auto_devices_next = 0;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    bt_buds_reset(NULL);
    pthread_mutex_lock(&bt_codec_applied_mutex);
    bt_codec_applied_path[0] = '\0';
    bt_codec_clears++;
    pthread_mutex_unlock(&bt_codec_applied_mutex);
    bt_control_restore_codec_preference("auto");
}

static void test_missing_volume_uses_software_gain(void) {
    reset_policy();
    assert(bt_control_prepare_source_volume());
    assert(soft_volume_sets == 1);
    assert(atomic_load(&bt_source_soft_volume));
}

static void test_soft_volume_failure_does_not_mark_pcm_ready(void) {
    reset_policy();
    fail_soft_volume = true;
    assert(!bt_control_prepare_source_volume());
    assert(soft_volume_sets == 1);
    assert(bt_soft_volume_applied_path[0] == '\0');
}

static void test_fresh_software_pcm_is_seeded_before_ready(void) {
    reset_policy();
    soft_reply = "SoftVolume: true\n";
    player_volume = 0.20f;
    assert(bt_control_prepare_source_volume());
    assert(soft_volume_sets == 0);
    assert(pcm_volume_sets == 1);
    assert(last_pcm_raw == 25);
    assert(atomic_load(&bt_source_soft_volume));
    player_volume = 0.42f;
}

static void test_delayed_hardware_volume_is_ready_before_open(void) {
    reset_policy();
    transport_available_after = 3;
    assert(bt_control_prepare_source_volume());
    assert(transport_probes >= 3);
    assert(!atomic_load(&bt_source_soft_volume));
    assert(dbus_volume_sets == 1);
    assert(pcm_volume_sets == 1);
    assert(max_command_timeout_ms <= BT_SOURCE_VOLUME_READINESS_MS);
}

static void test_missing_volume_property_uses_software_gain(void) {
    reset_policy();
    transport_has_volume = true;
    volume_property_present = false;
    assert(bt_control_prepare_source_volume());
    assert(atomic_load(&bt_source_soft_volume));
    assert(dbus_volume_sets == 0);
    assert(pcm_volume_sets == 1);
}

static void test_transient_transport_property_error_retries_within_deadline(void) {
    reset_policy();
    transport_has_volume = true;
    fail_transport_getall_once = true;
    assert(bt_control_prepare_source_volume());
    assert(transport_getall_calls >= 2);
    assert(!atomic_load(&bt_source_soft_volume));
    assert(max_command_timeout_ms <= BT_SOURCE_VOLUME_READINESS_MS);
}

static void test_live_or_unknown_pcm_never_switches_to_hardware_gain(void) {
    for (int mode = 0; mode < 2; mode++) {
        reset_policy();
        transport_has_volume = true;
        codec_info_reply = mode == 0 ? "Running: true\nAvailable codecs: AAC SBC\nSelected codec: AAC\n"
                                     : "State: unknown\nAvailable codecs: AAC SBC\nSelected codec: AAC\n";
        assert(bt_control_prepare_source_volume());
        assert(atomic_load(&bt_source_soft_volume));
        assert(soft_volume_sets == 1);
        assert(dbus_volume_sets == 0);
    }
}

static void test_readiness_wait_is_once_per_pcm_generation(void) {
    reset_policy();
    assert(bt_control_prepare_source_volume());
    int first_probes = transport_probes;
    assert(first_probes > 0);
    uint32_t start = bt_control_monotonic_ms();
    assert(bt_control_prepare_source_volume());
    uint32_t elapsed = bt_control_monotonic_ms() - start;
    assert(transport_probes > first_probes);
    assert(elapsed < 700);
}

static void test_disconnect_generation_allows_one_fresh_wait(void) {
    reset_policy();
    assert(bt_control_prepare_source_volume());
    int first_probes = transport_probes;
    bluealsa_clear_soft_volume_path(pcm_path);
    assert(bt_source_readiness_generation == 1);
    assert(bt_control_prepare_source_volume());
    assert(transport_probes > first_probes + 1);
}

static void test_non_buds_explicit_ssc_falls_back_without_recovery(void) {
    reset_policy();
    bluetooth_info_reply = "Alias: Sony WH-1000XM5\n";
    codec_info_reply = "Running: false\nAvailable codecs: AAC SBC\nSelected codec: AAC\n";
    bt_control_restore_codec_preference("ssc");
    bool applied = bluealsa_apply_codec_preference_deferred(pcm_path, NULL, NULL);
    assert(applied);
    assert(codec_selects == 0);
    char mac[18];
    assert(!bt_control_ssc_discovery_recovery_peek(mac, sizeof(mac)));
}

static void test_buds_missing_ssc_queues_recovery_once_across_reset(void) {
    reset_policy();
    bluetooth_info_reply = "Alias: Galaxy Buds Pro\n";
    codec_info_reply = "Running: false\nAvailable codecs: AAC SBC\nSelected codec: AAC\n";
    bt_control_restore_codec_preference("ssc");
    pthread_mutex_lock(&bt_buds_state_mutex);
    snprintf(bt_buds_state_path, sizeof(bt_buds_state_path), "%s", pcm_path);
    bt_buds_name_checked = true;
    bt_buds_name_match = true;
    bt_buds_wait_started = bt_control_monotonic_ms() - BT_BUDS_SSC_DISCOVERY_GRACE_MS - 10;
    pthread_mutex_unlock(&bt_buds_state_mutex);

    uint32_t defer_until = 0;
    assert(!bluealsa_apply_codec_preference_deferred(pcm_path, NULL, &defer_until));
    assert(defer_until != 0);
    assert(codec_selects == 0);
    char mac[18];
    assert(bt_control_ssc_discovery_recovery_peek(mac, sizeof(mac)));
    assert(strcmp(mac, "AA:BB:CC:DD:EE:FF") == 0);
    bt_buds_reset(pcm_path);
    assert(bt_control_ssc_discovery_recovery_peek(mac, sizeof(mac)));
    assert(bt_control_ssc_discovery_recovery_take(mac));
    assert(!bt_control_ssc_discovery_recovery_take(mac));
    assert(!bt_control_ssc_discovery_recovery_peek(mac, sizeof(mac)));
}

static void test_old_finisher_cannot_clear_new_claim(void) {
    const char *path_a = "/org/bluealsa/hci0/dev_00_11_22_33_44_55/a2dpsrc/sink";
    const char *path_b = "/org/bluealsa/hci0/dev_66_77_88_99_AA_BB/a2dpsrc/sink";
    reset_policy();
    bt_control_restore_codec_preference("ssc");
    assert(bt_ssc_discovery_recovery_queue(path_a));
    assert(bt_control_ssc_discovery_recovery_take("00:11:22:33:44:55"));
    bt_control_ssc_discovery_recovery_cancel();
    assert(bt_ssc_discovery_recovery_queue(path_b));
    bt_control_ssc_discovery_recovery_finish("00:11:22:33:44:55");
    char mac[18];
    assert(bt_control_ssc_discovery_recovery_peek(mac, sizeof(mac)));
    assert(strcmp(mac, "66:77:88:99:AA:BB") == 0);
}

static void test_explicit_ssc_failure_does_not_poison_retry(void) {
    reset_policy();
    codec_info_reply = "Running: false\nAvailable codecs: samsung-SC AAC\nSelected codec: AAC\n";
    bt_control_restore_codec_preference("ssc");
    fail_codec_attempts = 1;
    assert(!bluealsa_apply_codec_preference_deferred(pcm_path, NULL, NULL));
    assert((bt_auto_rejected(pcm_path) & (1u << bt_auto_rank_index("samsung-SC"))) == 0);
    assert(bluealsa_apply_codec_preference_deferred(pcm_path, NULL, NULL));
    assert(codec_selects == 2);
    assert((bt_auto_rejected(pcm_path) & (1u << bt_auto_rank_index("samsung-SC"))) == 0);
}

static void test_explicit_ssc_readback_failure_does_not_poison_retry(void) {
    reset_policy();
    codec_info_reply = "Running: false\nAvailable codecs: samsung-SC AAC\nSelected codec: AAC\n";
    bt_control_restore_codec_preference("ssc");
    hide_next_codec_readback = true;
    assert(!bluealsa_apply_codec_preference_deferred(pcm_path, NULL, NULL));
    assert((bt_auto_rejected(pcm_path) & (1u << bt_auto_rank_index("samsung-SC"))) == 0);
    assert(bluealsa_apply_codec_preference_deferred(pcm_path, NULL, NULL));
    assert(codec_selects == 2);
    assert((bt_auto_rejected(pcm_path) & (1u << bt_auto_rank_index("samsung-SC"))) == 0);
}

static bool cancelled(void *ctx) { return *(bool *)ctx; }

static void test_ssc_disconnect_after_missing_readback_is_rejected(void) {
    for (int deliberate = 0; deliberate < 2; ++deliberate) {
        reset_policy();
        codec_info_reply = "Running: false\nAvailable codecs: samsung-SC AAC\nSelected codec: AAC\n";
        bt_control_restore_codec_preference("ssc");
        hide_next_codec_readback = true;
        assert(!bluealsa_apply_codec_preference_deferred(pcm_path, NULL, NULL));
        if (deliberate) bt_auto_forget_switch(pcm_path);
        bt_auto_note_disconnect(pcm_path);
        bool rejected = (bt_auto_rejected(pcm_path) & (1u << bt_auto_rank_index("samsung-SC"))) != 0;
        assert(rejected == !deliberate);
    }
}

static void test_ssc_recovery_backend_guards(void) {
    const char *target = "/org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/a2dpsrc/sink";
    reset_policy();
    reconnect_backend = true;
    bt_control_restore_codec_preference("ssc");
    bool is_cancelled = true;
    assert(!bt_control_reconnect_for_ssc_discovery("AA:BB:CC:DD:EE:FF", 44100,
                                                   cancelled, &is_cancelled));
    assert(list_pcms_calls == 0);

    is_cancelled = false;
    backend_pcm_list = target;
    codec_info_reply = "Running: false\nAvailable codecs: samsung-SC AAC\nSelected codec: AAC\n";
    assert(bt_control_reconnect_for_ssc_discovery("AA:BB:CC:DD:EE:FF", 44100,
                                                  cancelled, &is_cancelled));
    assert(backend_disconnect_calls == 0 && backend_connect_calls == 0);

    reset_policy();
    reconnect_backend = true;
    backend_pcm_list = "/org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/a2dpsrc/sink\n"
                       "/org/bluealsa/hci0/dev_11_22_33_44_55_66/a2dpsrc/sink\n";
    bt_control_restore_codec_preference("ssc");
    assert(!bt_control_reconnect_for_ssc_discovery("AA:BB:CC:DD:EE:FF", 44100,
                                                   cancelled, &is_cancelled));
    assert(backend_disconnect_calls == 0 && backend_connect_calls == 0);
}

static void test_ssc_recovery_timeout_reconnects_only_target(void) {
    reset_policy();
    reconnect_backend = true;
    backend_pcm_list = "/org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/a2dpsrc/sink";
    pcm_after_disconnect = "";
    fail_disconnect_command = true;
    codec_info_reply = "Running: false\nAvailable codecs: AAC SBC\nSelected codec: AAC\n";
    bt_control_restore_codec_preference("ssc");
    bool is_cancelled = false;
    assert(bt_control_reconnect_for_ssc_discovery("AA:BB:CC:DD:EE:FF", 44100,
                                                  cancelled, &is_cancelled));
    assert(backend_disconnect_calls == 1);
    assert(backend_connect_calls == 1);
    assert(strcmp(backend_connect_target, "AA:BB:CC:DD:EE:FF") == 0);
}

static void test_ssc_recovery_does_not_connect_over_new_output(void) {
    reset_policy();
    reconnect_backend = true;
    backend_pcm_list = "/org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/a2dpsrc/sink";
    pcm_after_disconnect = "/org/bluealsa/hci0/dev_11_22_33_44_55_66/a2dpsrc/sink";
    fail_disconnect_command = true;
    codec_info_reply = "Running: false\nAvailable codecs: AAC SBC\nSelected codec: AAC\n";
    bt_control_restore_codec_preference("ssc");
    bool is_cancelled = false;
    assert(bt_control_reconnect_for_ssc_discovery("AA:BB:CC:DD:EE:FF", 44100,
                                                  cancelled, &is_cancelled));
    assert(backend_disconnect_calls == 1);
    assert(backend_connect_calls == 0);
}

int main(void) {
    test_missing_volume_uses_software_gain();
    test_soft_volume_failure_does_not_mark_pcm_ready();
    test_fresh_software_pcm_is_seeded_before_ready();
    test_delayed_hardware_volume_is_ready_before_open();
    test_missing_volume_property_uses_software_gain();
    test_transient_transport_property_error_retries_within_deadline();
    test_live_or_unknown_pcm_never_switches_to_hardware_gain();
    test_readiness_wait_is_once_per_pcm_generation();
    test_disconnect_generation_allows_one_fresh_wait();
    test_non_buds_explicit_ssc_falls_back_without_recovery();
    test_buds_missing_ssc_queues_recovery_once_across_reset();
    test_old_finisher_cannot_clear_new_claim();
    test_explicit_ssc_failure_does_not_poison_retry();
    test_explicit_ssc_readback_failure_does_not_poison_retry();
    test_ssc_disconnect_after_missing_readback_is_rejected();
    test_ssc_recovery_backend_guards();
    test_ssc_recovery_timeout_reconnects_only_target();
    test_ssc_recovery_does_not_connect_over_new_output();
    puts("Bluetooth readiness host tests passed");
    return 0;
}
