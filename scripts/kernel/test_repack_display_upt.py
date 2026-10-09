#!/usr/bin/env python3
"""Host-side end-to-end tests for the R1 display UPT repacker."""
from __future__ import annotations

import hashlib
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import repack_display_upt as pack_display  # noqa: E402
from repack_kernel_upt import package, sha, validate_uimage  # noqa: E402

MODULE_NAMES = pack_display.DISPLAY_MODULE_NAMES


def write(path: pathlib.Path, value: bytes | str) -> pathlib.Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(value.encode() if isinstance(value, str) else value)
    return path


def uimage(payload: bytes) -> bytes:
    fields = [0x27051956, 0, 0, len(payload), 0x80F00000, 0x80F00000,
              zlib.crc32(payload) & 0xFFFFFFFF]
    tail = bytes((5, 5, 2, 0)) + b"test" + b"\0" * 28
    header = bytearray(struct.pack(">7I", *fields) + tail)
    fields[1] = zlib.crc32(header) & 0xFFFFFFFF
    result = struct.pack(">7I", *fields) + tail + payload
    validate_uimage_bytes(result)
    return result


def validate_uimage_bytes(data: bytes) -> None:
    with tempfile.NamedTemporaryFile() as file:
        file.write(data)
        file.flush()
        validate_uimage(pathlib.Path(file.name))


class DisplayUptRepackTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.base_upt = self.root / "base.upt"
        self.candidate = self.root / "candidate"
        self.candidate_modules = self.candidate / "rootfs/module_driver"
        self.candidate_modules.mkdir(parents=True)
        self.review_path = self.root / "review.json"
        self.output = self.root / "candidate.upt"
        self.repo = self.root / "repo"
        self.cleanup_patcher = mock.patch.object(pack_display.verify_panel_module,
                                                 "has_cleanup_module", return_value=False)
        self.cleanup_patcher.start()
        self.source_dir = self.repo / "firmware/kernel/display-experimental"
        self.sources = {
            "soc_fb.c": "int soc_fb(void) { return 0; }\n",
            "core.h": "struct framebuffer_core;\n",
            "Makefile": "obj-m := soc_fb.o\nKBUILD_EXTRA_SYMBOLS += $(src)/hiby.symvers\n",
            "hiby.symvers": "0x00000000\trmem_alloc_aligned\trmem_manager\tEXPORT_SYMBOL\n0x00000000\trmem_free\trmem_manager\tEXPORT_SYMBOL\n0x00000000\tgpio_port_set_func\tutils\tEXPORT_SYMBOL\n",
        }
        source_hashes = {}
        for name, content in self.sources.items():
            path = write(self.source_dir / name, content)
            source_hashes[f"firmware/kernel/display-experimental/{name}"] = sha(path)
        self.source_hashes = source_hashes

        self.base_root = self.root / "base-root"
        self.driver_dir = self.base_root / "module_driver"
        for name in MODULE_NAMES:
            data = (f"module content {name}".encode() if name in pack_display.VENDOR_DISPLAY_MODULE_NAMES
                    else f"old compiled module {name}".encode())
            write(self.driver_dir / f"{name}.ko", data)
        write(self.driver_dir / "soc_fb.ko", b"old display module")
        write(self.driver_dir / "soc_fb.sh", "#!/bin/sh\ninsmod soc_fb.ko\n")
        write(self.driver_dir / "driver_default_init_script.sh", "#!/bin/sh\nsh soc_fb.sh\n")
        write(self.base_root / "usr/bin/compas_player", b"production player")
        write(self.base_root / "usr/bin/compas_bootloader", b"production bootloader")
        (self.base_root / "etc").mkdir(parents=True, exist_ok=True)
        write(self.base_root / "etc/settings.conf", "preserve me\n")
        self.base_kernel = uimage(b"old validated kernel")
        self.new_kernel = uimage(b"reviewed optimized display kernel")
        self._make_base_upt()
        self._make_candidate()

    def tearDown(self) -> None:
        self.cleanup_patcher.stop()
        self.temp.cleanup()

    def _make_base_upt(self) -> None:
        if not all(shutil.which(tool) for tool in ("mksquashfs", "genisoimage", "7z", "unsquashfs")):
            self.skipTest("host UPT packing tools are not installed")
        work = self.root / "base-package"
        ota = work / "iso/ota_v0"
        ota.mkdir(parents=True)
        image = write(work / "xImage", self.base_kernel)
        rootfs = work / "rootfs.squashfs"
        subprocess.run(["mksquashfs", str(self.base_root), str(rootfs), "-comp", "lzo",
                        "-all-root", "-noappend", "-no-xattrs", "-processors", "1"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        kernel_size, kernel_md5 = package(image, ota, "xImage")
        root_size, root_md5 = package(rootfs, ota, "rootfs.squashfs")
        write(ota / "ota_update.in", f"ota_version=0\nimg_type=kernel\nimg_name=xImage\nimg_size={kernel_size}\nimg_md5={kernel_md5}\nimg_type=rootfs\nimg_name=rootfs.squashfs\nimg_size={root_size}\nimg_md5={root_md5}\n")
        write(ota / "ota_v0.ok", b"")
        write(work / "iso/ota_config.in", "current_version=0\n")
        subprocess.run(["genisoimage", "-f", "-U", "-J", "-joliet-long", "-r",
                        "-allow-lowercase", "-allow-multidot", "-o", str(self.base_upt),
                        str(work / "iso")], check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        self.base_hash = sha(self.base_upt)
        self.base_kernel_hash = hashlib.sha256(self.base_kernel).hexdigest()
        self.base_rootfs_hash = sha(rootfs)
        write(self.base_upt.with_suffix(".upt.json"), json.dumps({
            "output_upt_sha256": self.base_hash,
            "output_upt_size": self.base_upt.stat().st_size,
            "base_upt_sha256": pack_display.BASE_PACKAGE_SOURCE_SHA256,
            "kernel_sha256": self.base_kernel_hash,
            "rootfs_after_sha256": self.base_rootfs_hash,
            "rootfs_unchanged_except_replacements": True,
            "replacement_module_sha256": {name: sha(self.driver_dir / f"{name}.ko")
                                           for name in pack_display.REVIEWED_DRIVER_NAMES},
            "driver_review_sha256": pack_display.BASE_DRIVER_REVIEW_SHA256,
        }))

    def _make_candidate(self) -> None:
        self.build = self.root / "build"
        prep_path = self.build / "preparation.json"
        artifacts = {
            "ximage": write(self.build / "xImage", self.new_kernel),
            "config": write(self.build / ".config", "CONFIG_FB=y\n"),
            "system_map": write(self.build / "System.map", "00000000 T _text\n"),
        }
        module_hashes = {}
        module_records = {}
        selected = {}
        fresh_modules = self.build / "modules-compas-r1"
        for name in sorted(MODULE_NAMES):
            if name in pack_display.VENDOR_DISPLAY_MODULE_NAMES:
                content = f"module content {name}".encode()
            else:
                content = ("new source soc_fb" if name == "soc_fb" else f"module content {name}").encode()
            path = write(self.candidate_modules / f"{name}.ko", content)
            module_hashes[name] = sha(path)
            source = "vendor" if name in pack_display.VENDOR_DISPLAY_MODULE_NAMES else "built"
            if source == "built":
                write(fresh_modules / f"{name}.ko", content)
            module_records[name] = {"source": source, "sha256": sha(path), "vermagic": "test"}
            selected[name] = {"path": str(path), "source": source, "sha256": sha(path),
                              "vermagic": "test"}
        prep_validation = {
            "display_stack": "compas",
            "ximage_path": str(artifacts["ximage"]), "ximage_sha256": sha(artifacts["ximage"]),
            "config_path": str(artifacts["config"]), "compiled_config_sha256": sha(artifacts["config"]),
            "system_map_path": str(artifacts["system_map"]), "system_map_sha256": sha(artifacts["system_map"]),
            "soc_fb_module_path": str(fresh_modules / "soc_fb.ko"),
            "soc_fb_module_sha256": sha(fresh_modules / "soc_fb.ko"),
        }
        preparation = {
            "build_requested": True, "profile": "optimized", "display_stack": "compas",
            "wifi_stack": "vendor", "upstream_head": pack_display.PINNED_KERNEL_COMMIT,
            "upstream": {"commit": pack_display.PINNED_KERNEL_COMMIT}, "build_validation": prep_validation,
            "stock_kernel": {"sha256": pack_display.STOCK_KERNEL_SHA256, "size": 3731520},
        }
        write(prep_path, json.dumps(preparation))
        prep_hash = sha(prep_path)
        abi_path = self.build / "abi.json"
        abi = {"valid": True, "selected_module_count": 29, "selected_modules": selected,
               "missing_imports_by_module": {}, "vermagic_errors": [],
               "system_map": str(artifacts["system_map"]),
               "system_map_sha256": sha(artifacts["system_map"])}
        write(abi_path, json.dumps(abi))
        abi_hash = sha(abi_path)
        display_sources = self.source_hashes
        loader_hash = sha(self.driver_dir / "soc_fb.sh")
        candidate = {
            "status": "host-only candidate; not validated on device; not a flash/package artifact",
            "display_stack": "compas", "wifi_stack": "vendor", "exclusive_soc_fb_controller": True,
            "build": {"preparation_path": str(prep_path), "preparation_sha256": prep_hash,
                      "ximage_sha256": sha(artifacts["ximage"]),
                      "system_map_sha256": sha(artifacts["system_map"])},
            "abi_report": {"path": str(abi_path), "sha256": abi_hash, "selected_module_count": 29},
            "display_sources": display_sources,
            "modules": {name: {**module_records[name], "path": str(self.candidate_modules / f"{name}.ko")}
                        for name in module_hashes},
            "loader": {"sha256": loader_hash, "module": "soc_fb.ko", "startup_invocations": 1},
        }
        write(self.candidate / "DISPLAY-CANDIDATE.json", json.dumps(candidate))
        report_path = self.root / "opus-report.txt"
        write(report_path, "Reviewed display foundation and module hashes.\n")
        review = {
            "status": "source_static_review_passed", "model": "claude-opus-5-5",
            "report": str(report_path), "report_sha256": sha(report_path),
            "display_sources": display_sources, "modules": module_hashes,
            "ximage_sha256": sha(artifacts["ximage"]),
            "system_map_sha256": sha(artifacts["system_map"]),
            "abi_report_sha256": abi_hash, "preparation_sha256": prep_hash,
            "base_upt_sha256": pack_display.BASE_UPT_SHA256,
        }
        write(self.review_path, json.dumps(review))

    def test_full_upt_round_trip_changes_only_kernel_and_module_payload(self) -> None:
        with mock.patch.object(pack_display, "BASE_UPT_SHA256", self.base_hash), \
             mock.patch.object(pack_display, "BASE_UPT_SIDECAR_SHA256",
                               sha(self.base_upt.with_suffix(".upt.json"))), \
             mock.patch.object(pack_display, "BASE_KERNEL_SHA256", self.base_kernel_hash):
            review = json.loads(self.review_path.read_text())
            review["base_upt_sha256"] = self.base_hash
            write(self.review_path, json.dumps(review))
            result = pack_display.create(self.base_upt, self.candidate, self.review_path,
                                         self.output, self.repo)
        self.assertEqual(result, self.output)
        self.assertTrue(self.output.is_file())
        sidecar = json.loads(self.output.with_suffix(".upt.json").read_text())
        self.assertEqual(sidecar["candidate_kernel_sha256"], sha(self.build / "xImage"))
        self.assertEqual(sidecar["module_count"], 29)
        self.assertTrue(sidecar["rootfs_changed_only_selected_module_files"])
        self.assertEqual(sidecar["production_executables_sha256"]["usr/bin/compas_player"],
                         sha(self.base_root / "usr/bin/compas_player"))
        self.assertEqual(sidecar["wifi_stack"], "vendor")
        self.assertIn("not flashed", sidecar["candidate_type"])

    def test_reviewed_module_hash_mismatch_is_rejected(self) -> None:
        review = json.loads(self.review_path.read_text())
        review["modules"]["soc_fb"] = "0" * 64
        write(self.review_path, json.dumps(review))
        with self.assertRaisesRegex(pack_display.PackError, "reviewed module hashes"):
            pack_display.read_candidate(self.candidate, self.review_path, self.repo)

    def test_source_hash_mismatch_is_rejected(self) -> None:
        path = self.source_dir / "soc_fb.c"
        path.write_text("tampered\n")
        with self.assertRaisesRegex(pack_display.PackError, "source changed"):
            pack_display.read_candidate(self.candidate, self.review_path, self.repo)

    def test_abi_report_must_be_fresh_and_complete(self) -> None:
        candidate_path = self.candidate / "DISPLAY-CANDIDATE.json"
        candidate = json.loads(candidate_path.read_text())
        abi_path = pathlib.Path(candidate["abi_report"]["path"])
        abi = json.loads(abi_path.read_text())
        abi["selected_module_count"] = 30
        write(abi_path, json.dumps(abi))
        with self.assertRaisesRegex(pack_display.PackError, "ABI report changed"):
            pack_display.read_candidate(self.candidate, self.review_path, self.repo)

    def test_candidate_payload_rejects_extra_module(self) -> None:
        write(self.candidate_modules / "second_fb.ko", b"second controller")
        with self.assertRaisesRegex(pack_display.PackError, "exactly its reviewed 29 modules"):
            pack_display.read_candidate(self.candidate, self.review_path, self.repo)

    def test_nm_cleanup_module_detection(self) -> None:
        self.cleanup_patcher.stop()
        path = self.build / "sample.ko"
        write(path, b"not parsed by mocked nm")
        with mock.patch.object(pack_display.verify_panel_module.subprocess, "run",
                               return_value=type("Result", (), {"stdout": "00000000 T cleanup_module\n"})()):
            self.assertTrue(pack_display.verify_panel_module.has_cleanup_module(path, "nm"))
        with mock.patch.object(pack_display.verify_panel_module.subprocess, "run",
                               return_value=type("Result", (), {"stdout": "00000000 T init_module\n"})()):
            self.assertFalse(pack_display.verify_panel_module.has_cleanup_module(path, "nm"))

    def test_base_vendor_module_drift_is_rejected(self) -> None:
        source = self.candidate_modules / "cywdhd.ko"
        source.write_bytes(b"changed vendor wifi module")
        candidate = json.loads((self.candidate / "DISPLAY-CANDIDATE.json").read_text())
        candidate["modules"]["cywdhd"]["sha256"] = sha(source)
        candidate["abi_report"]["path"] = candidate["abi_report"]["path"]
        write(self.candidate / "DISPLAY-CANDIDATE.json", json.dumps(candidate))
        with self.assertRaisesRegex(pack_display.PackError, "retain pinned vendor module bytes: cywdhd"):
            pack_display.replace_modules(self.base_root, self.candidate,
                                         self.candidate_modules,
                                         {name: record["sha256"] for name, record in candidate["modules"].items()})


if __name__ == "__main__":
    unittest.main()
