#!/usr/bin/env python3
"""Compile and exercise the continuous-pan fence with scripted fake MMIO."""
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class CompasFbLivePanTests(unittest.TestCase):
    def test_fake_mmio_live_pan_harness(self):
        with tempfile.TemporaryDirectory(prefix="compas-fb-live-pan-") as tmp:
            binary = pathlib.Path(tmp) / "compas_fb_live_pan_harness"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(ROOT / "scripts/kernel/compas_fb_live_pan_harness.c"),
                    str(ROOT / "firmware/kernel/display-experimental/compas_fb_hw.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
                cwd=ROOT,
            )
            result = subprocess.run(
                [str(binary)], check=True, text=True, capture_output=True
            )
            self.assertIn("compas_fb_live_pan: PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
