#define USB_MODE_CONTROL_ANDROID0_DIR "android0"
#define USB_MODE_CONTROL_ADB_DEMO_DIR "adb_demo"
#define USB_MODE_CONTROL_UDC_STATE_DIR "udc"
#define USB_MODE_CONTROL_R1_ROLE_PATH "role"
#define USB_MODE_CONTROL_DAC_JOURNAL_PATH "journal/usb_dac_session.txt"
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
/* Directory syncs and the journal's unlink can be made to fail. */
static bool dir_sync_fails, journal_unlink_fails;
int test_fsync(int fd) {
    struct stat st;
    if (dir_sync_fails && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) return -1;
    return fsync(fd);
}
int test_unlink(const char *path) {
    if (journal_unlink_fails && strcmp(path, "journal/usb_dac_session.txt") == 0) return -1;
    return unlink(path);
}
#define fsync test_fsync
#define unlink test_unlink
#include "../usb_mode_control.c"
#undef fsync
#undef unlink
#include "../../power/charge_limiter.h"
#include <assert.h>

#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdarg.h>

/* The real charge_limiter.c runs against this AXP register file. Every REG18
 * write asserts the journal already exists; fail_from makes I/O fail. */
#define FAKE_I2C_FD 1000
#define AXP_MODULE_EN 0x18
#define AXP_CHARGE 0x02
static uint8_t axp_regs[256] = { [AXP_MODULE_EN] = AXP_CHARGE };
static int axp_reads, axp_writes, axp_fail_from = -1;
/* Holds a DAC session's REG18 write, and with it the charger mutex. */
static atomic_bool block_charger_write, charger_write_blocked;
static bool journal_exists(void) { return access(USB_MODE_CONTROL_DAC_JOURNAL_PATH, F_OK) == 0; }
static bool charging(void) { return axp_regs[AXP_MODULE_EN] & AXP_CHARGE; }
int fake_i2c_open(const char *path, int flags, ...) {
    if (strcmp(path, "/dev/i2c-0") == 0) return FAKE_I2C_FD;
    va_list ap;
    va_start(ap, flags);
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    va_end(ap);
    return open(path, flags, mode);
}
int fake_i2c_close(int fd) { return fd == FAKE_I2C_FD ? 0 : close(fd); }
int fake_i2c_ioctl(int fd, unsigned long request, ...) {
    assert(fd == FAKE_I2C_FD);
    if (request == I2C_SLAVE_FORCE) return 0;
    va_list ap;
    va_start(ap, request);
    struct i2c_smbus_ioctl_data *args = va_arg(ap, struct i2c_smbus_ioctl_data *);
    va_end(ap);
    if (axp_fail_from >= 0 && axp_reads + axp_writes >= axp_fail_from) return -1;
    /* One transfer at a time, like the adapter: a stalled one stalls the bus. */
    static pthread_mutex_t bus = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&bus);
    if (args->read_write == I2C_SMBUS_READ) {
        axp_reads++;
        args->data->byte = axp_regs[args->command];
    } else {
        axp_writes++;
        if (args->command == AXP_MODULE_EN) assert(journal_exists());
        if (args->command == AXP_MODULE_EN && atomic_load(&block_charger_write)) {
            atomic_store(&charger_write_blocked, true);
            while (atomic_load(&block_charger_write)) usleep(1000);
        }
        axp_regs[args->command] = args->data->byte;
    }
    pthread_mutex_unlock(&bus);
    return 0;
}

static void put(const char *path, const char *value) {
    FILE *f = fopen(path, "w");
    assert(f && fputs(value, f) >= 0 && fclose(f) == 0);
}
static bool role_is(const char *want) {
    char role[24];
    return r1_usb_role_read(role, sizeof(role)) && strcmp(role, want) == 0;
}
/* The USB DAC screen's hold: Sink and no charging before any host, released
 * when no DAC gadget is bound, kept for a bound DAC session to restore. */
/* A journal that cannot be written cancels the untouched baseline reservation
 * and the provisional ownership without any charger write, even if the charger
 * I/O fails right after the capture. */
