#!/usr/bin/env python3
"""Host tests for fixed R1 framebuffer mode, palette, and mmap bounds."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "firmware/kernel/display-experimental/compas_fb_fops.c"

STUBS = {
    "linux/fb.h": r'''#ifndef TEST_LINUX_FB_H
#define TEST_LINUX_FB_H
typedef unsigned int u32;
struct fb_bitfield { unsigned int offset, length, msb_right; };
struct fb_var_screeninfo {
 unsigned int xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
 unsigned int bits_per_pixel, grayscale, nonstd, vmode;
 unsigned int pixclock, left_margin, right_margin, upper_margin, lower_margin;
 unsigned int hsync_len, vsync_len, sync;
 struct fb_bitfield red, green, blue, transp;
};
struct fb_info { void *pseudo_palette; };
#define FB_VMODE_NONINTERLACED 0U
#define FB_SYNC_HOR_HIGH_ACT 1U
#define FB_SYNC_VERT_HIGH_ACT 2U
#endif
''',
    "linux/mm.h": r'''#ifndef TEST_LINUX_MM_H
#define TEST_LINUX_MM_H
#define PAGE_SHIFT 12
struct vm_area_struct { unsigned long vm_start, vm_end, vm_pgoff; };
#endif
''',
    "linux/errno.h": r'''#define EINVAL 22''',
    "linux/kernel.h": r'''#define ULONG_MAX (~0UL)''',
    "linux/types.h": r'''typedef unsigned int u32;''',
}

HARNESS = r'''#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "compas_fb_fops.h"

static struct fb_var_screeninfo valid_mode(void) {
 struct fb_var_screeninfo v;
 memset(&v, 0, sizeof(v));
 v.xres=480; v.yres=800; v.xres_virtual=480; v.yres_virtual=1600;
 v.yoffset=800; v.bits_per_pixel=16;
 v.red.offset=11; v.red.length=5;
 v.green.offset=5; v.green.length=6;
 v.blue.offset=0; v.blue.length=5;
 v.pixclock=35332; v.left_margin=24; v.right_margin=24;
 v.upper_margin=8; v.lower_margin=14; v.hsync_len=24; v.vsync_len=5;
 v.sync=FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT;
 v.vmode=FB_VMODE_NONINTERLACED;
 return v;
}
int main(void) {
 struct fb_var_screeninfo v=valid_mode(), before;
 struct vm_area_struct area;
 struct fb_info info;
 unsigned int colors[16];
 before=v;
 assert(compas_fb_check_var(&v,35332)==0);
 assert(memcmp(&before, &v, sizeof(v))==0);
 v.yoffset=801; assert(compas_fb_check_var(&v,35332)==-EINVAL);
 v=before; v.green.length=5; assert(compas_fb_check_var(&v,35332)==-EINVAL);
 v=before; v.vmode=1; assert(compas_fb_check_var(&v,35332)==-EINVAL);
 v=before; v.nonstd=1; assert(compas_fb_check_var(&v,35332)==-EINVAL);
 v=before; v.xres=479; assert(compas_fb_check_var(&v,35332)==-EINVAL);
 v=before; v.pixclock=35714;
 assert(compas_fb_check_var(&v,35332)==-EINVAL);
 assert(compas_fb_check_var(&v,35714)==0);
 info.pseudo_palette=colors;
 assert(compas_fb_setcolreg(0, 0xffff, 0xffff, 0xffff, 0, &info)==0);
 assert(colors[0]==0xffffU);
 assert(compas_fb_setcolreg(15, 0, 0, 0, 0, &info)==0);
 assert(colors[15]==0);
 assert(compas_fb_setcolreg(16, 0, 0, 0, 0, &info)==-EINVAL);
 area.vm_start=0x1000; area.vm_end=0x1000+768000; area.vm_pgoff=0;
 assert(compas_fb_mmap_bounds(&area, 1536000)==0);
 area.vm_pgoff=188; /* byte offset is 770048; complete range exceeds memory. */
 assert(compas_fb_mmap_bounds(&area, 1536000)==-EINVAL);
 area.vm_pgoff=0; area.vm_end=area.vm_start;
 assert(compas_fb_mmap_bounds(&area, 1536000)==-EINVAL);
 area.vm_end=area.vm_start+4096; area.vm_pgoff=(~0UL >> 12)+1;
 assert(compas_fb_mmap_bounds(&area, 1536000)==-EINVAL);
 puts("compas_fb_fops passed (mode, palette, mmap bounds)");
 return 0;
}
'''


class CompasFramebufferFopsTest(unittest.TestCase):
    def test_mode_palette_and_mapping_bounds(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if not compiler:
            self.skipTest("host C compiler unavailable")
        with tempfile.TemporaryDirectory(prefix="compas-fb-fops-") as temp:
            base = pathlib.Path(temp)
            include = base / "include"
            for name, content in STUBS.items():
                path = include / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            (include / "compas_fb_fops.h").write_text(
                (ROOT / "firmware/kernel/display-experimental/compas_fb_fops.h").read_text()
            )
            harness = base / "harness.c"
            harness.write_text(HARNESS)
            binary = base / "test"
            result = subprocess.run(
                [compiler, "-std=gnu89", "-Wall", "-Wextra", "-Werror",
                 "-I", str(include), str(SOURCE), str(harness), "-o", str(binary)],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True, check=False)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("passed", run.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
