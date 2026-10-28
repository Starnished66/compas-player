/* Host tests for the Samsung SSC reliability policy: recognising SSC
 * devices, telling a codec reconfigure from a disconnect, fallback and the
 * profile-only recovery. */
#include "bt_ssc_fake.h"

static void test_switch_is_noted_before_readback(void) {
    setup("ssc", "samsung-SC AAC", "AAC");
    hide_readback = true;
    assert(!apply(NULL));
    assert(bt_auto_devices[bt_auto_device_slot_locked(pcm_path, false)].switched >= 0);
    assert(!ssc_rejected()); /* explicit SSC is not rejected on a missing readback alone */
}

static void test_link_drop_after_switch_without_readback_is_rejected(void) {
    const char *modes[] = { "ssc", "auto" };
    for (int m = 0; m < 2; m++) {
        setup(modes[m], "samsung-SC AAC", "AAC");
        snprintf(bluez_modalias, sizeof(bluez_modalias), "bluetooth:v0075pA013d0001");
        hide_readback = true;
        assert(!apply(NULL));
        bluez_connected = false;
        bt_auto_drop_confirmed(pcm_path, bt_control_monotonic_ms());
        assert(ssc_rejected());
    }
}

static void test_reconfigure_gap_is_not_a_disconnect(void) {
    setup("ssc", "samsung-SC AAC", "AAC");
    hide_readback = true;
    assert(!apply(NULL));
    assert(bt_control_reconfigure_hold_ms() > 1000); /* the GUI keeps the route meanwhile */
    char pending[256] = "";
    uint32_t deadline = 0;
    char added[BT_OUTPUT_DISCONNECT_MAX_ADDED_PATHS][256];
    int added_count = 0;
    bt_output_disconnect_process_line("PCMRemoved ", pending, &deadline, added, &added_count);
    char line[300];
    snprintf(line, sizeof(line), "PCMRemoved %s", pcm_path);
    bt_output_disconnect_process_line(line, pending, &deadline, added, &added_count);
    assert(pending[0]);
    /* The ordinary grace ran out, but the switch is still in flight. */
    deadline = bt_control_monotonic_ms() - 1;
    atomic_store(&bt_output_disconnect_active, true);
    atomic_store(&bt_output_disconnect_flag, false);
    bt_output_disconnect_publish_if_due(pending, &deadline);
    assert(pending[0] && !atomic_load(&bt_output_disconnect_flag));
    assert((int32_t)(deadline - bt_control_monotonic_ms()) > 1000); /* held, up to the 3 s cap */
    /* The PCM returns 2 s in: nothing was a disconnect. */
    snprintf(line, sizeof(line), "PCMAdded %s", pcm_path);
    bt_output_disconnect_process_line(line, pending, &deadline, added, &added_count);
    assert(!pending[0] && !ssc_rejected() && !atomic_load(&bt_output_disconnect_flag));
    assert(bt_control_reconfigure_hold_ms() == 0);
    /* Past the cap with the device still connected: a lost PCM, not the codec. */
    dev()->switch_begin = bt_control_monotonic_ms() - BT_SSC_SWITCH_CAP_MS - 50;
    dev()->pcm_back = 0;
    snprintf(line, sizeof(line), "PCMRemoved %s", pcm_path);
    bt_output_disconnect_process_line(line, pending, &deadline, added, &added_count);
    deadline = bt_control_monotonic_ms() - 1;
    bt_output_disconnect_publish_if_due(pending, &deadline);
    assert(!pending[0] && atomic_load(&bt_output_disconnect_flag));
    assert(!ssc_rejected());
    atomic_store(&bt_output_disconnect_active, false);
}

static void test_reconfigure_deadline_unit(void) {
    bt_ssc_dev_t d = {0};
    assert(bt_ssc_dev_reconfigure_deadline(&d, 1000) == 0);
    bt_ssc_dev_switch_begin(&d, 1000);
    assert(bt_ssc_dev_reconfigure_deadline(&d, 1100) == 1000 + BT_SSC_SWITCH_CAP_MS);
    assert(bt_ssc_dev_reconfigure_deadline(&d, 1000 + BT_SSC_SWITCH_CAP_MS + 1) == 0);
    bt_ssc_dev_pcm_added(&d, 2500);
    assert(bt_ssc_dev_reconfigure_deadline(&d, 2600) == 0);
    bt_ssc_dev_switch_begin(&d, 5000);
    assert(bt_ssc_dev_reconfigure_deadline(&d, 4900) == 0); /* a removal before our switch */
    bt_ssc_dev_confirmed(&d, 5200, true);
    bt_ssc_dev_confirmed(&d, 9000, true); /* "already X" must not restart the flowing clock */
    assert(d.confirmed == 5200 && bt_ssc_dev_flowing(&d, 5200 + BT_SSC_FLOWING_MS));
}

