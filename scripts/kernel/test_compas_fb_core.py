#!/usr/bin/env python3
"""Compile and run the hardware-independent experimental framebuffer core."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
CORE = ROOT / "firmware/kernel/display-experimental/compas_fb_core.c"
HARNESS = pathlib.Path(__file__).with_name("compas_fb_core_test.c")


class CompasFramebufferCoreTest(unittest.TestCase):
    def test_geometry_and_pan_state_machine(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if not compiler:
            self.skipTest("host C compiler unavailable")
        with tempfile.TemporaryDirectory(prefix="compas-fb-core-") as temp:
            temp_path = pathlib.Path(temp)
            base_args = [compiler, "-std=gnu89", "-Wall", "-Wextra", "-Werror"]
            source_args = [str(CORE), str(HARNESS)]

            def compile_and_run(binary: pathlib.Path, flags: list[str]) -> subprocess.CompletedProcess[str]:
                result = subprocess.run(
                    base_args + flags + source_args + ["-o", str(binary)],
                    text=True, capture_output=True, check=False,
                )
                self.assertEqual(result.returncode, 0,
                                 result.stdout + result.stderr)
                run = subprocess.run([str(binary)], text=True,
                                     capture_output=True, check=False)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn("passed (6 groups)", run.stdout)
                return run

            compile_and_run(temp_path / "compas_fb_core_test", [])
            sanitizer_flags = ["-fsanitize=undefined", "-fno-sanitize-recover=all"]
            supported = subprocess.run(
                base_args + sanitizer_flags + ["-x", "c", "-", "-o", str(temp_path / "ubsan_probe")],
                input="int main(void) { return 0; }\n",
                text=True, capture_output=True, check=False,
            )
            if supported.returncode == 0:
                compile_and_run(temp_path / "compas_fb_core_ubsan", sanitizer_flags)



if __name__ == "__main__":
    unittest.main(verbosity=2)
