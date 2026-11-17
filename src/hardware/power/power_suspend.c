#include "power_suspend.h"
#include "battery.h"
#include "charge_limiter.h"
#include "../../network/bluetooth/control/bluetooth_control.h"
#include "../../core/system/logging/db_log.h"
#include "../../core/process/subprocess.h"
#include "../../core/persistence/settings.h"
#include "../../network/wifi/wifi_control.h"
#include "../../network/wifi/wifi_dhcp.h"
#include "../../network/radio/aic_radio.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Monotonic timestamp in milliseconds. Uses CLOCK_MONOTONIC to align with
 * hw_buttons timestamps on the same timeline. */
static uint32_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Persist phase markers only when the user has enabled the runtime
 * diagnostic log. Suspend is infrequent, so syncing each marker is a small
 * bounded cost and leaves the last reached stage on storage after a hang. */
#define SUSPEND_LOG(fmt, ...) \
    do { \
        if (db_log_enabled()) { \
            db_log("power_suspend", fmt, ##__VA_ARGS__); \
            (void) db_log_flush(); \
        } \
    } while (0)

static void write_sysfs(const char * path, const char * value) {
    FILE * f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s", value);
    fclose(f);
}

/* Writes a sysfs value and reports success/failure.
 * Used for /sys/power/state to detect if the write itself failed. */
static bool write_sysfs_checked(const char * path, const char * value, int * error_out) {
    int write_error = 0;
    FILE * f = fopen(path, "w");
    if (!f) {
        write_error = errno;
        if (error_out) *error_out = write_error;
        return false;
    }
    bool ok = fprintf(f, "%s", value) >= 0;
    if (!ok) write_error = errno ? errno : EIO;
    if (fclose(f) != 0 && ok) {
        write_error = errno ? errno : EIO;
        ok = false;
    }
    if (error_out) *error_out = write_error;
    return ok;
}

/* CLOCK_MONOTONIC does not advance across Linux suspend-to-RAM.
 * CLOCK_BOOTTIME is used to measure elapsed time across suspend. */
static clockid_t suspend_duration_clock_id(void) {
    static clockid_t cached = CLOCK_MONOTONIC;
    static bool probed = false;
    if (!probed) {
        struct timespec ts;
        cached = (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) ? CLOCK_BOOTTIME : CLOCK_MONOTONIC;
        probed = true;
    }
    return cached;
}

static uint32_t suspend_duration_ms_now(void) {
    struct timespec ts;
    clock_gettime(suspend_duration_clock_id(), &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Logs elapsed suspend time and the last wake IRQ when runtime logging is
 * enabled. */
static void log_suspend_diagnostics(uint32_t slept_ms, bool suspend_write_ok, int write_errno) {
    static unsigned int suspend_cycle_count;
    ++suspend_cycle_count;
    char wakeup_irq[32] = "";
    FILE * f = fopen("/sys/power/pm_wakeup_irq", "r");
    if (f) {
        if (!fgets(wakeup_irq, sizeof(wakeup_irq), f)) wakeup_irq[0] = '\0';
        fclose(f);
    }
    size_t irq_len = strlen(wakeup_irq);
    if (irq_len > 0 && wakeup_irq[irq_len - 1] == '\n') wakeup_irq[irq_len - 1] = '\0';
    SUSPEND_LOG("cycle=%u returned slept_ms=%u write_ok=%d errno=%d wake_irq=%s",
                suspend_cycle_count, slept_ms, suspend_write_ok, write_errno,
                wakeup_irq[0] ? wakeup_irq : "(unavailable)");
}

/* Wi-Fi comes back through the shell's toggle worker (gui_shell_poll picks
 * this up), so a tap during the restore cannot start a second wifi_on.sh. */
static atomic_bool wifi_restore_pending;
/* Set when a Bluetooth restore is queued, cleared once it has finished. The shell cannot see
 * this detached work, so its own Bluetooth restore waits on it. */
static atomic_bool bt_restore_pending;
static bool woke_on_external_power;

#ifndef AXP_WAKE_ARMED_FILE
#define AXP_WAKE_ARMED_FILE "/sys/module/axp2101_wake/parameters/armed"
#endif

/* True only while VBUS-insert wake is armed. The module sets the parameter
 * after arming and clears it when it stops the wake (storm, I2C give-up,
 * exit). A missing file means the module is not loaded. Directory presence
 * is not enough: a resident module can be disarmed. One open, no wait. */
static bool axp2101_wake_is_armed(void) {
    FILE *f = fopen(AXP_WAKE_ARMED_FILE, "r");
    if (!f) return false;
    int value = 0;
    int got = fscanf(f, "%d", &value);
    fclose(f);
    return got == 1 && value == 1;
}

bool power_suspend_take_wifi_restore(void) {
    return atomic_exchange(&wifi_restore_pending, false);
}

bool power_suspend_bt_restore_pending(void) {
    return atomic_load(&bt_restore_pending);
}

bool power_suspend_woke_on_external_power(void) {
    return woke_on_external_power;
}

static void radio_restore_perform(bool wifi_was_on, bool bt_was_on) {
    if (bt_was_on) {
        bt_control_init_chip();
        bt_control_enable();
        atomic_store(&bt_restore_pending, false);
    }
    if (wifi_was_on) atomic_store(&wifi_restore_pending, true);
}

/* Restores Bluetooth and Wi-Fi in a detached thread after resume so that
 * the caller and display unblank are not blocked by radio re-initialization. */
static void * radio_restore_thread_func(void * arg) {
    uintptr_t flags = (uintptr_t) arg;
    bool wifi_was_on = flags & 1;
    bool bt_was_on = flags & 2;
    radio_restore_perform(wifi_was_on, bt_was_on);
    return NULL;
}

/* Negative means completion is unknown (spawn failure, signal or timeout).
 * A timed-out parent may leave an in-flight vendor child; keep the gate shut. */
#define RADIO_UI_ACTION_TIMEOUT_MS 5000
static int experimental_radio_action(const char * action, int timeout_ms) {
    int status = -1;
    char * argv[] = { (char *) "/usr/bin/compas-radio", (char *) action, NULL };
    return subprocess_run_checked_group(argv, NULL, 0, timeout_ms, &status) ? status : -1;
}

static void queue_radio_restore(bool wifi_was_on, bool bt_was_on) {
    if (!bt_was_on && !wifi_was_on) return;
    if (bt_was_on) atomic_store(&bt_restore_pending, true);
    uintptr_t flags = (wifi_was_on ? 1u : 0u) | (bt_was_on ? 2u : 0u);
    pthread_t restore_thread;
    if (pthread_create(&restore_thread, NULL, radio_restore_thread_func, (void *)(uintptr_t) flags) == 0) {
        pthread_detach(restore_thread);
    } else {
        radio_restore_perform(wifi_was_on, bt_was_on);
    }
}

/* Background recovery has the full radio budget; the synchronous UI path
 * cancels preparation after five seconds and lets the GUI regain input.
 * Retry a fresh serialized shutdown before opening a gate left by uncertain
 * completion. This also waits for a surviving transaction to release its lock.
 * Do not restore preferences until both shutdown and gate release succeed. */
static void * radio_recovery_thread_func(void * arg) {
    uintptr_t flags = (uintptr_t) arg;
    for (int attempt = 0; attempt < 3; ++attempt) {
        usleep(1000000);
        if (((flags & 4u) || experimental_radio_action("suspend-prepare", SUBPROCESS_RADIO_TIMEOUT_MS) == 0) &&
            experimental_radio_action("suspend-end", SUBPROCESS_RADIO_TIMEOUT_MS) == 0) {
            radio_restore_perform(flags & 1u, flags & 2u);
            return NULL;
        }
    }
    SUSPEND_LOG("radio recovery failed: suspend gate remains closed");
    atomic_store(&bt_restore_pending, false); /* nothing will restore it: do not hold the shell */
    return NULL;
}

static void queue_radio_recovery(bool wifi_was_on, bool bt_was_on, bool end_only) {
    if (bt_was_on) atomic_store(&bt_restore_pending, true);
    uintptr_t flags = (wifi_was_on ? 1u : 0u) | (bt_was_on ? 2u : 0u) | (end_only ? 4u : 0u);
    pthread_t thread;
    if (pthread_create(&thread, NULL, radio_recovery_thread_func, (void *) flags) == 0)
        pthread_detach(thread);
    else {
        SUSPEND_LOG("radio recovery unavailable: suspend gate remains closed");
        atomic_store(&bt_restore_pending, false);
    }
}

void power_suspend_now(void) {
    woke_on_external_power = false;
    /* Recheck at the suspend boundary, independently of the UI idle gate.
     * An uncertain sample must never authorize sleeping on external power. */
    if (battery_get_external_power_state() != BATTERY_EXTERNAL_POWER_DISCONNECTED) {
        SUSPEND_LOG("blocked: external power connected or unknown");
        return;
    }
    settings_save_drain(); /* queued toggle changes must be on storage before sleeping */
    const bool diagnostics_enabled = db_log_enabled();
    bool wifi_was_on = wifi_control_is_enabled();
    bool bt_was_on = bt_control_is_powered();
    SUSPEND_LOG("begin wifi_was_on=%d bt_was_on=%d", wifi_was_on, bt_was_on);

    const bool experimental_radio = access("/sys/devices/platform/bcm_wlbt_power/identity_state", F_OK) == 0;
    if (experimental_radio) {
        /* The backend blocks new bringups before stopping both physical radios.
         * A completed child is insufficient: require its actual exit status. */
        if (bt_was_on) bt_control_disable(); /* Graceful D-Bus link shutdown. */
        if (wifi_was_on) wifi_dhcp_stop(); /* the backend kills udhcpc; reap ours first */
        int prepare_status = experimental_radio_action("suspend-prepare", RADIO_UI_ACTION_TIMEOUT_MS);
        if (prepare_status != 0) {
            SUSPEND_LOG("cancelled: experimental radio shutdown failed");
            if (prepare_status < 0) {
                SUSPEND_LOG("radio restore deferred: shutdown completion unknown");
                queue_radio_recovery(wifi_was_on, bt_was_on, false);
                return;
            }
            if (experimental_radio_action("suspend-end", RADIO_UI_ACTION_TIMEOUT_MS) == 0) {
                queue_radio_restore(wifi_was_on, bt_was_on);
            } else {
                SUSPEND_LOG("radio restore deferred: suspend-end failed");
                queue_radio_recovery(wifi_was_on, bt_was_on, true);
            }
            return;
        }
    } else {
        if (wifi_was_on) {
            SUSPEND_LOG("wifi_disable begin");
            wifi_control_disable();
        }
        /* Gracefully disconnect active links before raw vendor shutdown. */
        if (bt_was_on) {
            SUSPEND_LOG("bluetooth_disable begin");
            bt_control_disable();
        }
        char * bt_suspend_argv[] = { (char *) "/usr/bin/bt_suspend", NULL };
        SUSPEND_LOG("bt_suspend begin");
        subprocess_run(bt_suspend_argv, NULL, 0);
        if (aic_radio_active() && !aic_radio_suspend_prepare()) {
            /* AIC8800D40 must have hci0 and aic8800_fdrv gone before sleeping,
             * as in the stock player. Stay awake and bring back what was on. */
            SUSPEND_LOG("cancelled: AIC radio shutdown incomplete");
            queue_radio_restore(wifi_was_on, bt_was_on);
            return;
        }
    }

    SUSPEND_LOG("framebuffer_blank begin");
    write_sysfs("/sys/class/graphics/fb0/blank", "4"); /* FB_BLANK_POWERDOWN */

    /* USB can be inserted while radio shutdown is in progress. In that
     * case unwind the preparation below without writing the sleep request. */
    if (battery_get_external_power_state() == BATTERY_EXTERNAL_POWER_DISCONNECTED) {
        SUSPEND_LOG("mem_write begin monotonic_ms=%u", monotonic_ms());
        uint32_t sleep_start_ms = diagnostics_enabled ? suspend_duration_ms_now() : 0;
        int suspend_write_errno = 0;
        bool suspend_write_ok = write_sysfs_checked("/sys/power/state", "mem", &suspend_write_errno);
        bool vbus_present = false;
        /* Charger wake only if the module is armed. Otherwise a button
         * press with the cable in would leave the screen off. Armed: one
         * REG00h read. The 1 Hz sysfs online flag is the fallback when
         * that read fails, and only CONNECTED counts. No wait. */
        if (!axp2101_wake_is_armed()) {
            woke_on_external_power = false;
        } else if (charge_limiter_vbus_present(&vbus_present)) {
            woke_on_external_power = vbus_present;
        } else {
            battery_external_power_state_t wake_power = battery_get_external_power_state();
            woke_on_external_power = (wake_power == BATTERY_EXTERNAL_POWER_CONNECTED);
        }
        if (diagnostics_enabled) {
            log_suspend_diagnostics(suspend_duration_ms_now() - sleep_start_ms, suspend_write_ok, suspend_write_errno);
        }
        SUSPEND_LOG("woke_on_external_power=%d", woke_on_external_power);
    } else {
        SUSPEND_LOG("cancelled: external power connected or unknown after preparation");
    }

    /* The screen-off GUI caller selects and presents its wake surface through
     * the backlight worker before revealing the panel. Do not unblank early. */

    if (experimental_radio && experimental_radio_action("suspend-end", RADIO_UI_ACTION_TIMEOUT_MS) != 0) {
        SUSPEND_LOG("radio restore deferred: suspend-end failed");
        queue_radio_recovery(wifi_was_on, bt_was_on, true);
        return;
    }
    queue_radio_restore(wifi_was_on, bt_was_on);
}
