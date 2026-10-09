#!/usr/bin/env python3
"""Host tests for R1 LCD clock-register and pixel-period helpers."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "firmware/kernel/display-experimental/compas_fb_clock.c"

STUBS = {
    "linux/types.h": "typedef unsigned int u32; typedef unsigned long long u64;\n",
    "linux/errno.h": "#define EINVAL 22\n#define EBUSY 16\n#define ERANGE 34\n",
    "linux/math64.h": "static inline unsigned long long div_u64(unsigned long long a, unsigned long long b) { return a / b; }\n",
}

HARNESS = r'''#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include "compas_fb_clock.h"

int main(void) {
 uint32_t after=0, before=0x45000000U | COMPAS_FB_CLOCK_INVERT_BIT;
 assert(compas_fb_clock_clear_invert(before,&after)==0);
 assert(after==(before & ~COMPAS_FB_CLOCK_INVERT_BIT));
 assert((after & ~COMPAS_FB_CLOCK_INVERT_BIT)==(before & ~COMPAS_FB_CLOCK_INVERT_BIT));
 assert(compas_fb_clock_clear_invert(after,&before)==0 && before==after);
 assert(compas_fb_clock_clear_invert(COMPAS_FB_CLOCK_BUSY_BIT,&after)==-EBUSY);
 assert(compas_fb_clock_clear_invert(COMPAS_FB_CLOCK_CE_BIT,&after)==-EBUSY);
 assert(compas_fb_clock_clear_invert(0,&after)==0 && after==0);
 assert(compas_fb_clock_clear_invert(0,0)==-EINVAL);
 assert(compas_fb_clock_rate_valid(28303248ULL,28303248ULL));
 assert(compas_fb_clock_rate_valid(25472924ULL,28303248ULL));
 assert(compas_fb_clock_rate_valid(31133572ULL,28303248ULL));
 assert(!compas_fb_clock_rate_valid(25472923ULL,28303248ULL));
 assert(!compas_fb_clock_rate_valid(31133573ULL,28303248ULL));
 assert(!compas_fb_clock_rate_valid(0,28303248ULL));
 assert(!compas_fb_clock_rate_valid(1,0));
 assert(compas_fb_clock_pixclock_ps(28303248ULL,&after)==0 && after==35332U);
 assert(compas_fb_clock_pixclock_ps(28000000ULL,&after)==0 && after==35714U);
 assert(compas_fb_clock_pixclock_ps(0,&after)==-EINVAL);
 assert(compas_fb_clock_pixclock_ps(28000000ULL,0)==-EINVAL);
 return 0;
}
'''


class CompasFramebufferClockTest(unittest.TestCase):
    def test_clock_helpers(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if not compiler:
            self.skipTest("host C compiler unavailable")
        with tempfile.TemporaryDirectory(prefix="compas-fb-clock-") as temp:
            base = pathlib.Path(temp)
            include = base / "include"
            for name, content in STUBS.items():
                path = include / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            (include / "compas_fb_clock.h").write_text(
                (ROOT / "firmware/kernel/display-experimental/compas_fb_clock.h").read_text()
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