static void test_journal_failure(void) {
    assert(mkdir("journal", 0700) == 0 && rmdir("journal") == 0);
    put("journal", "not a directory\n");
    put("role", "NormalDRP\n");
    axp_reads = axp_writes = 0;
    axp_fail_from = 1;
    assert(!usb_mode_control_hold_dac_policy(true));
    assert(axp_reads == 1 && axp_writes == 0 && charging() && role_is("NormalDRP"));
    axp_fail_from = -1;
    assert(unlink("journal") == 0 && mkdir("journal", 0700) == 0);
    /* Owned would resume (write REG18) before journaling; the fake asserts it did not. */
    assert(usb_mode_control_hold_dac_policy(true));
    assert(role_is("Sink") && !charging() && journal_exists());
    assert(usb_mode_control_hold_dac_policy(false));
    assert(role_is("NormalDRP") && charging() && !journal_exists());
}
/* Restored role and charger end ownership even if the journal's directory
 * sync fails after the unlink: only the sync is retried, and a reopened
 * screen journals its new baseline before touching the charger. */
static void test_journal_sync_failure(void) {
    put("role", "NormalDRP\n");
    for (int reopen = 0; reopen < 2; ++reopen) {
        assert(usb_mode_control_hold_dac_policy(true));
        dir_sync_fails = true;
        assert(!usb_mode_control_hold_dac_policy(false));
        assert(role_is("NormalDRP") && charging() && !journal_exists() && !dac_usb_policy_owned);
        int writes = axp_writes;
        if (reopen) {
            /* The save's own sync fails too: nothing is touched. */
            assert(!usb_mode_control_hold_dac_policy(true));
            assert(axp_writes == writes && charging() && role_is("NormalDRP"));
            dir_sync_fails = false;
            assert(usb_mode_control_hold_dac_policy(true)); /* fake asserts journal first */
            assert(role_is("Sink") && !charging() && dac_journal_cleanup == DAC_JOURNAL_CLEAN);
            assert(usb_mode_control_hold_dac_policy(false));
        } else {
            assert(!usb_mode_control_hold_dac_policy(false)); /* closed: retries the sync only */
            assert(axp_writes == writes && role_is("NormalDRP"));
            dir_sync_fails = false;
            assert(usb_mode_control_hold_dac_policy(false) && dac_journal_cleanup == DAC_JOURNAL_CLEAN);
        }
        assert(role_is("NormalDRP") && charging() && !journal_exists());
    }
}
/* A failed fresh acquisition leaves no journal and no hardware change, and
 * closing the screen retries only the outstanding directory sync. */
static void test_failed_acquisition_cleanup(void) {
    put("role", "NormalDRP\n");
    int writes = axp_writes;
    dir_sync_fails = true; /* the save published the journal, then its sync failed */
    assert(!usb_mode_control_hold_dac_policy(true));
    assert(!journal_exists() && axp_writes == writes && role_is("NormalDRP") && !dac_usb_policy_owned);
    assert(!usb_mode_control_hold_dac_policy(false)); /* screen closed */
    dir_sync_fails = false;
    assert(usb_mode_control_hold_dac_policy(false) && dac_journal_cleanup == DAC_JOURNAL_CLEAN);
    assert(axp_writes == writes && role_is("NormalDRP") && charging());
}
/* Ordinary session teardown ends ownership once the hardware is restored and
 * retries only the cleanup step that failed, on each Storage apply. */
static void test_teardown_cleanup(void) {
    put("role", "NormalDRP\n");
    for (int step = 0; step < 2; ++step) {
        assert(prepare_dac_usb_policy()); /* a DAC session's apply */
        journal_unlink_fails = step == 0;
        dir_sync_fails = step == 1;
        assert(finish_dac_usb_policy_apply(USB_MODE_STORAGE, true, false));
        assert(!dac_usb_policy_owned && role_is("NormalDRP") && charging());
        assert(journal_exists() == (step == 0));
        int writes = axp_writes;
        assert(finish_dac_usb_policy_apply(USB_MODE_STORAGE, true, false)); /* next Storage apply */
        assert(axp_writes == writes && dac_journal_cleanup != DAC_JOURNAL_CLEAN);
        journal_unlink_fails = dir_sync_fails = false;
        assert(finish_dac_usb_policy_apply(USB_MODE_STORAGE, true, false));
        assert(axp_writes == writes && dac_journal_cleanup == DAC_JOURNAL_CLEAN && !journal_exists());
    }
}
static void *hold_in_worker(void *arg) {
    (void)arg;
    assert(usb_mode_control_hold_dac_policy(true));
    return NULL;
}
/* The periodic limiter and safe-charging polls defer before their first
 * transfer while a worker's charger write stalls the bus. */
