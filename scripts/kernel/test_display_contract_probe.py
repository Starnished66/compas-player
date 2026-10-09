#!/usr/bin/env python3
"""Mocked syscall tests for the read-only framebuffer contract probe."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SOURCE = REPO / "scripts/kernel/display_contract_probe.c"

HARNESS = r'''#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>

int display_probe_main(int argc, char **argv);
static const char *test_case;
static int open_count, close_count, fix_count, var_count, unexpected_ioctl;
static int seen_flags;
static long clock_step, clock_calls;

int __wrap_open(const char *path, int flags, ...)
{
    (void)path;
    open_count++;
    seen_flags = flags;
    return 42;
}

int __wrap_close(int fd)
{
    if(fd != 42) return -1;
    close_count++;
    return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if(fd != 42) { errno = EBADF; return -1; }
    if(request == FBIOGET_FSCREENINFO) {
        fix_count++;
        if(strcmp(test_case, "fix_error") == 0 ||
           strcmp(test_case, "clock_before_fix_error") == 0 ||
           strcmp(test_case, "clock_after_fix_error") == 0) { errno = EIO; return -1; }
        struct fb_fix_screeninfo *fix = arg;
        fix->line_length = strcmp(test_case, "overflow") == 0 ? 0xffffffffU :
                           strcmp(test_case, "bad_stride") == 0 ? 959U :
                           strcmp(test_case, "padded_virtual") == 0 ? 1408U : 960U;
        if(strcmp(test_case, "short_memory") == 0) fix->smem_len = 1000U;
        else if(strcmp(test_case, "active_oob") == 0) fix->smem_len = 768000U;
        else if(strcmp(test_case, "virtual_extent") == 0) fix->smem_len = 1000000U;
        else if(strcmp(test_case, "overflow") == 0) fix->smem_len = 0xffffffffU;
        else if(strcmp(test_case, "padded_virtual") == 0) fix->smem_len = 2252800U;
        else fix->smem_len = 1536000U;
        fix->mmio_len = 4096U;
        fix->xpanstep = 1U; fix->ypanstep = 1U;
        fix->visual = FB_VISUAL_TRUECOLOR; fix->type = FB_TYPE_PACKED_PIXELS;
        fix->type_aux = 7U; fix->accel = 3U;
        if(strcmp(test_case, "unsupported_format") == 0) fix->visual = FB_VISUAL_PSEUDOCOLOR;
        return 0;
    }
    if(request == FBIOGET_VSCREENINFO) {
        var_count++;
        if(strcmp(test_case, "var_error") == 0) { errno = EIO; return -1; }
        struct fb_var_screeninfo *var = arg;
        var->xres = strcmp(test_case, "zero_width") == 0 ? 0U :
                    strcmp(test_case, "overflow") == 0 ? 0xffffffffU : 480U;
        var->yres = strcmp(test_case, "overflow") == 0 ? 1U : 800U;
        var->xres_virtual = strcmp(test_case, "wide_virtual") == 0 ? 700U :
                            strcmp(test_case, "overflow") == 0 ? 0xffffffffU : 480U;
        var->yres_virtual = strcmp(test_case, "overflow") == 0 ? 0xffffffffU : 1600U;
        var->bits_per_pixel = strcmp(test_case, "overflow") == 0 ? 32U : 16U;
        var->xoffset = 0U;
        var->yoffset = strcmp(test_case, "overflow") == 0 ? 0xfffffffeU :
                       strcmp(test_case, "virtual_extent") == 0 ? 0U : 800U;
        if(strcmp(test_case, "bad_offset") == 0) var->yoffset = 801U;
        if(strcmp(test_case, "overflow") == 0) {
            var->red.offset = 16U; var->red.length = 8U;
            var->green.offset = 8U; var->green.length = 8U;
            var->blue.offset = 0U; var->blue.length = 8U;
        }
        if(strcmp(test_case, "unsupported_format") == 0) var->grayscale = 1U;
        if(strcmp(test_case, "unsupported_format") == 0) var->accel_flags = 0x1234U;
        var->red.offset = 11U; var->red.length = 5U;
        var->green.offset = 5U; var->green.length = 6U;
        var->blue.offset = 0U; var->blue.length = 5U;
        var->transp.offset = 0U; var->transp.length = 0U;
        return 0;
    }
    unexpected_ioctl++;
    errno = ENOTTY;
    return -1;
}

int __wrap_clock_gettime(clockid_t clock_id, struct timespec *ts)
{
    clock_calls++;
    if((strcmp(test_case, "clock_before") == 0 && clock_calls == 1) ||
       (strcmp(test_case, "clock_after") == 0 && clock_calls == 2) ||
       (strcmp(test_case, "clock_before_fix_error") == 0 && clock_calls == 1) ||
       (strcmp(test_case, "clock_after_fix_error") == 0 && clock_calls == 2)) {
        errno = EFAULT;
        return -1;
    }
    if(clock_id != CLOCK_MONOTONIC) { errno = EINVAL; return -1; }
    ts->tv_sec = 10;
    ts->tv_nsec = clock_step++ * 1000000L;
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    test_case = getenv("PROBE_CASE");
    if(!test_case) test_case = "valid";
    int rc = display_probe_main(1, argv);
    fprintf(stderr, "MOCK open=%d close=%d fix=%d var=%d bad_ioctl=%d flags=%d clock=%ld\n",
            open_count, close_count, fix_count, var_count, unexpected_ioctl, seen_flags, clock_calls);
    return rc;
}
'''


class DisplayContractProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.build = Path(cls.temp.name)
        (cls.build / "harness.c").write_text(HARNESS)
        cls.binary = cls.build / "display-probe-test"
        commands = [
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-Dmain=display_probe_main", "-c", str(SOURCE), "-o", str(cls.build / "probe.o")],
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             str(cls.build / "harness.c"), str(cls.build / "probe.o"),
             "-Wl,--wrap=open", "-Wl,--wrap=ioctl", "-Wl,--wrap=close", "-Wl,--wrap=clock_gettime",
             "-o", str(cls.binary)],
        ]
        for command in commands:
            result = subprocess.run(command, text=True, capture_output=True, timeout=30)
            if result.returncode:
                raise RuntimeError(f"compile failed: {result.stderr}")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_probe(self, case="valid"):
        env = os.environ.copy()
        env["PROBE_CASE"] = case
        result = subprocess.run([str(self.binary)], env=env, text=True,
                                capture_output=True, timeout=5)
        self.assertIn("bad_ioctl=0", result.stderr)
        return result

    def test_read_only_contract_and_page_geometry(self):
        result = self.run_probe()
        self.assertEqual(0, result.returncode, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual("fbdev", data["device"])
        self.assertTrue(data["geometry_valid"])
        self.assertEqual([480, 800, 480, 1600],
                         [data[k] for k in ("xres", "yres", "xres_virtual", "yres_virtual")])
        self.assertEqual(960, data["stride_bytes"])
        self.assertEqual(1536000, data["smem_len"])
        self.assertEqual(2, data["complete_pages"])
        self.assertTrue(data["stride_valid"])
        self.assertTrue(data["active_viewport_memory_valid"])
        self.assertTrue(data["virtual_memory_bounds_valid"])
        self.assertTrue(data["offsets_valid"])
        self.assertTrue(data["rgb565_supported"])
        self.assertEqual(768000, data["page_bytes"])
        self.assertEqual({"offset": 11, "length": 5, "msb_right": 0}, data["bitfields"]["red"])
        self.assertEqual(1000, data["fixed_ioctl_us"])
        self.assertTrue(data["fixed_timing_valid"])
        self.assertEqual(1000, data["variable_ioctl_us"])
        self.assertTrue(data["variable_timing_valid"])
        self.assertEqual(3, data["accel_id"])
        self.assertIn("open=1 close=1 fix=1 var=1 bad_ioctl=0", result.stderr)
        flags = int(result.stderr.rsplit("flags=", 1)[1].split()[0])
        self.assertEqual(os.O_RDONLY, flags & os.O_ACCMODE)
        if hasattr(os, "O_CLOEXEC"):
            self.assertTrue(flags & os.O_CLOEXEC)

    def test_invalid_stride_emits_invalid_geometry(self):
        result = self.run_probe("bad_stride")
        self.assertEqual(3, result.returncode)
        data = json.loads(result.stdout)
        self.assertFalse(data["geometry_valid"])
        self.assertEqual(0, data["complete_pages"])
        self.assertIn("bad_ioctl=0", result.stderr)

    def test_insufficient_frame_memory_is_reported(self):
        result = self.run_probe("short_memory")
        self.assertEqual(3, result.returncode)
        data = json.loads(result.stdout)
        self.assertFalse(data["geometry_valid"])
        self.assertFalse(data["virtual_memory_bounds_valid"])
        self.assertEqual(0, data["complete_pages"])
        self.assertIn("bad_ioctl=0", result.stderr)

    def test_scanout_offset_must_fit_virtual_geometry(self):
        result = self.run_probe("bad_offset")
        self.assertEqual(3, result.returncode)
        data = json.loads(result.stdout)
        self.assertFalse(data["geometry_valid"])
        self.assertFalse(data["offsets_valid"])
        self.assertIn("bad_ioctl=0", result.stderr)

    def test_active_page_must_fit_framebuffer_memory(self):
        result = self.run_probe("active_oob")
        self.assertEqual(3, result.returncode)
        data = json.loads(result.stdout)
        self.assertFalse(data["geometry_valid"])
        self.assertEqual(1, data["complete_pages"])
        self.assertFalse(data["active_viewport_memory_valid"])
        self.assertFalse(data["virtual_memory_bounds_valid"])
        self.assertEqual(1536000, data["active_viewport_end_bytes"])
        self.assertIn("bad_ioctl=0", result.stderr)

    def test_stride_must_cover_wide_virtual_row(self):
        result = self.run_probe("wide_virtual")
        self.assertEqual(3, result.returncode)
        data = json.loads(result.stdout)
        self.assertFalse(data["geometry_valid"])
        self.assertFalse(data["stride_valid"])
        self.assertIn("bad_ioctl=0", result.stderr)

    def test_padded_stride_covering_virtual_width_is_valid(self):
        result = self.run_probe("padded_virtual")
        self.assertEqual(0, result.returncode, result.stderr)
        data = json.loads(result.stdout)
        self.assertTrue(data["stride_valid"])
        self.assertEqual(1408, data["stride_bytes"])
        self.assertTrue(data["virtual_memory_bounds_valid"])
        self.assertEqual(2, data["complete_pages"])

    def test_virtual_extent_can_exceed_memory_when_active_viewport_fits(self):
        result = self.run_probe("virtual_extent")
        self.assertEqual(0, result.returncode, result.stderr)
        data = json.loads(result.stdout)
        self.assertTrue(data["geometry_valid"])
        self.assertTrue(data["active_viewport_memory_valid"])
        self.assertFalse(data["virtual_memory_bounds_valid"])

    def test_rgb565_support_is_separate_from_geometry(self):
        result = self.run_probe("unsupported_format")
        self.assertEqual(0, result.returncode, result.stderr)
        data = json.loads(result.stdout)
        self.assertTrue(data["geometry_valid"])
        self.assertFalse(data["rgb565_supported"])
        self.assertEqual(1, data["grayscale"])
        self.assertEqual(0x1234, data["var_accel_flags"])

    def test_large_fields_do_not_overflow_extent_arithmetic(self):
        result = self.run_probe("overflow")
        self.assertEqual(3, result.returncode)
        data = json.loads(result.stdout)
        self.assertFalse(data["geometry_valid"])
        self.assertFalse(data["active_viewport_memory_valid"])
        self.assertEqual(2**64 - 1, data["active_viewport_end_bytes"])
        self.assertIn("bad_ioctl=0", result.stderr)

    def test_zero_dimensions_are_rejected(self):
        result = self.run_probe("zero_width")
        self.assertEqual(3, result.returncode)
        data = json.loads(result.stdout)
        self.assertFalse(data["geometry_valid"])

    def test_fixed_info_ioctl_error_is_reported(self):
        result = self.run_probe("fix_error")
        self.assertEqual(1, result.returncode)
        self.assertIn("FBIOGET_FSCREENINFO failed", result.stderr)
        self.assertEqual("", result.stdout)
        self.assertIn("open=1 close=1 fix=1 var=0 bad_ioctl=0", result.stderr)

    def test_variable_info_ioctl_error_is_reported(self):
        result = self.run_probe("var_error")
        self.assertEqual(1, result.returncode)
        self.assertIn("FBIOGET_VSCREENINFO failed", result.stderr)
        self.assertEqual("", result.stdout)
        self.assertIn("open=1 close=1 fix=1 var=1 bad_ioctl=0", result.stderr)

    def test_clock_failures_do_not_discard_metadata_or_ioctl_errors(self):
        result = self.run_probe("clock_before")
        self.assertEqual(0, result.returncode, result.stderr)
        data = json.loads(result.stdout)
        self.assertFalse(data["fixed_timing_valid"])
        self.assertEqual(0, data["fixed_ioctl_us"])
        self.assertTrue(data["variable_timing_valid"])
        self.assertIn("bad_ioctl=0", result.stderr)

        result = self.run_probe("clock_after")
        self.assertEqual(0, result.returncode, result.stderr)
        data = json.loads(result.stdout)
        self.assertFalse(data["fixed_timing_valid"])
        self.assertEqual(0, data["fixed_ioctl_us"])
        self.assertIn("bad_ioctl=0", result.stderr)

        for case in ("clock_before_fix_error", "clock_after_fix_error"):
            with self.subTest(case=case):
                result = self.run_probe(case)
                self.assertEqual(1, result.returncode)
                self.assertIn("FBIOGET_FSCREENINFO failed: Input/output error", result.stderr)
                self.assertNotIn("clock error", result.stderr)
                self.assertIn("bad_ioctl=0", result.stderr)

    def test_output_write_error_has_distinct_exit_status(self):
        if not Path("/dev/full").exists():
            self.skipTest("/dev/full is unavailable")
        env = os.environ.copy()
        env["PROBE_CASE"] = "valid"
        with open("/dev/full", "wb") as output:
            result = subprocess.run([str(self.binary)], env=env, text=True,
                                    stdout=output, stderr=subprocess.PIPE, timeout=5)
        self.assertEqual(4, result.returncode)
        self.assertIn("output failed", result.stderr)
        self.assertIn("bad_ioctl=0", result.stderr)


if __name__ == "__main__":
    unittest.main()
