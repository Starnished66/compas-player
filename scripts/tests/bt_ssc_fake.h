/* Shared harness of the Samsung SSC host tests: includes the production
 * translation unit like test_bluetooth_readiness.c, with deterministic fake
 * subprocesses. */
#ifndef BT_SSC_FAKE_H
#define BT_SSC_FAKE_H
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/network/bluetooth/control/bluetooth_control.c"
#include "../../src/core/persistence/bt_ssc_settings.h"

#define MAC "AA:BB:CC:DD:EE:FF"
static const char *pcm_path = "/org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/a2dpsrc/sink";
static char info_codecs[96];          /* "Available codecs:" payload */
static char info_selected[32];
static const char *forced_selected;   /* the accessory settles on this instead */
static bool hide_readback, fail_codec;
static bool hidden_once;
static int codec_selects, ssc_selects;
static char bluez_alias[64], bluez_modalias[64];
static bool bluez_connected;
static bool reconnect_backend, profile_disconnect_ok, profile_connect_ok;
static bool pcm_listed;
static int device_info_calls, bluetoothctl_infos;
static void (*on_profile_disconnect)(void);
static int profile_disconnects, profile_connects, full_disconnects, full_connects;

static void reply(char *out, size_t size, const char *value) {
    if (out && size) snprintf(out, size, "%s", value);
}

static bool has_arg(char *const argv[], const char *word) {
    for (int i = 1; argv[i]; i++) if (strcmp(argv[i], word) == 0) return true;
    return false;
}

static bool fake_run(char *const argv[], char *out, size_t size, int *status) {
    const char *cmd = argv[0], *sub = argv[1] ? argv[1] : "";
    if (status) *status = 0;
    if (strcmp(cmd, "dbus-send") == 0 && argv[5] && strstr(argv[5], "Device1.")) {
        bool disc = strstr(argv[5], "DisconnectProfile") != NULL;
        assert(strstr(argv[6], "0000110b-0000-1000-8000-00805f9b34fb"));
        assert(strstr(argv[4], "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"));
        if (disc) {
            profile_disconnects++;
            if (profile_disconnect_ok) pcm_listed = false;
            if (on_profile_disconnect) on_profile_disconnect();
        }
        else { profile_connects++; if (profile_connect_ok) pcm_listed = true; }
        if (status) *status = (disc ? profile_disconnect_ok : profile_connect_ok) ? 0 : 1;
    } else if (strcmp(cmd, "bluetoothctl") == 0 && has_arg(argv, "disconnect")) {
        full_disconnects++;
        bluez_connected = false;
        pcm_listed = false;
    } else if (strcmp(cmd, "bluetoothctl") == 0 && has_arg(argv, "connect")) {
        full_connects++;
        bluez_connected = true;
        pcm_listed = true;
        reply(out, size, "Connection successful\n");
    } else if (strcmp(cmd, "dbus-send") == 0 && argv[6] && strstr(argv[6], "org.bluez.Device1")) {
        device_info_calls++;
        snprintf(out, size, "method return\n   array [\n      dict entry(\n         string \"Alias\"\n"
                 "         variant             string \"%s\"\n      )\n      dict entry(\n"
                 "         string \"Connected\"\n         variant             boolean %s\n      )\n"
                 "      dict entry(\n         string \"Modalias\"\n         variant             string \"%s\"\n"
                 "      )\n   ]\n", bluez_alias, bluez_connected ? "true" : "false", bluez_modalias);
    } else if (strcmp(cmd, "bluetoothctl") == 0 && strcmp(sub, "info") == 0) {
        bluetoothctl_infos++;
        snprintf(out, size, "Device " MAC "\n Alias: %s\n Paired: yes\n Trusted: yes\n Blocked: no\n"
                 " Connected: %s\n UUID: Audio Sink (0000110b-0000-1000-8000-00805f9b34fb)\n%s%s\n",
                 bluez_alias, bluez_connected ? "yes" : "no",
                 bluez_modalias[0] ? " Modalias: " : "", bluez_modalias);
    } else if (strstr(cmd, "bluealsactl") && strcmp(sub, "list-pcms") == 0) {
        reply(out, size, pcm_listed ? pcm_path : "");
    } else if (strstr(cmd, "bluealsactl") && strcmp(sub, "info") == 0) {
        if (hidden_once) { hidden_once = false; reply(out, size, "Running: false\n"); }
        else snprintf(out, size, "Running: false\nAvailable codecs: %s\nSelected codec: %s\n",
                      info_codecs, info_selected);
    } else if (strstr(cmd, "bluealsactl") && strcmp(sub, "codec") == 0) {
        codec_selects++;
        if (strcmp(argv[3], "samsung-SC") == 0) ssc_selects++;
        if (fail_codec) { if (status) *status = 1; return true; }
        snprintf(info_selected, sizeof(info_selected), "%s", forced_selected ? forced_selected : argv[3]);
        hidden_once = hide_readback;
    } else if (strcmp(cmd, "ps") == 0) {
        reply(out, size, "  PID USER COMMAND\n  100 root /usr/bin/bluealsad\n");
    } else if (strcmp(cmd, "bluetoothctl") == 0 && strcmp(sub, "show") == 0) {
        reply(out, size, "Controller AA:BB:CC:DD:EE:FF\n Powered: yes\n");
    } else {
        fprintf(stderr, "unexpected subprocess in host test: %s %s\n", cmd, sub);
        abort();
    }
    return true;
}

