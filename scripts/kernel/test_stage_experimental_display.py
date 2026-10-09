#!/usr/bin/env python3
"""Host-only tests for the provenance-gated R1 display candidate stager."""

from __future__ import annotations

import hashlib
import json
import pathlib
import tempfile
import unittest
from unittest import mock

import stage_experimental_display as stage_display


BASE_SHA = "a" * 64


def sha(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path: pathlib.Path, content: bytes | str) -> pathlib.Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content.encode() if isinstance(content, str) else content)
    return path


class DisplayStageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.repo = self.root / "repo"
        self.base = self.root / "base"
        self.build = self.root / "build"
        self.modules = self.build / "modules-compas-r1"
        self.modules.mkdir(parents=True)
        self.output = self.root / "candidate"
        self.cleanup_patcher = mock.patch.object(stage_display.verify_panel_module,
                                                 "has_cleanup_module", return_value=False)
        self.cleanup_patcher.start()
        self.source_dir = self.repo / "firmware/kernel/display-experimental"
        self.source_files = {
            "soc_fb.c": "int soc_fb_module;\n",
            "compas_fb_core.h": "struct core;\n",
            "Makefile": "obj-m := soc_fb.o\nsoc_fb-y := soc_fb.o compas_fb_core.o\nKBUILD_EXTRA_SYMBOLS += $(src)/hiby.symvers\n",
            "hiby.symvers": "0x00000000\trmem_alloc_aligned\trmem_manager\tEXPORT_SYMBOL\n0x00000000\trmem_free\trmem_manager\tEXPORT_SYMBOL\n0x00000000\tgpio_port_set_func\tutils\tEXPORT_SYMBOL\n",
        }
        records = []
        for name, content in self.source_files.items():
            original = write(self.source_dir / name, content)
            copied = write(self.build / "hiby-custom-kernel/modules/soc_fb" / name, content)
            records.append({
                "original_path": f"firmware/kernel/display-experimental/{name}",
                "sha256": sha(original),
                "copied_to": str(copied.relative_to(self.build)),
                "prepared_sha256": sha(copied),
            })
        self.display_sources = records

        self.base_modules = self.base / "module_driver"
        base_files = {
            "soc_fb.ko": b"vendor controller\n",
            "soc_fb.sh": "#!/bin/sh\ninsmod soc_fb.ko lcd_is_inited=0 frame_num=2 pan_display_sync=1\n",
            "driver_default_init_script.sh": "#!/bin/sh\nsh soc_fb.sh\n",
        }
        for name in stage_display.REVIEWED_DRIVER_NAMES:
            base_files[f"{name}.ko"] = f"old reviewed module {name}\n".encode()
        for name in stage_display.VENDOR_DISPLAY_MODULE_NAMES:
            base_files[f"{name}.ko"] = f"vendor module {name}\n".encode()
        installed = {}
        for name, content in base_files.items():
            path = write(self.base_modules / name, content)
            installed[f"/module_driver/{name}"] = {"sha256": sha(path), "size": path.stat().st_size}
        write(self.base / "usr/share/compas/vendor-driver-provenance.json", json.dumps({
            "board": "r1", "archive_sha256": BASE_SHA, "installed_files": installed,
        }))

        self.built_soc = write(self.modules / "soc_fb.ko", b"fresh Compas soc_fb\n")
        self.built_modules = {"soc_fb": self.built_soc}
        for name in stage_display.REVIEWED_DRIVER_NAMES:
            self.built_modules[name] = write(self.modules / f"{name}.ko", f"fresh reviewed module {name}\n")
        self.artifacts = {}
        for name, content in (("xImage", b"kernel image"),
                              (".config", b"CONFIG_FB=y\n"),
                              ("System.map", b"00000000 T _text\n")):
            self.artifacts[name] = write(self.build / name, content)
        self.manifest = self.build / "preparation.json"
        validation = {
            "display_stack": "compas",
            "display_source_sha256": {record["original_path"]: record["sha256"]
                                       for record in records},
            "ximage_path": str(self.artifacts["xImage"]),
            "ximage_sha256": sha(self.artifacts["xImage"]),
            "config_path": str(self.artifacts[".config"]),
            "compiled_config_sha256": sha(self.artifacts[".config"]),
            "system_map_path": str(self.artifacts["System.map"]),
            "system_map_sha256": sha(self.artifacts["System.map"]),
            "soc_fb_module_path": str(self.built_soc),
            "soc_fb_module_sha256": sha(self.built_soc),
        }
        write(self.manifest, json.dumps({
            "build_requested": True,
            "display_stack": "compas",
            "wifi_stack": "vendor",
            "upstream_head": "b" * 40,
            "upstream": {"commit": "b" * 40},
            "stock_kernel": {"sha256": "c" * 64},
            "display_sources": records,
            "build_validation": validation,
        }))
        self.abi_path = self.root / "abi.json"
        self.abi_vendor_modules = self.root / "abi-vendor-modules"
        self.abi_vendor_modules.mkdir()
        self._write_abi()

    def tearDown(self) -> None:
        self.cleanup_patcher.stop()
        self.temp.cleanup()

    def _write_abi(self, mutate=None) -> dict:
        selected = {}
        for name, path in self.built_modules.items():
            selected[name] = {"path": str(path), "source": "built",
                              "sha256": sha(path), "vermagic": "4.4.94+ MIPS32_R2"}
        for name in stage_display.VENDOR_DISPLAY_MODULE_NAMES:
            path = self.base_modules / f"{name}.ko"
            scan_path = self.abi_vendor_modules / path.name
            scan_path.write_bytes(path.read_bytes())
            selected[name] = {"path": str(scan_path), "source": "vendor", "sha256": sha(path),
                              "vermagic": "4.4.94+ MIPS32_R2"}
        report = {
            "valid": True,
            "selected_module_count": 29,
            "selected_modules": selected,
            "missing_imports_by_module": {},
            "vermagic_errors": [],
            "system_map": str(self.artifacts["System.map"]),
            "system_map_sha256": sha(self.artifacts["System.map"]),
        }
        if mutate:
            mutate(report)
        write(self.abi_path, json.dumps(report))
        return report

    def stage(self) -> pathlib.Path:
        return stage_display.stage(self.base, BASE_SHA, self.modules, self.manifest,
                                   self.abi_path, self.output, self.repo)

    def test_stages_exact_abi_set_and_preserves_single_stock_loader(self) -> None:
        self.stage()
        root = self.output / "rootfs/module_driver"
        self.assertEqual({path.stem for path in root.glob("*.ko")}, stage_display.DISPLAY_MODULE_NAMES)
        self.assertEqual(sha(root / "soc_fb.ko"), sha(self.built_soc))
        self.assertNotEqual(sha(root / "soc_fb.ko"), sha(self.base_modules / "soc_fb.ko"))
        self.assertEqual(sha(root / "soc_fb.sh"), sha(self.base_modules / "soc_fb.sh"))
        self.assertEqual(sha(root / "driver_default_init_script.sh"),
                         sha(self.base_modules / "driver_default_init_script.sh"))
        for name in stage_display.VENDOR_DISPLAY_MODULE_NAMES:
            self.assertEqual(sha(root / f"{name}.ko"), sha(self.base_modules / f"{name}.ko"))
        for name in stage_display.REVIEWED_DRIVER_NAMES:
            self.assertEqual(sha(root / f"{name}.ko"), sha(self.built_modules[name]))
        candidate = json.loads((self.output / "DISPLAY-CANDIDATE.json").read_text())
        self.assertTrue(candidate["exclusive_soc_fb_controller"])
        self.assertEqual(candidate["display_stack"], "compas")
        self.assertEqual(candidate["wifi_stack"], "vendor")
        self.assertEqual(candidate["abi_report"]["selected_module_count"], 29)
        self.assertEqual(candidate["loader"]["startup_invocations"], 1)
        self.assertIn("not validated on device", candidate["status"])

    def test_missing_driver_source_is_rejected(self) -> None:
        (self.source_dir / "soc_fb.c").unlink()
        with self.assertRaisesRegex(stage_display.DisplayStageError, "checked-in display source"):
            self.stage()

    def test_tampered_copied_source_is_rejected(self) -> None:
        copied = self.build / "hiby-custom-kernel/modules/soc_fb/soc_fb.c"
        copied.write_text("changed after build\n")
        with self.assertRaisesRegex(stage_display.DisplayStageError, "changed since build"):
            self.stage()

    def test_unsafe_preparation_source_path_is_rejected(self) -> None:
        manifest = json.loads(self.manifest.read_text())
        manifest["display_sources"][0]["copied_to"] = "../../outside/soc_fb.c"
        write(self.manifest, json.dumps(manifest))
        with self.assertRaisesRegex(stage_display.DisplayStageError, "unsafe display source path"):
            self.stage()

    def test_incomplete_abi_set_is_rejected(self) -> None:
        self._write_abi(lambda report: report["selected_modules"].pop("axp2101"))
        with self.assertRaisesRegex(stage_display.DisplayStageError, "exactly 29"):
            self.stage()

    def test_unsafe_module_path_is_rejected(self) -> None:
        self._write_abi(lambda report: report["selected_modules"]["soc_fb"].update(
            path=str(self.base_modules / "soc_fb.ko")))
        with self.assertRaisesRegex(stage_display.DisplayStageError, "ABI module hash changed"):
            self.stage()

    def test_tampered_frozen_vendor_module_is_rejected(self) -> None:
        (self.base_modules / "r1_vendor_00.ko").write_text("modified\n")
        with self.assertRaisesRegex(stage_display.DisplayStageError, "not pinned|hash changed"):
            self.stage()

    def test_duplicate_controller_rejected(self) -> None:
        self._write_abi(lambda report: report["selected_modules"].update({
            "other_fb": dict(report["selected_modules"]["soc_fb"])
        }))
        with self.assertRaisesRegex(stage_display.DisplayStageError, "exactly 29"):
            self.stage()

    def test_unknown_abi_module_inventory_is_rejected(self) -> None:
        self._write_abi(lambda report: (report["selected_modules"].pop("codec_cs43131"),
                                        report["selected_modules"].update({"arbitrary": {
                                            "path": str(self.built_soc), "source": "built",
                                            "sha256": sha(self.built_soc)}})))
        with self.assertRaisesRegex(stage_display.DisplayStageError, "exact 22 reviewed drivers"):
            self.stage()

    def test_base_inventory_must_be_exact(self) -> None:
        extra = write(self.base_modules / "extra.ko", b"not approved")
        provenance_path = self.base / "usr/share/compas/vendor-driver-provenance.json"
        provenance = json.loads(provenance_path.read_text())
        provenance["installed_files"]["/module_driver/extra.ko"] = {
            "sha256": sha(extra), "size": extra.stat().st_size}
        write(provenance_path, json.dumps(provenance))
        with self.assertRaisesRegex(stage_display.DisplayStageError, "exact production 29-module"):
            self.stage()

    def test_unloadable_fresh_panel_module_is_rejected(self) -> None:
        with mock.patch.object(stage_display.verify_panel_module, "has_cleanup_module",
                               return_value=True):
            with self.assertRaisesRegex(stage_display.DisplayStageError, "defines cleanup_module"):
                self.stage()

    def test_default_base_loader_must_not_start_another_controller(self) -> None:
        startup = self.base_modules / "driver_default_init_script.sh"
        startup.write_text("sh soc_fb.sh\nsh other_fb.sh\n")
        self._refresh_base_manifest_file("driver_default_init_script.sh")
        with self.assertRaisesRegex(stage_display.DisplayStageError, "must invoke soc_fb.sh once"):
            self.stage()

    def test_base_startup_may_load_other_production_modules(self) -> None:
        startup = self.base_modules / "driver_default_init_script.sh"
        startup.write_text("#!/bin/sh\nsh utils.sh\nsh rmem_manager.sh\nsh soc_fb.sh\nsh soc_aic.sh\n")
        self._refresh_base_manifest_file("driver_default_init_script.sh")
        self.stage()

    def _refresh_base_manifest_file(self, name: str) -> None:
        path = self.base_modules / name
        provenance_path = self.base / "usr/share/compas/vendor-driver-provenance.json"
        provenance = json.loads(provenance_path.read_text())
        provenance["installed_files"][f"/module_driver/{name}"] = {
            "sha256": sha(path), "size": path.stat().st_size}
        write(provenance_path, json.dumps(provenance))


if __name__ == "__main__":
    unittest.main()