static void test_poll_defers_to_worker(void) {
    put("role", "NormalDRP\n");
    charge_limiter_poll(true, true); /* the voltage cap makes the status query read the bus */
    atomic_store(&block_charger_write, true);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, hold_in_worker, NULL) == 0);
    while (!atomic_load(&charger_write_blocked)) usleep(1000);
    alarm(5); /* a blocked poll fails the test instead of hanging it */
    charge_limiter_poll(false, true);
    safe_charging_poll(false, true);
    (void) charge_limiter_is_confirmed_off(); /* LED and top bar status, same UI tick */
    alarm(0);
    atomic_store(&block_charger_write, false);
    assert(pthread_join(worker, NULL) == 0);
    assert(role_is("Sink") && !charging());
    assert(usb_mode_control_hold_dac_policy(false));
}
static void test_policy_hold(void) {
    put("android0/UDC", "\n");
    put("adb_demo/UDC", "\n");
    put("role", "NormalDRP\n");
    assert(usb_mode_control_hold_dac_policy(true));
    assert(role_is("Sink") && !charging() && journal_exists());
    assert(usb_mode_control_hold_dac_policy(false));
    assert(role_is("NormalDRP") && charging() && !journal_exists());
    /* An unreadable gadget is not unbound: keep the policy and report a retry. */
    assert(usb_mode_control_hold_dac_policy(true));
    assert(unlink("android0/UDC") == 0 && mkdir("android0/UDC", 0700) == 0);
    assert(!usb_mode_control_hold_dac_policy(false));
    assert(role_is("Sink") && !charging() && journal_exists());
    assert(rmdir("android0/UDC") == 0);
    put("android0/UDC", "\n");
    assert(usb_mode_control_hold_dac_policy(false));
    assert(role_is("NormalDRP") && charging() && !journal_exists());
    assert(usb_mode_control_hold_dac_policy(true));
    assert(mkdir("android0/configs", 0700) == 0 && mkdir("android0/configs/c.1", 0700) == 0 &&
           mkdir("android0/configs/c.1/uac_sa.0", 0700) == 0);
    put("android0/UDC", "test-controller\n");
    assert(usb_mode_control_hold_dac_policy(false));
    assert(role_is("Sink") && !charging() && journal_exists());
}
int main(void) {
    assert(mkdir("android0", 0700) == 0);
    assert(mkdir("adb_demo", 0700) == 0);
    assert(mkdir("udc", 0700) == 0);
    assert(mkdir("udc/test-controller", 0700) == 0);
    put("android0/UDC", "test-controller\n");
    assert(!usb_mode_control_host_connected());
    const char *negative[] = {"not attached\n", "powered\n", "attached\n", "unknown\n", "configured junk\n"};
    for (size_t i = 0; i < sizeof(negative)/sizeof(negative[0]); ++i) {
        put("udc/test-controller/state", negative[i]);
        assert(!usb_mode_control_host_connected());
    }
    const char *positive[] = {"configured\n", "suspended\n", "address\n", "addressed\n"};
    for (size_t i = 0; i < sizeof(positive)/sizeof(positive[0]); ++i) {
        put("udc/test-controller/state", positive[i]);
        assert(usb_mode_control_host_connected());
    }
    put("android0/UDC", "../test-controller\n");
    assert(!usb_mode_control_host_connected());
    put("android0/UDC", "\n");
    put("adb_demo/UDC", "test-controller\n");
    assert(usb_mode_control_host_connected());
    test_journal_failure();
    test_journal_sync_failure();
    test_failed_acquisition_cleanup();
    test_teardown_cleanup();
    test_poll_defers_to_worker();
    test_policy_hold();
    puts("USB DAC data-host presence and policy hold tests passed");
}
