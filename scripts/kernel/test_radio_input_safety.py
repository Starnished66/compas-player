#!/usr/bin/env python3
"""Host checks for bcm_wlbt_power pin parsing and power error paths.

The harness compiles the exact selected functions extracted from the supplied
module source, substituting only Linux types and external APIs with host stubs.
It does not need a kernel build or device.
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile


HARNESS = r'''
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#define PAGE_SIZE 4096
#define dev_err(dev, ...) ((void)(dev))
struct kernel_param { void *arg; };
struct kernel_param_ops { int (*set)(const char *, const struct kernel_param *); int (*get)(char *, const struct kernel_param *); };
struct kobject { int unused; };
struct attribute_group { int unused; };
struct device { int registered; int refs; struct kobject kobj; };
struct device_attribute { int unused; };
struct work_struct { int unused; };
struct mmc_card { int unused; };
struct mmc_host { struct mmc_card *card; struct device class_dev; };
struct sdio_func { struct mmc_card *card; struct device dev; };
struct platform_device { struct device dev; };
static struct platform_device radio_storage;
static struct platform_device *radio_pdev = &radio_storage;
static struct device *msc_dev;
static struct device_attribute dev_attr_chipvendor;
static struct attribute_group radio_group;
static bool radio_sysfs_attempted;
static struct mmc_host *host;
static struct sdio_func *vendor_func;
static struct work_struct probe_work;
static int wl_reg_on = 7;
static int wl_mmc = 0;
static bool wifi_on;
static int detect_result, save_result, restore_result;
static int detect_calls, wait_calls, rtc_enable_calls, rtc_disable_calls;
static int gpio_calls, gpio_last;
static int sysfs_remove_calls, attr_remove_calls, put_calls;
static struct sdio_func vendor_storage;
static struct device msc_storage;
static int refcount;
static struct mmc_host host_storage;
static struct mmc_card card_storage;
static void mutex_lock(void *lock) { (void)lock; }
static void mutex_unlock(void *lock) { (void)lock; }
static int lock_storage;
#define radio_lock lock_storage
static void flush_work(struct work_struct *work) { (void)work; }
static int strtobool(const char *s, bool *value) {
    if (!strcmp(s, "1") || !strcmp(s, "y") || !strcmp(s, "yes") || !strcmp(s, "true") || !strcmp(s, "on") || !strcmp(s, "0\n")) {
        *value = strcmp(s, "0\n") != 0;
        return 0;
    }
    if (!strcmp(s, "0") || !strcmp(s, "n") || !strcmp(s, "no") || !strcmp(s, "false") || !strcmp(s, "off")) {
        *value = false;
        return 0;
    }
    return -EINVAL;
}
static void gpio_set_value_cansleep(int gpio, int value) { (void)gpio; gpio_calls++; gpio_last = value; }
static void msleep(unsigned int ms) { (void)ms; }
static void ingenic_rtc32k_enable(void) { rtc_enable_calls++; refcount++; }
static void ingenic_rtc32k_disable(void) { rtc_disable_calls++; refcount--; }
static int jzmmc_manual_detect(int index, int on) { (void)index; (void)on; detect_calls++; return detect_result; }
static struct mmc_host *find_host(void) { return host; }
static bool wait_for_card(void) { wait_calls++; return true; }
static void read_vendor(void) { }
static void forget_vendor_func(void);
static bool device_is_registered(struct device *dev) { return dev->registered; }
static void sysfs_remove_group(struct kobject *kobj, const struct attribute_group *group) { (void)kobj; (void)group; sysfs_remove_calls++; }
static void device_remove_file(struct device *dev, struct device_attribute *attr) { (void)attr; attr_remove_calls++; dev->registered = 0; }
static void put_device(struct device *dev) { put_calls++; dev->refs--; }
static void mmc_claim_host(struct mmc_host *h) { (void)h; }
static void mmc_release_host(struct mmc_host *h) { (void)h; }
static int mmc_power_restore_host(struct mmc_host *h) { (void)h; return restore_result; }
static int mmc_power_save_host(struct mmc_host *h) { (void)h; return save_result; }

__PARSER__
static void wl_reg_on_pulse(void) { gpio_set_value_cansleep(wl_reg_on, 0); msleep(100); gpio_set_value_cansleep(wl_reg_on, 1); }
__WIFI_UP__
__WIFI_DOWN__
__FORCE_OFF__
__FORGET_VENDOR__
__QUIESCE_SYSFS__
__RELEASE_REFS__
__TEARDOWN__
__STORE__

static void reset(void) {
    memset(&host_storage, 0, sizeof host_storage);
    memset(&card_storage, 0, sizeof card_storage);
    host = &host_storage; vendor_func = NULL;
    wifi_on = false;
    memset(&vendor_storage, 0, sizeof vendor_storage); memset(&msc_storage, 0, sizeof msc_storage);
    msc_dev = NULL; radio_sysfs_attempted = false;
    detect_result = save_result = restore_result = 0;
    detect_calls = wait_calls = rtc_enable_calls = rtc_disable_calls = gpio_calls = 0;
    gpio_last = 1; refcount = 0; sysfs_remove_calls = attr_remove_calls = put_calls = 0;
}
static void test_pins(void) {
    struct kernel_param kp;
    int pin = 42;
    kp.arg = &pin;
    assert(param_set_pin("PA00", &kp) == 0 && pin == 0);
    assert(param_set_pin("pf31", &kp) == 0 && pin == 191);
    assert(param_set_pin("0", &kp) == 0 && pin == 0);
    assert(param_set_pin("191", &kp) == 0 && pin == 191);
    assert(param_set_pin("-1", &kp) == 0 && pin == -1);
    const char *bad[] = { "2junk", "192", "-2", "+1", "PG00", "PA32", "PA1x", "PA", "" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        pin = 42;
        assert(param_set_pin(bad[i], &kp) == -EINVAL && pin == 42);
    }
}
static void test_manual_detect_failure_unwinds(void) {
    reset();
    detect_result = -EIO;
    assert(wifi_power_up() == -EIO);
    assert(detect_calls == 1 && wait_calls == 0);
    assert(!wifi_on && rtc_enable_calls == 1 && rtc_disable_calls == 1 && refcount == 0);
    assert(gpio_last == 0);

    reset();
    detect_result = -1;
    assert(wifi_power_up() == -ENODEV);
    assert(rtc_enable_calls == 1 && rtc_disable_calls == 1 && refcount == 0);
}
static void test_partial_sysfs_init_unwind_releases_every_reference_once(void) {
    reset();
    radio_sysfs_attempted = true; /* create_group failed after publishing one file */
    vendor_storage.dev.registered = 1; vendor_storage.dev.refs = 1;
    vendor_func = &vendor_storage;
    host_storage.class_dev.refs = 1;
    host = &host_storage;
    msc_storage.refs = 1; msc_dev = &msc_storage;

    radio_quiesce_sysfs(); /* sysfs callback drain must precede the radio lock */
    mutex_lock(&radio_lock);
    radio_release_references_locked();
    mutex_unlock(&radio_lock);

    assert(sysfs_remove_calls == 1 && !radio_sysfs_attempted);
    assert(attr_remove_calls == 1 && vendor_storage.dev.refs == 0);
    assert(host_storage.class_dev.refs == 0 && msc_storage.refs == 0);
    assert(put_calls == 3 && vendor_func == NULL && host == NULL && msc_dev == NULL);

    radio_quiesce_sysfs();
    mutex_lock(&radio_lock);
    radio_release_references_locked();
    mutex_unlock(&radio_lock);
    assert(sysfs_remove_calls == 1 && attr_remove_calls == 1 && put_calls == 3);
}
static void test_power_save_failure_is_propagated_and_retained_until_teardown(void) {
    reset();
    host_storage.card = &card_storage;
    wifi_on = true;
    refcount = 1;
    save_result = -EBUSY;
    assert(wifi_power_down() == -EBUSY);
    save_result = -EINVAL;
    assert(wifi_power_down() == -EBUSY);
    save_result = -EBUSY;
    assert(wifi_on && gpio_calls == 0 && rtc_disable_calls == 0 && refcount == 1);

    assert(wifi_power_store(NULL, NULL, "0", 1) == -EBUSY);
    assert(wifi_on && gpio_calls == 0 && rtc_disable_calls == 0 && refcount == 1);

    /* Exercise the exact teardown helper used after callbacks are detached. */
    wifi_power_teardown();
    assert(!wifi_on && gpio_last == 0 && rtc_disable_calls == 1 && refcount == 0);
}
int main(void) { test_pins(); test_manual_detect_failure_unwinds(); test_partial_sysfs_init_unwind_releases_every_reference_once(); test_power_save_failure_is_propagated_and_retained_until_teardown(); return 0; }
'''


def extract_function(source: str, name: str) -> str:
    marker = f"static int {name}(" if name in {"param_set_pin", "wifi_power_up", "wifi_power_down"} else None
    if name in {"wifi_power_force_off", "wifi_power_teardown", "forget_vendor_func", "radio_quiesce_sysfs", "radio_release_references_locked"}:
        marker = f"static void {name}("
    if name == "wifi_power_store":
        marker = "static ssize_t wifi_power_store("
    assert marker is not None
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start:end + 1]
    raise ValueError(f"unterminated function {name}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=pathlib.Path, required=True,
                        help="path to bcm_wlbt_power.c")
    args = parser.parse_args()
    source = args.source.read_text()
    unwind = source[source.index("err_rfkill:"):source.index("err_wake_wl:", source.index("err_rfkill:"))]
    if unwind.index("radio_quiesce_sysfs();") >= unwind.index("mutex_lock(&radio_lock);"):
        raise ValueError("sysfs callbacks must drain before radio_lock is taken")
    if unwind.index("radio_release_references_locked();") >= unwind.index("mutex_unlock(&radio_lock);"):
        raise ValueError("references must be released under radio_lock")
    replacements = {
        "__PARSER__": extract_function(source, "param_set_pin"),
        "__WIFI_UP__": extract_function(source, "wifi_power_up"),
        "__WIFI_DOWN__": extract_function(source, "wifi_power_down"),
        "__FORCE_OFF__": extract_function(source, "wifi_power_force_off"),
        "__FORGET_VENDOR__": extract_function(source, "forget_vendor_func"),
        "__QUIESCE_SYSFS__": extract_function(source, "radio_quiesce_sysfs"),
        "__RELEASE_REFS__": extract_function(source, "radio_release_references_locked"),
        "__TEARDOWN__": extract_function(source, "wifi_power_teardown"),
        "__STORE__": extract_function(source, "wifi_power_store"),
    }
    harness = HARNESS
    for marker, code in replacements.items():
        harness = harness.replace(marker, code)
    with tempfile.TemporaryDirectory(prefix="radio-input-safety-") as temp:
        cfile = pathlib.Path(temp) / "harness.c"
        binary = pathlib.Path(temp) / "harness"
        cfile.write_text(harness)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-sign-compare",
                        "-o", str(binary), str(cfile)], check=True)
        subprocess.run([str(binary)], check=True)
    print("radio input and power safety checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
