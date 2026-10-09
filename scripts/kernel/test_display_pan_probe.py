#!/usr/bin/env python3
"""Mocked syscall tests for the bounded RGB565 framebuffer pan benchmark."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SOURCE = REPO / "scripts/kernel/display_pan_probe.c"

HARNESS = r'''#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/mman.h>
#include <unistd.h>

int display_pan_probe_main(int argc, char **argv);
static const char *test_case;
static int open_count, close_count, fixed_count, variable_count;
static int pan_count, unexpected_ioctl, seen_flags, active_yoffset;
static int clock_count;
static int unexpected_map, unexpected_write;

ssize_t __real_write(int fd, const void *buf, size_t count);
int __real_close(int fd);

int __wrap_open(const char *path, int flags, ...)
{
    if(strcmp(path, "/dev/fb0") != 0) { errno = ENODEV; return -1; }
    if(strcmp(test_case, "open_error") == 0) { errno = EACCES; return -1; }
    open_count++;
    seen_flags = flags;
    return 42;
}

int __wrap_close(int fd)
{
    if(fd == 1) return __real_close(fd);
    if(fd != 42) { errno = EBADF; return -1; }
    close_count++;
    if(strcmp(test_case, "close_error") == 0) { errno = EIO; return -1; }
    return 0;
}

int __wrap_fstat(int fd, struct stat *st)
{
    if(fd != 42) { errno = EBADF; return -1; }
    if(strcmp(test_case, "fstat_error") == 0) { errno = EIO; return -1; }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0600;
    st->st_rdev = makedev(strcmp(test_case, "bad_major") == 0 ? 1 : 29, 0);
    return 0;
}

void *__wrap_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t off)
{
    (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)off;
    unexpected_map++;
    errno = ENOSYS;
    return MAP_FAILED;
}

ssize_t __wrap_write(int fd, const void *buf, size_t count)
{
    if(fd == 1 && strcmp(test_case, "stdout_error") == 0) { errno = EIO; return -1; }
    if(fd == 42) { unexpected_write++; errno = EIO; return -1; }
    return __real_write(fd, buf, count);
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if(fd != 42) { errno = EBADF; return -1; }
    if(request == FBIOGET_FSCREENINFO) {
        struct fb_fix_screeninfo *fix = arg;
        fixed_count++;
        fix->type = FB_TYPE_PACKED_PIXELS;
        if(strcmp(test_case, "get_fix_error") == 0) { errno = EIO; return -1; }
        fix->visual = strcmp(test_case, "bad_format") == 0 ?
                      FB_VISUAL_PSEUDOCOLOR : FB_VISUAL_TRUECOLOR;
        fix->line_length = strcmp(test_case, "bad_stride") == 0 ? 958 : 960;
        fix->smem_len = strcmp(test_case, "bad_smem") == 0 ? 100 : 1536000;
        fix->ypanstep = strcmp(test_case, "bad_step") == 0 ? 3 : 1;
        return 0;
    }
    if(request == FBIOGET_VSCREENINFO) {
        struct fb_var_screeninfo *var = arg;
        variable_count++;
        if(strcmp(test_case, "get_var_error") == 0) { errno = EIO; return -1; }
        var->xres = 480; var->yres = 800;
        var->xres_virtual = 480;
        var->yres_virtual = strcmp(test_case, "bad_vheight") == 0 ? 1601 : 1600;
        var->bits_per_pixel = 16;
        var->xoffset = strcmp(test_case, "bad_xoffset") == 0 ? 1 : 0;
        var->yoffset = (strcmp(test_case, "orig800_odd") == 0 ||
                        strcmp(test_case, "orig800_even") == 0) ? 800 :
                       strcmp(test_case, "bad_yoffset") == 0 ? 1 : 0;
        active_yoffset = (int)var->yoffset;
        var->rotate = strcmp(test_case, "bad_rotate") == 0 ? 1 : 0;
        var->vmode = strcmp(test_case, "bad_vmode") == 0 ? FB_VMODE_YWRAP : 0;
        var->red.offset = 11; var->red.length = 5;
        var->green.offset = 5; var->green.length = 6;
        var->blue.offset = 0; var->blue.length = 5;
        if(strcmp(test_case, "bad_color") == 0) var->green.length = 5;
        return 0;
    }
    if(request == FBIOPAN_DISPLAY) {
        struct fb_var_screeninfo *var = arg;
        pan_count++;
        if((strcmp(test_case, "pan_error_first") == 0 && pan_count == 1) ||
           (strcmp(test_case, "pan_error_second") == 0 && pan_count == 2) ||
           (strcmp(test_case, "restore_error") == 0 && pan_count == 2)) {
            errno = EIO;
            return -1;
        }
        if(strcmp(test_case, "bad_return_offset") == 0) var->yoffset = 77;
        active_yoffset = (int)var->yoffset;
        return 0;
    }
    unexpected_ioctl++;
    errno = ENOTTY;
    return -1;
}

int __wrap_clock_getres(clockid_t clock_id, struct timespec *ts)
{
    if(clock_id != CLOCK_MONOTONIC) { errno = EINVAL; return -1; }
    if(strcmp(test_case, "getres_error") == 0) { errno = EIO; return -1; }
    ts->tv_sec = 0; ts->tv_nsec = 1000;
    return 0;
}

int __wrap_clock_gettime(clockid_t clock_id, struct timespec *ts)
{
    clock_count++;
    if(clock_id != CLOCK_MONOTONIC) { errno = EINVAL; return -1; }
    if(strcmp(test_case, "clock_before") == 0 && clock_count == 3) {
        errno = EFAULT; return -1;
    }
    if(strcmp(test_case, "clock_after") == 0 && clock_count == 4) {
        errno = EFAULT; return -1;
    }
    if(strcmp(test_case, "clock_overflow") == 0 && clock_count >= 3) {
        ts->tv_sec = (time_t)0x7fffffffffffffffLL;
        ts->tv_nsec = 0;
        return 0;
    }
    if(strcmp(test_case, "budget") == 0 && clock_count == 5) {
        ts->tv_sec = 32;
        ts->tv_nsec = 0;
        return 0;
    }
    ts->tv_sec = 1;
    ts->tv_nsec = (clock_count * clock_count * 137000L) % 900000000L;
    return 0;
}

int main(int argc, char **argv)
{
    test_case = getenv("PAN_CASE");
    if(!test_case) test_case = "normal";
    if(strcmp(test_case, "stdout_error") == 0) (void)close(1);
    int rc = display_pan_probe_main(argc, argv);
    fprintf(stderr, "MOCK open=%d close=%d fixed=%d variable=%d pan=%d bad_ioctl=%d flags=%d yoffset=%d clock=%d map=%d write=%d\n",
            open_count, close_count, fixed_count, variable_count, pan_count,
            unexpected_ioctl, seen_flags, active_yoffset, clock_count,
            unexpected_map, unexpected_write);
    return rc;
}
'''


class DisplayPanProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.build = Path(cls.temp.name)
        (cls.build / "harness.c").write_text(HARNESS)
        cls.binary = cls.build / "display-pan-probe-test"
        commands = [
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-Dmain=display_pan_probe_main", "-c", str(SOURCE),
             "-o", str(cls.build / "probe.o")],
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             str(cls.build / "harness.c"), str(cls.build / "probe.o"),
             "-Wl,--wrap=open", "-Wl,--wrap=ioctl", "-Wl,--wrap=close",
             "-Wl,--wrap=fstat", "-Wl,--wrap=mmap", "-Wl,--wrap=write",
             "-Wl,--wrap=clock_gettime", "-Wl,--wrap=clock_getres",
             "-o", str(cls.binary)],
        ]
        for command in commands:
            result = subprocess.run(command, text=True, capture_output=True, timeout=30)
            if result.returncode:
                raise RuntimeError(f"compile failed: {result.stderr}")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_probe(self, case="normal", *args):
        env = os.environ.copy()
        env["PAN_CASE"] = case
        result = subprocess.run([str(self.binary), *args], env=env, text=True,
                                capture_output=True, timeout=5)
        self.assertIn("bad_ioctl=0", result.stderr)
        self.assertIn("map=0 write=0", result.stderr)
        return result

    def test_success_restores_original_page_and_emits_metrics(self):
        result = self.run_probe("normal", "--flips", "3")
        self.assertEqual(0, result.returncode, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(3, data["successful_pan_ioctls"])
        self.assertEqual(3, data["latency_samples"])
        flag_text = int(result.stderr.rsplit("flags=", 1)[1].split()[0])
        self.assertEqual(os.O_RDWR | os.O_CLOEXEC | os.O_NOCTTY, flag_text)
        self.assertEqual(4, int(result.stderr.rsplit("pan=", 1)[1].split()[0]))
        self.assertTrue(data["restore_attempted"])
        self.assertFalse(data["restore_failed"])
        self.assertFalse(data["restore_skipped_uncertain"])
        self.assertEqual(0, int(result.stderr.rsplit("yoffset=", 1)[1].split()[0]))
        self.assertLess(data["latency_min_us"], data["latency_max_us"])
        self.assertGreaterEqual(data["latency_p95_us"], data["latency_p50_us"])
        self.assertEqual(1000, data["clock_resolution_ns"])
        self.assertEqual(16129, data["frame_interval_us_62hz"])
        self.assertEqual(0, data["over_62hz_deadline_count"])
        self.assertEqual(0, data["original_yoffset"])
        self.assertEqual(0, data["last_known_yoffset"])

    def test_even_flip_count_needs_no_extra_restore(self):
        result = self.run_probe("normal", "--flips", "2")
        self.assertEqual(0, result.returncode, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(2, data["successful_pan_ioctls"])
        self.assertFalse(data["restore_attempted"])
        self.assertEqual(2, int(result.stderr.rsplit("pan=", 1)[1].split()[0]))
        self.assertEqual(0, int(result.stderr.rsplit("yoffset=", 1)[1].split()[0]))

    def test_original_eight_hundred_odd_and_even_flips(self):
        odd = self.run_probe("orig800_odd", "--flips", "3")
        even = self.run_probe("orig800_even", "--flips", "2")
        self.assertEqual(0, odd.returncode, odd.stderr)
        self.assertEqual(0, even.returncode, even.stderr)
        odd_data = json.loads(odd.stdout)
        even_data = json.loads(even.stdout)
        self.assertEqual(800, odd_data["original_yoffset"])
        self.assertEqual(800, odd_data["last_known_yoffset"])
        self.assertTrue(odd_data["restore_attempted"])
        self.assertFalse(even_data["restore_attempted"])
        self.assertEqual(800, even_data["last_known_yoffset"])

    def test_first_pan_failure_aborts_without_restore(self):
        result = self.run_probe("pan_error_first", "--flips", "4")
        self.assertEqual(1, result.returncode)
        data = json.loads(result.stdout)
        self.assertEqual(0, data["successful_pan_ioctls"])
        self.assertEqual(1, data["pan_failures"])
        self.assertTrue(data["restore_skipped_uncertain"])
        self.assertTrue(data["state_uncertain"])
        self.assertEqual(0, data["last_known_yoffset"])
        self.assertEqual(800, data["requested_yoffset"])
        self.assertFalse(data["restore_attempted"])
        self.assertEqual(1, int(result.stderr.rsplit("pan=", 1)[1].split()[0]))

    def test_first_failure_aborts_later_pan_and_skips_restore(self):
        result = self.run_probe("pan_error_second", "--flips", "4")
        self.assertEqual(1, result.returncode)
        data = json.loads(result.stdout)
        self.assertEqual(1, data["successful_pan_ioctls"])
        self.assertEqual(1, data["pan_failures"])
        self.assertTrue(data["restore_skipped_uncertain"])
        self.assertEqual(2, int(result.stderr.rsplit("pan=", 1)[1].split()[0]))
        self.assertEqual(800, int(result.stderr.rsplit("yoffset=", 1)[1].split()[0]))

    def test_unexpected_returned_yoffset_aborts_without_restore(self):
        result = self.run_probe("bad_return_offset", "--flips", "3")
        self.assertEqual(1, result.returncode)
        data = json.loads(result.stdout)
        self.assertEqual(1, data["successful_pan_ioctls"])
        self.assertEqual(1, data["latency_samples"])
        self.assertEqual(800, data["requested_yoffset"])
        self.assertEqual(0, data["last_known_yoffset"])
        self.assertTrue(data["state_uncertain"])
        self.assertFalse(data["restore_attempted"])

    def test_bad_metadata_refuses_pan(self):
        for case in ("bad_format", "bad_step", "bad_stride", "bad_smem",
                     "bad_vheight", "bad_xoffset", "bad_yoffset", "bad_rotate",
                     "bad_vmode", "bad_color"):
            with self.subTest(case=case):
                result = self.run_probe(case, "--flips", "2")
                self.assertEqual(3, result.returncode)
                data = json.loads(result.stdout)
                self.assertFalse(data["metadata_valid"])
                self.assertEqual(0, data["successful_pan_ioctls"])
                self.assertEqual(0, int(result.stderr.rsplit("pan=", 1)[1].split()[0]))

    def test_open_fstat_and_get_failures_do_not_pan(self):
        for case in ("open_error", "fstat_error", "bad_major", "get_fix_error",
                     "get_var_error", "getres_error"):
            with self.subTest(case=case):
                result = self.run_probe(case, "--flips", "2")
                self.assertNotEqual(0, result.returncode)
                self.assertEqual(0, int(result.stderr.rsplit("pan=", 1)[1].split()[0]))

    def test_close_and_stdout_failures_are_nonzero(self):
        close = self.run_probe("close_error", "--flips", "2")
        self.assertNotEqual(0, close.returncode)
        self.assertTrue(json.loads(close.stdout)["close_failed"])
        output = self.run_probe("stdout_error", "--flips", "2")
        self.assertNotEqual(0, output.returncode)

    def test_clock_failure_before_pan_is_fatal_without_pan(self):
        result = self.run_probe("clock_before", "--flips", "2")
        self.assertEqual(1, result.returncode)
        data = json.loads(result.stdout)
        self.assertEqual(1, data["timing_failures"])
        self.assertEqual(0, data["successful_pan_ioctls"])
        self.assertTrue(data["clock_fatal"])
        self.assertFalse(data["restore_skipped_uncertain"])
        self.assertEqual(0, int(result.stderr.rsplit("pan=", 1)[1].split()[0]))

    def test_clock_failure_after_pan_marks_state_uncertain(self):
        result = self.run_probe("clock_after", "--flips", "2")
        self.assertEqual(1, result.returncode)
        data = json.loads(result.stdout)
        self.assertEqual(1, data["successful_pan_ioctls"])
        self.assertEqual(0, data["latency_samples"])
        self.assertEqual(1, data["timing_failures"])
        self.assertFalse(data["restore_skipped_uncertain"])
        self.assertTrue(data["restore_attempted"])
        self.assertEqual(2, int(result.stderr.rsplit("pan=", 1)[1].split()[0]))

    def test_clock_overflow_is_fatal_before_pan(self):
        result = self.run_probe("clock_overflow", "--flips", "2")
        self.assertEqual(1, result.returncode)
        data = json.loads(result.stdout)
        self.assertTrue(data["clock_fatal"])
        self.assertEqual(0, data["successful_pan_ioctls"])
        self.assertEqual(0, int(result.stderr.rsplit("pan=", 1)[1].split()[0]))

    def test_time_budget_aborts_and_restores_known_page(self):
        result = self.run_probe("budget", "--flips", "5")
        self.assertEqual(1, result.returncode)
        data = json.loads(result.stdout)
        self.assertTrue(data["budget_exhausted"])
        self.assertEqual(1, data["successful_pan_ioctls"])
        self.assertTrue(data["restore_attempted"])
        self.assertFalse(data["state_uncertain"])
        self.assertEqual(0, data["last_known_yoffset"])

    def test_only_supported_device_path_is_accepted(self):
        result = self.run_probe("normal", "--flips", "2", "--device", "/dev/fb0")
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual("/dev/fb0", json.loads(result.stdout)["device"])
        rejected = self.run_probe("normal", "--device", "/dev/watchdog")
        self.assertEqual(2, rejected.returncode)
        self.assertIn("open=0", rejected.stderr)

    def test_restore_failure_is_reported(self):
        result = self.run_probe("restore_error", "--flips", "1")
        self.assertEqual(1, result.returncode)
        data = json.loads(result.stdout)
        self.assertEqual(1, data["successful_pan_ioctls"])
        self.assertTrue(data["restore_attempted"])
        self.assertTrue(data["restore_failed"])
        self.assertTrue(data["restore_skipped_uncertain"])
        self.assertEqual(2, int(result.stderr.rsplit("pan=", 1)[1].split()[0]))

    def test_flip_count_is_bounded(self):
        result = self.run_probe("normal", "--flips", "10001")
        self.assertEqual(2, result.returncode)
        self.assertIn("1..1000", result.stderr)
        too_long = self.run_probe("normal", "--flips", "1001")
        self.assertEqual(2, too_long.returncode)


if __name__ == "__main__":
    unittest.main()
