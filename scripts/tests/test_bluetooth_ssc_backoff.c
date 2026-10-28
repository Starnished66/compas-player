/* Host tests for SSC recognition limits, the persisted backoff, the
 * switch-drop cycle cap and the recovery bookkeeping. */
#include "bt_ssc_fake.h"

static void drop_at(uint32_t removed_at) {
    bt_auto_drop_confirmed(pcm_path, removed_at);
    bluealsa_clear_codec_path(pcm_path);
}

static void restart_player(void) { /* only the persisted memory remains */
    pthread_mutex_lock(&bt_auto_devices_mutex);
    memset(bt_auto_devices, 0, sizeof(bt_auto_devices));
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    bluealsa_clear_codec_path(pcm_path);
}

static bt_ssc_dev_t *slot(void) {
    return &bt_auto_devices[bt_auto_device_slot_locked(pcm_path, true)].ssc;
}

static int memory_skip(void) {
    bt_ssc_memory_t m;
    return bt_ssc_memory_get(MAC, &m) ? m.skip : -1;
}

static void test_name_only_waits_but_never_recovers_in_automatic(void) {
    setup("auto", "AAC SBC", "AAC");
    snprintf(bluez_alias, sizeof(bluez_alias), "Pixel Buds Pro");
    uint32_t defer = 0;
    assert(!apply(&defer) && defer != 0); /* the wait for the endpoint */
    expire_buds_wait();
    char mac[18];
    assert(apply(&defer) && ssc_selects == 0); /* AAC, no recovery */
    assert(!bt_control_ssc_discovery_recovery_peek(mac, sizeof(mac)));
    bt_ssc_memory_t out[BT_SSC_MEMORY_MAX];
    int n = 0;
    assert(!bt_ssc_memory_export_if_dirty(out, &n)); /* no failure recorded */
}

static void test_other_vendor_modalias_rules_the_name_out(void) {
    setup("auto", "AAC SBC", "AAC");
    snprintf(bluez_alias, sizeof(bluez_alias), "Galaxy Buds Clone");
    snprintf(bluez_modalias, sizeof(bluez_modalias), "bluetooth:v004Cp1234d0001");
    uint32_t defer = 0;
    assert(apply(&defer) && defer == 0 && ssc_selects == 0); /* no wait, AAC */
}

static void test_multipoint_select_failure_is_session_only(void) {
    setup("auto", "samsung-SC AAC SBC", "AAC");
    fail_codec = true; /* the SSC endpoint is busy with a phone */
    assert(!apply(NULL) && ssc_rejected());
    fail_codec = false;
    assert(apply(NULL) && ssc_selects == 1);
    restart_player();
    assert(memory_skip() == -1); /* nothing persisted */
    assert(apply(NULL) && ssc_selects == 2); /* SSC tried again next time */
}

static void test_blamed_drops_back_off_automatic_only(void) {
    setup("auto", "samsung-SC AAC SBC", "AAC");
    for (int cycle = 0; cycle < 2; cycle++) { /* a worked device backs off at the second blame */
        info_selected[0] = 'A';
        assert(apply(NULL));
        bluez_connected = false;
        drop_at(bt_control_monotonic_ms());
        bluez_connected = true;
        restart_player();
    }
    assert(ssc_selects == 2 && memory_skip() == 1);
    snprintf(info_selected, sizeof(info_selected), "SBC");
    assert(apply(NULL) && ssc_selects == 2 && strcmp(info_selected, "AAC") == 0); /* skipped */
    assert(memory_skip() == 0);
    restart_player();
    snprintf(info_selected, sizeof(info_selected), "SBC");
    assert(apply(NULL) && ssc_selects == 3); /* tried again */
    /* Explicit SSC ignores the persisted skip. */
    bt_ssc_memory_note_failure(MAC);
    bt_ssc_memory_note_failure(MAC);
    assert(memory_skip() > 0);
    restart_player();
    bt_control_restore_codec_preference("ssc");
    snprintf(info_selected, sizeof(info_selected), "SBC");
    assert(apply(NULL) && ssc_selects == 4);
}

static void test_flowing_ssc_lifts_the_backoff_while_playing(void) {
    setup("auto", "samsung-SC AAC SBC", "AAC");
    bt_ssc_memory_note_worked(MAC);
    bt_ssc_memory_note_failure(MAC);
    bt_ssc_memory_note_failure(MAC);
    assert(bt_ssc_memory_connection_begin(MAC)); /* the skipped connection */
    assert(apply(NULL) && ssc_selects == 1);
    bt_auto_flowing_tick();
    assert(memory_skip() >= 0);
    slot()->confirmed -= BT_SSC_FLOWING_MS + 100;
    bt_auto_flowing_tick();
    bt_ssc_memory_t out[BT_SSC_MEMORY_MAX];
    int n = 0;
    assert(bt_ssc_memory_export_if_dirty(out, &n) && out[0].failures == 0 && out[0].skip == 0);
}

static void test_cycle_cap_rejects_without_link_state(void) {
    setup("auto", "samsung-SC AAC SBC", "AAC");
    assert(apply(NULL));
    drop_at(bt_control_monotonic_ms()); /* BlueZ shows the device connected again */
    assert(!ssc_rejected());
    info_selected[0] = 'A';
    assert(apply(NULL));
    drop_at(bt_control_monotonic_ms());
    assert(ssc_rejected()); /* second switch-then-drop in this session */
    bt_ssc_memory_t out[BT_SSC_MEMORY_MAX];
    int n = 0;
    assert(!bt_ssc_memory_export_if_dirty(out, &n) || n == 1); /* only "worked" at most */
}

