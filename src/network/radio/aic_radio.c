#include "aic_radio.h"
#include "aic_radio_policy.h"
#include "../../core/process/subprocess.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The host test (scripts/tests/test_aic_radio.c) points every path at a scratch tree. */
#ifndef AIC_RADIO_ROOT
#define AIC_RADIO_ROOT ""
#endif
#define AIC_TYPE_PATH AIC_RADIO_ROOT "/run/r1_wireless_type"
#define AIC_LOADED_PATH AIC_RADIO_ROOT "/run/r1_wireless_loaded"
#define AIC_BT_GATE_PATH AIC_RADIO_ROOT "/run/aic_bt_power_gate"
#define AIC_HCI0_PATH AIC_RADIO_ROOT "/sys/class/bluetooth/hci0"
#define AIC_FDRV_PATH AIC_RADIO_ROOT "/sys/module/aic8800_fdrv"
#define AIC_WPA_SOCKET_PATH AIC_RADIO_ROOT "/var/run/wpa_supplicant/wlan0"
#define AIC_WIFI_ON_SCRIPT AIC_RADIO_ROOT "/usr/bin/wifi_on.sh"
#define AIC_WIFI_OFF_SCRIPT AIC_RADIO_ROOT "/usr/bin/wifi_off.sh"
#define AIC_BT_ENABLE_SCRIPT AIC_RADIO_ROOT "/usr/bin/bt_enable_aic.sh"
#define AIC_BT_DISABLE_SCRIPT AIC_RADIO_ROOT "/usr/bin/bt_disable"

/* The background load of aic8800_bsp and aic8800_btlpm downloads firmware for
 * about seven seconds after boot; HiBy's own scripts give up after ten. */
#define AIC_LOADED_WAIT_MS 40000
#define AIC_POLL_MS 100

/* One radio transition at a time. The vendor scripts also serialise on
 * /run/aic_radio.lock, but the arbitration decision and the script it leads
 * to must not interleave with another thread's. */
static pthread_mutex_t aic_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_int aic_notice = AIC_RADIO_NOTICE_NONE;

/* Reads a small /run file. Returns false when it cannot be opened. */
static bool read_small_file(const char * path, char * buf, size_t cap) {
    FILE * f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
    fclose(f);
    return true;
}

bool aic_radio_active(void) {
    static atomic_int cached = -1; /* -1 unknown, 0 not AIC, 1 AIC */
    int value = atomic_load(&cached);
    if (value >= 0) return value == 1;
    char buf[32];
    if (!read_small_file(AIC_TYPE_PATH, buf, sizeof(buf))) return false; /* boot not far enough: Broadcom behaviour, retry later */
    value = aic_radio_type_is_aic(buf) ? 1 : 0;
    atomic_store(&cached, value);
    return value == 1;
}

static aic_base_state_t read_base_state(void) {
    char buf[32];
    return aic_radio_parse_base(read_small_file(AIC_LOADED_PATH, buf, sizeof(buf)) ? buf : NULL);
}

bool aic_radio_boot_settled(void) {
    return aic_radio_active() && read_base_state() != AIC_BASE_PENDING;
}

/* Blocks until the boot-time driver load has finished. */
static void wait_for_base(void) {
    for (int waited = 0; waited < AIC_LOADED_WAIT_MS && read_base_state() == AIC_BASE_PENDING; waited += AIC_POLL_MS)
        usleep(AIC_POLL_MS * 1000);
}

static aic_radio_state_t snapshot(void) {
    aic_radio_state_t state;
    state.base = read_base_state();
    state.bt_active = access(AIC_BT_GATE_PATH, F_OK) == 0 || access(AIC_HCI0_PATH, F_OK) == 0;
    state.wifi_active = access(AIC_FDRV_PATH, F_OK) == 0 || access(AIC_WPA_SOCKET_PATH, F_OK) == 0;
    return state;
}

static bool run_script(const char * path) {
    char * argv[] = { (char *) path, NULL };
    int status = -1;
    return subprocess_run_checked_group(argv, NULL, 0, SUBPROCESS_RADIO_TIMEOUT_MS, &status) && status == 0;
}

static void note_refusal(aic_radio_decision_t decision) {
    if (decision == AIC_RADIO_REFUSE_BT_ACTIVE) atomic_store(&aic_notice, AIC_RADIO_NOTICE_WIFI_BLOCKED_BY_BT);
    else if (decision == AIC_RADIO_REFUSE_WIFI_ACTIVE) atomic_store(&aic_notice, AIC_RADIO_NOTICE_BT_BLOCKED_BY_WIFI);
}