bool subprocess_run(char *const argv[], char *out, size_t size) {
    int status = 0;
    return fake_run(argv, out, size, &status);
}
bool subprocess_run_low_priority(char *const argv[], char *out, size_t size) { return subprocess_run(argv, out, size); }
bool subprocess_run_checked(char *const argv[], char *out, size_t size, int timeout_ms, int *status) {
    (void)timeout_ms;
    return fake_run(argv, out, size, status);
}
bool subprocess_spawn_daemon(char *const argv[]) { (void)argv; return true; }
void subprocess_kill_all_matching(const char *needle) { (void)needle; }
float audio_get_volume(void) { return 0.4f; }

static void setup(const char *preference, const char *codecs, const char *selected) {
    snprintf(info_codecs, sizeof(info_codecs), "%s", codecs);
    snprintf(info_selected, sizeof(info_selected), "%s", selected);
    forced_selected = NULL;
    hide_readback = hidden_once = fail_codec = false;
    codec_selects = ssc_selects = 0;
    snprintf(bluez_alias, sizeof(bluez_alias), "Generic Headphones");
    bluez_modalias[0] = '\0';
    bluez_connected = true;
    pcm_listed = true;
    profile_disconnect_ok = profile_connect_ok = true;
    profile_disconnects = profile_connects = full_disconnects = full_connects = 0;
    on_profile_disconnect = NULL;
    device_info_calls = bluetoothctl_infos = 0;
    bt_control_ssc_discovery_recovery_cancel();
    pthread_mutex_lock(&bt_auto_devices_mutex);
    memset(bt_auto_devices, 0, sizeof(bt_auto_devices));
    bt_auto_devices_next = 0;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    bt_ssc_memory_reset();
    bt_buds_reset(NULL);
    pthread_mutex_lock(&bt_codec_applied_mutex);
    bt_codec_applied_path[0] = '\0';
    bt_codec_clears++;
    pthread_mutex_unlock(&bt_codec_applied_mutex);
    bt_control_restore_codec_preference(preference);
}

/* The watch's confirmed disconnect: classification, then the latch clears. */
static void drop(void) {
    bt_auto_drop_confirmed(pcm_path, bt_control_monotonic_ms());
    bluealsa_clear_codec_path(pcm_path);
}

static bool apply(uint32_t *defer) { return bluealsa_apply_codec_preference_deferred(pcm_path, NULL, defer); }
static bool ssc_rejected(void) {
    return (bt_auto_rejected(pcm_path) & (1u << bt_auto_rank_index("samsung-SC"))) != 0;
}
static bt_ssc_dev_t *dev(void) { return &bt_auto_devices[bt_auto_device_slot_locked(pcm_path, true)].ssc; }
static void expire_buds_wait(void) {
    pthread_mutex_lock(&bt_buds_state_mutex);
    bt_buds_wait_started = bt_control_monotonic_ms() - BT_BUDS_SSC_DISCOVERY_GRACE_MS - 10;
    pthread_mutex_unlock(&bt_buds_state_mutex);
}


#endif /* BT_SSC_FAKE_H */