static void test_flowing_is_judged_at_removal_time(void) {
    setup("auto", "samsung-SC AAC SBC", "AAC");
    assert(apply(NULL));
    uint32_t confirmed = slot()->confirmed;
    slot()->confirmed = confirmed - 7000;
    bluez_connected = false;
    drop_at(confirmed - 7000 + 1000); /* removed 1 s in, published much later */
    assert(ssc_rejected());
}

static void test_watch_stop_with_a_removal_pending(void) {
    setup("ssc", "samsung-SC AAC", "AAC");
    hide_readback = true;
    assert(!apply(NULL));
    char mac[18];
    assert(bt_ssc_discovery_recovery_queue(pcm_path));
    bluez_connected = false;
    bt_output_disconnect_finish_pending(pcm_path, bt_control_monotonic_ms());
    assert(ssc_rejected());
    assert(!bt_control_ssc_discovery_recovery_peek(mac, sizeof(mac)));
    assert(!bt_ssc_dev_flowing(slot(), bt_control_monotonic_ms()) && !slot()->conn_started);
    bt_output_disconnect_finish_pending("", 0); /* nothing pending: no effect */
}

static void test_own_reconnect_is_not_a_new_connection(void) {
    setup("auto", "samsung-SC AAC SBC", "AAC");
    bt_ssc_memory_note_failure(MAC);
    bt_ssc_memory_note_failure(MAC);
    int before = memory_skip();
    assert(before > 1);
    assert(apply(NULL) && memory_skip() == before - 1); /* this connection consumed one */
    bt_auto_expect_reconnect(pcm_path);
    drop_at(bt_control_monotonic_ms());
    assert(apply(NULL) && memory_skip() == before - 1); /* the recovery PCM did not */
    drop_at(bt_control_monotonic_ms()); /* a real new connection */
    assert(apply(NULL) && memory_skip() == before - 2);
}

static void test_memory_eviction_keeps_worked_and_recent(void) {
    bt_ssc_memory_reset();
    char mac[18];
    bt_ssc_memory_note_worked("00:00:00:00:00:01");
    for (int i = 2; i <= BT_SSC_MEMORY_MAX; i++) {
        snprintf(mac, sizeof(mac), "00:00:00:00:00:%02d", i);
        bt_ssc_memory_note_failure(mac);
    }
    assert(bt_ssc_memory_worked("00:00:00:00:00:01"));
    bt_ssc_memory_note_failure("00:00:00:00:00:99"); /* evicts the oldest never-worked one */
    assert(bt_ssc_memory_worked("00:00:00:00:00:01"));
    bt_ssc_memory_t out[BT_SSC_MEMORY_MAX];
    int n = 0;
    assert(bt_ssc_memory_export_if_dirty(out, &n) && n == BT_SSC_MEMORY_MAX);
    for (int i = 0; i < n; i++) assert(strcmp(out[i].mac, "00:00:00:00:00:02") != 0);
}

static bool is_cancelled_flag;
static void cancel_as_user_disconnect(void) {
    atomic_fetch_add(&bt_user_disconnect_generation, 1);
    is_cancelled_flag = true; /* the GUI cancels the job on a user disconnect */
}
static void cancel_only(void) { is_cancelled_flag = true; }
static bool cancelled(void *ctx) { return *(bool *)ctx; }

static void test_cancelled_recovery_still_reconnects_the_profile(void) {
    setup("ssc", "AAC SBC", "AAC");
    snprintf(bluez_alias, sizeof(bluez_alias), "Galaxy Buds2 Pro");
    is_cancelled_flag = false;
    on_profile_disconnect = cancel_only;
    (void) bt_control_reconnect_for_ssc_discovery(MAC, 44100, cancelled, &is_cancelled_flag);
    assert(profile_disconnects == 1 && profile_connects == 1 && full_disconnects == 0);
    /* A user's own disconnect does not get its profile put back. */
    setup("ssc", "AAC SBC", "AAC");
    snprintf(bluez_alias, sizeof(bluez_alias), "Galaxy Buds2 Pro");
    is_cancelled_flag = false;
    on_profile_disconnect = cancel_as_user_disconnect;
    assert(!bt_control_reconnect_for_ssc_discovery(MAC, 44100, cancelled, &is_cancelled_flag));
    assert(profile_disconnects == 1 && profile_connects == 0 && full_disconnects == 0);
}

int main(void) {
    test_name_only_waits_but_never_recovers_in_automatic();
    test_other_vendor_modalias_rules_the_name_out();
    test_multipoint_select_failure_is_session_only();
    test_blamed_drops_back_off_automatic_only();
    test_flowing_ssc_lifts_the_backoff_while_playing();
    test_cycle_cap_rejects_without_link_state();
    test_flowing_is_judged_at_removal_time();
    test_watch_stop_with_a_removal_pending();
    test_own_reconnect_is_not_a_new_connection();
    test_memory_eviction_keeps_worked_and_recent();
    test_cancelled_recovery_still_reconnects_the_profile();
    puts("Bluetooth SSC backoff host tests passed");
    return 0;
}