/* Drops the Bluetooth gate once the controller is really gone, so Wi-Fi can
 * load again. A controller that is still present keeps the gate. */
static void release_bt_gate_if_off(bool script_ok) {
    if (script_ok || access(AIC_HCI0_PATH, F_OK) != 0) unlink(AIC_BT_GATE_PATH);
}

/* Tears the controller down with bt_disable and releases the gate when it is gone. A failed bring-up
 * can leave hci0 present but unpowered; without this the gate would refuse Wi-Fi until the next
 * successful Bluetooth toggle or reboot. */
static void teardown_bt_and_release_gate(void) {
    release_bt_gate_if_off(run_script(AIC_BT_DISABLE_SCRIPT));
}

bool aic_radio_wifi_enable(void) {
    atomic_store(&aic_notice, AIC_RADIO_NOTICE_NONE); /* a refusal from an earlier call must not colour this toggle */
    wait_for_base();
    pthread_mutex_lock(&aic_mutex);
    aic_radio_state_t state = snapshot();
    aic_radio_decision_t decision = aic_radio_decide(&state, AIC_RADIO_WIFI_ON);
    bool ok = false;
    if (decision == AIC_RADIO_RUN) {
        ok = run_script(AIC_WIFI_ON_SCRIPT);
        if (ok) atomic_store(&aic_notice, AIC_RADIO_NOTICE_NONE);
    } else {
        note_refusal(decision);
    }
    pthread_mutex_unlock(&aic_mutex);
    return ok;
}

void aic_radio_wifi_disable(void) {
    atomic_store(&aic_notice, AIC_RADIO_NOTICE_NONE);
    pthread_mutex_lock(&aic_mutex);
    (void) run_script(AIC_WIFI_OFF_SCRIPT);
    pthread_mutex_unlock(&aic_mutex);
}

bool aic_radio_bt_power_on(void) {
    atomic_store(&aic_notice, AIC_RADIO_NOTICE_NONE);
    wait_for_base();
    pthread_mutex_lock(&aic_mutex);
    aic_radio_state_t state = snapshot();
    aic_radio_decision_t decision = aic_radio_decide(&state, AIC_RADIO_BT_ON);
    bool ok = false;
    if (decision == AIC_RADIO_RUN) {
        /* The gate is what makes the v2.0 scripts treat Bluetooth as the owner of the chip. */
        FILE * gate = fopen(AIC_BT_GATE_PATH, "w");
        if (gate) {
            fputs("1\n", gate);
            fclose(gate);
            ok = run_script(AIC_BT_ENABLE_SCRIPT);
            if (ok) atomic_store(&aic_notice, AIC_RADIO_NOTICE_NONE);
            else teardown_bt_and_release_gate();
        }
    } else {
        note_refusal(decision);
    }
    pthread_mutex_unlock(&aic_mutex);
    return ok;
}

void aic_radio_bt_power_off(void) {
    atomic_store(&aic_notice, AIC_RADIO_NOTICE_NONE);
    pthread_mutex_lock(&aic_mutex);
    teardown_bt_and_release_gate();
    pthread_mutex_unlock(&aic_mutex);
}

bool aic_radio_suspend_prepare(void) {
    pthread_mutex_lock(&aic_mutex);
    if (access(AIC_FDRV_PATH, F_OK) == 0) (void) run_script(AIC_WIFI_OFF_SCRIPT);
    bool bt_disabled = access(AIC_HCI0_PATH, F_OK) == 0 && run_script(AIC_BT_DISABLE_SCRIPT);
    /* bt_suspend (run just before) may already have removed hci0, so the gate is released whenever the
     * controller is confirmed gone, not only after our own bt_disable. */
    release_bt_gate_if_off(bt_disabled);
    bool safe = aic_radio_suspend_safe(access(AIC_HCI0_PATH, F_OK) == 0, access(AIC_FDRV_PATH, F_OK) == 0);
    pthread_mutex_unlock(&aic_mutex);
    return safe;
}

aic_radio_notice_t aic_radio_take_notice(void) {
    return (aic_radio_notice_t) atomic_exchange(&aic_notice, AIC_RADIO_NOTICE_NONE);
}