static void test_user_disconnect_is_not_blamed(void) {
    setup("ssc", "samsung-SC AAC", "AAC");
    hide_readback = true;
    assert(!apply(NULL));
    assert(bt_auto_reconfigure_deadline(pcm_path, bt_control_monotonic_ms()) != 0);
    assert(bt_control_disconnect(MAC));
    assert(bt_auto_reconfigure_deadline(pcm_path, bt_control_monotonic_ms()) == 0);
    bt_auto_drop_confirmed(pcm_path, bt_control_monotonic_ms());
    assert(!ssc_rejected());
}

static void test_out_of_range_after_flowing_audio_is_not_blamed(void) {
    setup("ssc", "samsung-SC AAC", "AAC");
    assert(apply(NULL));
    assert(ssc_selects == 1 && dev()->confirmed_ssc);
    bluez_connected = false;
    dev()->confirmed -= BT_SSC_FLOWING_MS + 100; /* flowing for 5 s */
    bt_auto_drop_confirmed(pcm_path, bt_control_monotonic_ms());
    assert(!ssc_rejected());
    /* The same drop 1 s after the switch is blamed. */
    setup("ssc", "samsung-SC AAC", "AAC");
    assert(apply(NULL));
    bluez_connected = false;
    bt_auto_drop_confirmed(pcm_path, bt_control_monotonic_ms());
    assert(ssc_rejected());
}

static void test_detection_signals(void) {
    assert(bt_ssc_policy_endpoint_offered("Available codecs: SBC samsung-SC\n"));
    assert(bt_ssc_policy_endpoint_offered("Available codecs: SBC 0075:0103\n"));
    assert(!bt_ssc_policy_endpoint_offered("Available codecs: SBC AAC\nSelected codec: SSC\n"));
    assert(bt_ssc_policy_modalias_is_samsung("Modalias: bluetooth:v0075pA013d0001\n"));
    assert(!bt_ssc_policy_modalias_is_samsung("Modalias: bluetooth:v004Cp1234d0001\n"));
    assert(!bt_ssc_policy_modalias_is_samsung("Modalias: usb:v0075p1234d0001\n"));
    assert(bt_ssc_policy_signal(true, true, true, true) == BT_SSC_SIGNAL_ENDPOINT);
    assert(bt_ssc_policy_signal(false, true, true, true) == BT_SSC_SIGNAL_MODALIAS);
    assert(bt_ssc_policy_signal(false, false, true, true) == BT_SSC_SIGNAL_REMEMBERED);
    assert(bt_ssc_policy_signal(false, false, false, true) == BT_SSC_SIGNAL_NAME);
    assert(bt_ssc_policy_signal(false, false, false, false) == BT_SSC_SIGNAL_NONE);
    for (int hint = 0; hint < 5; hint++) { /* none, Modalias, remembered, name, endpoint */
        setup("auto", hint == 4 ? "samsung-SC AAC SBC" : "AAC SBC", "AAC");
        if (hint == 1) snprintf(bluez_modalias, sizeof(bluez_modalias), "bluetooth:v0075pA013d0001");
        if (hint == 2) bt_ssc_memory_note_worked(MAC);
        if (hint == 3) snprintf(bluez_alias, sizeof(bluez_alias), "Galaxy Buds2 Pro");
        uint32_t defer = 0;
        bool done = apply(&defer);
        if (hint == 0) assert(done && ssc_selects == 0);          /* generic: AAC at once */
        else if (hint == 4) assert(done && ssc_selects == 1);     /* endpoint: SSC at once */
        else assert(!done && defer != 0 && codec_selects == 0);   /* wait for the endpoint */
    }
}

static void test_automatic_recovery_once_per_connection(void) {
    setup("auto", "AAC SBC", "AAC");
    snprintf(bluez_modalias, sizeof(bluez_modalias), "bluetooth:v0075pA013d0001");
    uint32_t defer = 0;
    assert(!apply(&defer)); /* name lookup, then the grace wait */
    expire_buds_wait();
    char mac[18];
    assert(!apply(&defer) && defer != 0);
    assert(bt_control_ssc_discovery_recovery_peek(mac, sizeof(mac)));
    assert(bt_control_ssc_discovery_recovery_take(MAC));
    bt_control_ssc_discovery_recovery_finish(MAC);
    /* Still no SSC afterwards: no second recovery on this connection, AAC. */
    assert(apply(NULL) && ssc_selects == 0);
    assert(!bt_control_ssc_discovery_recovery_peek(mac, sizeof(mac)));
    assert(!bt_ssc_discovery_recovery_queue(pcm_path));
    /* A new connection gets its own single recovery. */
    bt_auto_drop_confirmed(pcm_path, bt_control_monotonic_ms());
    assert(bt_ssc_discovery_recovery_queue(pcm_path));
    assert(bt_control_ssc_discovery_recovery_peek(mac, sizeof(mac)));
}

static void test_recovery_cancelled_by_confirmed_disconnect(void) {
    setup("auto", "AAC SBC", "AAC");
    char mac[18];
    assert(bt_ssc_discovery_recovery_queue(pcm_path));
    bt_auto_drop_confirmed(pcm_path, bt_control_monotonic_ms());
    assert(!bt_control_ssc_discovery_recovery_peek(mac, sizeof(mac)));
    /* A recovery already running is not cancelled by its own disconnect. */
    assert(bt_ssc_discovery_recovery_queue(pcm_path));
    assert(bt_control_ssc_discovery_recovery_take(MAC));
    bt_auto_drop_confirmed(pcm_path, bt_control_monotonic_ms());
    assert(bt_ssc_discovery_claimed);
    bt_control_ssc_discovery_recovery_finish(MAC);
    assert(!bt_ssc_discovery_recovery_queue(pcm_path)); /* its one attempt is spent */
}

