#!/usr/bin/env python3
"""Focused tests for provenance-pinned experimental radio assembly."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import shutil
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
STAGER = REPO / "scripts/kernel/stage_experimental_radio.py"
BASE = REPO.parent / "compas-ui-stall-fixes-20261008/flash/verify/root"
ABI_MODULES = {
    "brcmfmac.ko": "brcmfmac",
    "brcmutil.ko": "brcmutil",
    "bcm_wlbt_power.ko": "bcm_wlbt_power",
}


def load_stager():
    spec = importlib.util.spec_from_file_location("stage_experimental_radio_pinning", STAGER)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def copy_base_fixture(destination: Path) -> None:
    stage = load_stager()
    for relative in stage.EXPECTED:
        source = BASE / relative
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    for name in stage.WIFI_NAMES:
        source = BASE / "lib/firmware/wifi_bcm" / name
        target = destination / "lib/firmware/wifi_bcm" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    for name in stage.BT_NAMES:
        source = BASE / "lib/firmware/bt_bcm" / name
        target = destination / "lib/firmware/bt_bcm" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    for name in stage.RETAINED_VENDOR_MODULES:
        target = destination / "module_driver" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(("vendor:" + name).encode())


def make_provenance(temp: Path, modules: Path, base: Path) -> Path:
    stage = load_stager()
    kernel_dir = temp / "kernel-inputs"
    kernel_dir.mkdir()
    kernel_paths = {}
    for key in ("ximage", "config", "system_map"):
        path = kernel_dir / key
        if key == "config":
            path.write_text("CONFIG_BRCMFMAC=m\nCONFIG_BRCMUTIL=m\n# CONFIG_MODULE_FORCE_UNLOAD is not set\n")
        else:
            path.write_bytes(f"pinned-{key}".encode())
        kernel_paths[key] = {"path": str(path), "sha256": sha(path)}
    provider_source = temp / "provider.c"
    provider_source.write_text("static int candidate_provider;\n")
    report = {
        "valid": True,
        "selected_module_count": 31,
        "system_map_sha256": kernel_paths["system_map"]["sha256"],
        "missing_imports_by_module": {},
        "vermagic_errors": [],
        "selected_modules": {},
    }
    module_hashes = {}
    all_built = (*stage.REBUILT_DRIVER_MODULES, "bcm_wlbt_power.ko", "brcmfmac.ko", "brcmutil.ko")
    all_vendor = stage.RETAINED_VENDOR_MODULES
    for filename in all_built:
        module_path = modules / filename
        module_path.write_bytes(("pinned:" + filename).encode())
        module_hashes[filename] = sha(module_path)
        report["selected_modules"][Path(filename).stem] = {"sha256": module_hashes[filename], "source": "built"}
    for filename in all_vendor:
        module_path = base / "module_driver" / filename
        module_hashes[filename] = sha(module_path)
        report["selected_modules"][Path(filename).stem] = {"sha256": module_hashes[filename], "source": "vendor"}
    report_path = temp / "abi-report.json"
    report_path.write_text(json.dumps(report, sort_keys=True))
    module_patches = []
    for relative in (
        "firmware/kernel/module-patches/compas-radio-input-safety.patch",
        "firmware/kernel/module-patches/compas-radio-lifecycle.patch",
    ):
        source = REPO / relative
        module_patches.append({"path": str(source), "sha256": sha(source)})
    lifetime = REPO / "firmware/kernel/wifi-patches/compas-mmc-radio-lifetime.patch"
    power = REPO / "firmware/kernel/wifi-experimental/rootfs/module_driver/bcm_wlbt_power.sh"
    provenance = {
        "kernel": {
            **kernel_paths,
            "lifetime_patch": {"path": str(lifetime), "sha256": sha(lifetime)},
            "wifi_config_sha256": sha(REPO / "firmware/kernel/compas-r1-brcmfmac.config"),
        },
        "mixed_static_abi": {
            "report": str(report_path),
            "report_sha256": sha(report_path),
            "modules": {name: {"sha256": value} for name, value in module_hashes.items()},
        },
        "provider_module": {
            "path": str(modules / "bcm_wlbt_power.ko"),
            "sha256": module_hashes["bcm_wlbt_power.ko"],
        },
        "provider_source": {"path": str(provider_source), "sha256": sha(provider_source)},
        "module_patches": module_patches,
    }
    path = temp / "candidate-provenance.json"
    path.write_text(json.dumps(provenance, sort_keys=True))
    assert power.exists()
    return path


class StagePinnedRadioTests(unittest.TestCase):
    def test_exact_ap6212a_loader_is_repository_pinned(self):
        stage = load_stager()
        power = stage.checked_power_init(REPO)
        self.assertEqual(stage.EXPECTED_POWER_INIT_SHA256, sha(power))
        self.assertEqual(
            "insmod bcm_wlbt_power.ko wl_reg_on=PB03 wl_host_wake=PA08 wl_mmc=0 "
            "bt_reg_on=PB04 host_wake_bt=PB05 profile=ap6212a\n",
            power.read_text(),
        )

    def test_player_input_must_be_mips32_and_hash_is_available_for_assembly(self):
        stage = load_stager()
        stage.checked_player(REPO / "compas_player_target")
        with tempfile.TemporaryDirectory() as td:
            wrong = Path(td) / "player"
            wrong.write_bytes(b"not an ELF executable")
            with self.assertRaisesRegex(ValueError, "MIPS executable"):
                stage.checked_player(wrong)

    def test_stage_requires_matching_provenance_and_avoids_duplicate_provider_copy(self):
        stage = load_stager()
        with tempfile.TemporaryDirectory() as td:
            temp = Path(td)
            base = temp / "base"
            copy_base_fixture(base)
            for name in stage.RETAINED_VENDOR_MODULES:
                target = base / "module_driver" / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(("vendor:" + name).encode())
            modules = temp / "modules"
            modules.mkdir()
            provenance = make_provenance(temp, modules, base)
            output = temp / "overlay"
            stage.stage(base, modules, output, REPO, provenance, REPO / "compas_player_target")
            self.assertFalse((output / "usr/lib/compas-radio/modules/bcm_wlbt_power.ko").exists())
            self.assertTrue((output / "module_driver/bcm_wlbt_power.ko").is_file())
            for name in stage.REBUILT_DRIVER_MODULES:
                self.assertTrue((output / "module_driver" / name).is_file(), name)
            for name in stage.RETAINED_VENDOR_MODULES:
                self.assertFalse((output / "module_driver" / name).exists())
            self.assertTrue((output / "usr/lib/compas-radio/modules/brcmfmac.ko").is_file())
            assembly = json.loads((output / "CANDIDATE-ASSEMBLY.json").read_text())
            self.assertEqual("host-only candidate; not validated on device; not a flash/package artifact",
                             assembly["status"])
            self.assertEqual("ap6212a", assembly["wifi_profile"])
            self.assertEqual(sha(REPO / "firmware/kernel/wifi-experimental/rootfs/usr/bin/compas-radio"),
                             assembly["radio_control"]["sha256"])
            self.assertEqual(assembly["radio_control"]["sha256"], sha(output / "usr/bin/compas-radio"))
            for relative, expected in assembly["runtime_files"].items():
                self.assertEqual(expected, sha(output / relative), relative)
            for relative, expected in assembly["payload_metadata"].items():
                self.assertEqual(expected, sha(output / relative), relative)
            self.assertNotIn("usr/bin/compas_player", assembly["runtime_files"])
            self.assertIn("module_driver/driver_default_init_script.sh", assembly["runtime_files"])
            self.assertIn("usr/libexec/compas/radio-bt/bt_init.vendor.sh", assembly["runtime_files"])
            self.assertIn("system_map", assembly["kernel"])
            self.assertTrue(assembly["module_abi_report"]["valid"])
            self.assertEqual(len(assembly["modules"]), 31)
            self.assertEqual(len(list((output / "module_driver").glob("*.ko"))), 23)
            self.assertEqual(set(assembly["retained_vendor_modules"]), set(stage.RETAINED_VENDOR_MODULES))
            for name in stage.RETAINED_VENDOR_MODULES:
                self.assertEqual(assembly["retained_vendor_modules"][name], sha(base / "module_driver" / name))
            self.assertEqual(sha(REPO / "compas_player_target"), assembly["player"]["sha256"])
            self.assertEqual(sha(REPO / "compas_player_target"), sha(output / "usr/bin/compas_player"))

    def test_tampered_module_is_rejected_before_overlay_creation(self):
        stage = load_stager()
        with tempfile.TemporaryDirectory() as td:
            temp = Path(td)
            base = temp / "base"
            copy_base_fixture(base)
            for name in stage.RETAINED_VENDOR_MODULES:
                target = base / "module_driver" / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(("vendor:" + name).encode())
            modules = temp / "modules"
            modules.mkdir()
            provenance = make_provenance(temp, modules, base)
            (modules / "brcmfmac.ko").write_bytes(b"tampered")
            output = temp / "overlay"
            with self.assertRaisesRegex(ValueError, "not pinned by ABI provenance"):
                stage.stage(base, modules, output, REPO, provenance)
            self.assertFalse(output.exists())

    def test_report_claiming_31_with_only_three_selected_names_is_rejected(self):
        stage = load_stager()
        with tempfile.TemporaryDirectory() as td:
            temp = Path(td)
            base = temp / "base"
            copy_base_fixture(base)
            modules = temp / "modules"
            modules.mkdir()
            provenance = make_provenance(temp, modules, base)
            data = json.loads(provenance.read_text())
            report_path = Path(data["mixed_static_abi"]["report"])
            report = json.loads(report_path.read_text())
            report["selected_modules"] = {name: entry for name, entry in report["selected_modules"].items()
                                          if name in ABI_MODULES.values()}
            report_path.write_text(json.dumps(report, sort_keys=True))
            data["mixed_static_abi"]["report_sha256"] = sha(report_path)
            provenance.write_text(json.dumps(data, sort_keys=True))
            with self.assertRaisesRegex(ValueError, "selected module names do not match"):
                stage.pinned_provenance(provenance, modules, REPO, base)

    def test_tampered_retained_vendor_module_is_rejected_before_overlay_creation(self):
        stage = load_stager()
        with tempfile.TemporaryDirectory() as td:
            temp = Path(td)
            base = temp / "base"
            copy_base_fixture(base)
            modules = temp / "modules"
            modules.mkdir()
            provenance = make_provenance(temp, modules, base)
            (base / "module_driver/soc_msc.ko").write_bytes(b"wrong vendor module")
            output = temp / "overlay"
            with self.assertRaisesRegex(ValueError, "base retained vendor module does not match"):
                stage.stage(base, modules, output, REPO, provenance)
            self.assertFalse(output.exists())

    def test_module_directory_symlink_is_rejected(self):
        stage = load_stager()
        with tempfile.TemporaryDirectory() as td:
            temp = Path(td)
            base = temp / "base"
            copy_base_fixture(base)
            modules = temp / "modules"
            modules.mkdir()
            provenance = make_provenance(temp, modules, base)
            alias = temp / "modules-alias"
            alias.symlink_to(modules, target_is_directory=True)
            output = temp / "overlay"
            with self.assertRaisesRegex(ValueError, "real directory"):
                stage.stage(base, alias, output, REPO, provenance)
            self.assertFalse(output.exists())

    def test_noncanonical_power_init_file_is_rejected(self):
        stage = load_stager()
        with tempfile.TemporaryDirectory() as td:
            altered = Path(td) / "firmware/kernel/wifi-experimental/rootfs/module_driver/bcm_wlbt_power.sh"
            altered.parent.mkdir(parents=True)
            altered.write_text(stage.EXPECTED_POWER_INIT.replace("PB03", "PB02"))
            with self.assertRaisesRegex(ValueError, "reviewed AP6212A"):
                stage.checked_power_init(Path(td))

    def test_legacy_stock_paths_are_wrapped_or_disabled(self):
        stage = load_stager()
        with tempfile.TemporaryDirectory() as td:
            temp = Path(td)
            base = temp / "base"
            copy_base_fixture(base)
            modules = temp / "modules"
            modules.mkdir()
            provenance = make_provenance(temp, modules, base)
            output = temp / "overlay"
            stage.stage(base, modules, output, REPO, provenance)
            up = (output / "usr/bin/wifi_up.sh").read_text()
            down = (output / "usr/bin/wifi_down.sh").read_text()
            bsa = (output / "usr/bin/bt_enable_bsa.sh").read_text()
            s43 = (output / "etc/init.d/S43wifi_bcm_init_config").read_text()
            self.assertIn("compas-radio wifi-up", up)
            self.assertNotIn("rfkill", up)
            self.assertIn("compas-radio wifi-down", down)
            self.assertNotIn("rfkill", down)
            self.assertIn("disabled", bsa)
            self.assertNotIn("rfkill", bsa)
            self.assertNotIn("bsa_server", bsa)
            self.assertIn("COMPAS_WIFI_MAC_DOWN_SCRIPT=/usr/libexec/compas/wifi_off.brcmfmac.vendor.sh", s43)
            self.assertIn("COMPAS_WIFI_MAC_OFF_SCRIPT=/usr/libexec/compas/wifi_off.brcmfmac.vendor.sh", s43)


if __name__ == "__main__":
    unittest.main(verbosity=2)
