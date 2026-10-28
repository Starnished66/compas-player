/* Host tests: a stream already on SSC is never treated as unavailable, codec
 * commands wait for an open stream, the recovery does not depend on the top
 * bar, and device lookups avoid bluetoothctl. */
#include "bt_ssc_fake.h"

static void samsung_buds(void) {
    snprintf(bluez_alias, sizeof(bluez_alias), "Galaxy Buds2 Pro");
    snprintf(bluez_modalias, sizeof(bluez_modalias), "bluetooth:v0075pA013d0001");
}

static void test_offer_selected_unit(void) {
    char info[256] = "Available codecs: AAC SBC\nSelected codec: samsung-SC\n";
    assert(!bt_ssc_policy_endpoint_offered(info));
    bt_ssc_policy_offer_selected(info, sizeof(info));
    assert(bt_ssc_policy_endpoint_offered(info) && strstr(info, "Selected codec: samsung-SC"));
    char again[256] = "Available codecs: AAC SBC\nSelected codec: AAC\n";
    bt_ssc_policy_offer_selected(again, sizeof(again));
    assert(strcmp(again, "Available codecs: AAC SBC\nSelected codec: AAC\n") == 0);
    char unknown[64] = "Selected codec: AAC\n"; /* codecs not known yet: untouched */
    bt_ssc_policy_offer_selected(unknown, sizeof(unknown));
    assert(strcmp(unknown, "Selected codec: AAC\n") == 0);
}

static void test_live_ssc_stream_is_neither_recovered_nor_replaced(void) {
    const char *modes[] = { "ssc", "auto" };
    for (int m = 0; m < 2; m++) {
        setup(modes[m], "AAC SBC", "samsung-SC"); /* BlueZ skipped the in-use SEP */
        samsung_buds();
        expire_buds_wait();
        assert(apply(NULL));
        char mac[18];
        assert(!bt_control_ssc_discovery_recovery_peek(mac, sizeof(mac)));
        assert(codec_selects == 0 && strcmp(info_selected, "samsung-SC") == 0);
    }
    /* The recovery guard sees the same: nothing to reconnect. */
    setup("ssc", "AAC SBC", "samsung-SC");
    samsung_buds();
    bool is_cancelled = false;
    assert(bt_control_reconnect_for_ssc_discovery(MAC, 44100, NULL, &is_cancelled));
    assert(profile_disconnects == 0 && full_disconnects == 0);
    /* Any codec in use counts, not only SSC. */
    setup("auto", "AAC SBC", "aptX");
    assert(apply(NULL) && codec_selects == 0);
}

static void test_codec_command_waits_for_recovery_and_open_stream(void) {
    setup("auto", "samsung-SC AAC SBC", "AAC");
    uint32_t defer = 0;
    assert(bt_ssc_discovery_recovery_queue(pcm_path));
    assert(bt_control_ssc_discovery_recovery_take(MAC)); /* the recovery owns the link */
    assert(!apply(&defer) && defer != 0 && codec_selects == 0);
    bt_control_ssc_discovery_recovery_finish(MAC);
    bt_auto_pcm_added(pcm_path); /* the PCM is brand new */
    defer = 0;
    uint32_t before = bt_control_monotonic_ms();
    assert(!apply(&defer) && codec_selects == 0);
    assert((int32_t)(defer - before) > 500 && (int32_t)(defer - before) <= (int32_t) BT_SSC_STREAM_OPEN_MS + 50);
    dev()->pcm_added_at -= BT_SSC_STREAM_OPEN_MS + 100; /* an open stream */
    assert(apply(&defer) && ssc_selects == 1);
}

static void test_recovery_does_not_need_the_top_bar(void) {
    /* Screen off: the top bar never filled in its cached MAC. */
    assert(bt_ssc_policy_recovery_verdict("ssc", "", MAC) == BT_SSC_RECOVERY_START);
    assert(bt_ssc_policy_recovery_verdict("auto", "", MAC) == BT_SSC_RECOVERY_START);
    assert(bt_ssc_policy_recovery_verdict("auto", MAC, MAC) == BT_SSC_RECOVERY_START);
    assert(bt_ssc_policy_recovery_verdict("auto", "11:22:33:44:55:66", MAC) == BT_SSC_RECOVERY_CANCEL);
    assert(bt_ssc_policy_recovery_verdict("aac", "", MAC) == BT_SSC_RECOVERY_CANCEL);
}

static void test_device_lookup_avoids_bluetoothctl(void) {
    setup("auto", "AAC SBC", "AAC");
    samsung_buds();
    uint32_t defer = 0;
    (void) apply(&defer);
    assert(device_info_calls >= 1 && bluetoothctl_infos == 0);
    char info[256];
    bt_ssc_policy_dbus_to_info("string \"Connected\"\n variant boolean false\n", info, sizeof(info));
    assert(bt_ssc_policy_link_state(info) == BT_SSC_LINK_DISCONNECTED);
}

static void test_persisted_skip_keeps_a_live_ssc_stream(void) {
    setup("auto", "AAC SBC", "samsung-SC"); /* the Buds opened SSC themselves */
    bt_ssc_memory_note_failure(MAC);
    bt_ssc_memory_note_failure(MAC); /* persisted backoff: skip > 0 */
    assert(apply(NULL) && codec_selects == 0 && strcmp(info_selected, "samsung-SC") == 0);
    /* A rejection this session still moves off SSC. */
    setup("auto", "AAC SBC", "samsung-SC");
    bt_auto_reject(pcm_path, "samsung-SC");
    assert(apply(NULL) && strcmp(info_selected, "AAC") == 0);
}

static void test_unknown_pcm_age_and_rate_correction_defer(void) {
    setup("auto", "samsung-SC AAC SBC", "AAC");
    uint32_t defer = 0;
    bt_output_watch_started_at = bt_control_monotonic_ms(); /* PCM found by the startup scan */
    assert(!apply(&defer) && defer != 0 && codec_selects == 0);
    bt_output_watch_started_at -= BT_SSC_STREAM_OPEN_MS + 100;
    atomic_store(&bt_ssc_correction_running, 1); /* a rate correction owns the link */
    assert(!apply(&defer) && codec_selects == 0);
    atomic_store(&bt_ssc_correction_running, 0);
    snprintf(bt_ssc_correction_mac, sizeof(bt_ssc_correction_mac), "%s", MAC); /* or has it queued */
    assert(!apply(&defer) && codec_selects == 0);
    bt_control_ssc_rate_correction_cancel();
    assert(apply(&defer) && ssc_selects == 1);
    bt_output_watch_started_at = 0;
}

int main(void) {
    test_persisted_skip_keeps_a_live_ssc_stream();
    test_unknown_pcm_age_and_rate_correction_defer();
    test_offer_selected_unit();
    test_live_ssc_stream_is_neither_recovered_nor_replaced();
    test_codec_command_waits_for_recovery_and_open_stream();
    test_recovery_does_not_need_the_top_bar();
    test_device_lookup_avoids_bluetoothctl();
    puts("Bluetooth SSC stream host tests passed");
    return 0;
}