static void test_recovery_wait_is_capped(void) {
    setup("auto", "AAC SBC", "AAC");
    assert(bt_ssc_discovery_recovery_queue(pcm_path));
    assert(bt_ssc_discovery_recovery_queue(pcm_path));
    bt_ssc_discovery_queued_at = bt_control_monotonic_ms() - BT_SSC_RECOVERY_WAIT_CAP_MS - 1;
    assert(!bt_ssc_discovery_recovery_queue(pcm_path));
    char mac[18];
    assert(!bt_control_ssc_discovery_recovery_peek(mac, sizeof(mac)));
}

static void test_automatic_mismatch_falls_back_to_aac_for_the_session(void) {
    setup("auto", "samsung-SC AAC SBC", "AAC");
    forced_selected = "AAC"; /* the Buds settle on AAC after SelectCodec */
    assert(!apply(NULL) && ssc_rejected() && ssc_selects == 1);
    assert(apply(NULL) && ssc_selects == 1); /* AAC, SSC not retried */
    bt_ssc_memory_t out[BT_SSC_MEMORY_MAX];
    int n = 0;
    assert(!bt_ssc_memory_export_if_dirty(out, &n)); /* a mismatch is session-only */
}

static void test_explicit_mismatch_falls_back_after_one_try(void) {
    setup("ssc", "samsung-SC AAC SBC", "SBC");
    forced_selected = "SBC";
    assert(!apply(NULL) && ssc_rejected() && ssc_selects == 1);
    forced_selected = NULL;
    assert(apply(NULL));
    assert(ssc_selects == 1 && strcmp(info_selected, "AAC") == 0);
}

static void test_settings_round_trip(void) {
    player_settings_t a;
    memset(&a, 0, sizeof(a));
    bt_ssc_settings_parse(&a, MAC ",1,2,3");
    bt_ssc_settings_parse(&a, "garbage");
    bt_ssc_settings_parse(&a, "AA:BB:CC:DD:EE:00,2,0,0");
    assert(a.bt_ssc_device_count == 1 && a.bt_ssc_devices[0].worked && a.bt_ssc_devices[0].skip == 3);
    bt_ssc_settings_apply(&a);
    assert(bt_ssc_memory_worked(MAC));
    bt_ssc_memory_note_session_ok(MAC);
    assert(bt_ssc_settings_sync(&a) && a.bt_ssc_devices[0].failures == 0 && a.bt_ssc_devices[0].skip == 0);
    assert(!bt_ssc_settings_sync(&a));
    char buf[128] = "";
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    bt_ssc_settings_write(f, &a);
    fclose(f);
    assert(strcmp(buf, "bt_ssc_device=" MAC ",1,0,0\n") == 0);
}

static bool cancelled(void *ctx) { return *(bool *)ctx; }

static void profile_recovery(bool disconnect_ok, bool connect_ok) {
    setup("ssc", "AAC SBC", "AAC");
    snprintf(bluez_alias, sizeof(bluez_alias), "Galaxy Buds2 Pro");
    profile_disconnect_ok = disconnect_ok;
    profile_connect_ok = connect_ok;
    bool is_cancelled = false;
    assert(bt_control_reconnect_for_ssc_discovery(MAC, 44100, cancelled, &is_cancelled));
}

static void test_recovery_reconnects_only_the_profile(void) {
    profile_recovery(true, true);
    assert(profile_disconnects == 1 && profile_connects == 1);
    assert(full_disconnects == 0 && full_connects == 0);
}

static void test_recovery_falls_back_to_full_reconnect(void) {
    profile_recovery(false, true); /* the profile cannot be disconnected */
    assert(profile_connects == 0 && full_disconnects == 1);
    profile_recovery(true, false); /* the profile cannot be reconnected */
    assert(profile_disconnects == 1 && profile_connects == 1);
    assert(full_disconnects == 1 && full_connects == 1);
}

int main(void) {
    test_switch_is_noted_before_readback();
    test_link_drop_after_switch_without_readback_is_rejected();
    test_reconfigure_gap_is_not_a_disconnect();
    test_reconfigure_deadline_unit();
    test_user_disconnect_is_not_blamed();
    test_out_of_range_after_flowing_audio_is_not_blamed();
    test_detection_signals();
    test_automatic_recovery_once_per_connection();
    test_recovery_cancelled_by_confirmed_disconnect();
    test_recovery_wait_is_capped();
    test_automatic_mismatch_falls_back_to_aac_for_the_session();
    test_explicit_mismatch_falls_back_after_one_try();
    test_settings_round_trip();
    test_recovery_reconnects_only_the_profile();
    test_recovery_falls_back_to_full_reconnect();
    puts("Bluetooth SSC host tests passed");
    return 0;
}
