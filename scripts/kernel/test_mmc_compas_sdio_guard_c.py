#!/usr/bin/env python3
"""Compile and execute the actual helper C extracted from the opt-in patch."""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
PATCH = ROOT / "firmware/kernel/wifi-patches/compas-mmc-radio-lifetime.patch"
HARNESS = pathlib.Path(__file__).with_name("mmc_compas_sdio_guard_harness.c")


def extract_helper(patch_text: str) -> str:
    start = patch_text.index("+struct mmc_compas_sdio_guard {")
    end = patch_text.index("\n /*\n  * Flush the cache", start)
    added = patch_text[start:end]
    lines = []
    for line in added.splitlines():
        if not line.startswith("+"):
            raise AssertionError(f"unexpected non-addition in helper block: {line}")
        lines.append(line[1:])
    return "\n".join(lines) + "\n"


def extract_rescan_guard(patch_text: str) -> str:
    marker = "+\t/* A retained board radio card is physically off until guarded bring-up. */"
    start = patch_text.find(marker)
    if start < 0:
        raise AssertionError("actual mmc_rescan board-off guard not found")
    added = []
    for line in patch_text[start:].splitlines():
        if not line.startswith("+"):
            break
        added.append(line[1:])
    block = "\n".join(added)
    if "if (mmc_compas_sdio_board_off(host))" not in block or "return;" not in block:
        raise AssertionError("incomplete actual mmc_rescan guard")
    return block


def extract_sdio_pm_hooks(patch_text: str) -> tuple[str, str]:
    sdio = patch_text.split("--- a/drivers/mmc/core/sdio.c", 1)[1]
    hook_marker = "+\tif (mmc_compas_sdio_pm_off(host))"
    # Check that suspend's hook precedes the first host claim and resume's
    # hook follows both BUG_ON checks and precedes card reinitialization.
    if "+\tif (mmc_compas_sdio_pm_off(host))\n+\t\treturn 0;\n+\n \tmmc_claim_host(host);" not in sdio:
        raise AssertionError("SDIO suspend hook placement changed")
    resume_anchor = (
        " \tBUG_ON(!host);\n \tBUG_ON(!host->card);\n+\n"
        "+\tif (mmc_compas_sdio_pm_off(host))\n+\t\treturn 0;\n \n"
        " \t/* Basic card reinitialization. */"
    )
    if resume_anchor not in sdio:
        raise AssertionError("SDIO resume hook placement changed")
    locations = [i for i in range(len(sdio)) if sdio.startswith(hook_marker, i)]
    if len(locations) != 2:
        raise AssertionError("expected both SDIO PM hook blocks")

    def added_hook_at(index: int) -> str:
        added = []
        for line in sdio[index:].splitlines():
            if not line.startswith("+"):
                break
            added.append(line[1:])
            if "return 0;" in line:
                break
        if len(added) != 2 or "return 0;" not in added[-1]:
            raise AssertionError("incomplete SDIO PM hook")
        return "\n".join(added)

    return added_hook_at(locations[0]), added_hook_at(locations[1])


class ActualMmcGuardCHarness(unittest.TestCase):
    def test_extracted_kernel_helpers_against_sdk_layout_stubs(self):
        compiler = shutil.which("cc") or shutil.which("gcc")
        if not compiler:
            self.skipTest("host C compiler unavailable")
        harness = HARNESS.read_text()
        marker = "/* ACTUAL_HELPER_SOURCE */"
        self.assertEqual(harness.count(marker), 1)
        patch_text = PATCH.read_text()
        suspend_hook, resume_hook = extract_sdio_pm_hooks(patch_text)
        source = harness.replace(marker, extract_helper(patch_text))
        rescan_marker = "/* ACTUAL_RESCAN_GUARD */"
        self.assertEqual(source.count(rescan_marker), 1)
        source = source.replace(rescan_marker, extract_rescan_guard(patch_text))
        for pm_marker, hook in (("/* ACTUAL_PM_SUSPEND_HOOK */", suspend_hook),
                                ("/* ACTUAL_PM_RESUME_HOOK */", resume_hook)):
            self.assertEqual(source.count(pm_marker), 1)
            source = source.replace(pm_marker, hook)
        with tempfile.TemporaryDirectory(prefix="mmc-guard-c-harness-") as temp:
            temp_path = pathlib.Path(temp)
            source_path = temp_path / "harness.c"
            binary_path = temp_path / "harness"
            source_path.write_text(source)
            compile_result = subprocess.run(
                [compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 str(source_path), "-o", str(binary_path)],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout + compile_result.stderr)
            run_result = subprocess.run([str(binary_path)], text=True,
                                        capture_output=True, check=False)
            self.assertEqual(run_result.returncode, 0,
                             run_result.stdout + run_result.stderr)
            self.assertIn("PASS (7 scenarios)", run_result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
