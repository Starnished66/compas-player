#!/usr/bin/env python3
"""Compile and exercise the isolated R1 display backend with fake MMIO."""
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class CompasFbHardwareTests(unittest.TestCase):
    def test_fake_mmio_harness(self):
        with tempfile.TemporaryDirectory(prefix="compas-fb-hw-") as tmp:
            binary = pathlib.Path(tmp) / "compas_fb_hw_harness"
            subprocess.run(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(ROOT / "scripts/kernel/compas_fb_hw_harness.c"),
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
            self.assertIn("compas_fb_hw: PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
