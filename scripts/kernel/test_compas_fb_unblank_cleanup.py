#!/usr/bin/env python3
"""Compile and exercise the actual framebuffer unblank cleanup function."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "firmware/kernel/display-experimental/compas_fb_driver.c"

PREFIX = r"""
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define FB_BLANK_UNBLANK 0
#define COMPAS_FB_HEIGHT 800U
#define THIS_MODULE ((void *)0)
struct mutex { int unused; };
struct spinlock { int unused; };
struct compas_fb_hw_ops { int unused; };
struct fb_var_screeninfo { unsigned int yoffset; };
struct fb_info { struct fb_var_screeninfo var; };
struct panel_ops {
	int (*power_on)(void *);
	int (*power_off)(void *);
};
struct compas_fb_device {
	struct mutex op_mutex;
	struct spinlock irq_lock;
	struct compas_fb_hw_ops hw_ops;
	struct panel_ops panel;
	int pan;
	unsigned int committed_page;
	bool initialized, blanked, quarantined, pinned, panel_powered;
};
static struct compas_fb_device r1fb;
static int injected_start, injected_stop, injected_pin, injected_on, injected_off;
static int power_off_calls, pin_calls, disable_calls, quarantine_calls;
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
#define spin_lock_irqsave(p, f) do { (void)(p); (void)sizeof(f); } while (0)
#define spin_unlock_irqrestore(p, f) do { (void)(p); (void)sizeof(f); } while (0)
static int enable_clocks(void) { return 0; }
static void disable_clocks(void) { ++disable_calls; }
static int establish_idle_locked(void) { return 0; }
static int scanout_irq_faulted(void) { return 0; }
static int set_pixel_clock(void) { return 0; }
static int validate_pixel_clock_rate(bool initial) { (void)initial; return 0; }
static int clear_pixel_clock_invert(void) { return 0; }
static int select_display_pins(void) { return 0; }
static int select_power_off_pins(void) { ++pin_calls; return injected_pin; }
static int compas_fb_hw_program_r1_tft(struct compas_fb_hw_ops *ops)
{ (void)ops; return 0; }
static int compas_fb_pan_start(int *pan) { (void)pan; return 0; }
static int start_first_frame_locked(unsigned int page)
{ (void)page; return injected_start; }
static int stop_scanout_locked(void) { return injected_stop; }
static int panel_power_on(void *unused) { (void)unused; return injected_on; }
static int panel_power_off(void *unused)
{ (void)unused; ++power_off_calls; return injected_off; }
static void quarantine_resources(const char *reason)
{ (void)reason; ++quarantine_calls; r1fb.quarantined = true; }
static void __module_get(void *module) { (void)module; }
"""

SUFFIX = r"""
static void reset_case(void)
{
	r1fb = (struct compas_fb_device){0};
	r1fb.initialized = true;
	r1fb.blanked = true;
	r1fb.panel.power_on = panel_power_on;
	r1fb.panel.power_off = panel_power_off;
	injected_start = injected_stop = injected_pin = 0;
	injected_on = injected_off = 0;
	power_off_calls = pin_calls = disable_calls = quarantine_calls = 0;
}

static void expect(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

int main(void)
{
	struct fb_info info = {0};
	int ret;

	/* Start timeout survives successful stop, pin park, and panel rollback. */
	reset_case();
	injected_start = -ETIMEDOUT;
	ret = compas_fb_blank(FB_BLANK_UNBLANK, &info);
	expect(ret == -ETIMEDOUT && r1fb.blanked && !r1fb.quarantined,
	       "start error is returned after successful cleanup");
	expect(power_off_calls == 1 && disable_calls == 1,
	       "successful cleanup powers down once and disables clocks");

	/* A power-on failure also survives successful rollback. */
	reset_case();
	injected_on = -EIO;
	ret = compas_fb_blank(FB_BLANK_UNBLANK, &info);
	expect(ret == -EIO && r1fb.blanked && power_off_calls == 1,
	       "power-on error is returned after successful rollback");

	/* A failed stop takes precedence and prevents later cleanup operations. */
	reset_case();
	injected_start = -ETIMEDOUT;
	injected_stop = -EAGAIN;
	ret = compas_fb_blank(FB_BLANK_UNBLANK, &info);
	expect(ret == -EAGAIN && r1fb.quarantined && power_off_calls == 0 &&
	       pin_calls == 0 && disable_calls == 0,
	       "stop cleanup error is returned and resources are retained");

	/* Pin-park failure is reported and does not proceed to power-off. */
	reset_case();
	injected_start = -ETIMEDOUT;
	injected_pin = -EACCES;
	ret = compas_fb_blank(FB_BLANK_UNBLANK, &info);
	expect(ret == -EACCES && r1fb.quarantined && power_off_calls == 0 &&
	       disable_calls == 0,
	       "pin cleanup error is returned and clocks remain on");

	/* Power-off failure is reported without falsely declaring blanked. */
	reset_case();
	injected_start = -ETIMEDOUT;
	injected_off = -EBUSY;
	ret = compas_fb_blank(FB_BLANK_UNBLANK, &info);
	expect(ret == -EBUSY && r1fb.quarantined && !r1fb.blanked &&
	       r1fb.panel_powered && disable_calls == 0,
	       "panel cleanup error is returned with powered state retained");

	puts("unblank cleanup error paths passed");
	return 0;
}
"""


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
    raise AssertionError("function body did not close")


class UnblankCleanupTest(unittest.TestCase):
    def test_injected_cleanup_paths(self):
        source = SOURCE.read_text()
        function = extract_function(source, "static int compas_fb_blank(")
        with tempfile.TemporaryDirectory() as temp_dir:
            harness = Path(temp_dir) / "unblank_cleanup.c"
            binary = Path(temp_dir) / "unblank_cleanup"
            harness.write_text(PREFIX + function + SUFFIX)
            subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", str(harness), "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
