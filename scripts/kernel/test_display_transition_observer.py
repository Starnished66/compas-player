#!/usr/bin/env python3
"""Host-only CLI and read-only-contract checks for the DPU transition observer."""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
SOURCE = REPO / "scripts/kernel/display_transition_observer.c"


class DisplayTransitionObserverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.temp.name) / "display-transition-observer"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                   "-Werror", "-O2", str(SOURCE), "-o", str(cls.binary)]
        result = subprocess.run(command, text=True, capture_output=True, timeout=30)
        if result.returncode:
            raise RuntimeError(f"observer host build failed: {result.stderr}")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temp.cleanup()

    def run_cli(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([str(self.binary), *args], text=True,
                              capture_output=True, timeout=3)

    def test_help_is_available_without_device_access(self) -> None:
        result = self.run_cli("--help")
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("--duration-ms 1..3000", result.stdout)
        self.assertIn("--interval-us 250..1000", result.stdout)
        self.assertEqual("", result.stderr)

    def test_invalid_duration_and_interval_exit_before_device_access(self) -> None:
        cases = (
            ("--duration-ms", "0"),
            ("--duration-ms", "3001"),
            ("--duration-ms", "-1"),
            ("--duration-ms", "1x"),
            ("--duration-ms", "1", "--duration-ms", "2"),
            ("--interval-us", "249"),
            ("--interval-us", "1001"),
            ("--interval-us", "+250"),
            ("--unknown",),
        )
        for args in cases:
            with self.subTest(args=args):
                result = self.run_cli(*args)
                self.assertEqual(2, result.returncode)
                self.assertEqual("", result.stdout)
                self.assertIn("Usage:", result.stderr)
                self.assertNotIn("open /dev/mem", result.stderr)

    def test_source_uses_read_only_exact_mmio_mapping_and_bounded_capture(self) -> None:
        source = SOURCE.read_text()
        self.assertIn('open("/dev/mem", O_RDONLY | O_CLOEXEC)', source)
        self.assertIn("mmap(NULL, DPU_WINDOW_SIZE, PROT_READ, MAP_SHARED, fd,", source)
        self.assertIn("DPU_PHYS_BASE UINT32_C(0x13050000)", source)
        self.assertIn("DPU_WINDOW_SIZE UINT32_C(0x00010000)", source)
        self.assertIn("MAX_SAMPLES 12000U", source)
        self.assertIn("MAX_DURATION_MS 3000U", source)
        self.assertIn("MIN_INTERVAL_US 250U", source)
        self.assertNotIn("PROT_WRITE", source)
        self.assertNotIn("O_RDWR", source)
        self.assertNotIn("/dev/fb0", source)
        self.assertNotIn("ioctl(", source)


if __name__ == "__main__":
    unittest.main()
