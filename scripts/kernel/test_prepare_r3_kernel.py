from __future__ import annotations

import gzip
import hashlib
import importlib.util
import json
import pathlib
import struct
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

SCRIPT = pathlib.Path(__file__).with_name("prepare_r3_kernel.py")
SPEC = importlib.util.spec_from_file_location("prepare_r3_kernel", SCRIPT)
assert SPEC and SPEC.loader
r3 = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(r3)


def digest_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def stock_ximage(dtb: bytes) -> bytes:
    return b"stock xImage prefix" + gzip.compress(b"kernel" + dtb)


class PrepareR3KernelTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="prepare-r3-kernel-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.modules = self.root / "module_driver"
        self.modules.mkdir()
        module_bytes = b"test OEM module object\n"
        (self.modules / "soc_fb.ko").write_bytes(module_bytes)
        self.module_pin = {"soc_fb.ko": digest_bytes(module_bytes)}
        self.dtb_a = self.make_dtb(b"board-a")
        self.dtb_b = self.make_dtb(b"board-b")
        self.upt_a = self.root / "board-a.upt"
        self.upt_b = self.root / "board-b.upt"
        self.upt_a.write_bytes(b"upt-a")
        self.upt_b.write_bytes(b"upt-b")
        self.kernel_a = self.root / "board-a-xImage"
        self.kernel_b = self.root / "board-b-xImage"
        self.kernel_a.write_bytes(stock_ximage(self.dtb_a))
        self.kernel_b.write_bytes(stock_ximage(self.dtb_b))
        self.vmlinux = self.root / "vmlinux-stock"
        self.vmlinux.write_bytes(b"test pinned stock symbols")
        self.registry = {"schema_version": 1, "boards": {
            "r3proii": self.board_record(self.upt_a, self.kernel_a, self.dtb_a, self.vmlinux,
                                          r3.EXPECTED_SCREEN["r3proii"]),
            "r3ii_2025": self.board_record(self.upt_b, self.kernel_b, self.dtb_b, self.vmlinux,
                                           r3.EXPECTED_SCREEN["r3ii_2025"]),
        }}

    @staticmethod
    def make_dtb(tag: bytes) -> bytes:
        data = bytearray(1200)
        data[:4] = b"\xd0\x0d\xfe\xed"
        struct.pack_into(">I", data, 4, len(data))
        data[-len(tag):] = tag
        return bytes(data)

    def board_record(self, upt, kernel, dtb, vmlinux, screen):
        return {
            "stock_version": "test", "source_archive_filename": upt.name,
            "stock_upt_sha256": digest_bytes(upt.read_bytes()),
            "stock_kernel_sha256": digest_bytes(kernel.read_bytes()),
            "stock_vmlinux_sha256": digest_bytes(vmlinux.read_bytes()),
            "stock_dtb_sha256": digest_bytes(dtb), "stock_modules": self.module_pin,
            "expected_vermagic": r3.EXPECTED_VERMAGIC, "screen": screen,
        }

    def test_board_pins_keep_each_models_geometry_separate(self):
        self.assertEqual(r3.selected_board(self.registry, "r3proii")["screen"],
                         r3.EXPECTED_SCREEN["r3proii"])
        self.assertEqual(r3.selected_board(self.registry, "r3ii_2025")["screen"],
                         r3.EXPECTED_SCREEN["r3ii_2025"])
        altered = dict(self.registry["boards"]["r3ii_2025"])
        altered["screen"] = r3.EXPECTED_SCREEN["r3proii"]
        with self.assertRaisesRegex(r3.R3PreparationError, "geometry/format"):
            r3.selected_board({"boards": {"r3ii_2025": altered}}, "r3ii_2025")

    def test_repository_registry_and_board_configs_are_currently_pinned(self):
        registry = r3.load_registry()
        for board in r3.BOARD_NAMES:
            record = r3.selected_board(registry, board)
            for key in ("required_config", "parity_config", "profile_config"):
                path, digest = r3.config_source(r3.REPO, record, key)
                self.assertEqual(r3.sha256(path), digest)

    def test_build_command_is_model_specific_and_uses_no_r1_fragments(self):
        command = r3.build_command("podman", self.root / "build-space", None, None,
                                   "r3ii_2025", "parity",
                                   ["compas-dma-signed-gaps.patch", "compas-usb-dac-safety.patch"], 3)
        self.assertEqual(command[command.index("--model") + 1], "r3ii_2025")
        self.assertEqual(command[command.index("--name") + 1], "compas-r3ii_2025")
        fragments = [command[i + 1] for i, item in enumerate(command[:-1]) if item == "--fragment"]
        self.assertEqual(fragments, ["r3ii_2025-parity.config", "compas-r3ii_2025.config"])
        self.assertNotIn("--wifi-stack", command)
        self.assertNotIn("r1-parity.config", command)

    def test_cross_board_kernel_rejects_before_workspace_creation(self):
        workspace = self.root / "must-not-exist"
        args = SimpleNamespace(board="r3proii", stock_upt=self.upt_a, stock_vmlinux=self.vmlinux,
                               stock_kernel=self.kernel_b, stock_modules=self.modules,
                               upstream=self.root / "unused-upstream", workspace=workspace)
        with mock.patch.object(r3, "load_registry", return_value=self.registry):
            with self.assertRaisesRegex(r3.R3PreparationError, "belongs to r3ii_2025"):
                r3.prepare(args)
        self.assertFalse(workspace.exists())

    def test_stock_module_inventory_and_hashes_are_exact(self):
        valid = r3.validate_stock_inputs("r3proii", self.registry["boards"]["r3proii"],
                                         self.upt_a, self.kernel_a, self.modules, self.vmlinux, self.registry)
        self.assertEqual(valid["stock_module_count"], 1)
        (self.modules / "extra.ko").write_bytes(b"not pinned")
        with self.assertRaisesRegex(r3.R3PreparationError, "inventory differs"):
            r3.validate_stock_inputs("r3proii", self.registry["boards"]["r3proii"],
                                     self.upt_a, self.kernel_a, self.modules, self.vmlinux, self.registry)
        (self.modules / "extra.ko").unlink()
        nested = self.modules / "unexpected"
        nested.mkdir()
        (nested / "soc_fb.ko").write_bytes((self.modules / "soc_fb.ko").read_bytes())
        with self.assertRaisesRegex(r3.R3PreparationError, "nested"):
            r3.validate_stock_inputs("r3proii", self.registry["boards"]["r3proii"],
                                     self.upt_a, self.kernel_a, self.modules, self.vmlinux, self.registry)

    def test_module_symlink_and_changed_bytes_are_rejected(self):
        module = self.modules / "soc_fb.ko"
        pinned = module.read_bytes()
        module.unlink()
        try:
            module.symlink_to(self.root / "elsewhere.ko")
        except OSError as exc:
            self.skipTest(f"symlinks unavailable: {exc}")
        with self.assertRaisesRegex(r3.R3PreparationError, "symlink"):
            r3.validate_stock_inputs("r3proii", self.registry["boards"]["r3proii"],
                                     self.upt_a, self.kernel_a, self.modules, self.vmlinux, self.registry)
        module.unlink()
        module.write_bytes(pinned + b"changed")
        with self.assertRaisesRegex(r3.R3PreparationError, "hash differs"):
            r3.validate_stock_inputs("r3proii", self.registry["boards"]["r3proii"],
                                     self.upt_a, self.kernel_a, self.modules, self.vmlinux, self.registry)

    def test_stock_file_symlink_is_rejected(self):
        alias = self.root / "alias.upt"
        try:
            alias.symlink_to(self.upt_a)
        except OSError as exc:
            self.skipTest(f"symlinks unavailable: {exc}")
        with self.assertRaisesRegex(r3.R3PreparationError, "regular non-symlink"):
            r3.validate_stock_inputs("r3proii", self.registry["boards"]["r3proii"],
                                     alias, self.kernel_a, self.modules, self.vmlinux, self.registry)

    def test_registry_and_config_file_hashes_are_pinned(self):
        path = self.root / "r3-boards.json"
        path.write_text(json.dumps(self.registry))
        with mock.patch.object(r3, "REGISTRY_SHA256", digest_bytes(path.read_bytes())):
            self.assertEqual(r3.load_registry(path)["schema_version"], 1)
        with mock.patch.object(r3, "REGISTRY_SHA256", "0" * 64):
            with self.assertRaisesRegex(r3.R3PreparationError, "registry hash"):
                r3.load_registry(path)
        config = self.root / "r3-required.config"
        config.write_text("CONFIG_NLS_CODEPAGE_936=y\n")
        rec = {"required_config": "r3-required.config",
               "required_config_sha256": digest_bytes(config.read_bytes())}
        self.assertEqual(r3.config_source(self.root, rec, "required_config")[0], config)
        config.write_text("CONFIG_NLS_CODEPAGE_936=n\n")
        with self.assertRaisesRegex(r3.R3PreparationError, "changed from registry pin"):
            r3.config_source(self.root, rec, "required_config")

    def test_kernel_only_script_stops_before_any_module_build(self):
        script = self.root / "build.sh"
        script.write_text("#!/bin/bash\nmake xImage\ncp xImage out/\n# Modules:\nmake modules\n")
        original_sha, modified_sha = r3.kernel_only_build_script(script)
        output = script.read_text()
        self.assertNotEqual(original_sha, modified_sha)
        self.assertIn("make xImage", output)
        self.assertIn("KERNEL_ONLY_BUILD_OK", output)
        self.assertIn('kernel.release-$NAME', output)
        self.assertNotIn("make modules", output)

    def test_stock_kallsyms_decoder_checks_typed_names_and_markers(self):
        layout = {"count_offset": 0, "names_offset": 16, "markers_offset": 48,
                  "token_table_offset": 64, "token_index_offset": 576}
        image = bytearray(1088)
        image[0:4] = (2).to_bytes(4, "little")
        names = b"Tpca_probe"
        image[16] = len(names)
        image[17:17 + len(names)] = names
        second = b"Tother"
        second_at = 17 + len(names)
        image[second_at] = len(second)
        image[second_at + 1:second_at + 1 + len(second)] = second
        image[48:52] = (0).to_bytes(4, "little")
        for token in range(256):
            image[64 + token * 2] = token
            image[64 + token * 2 + 1] = 0
            image[576 + token * 2:578 + token * 2] = (token * 2).to_bytes(2, "little")
        self.assertEqual(r3.decode_kallsyms(bytes(image), layout), ["Tpca_probe", "Tother"])
        image[48:52] = (1).to_bytes(4, "little")
        with self.assertRaisesRegex(r3.R3PreparationError, "marker"):
            r3.decode_kallsyms(bytes(image), layout)

    def make_validation_fixture(self, suffix):
        kit = self.root / f"validation-kit-{suffix}"
        configs = kit / "configs"
        out = kit / "out"
        configs.mkdir(parents=True)
        out.mkdir()
        (configs / "r3proii-required.config").write_text("CONFIG_BASE=y\n")
        (configs / "r3proii-parity.config").write_text("CONFIG_PARITY=y\n")
        (configs / "compas-r3proii.config").write_text("CONFIG_NLS_CODEPAGE_936=y\n")
        (out / "xImage-compas-r3proii").write_bytes(b"candidate kernel image")
        (out / "kernel.release-compas-r3proii").write_text("4.4.94+\n")
        (out / "config-compas-r3proii").write_text(
            "CONFIG_BASE=y\nCONFIG_PARITY=y\nCONFIG_NLS_CODEPAGE_936=y\n"
            "CONFIG_PREEMPT=y\nCONFIG_MODULE_UNLOAD=y\nCONFIG_CPU_MIPS32_R2=y\nCONFIG_32BIT=y\n"
            "# CONFIG_MODVERSIONS is not set\n")
        (out / "System.map-compas-r3proii").write_text("c0000000 T _start\n")
        stock_kernel = self.root / "larger-stock-xImage"
        stock_kernel.write_bytes(b"stock image larger than candidate" * 4)
        modules = self.root / f"abi-modules-{suffix}"
        modules.mkdir()
        (modules / "soc_fb.ko").write_bytes(b"OEM module")
        record = {"stock_dtb_sha256": digest_bytes(self.dtb_a)}
        stock_manifest = {"stock_module_count": 1}
        report = {"valid": True, "selected_module_count": 1, "kernel_export_count": 5,
                  "missing_imports_by_module": {}, "vermagic_errors": []}
        return kit, stock_kernel, modules, record, stock_manifest, report

    def test_build_validation_rejects_bad_dtb_config_and_module_abi(self):
        # Exercise separate fail-closed checks while stubbing only external tool
        # output; the validator itself still reads the produced files/configs.
        for case in ("dtb", "config", "count", "imports", "smp", "release"):
            with self.subTest(case=case):
                kit, stock_kernel, modules, record, stock_manifest, report = self.make_validation_fixture(case)
                if case == "config":
                    config = kit / "out/config-compas-r3proii"
                    config.write_text(config.read_text().replace("CONFIG_BASE=y", "CONFIG_BASE=n"))
                if case == "smp":
                    config = kit / "out/config-compas-r3proii"
                    config.write_text(config.read_text() + "CONFIG_SMP=y\n")
                if case == "release":
                    (kit / "out/kernel.release-compas-r3proii").write_text("4.4.94\n")
                if case == "count":
                    report["selected_module_count"] = 0
                if case == "imports":
                    report["missing_imports_by_module"] = {"soc_fb.ko": ["unresolved_symbol"]}
                dtb_value = self.dtb_b if case == "dtb" else self.dtb_a
                with mock.patch.object(r3.repack_kernel_upt, "validate_uimage"), \
                        mock.patch.object(r3.common, "embedded_dtb", return_value=dtb_value), \
                        mock.patch.object(r3.verify_module_abi, "verify", return_value=report):
                    with self.assertRaises(r3.R3PreparationError):
                        r3.validate_kernel_build(kit, "r3proii", "parity", stock_kernel,
                                                 self.dtb_a, record, modules, stock_manifest)

    def test_build_validation_accepts_exact_release_and_static_oem_abi(self):
        kit, stock_kernel, modules, record, stock_manifest, report = self.make_validation_fixture("valid")
        with mock.patch.object(r3.repack_kernel_upt, "validate_uimage"), \
                mock.patch.object(r3.common, "embedded_dtb", return_value=self.dtb_a), \
                mock.patch.object(r3.verify_module_abi, "verify", return_value=report):
            result, _ = r3.validate_kernel_build(kit, "r3proii", "parity", stock_kernel,
                                                 self.dtb_a, record, modules, stock_manifest)
        self.assertEqual(result["kernel_release"], "4.4.94+")
        self.assertEqual(result["compiled_config_options"]["CONFIG_SMP"], "n")

    def test_preparation_manifest_pins_validator_and_shared_helper_bytes(self):
        pins = r3.validation_code_pins()
        self.assertEqual({item["path"] for item in pins}, {
            "scripts/kernel/prepare_r3_kernel.py", "scripts/kernel/prepare_r1_kernel.py",
            "scripts/kernel/verify_module_abi.py", "scripts/kernel/repack_kernel_upt.py"})
        self.assertTrue(all(len(item["sha256"]) == 64 for item in pins))

    def test_cli_requires_explicit_board_and_stock_inputs(self):
        base = ["--upstream", "upstream", "--stock-upt", "stock.upt",
                "--stock-kernel", "xImage", "--stock-vmlinux", "vmlinux-stock",
                "--stock-modules", "module_driver",
                "--workspace", "work"]
        with self.assertRaises(SystemExit):
            r3.parse_args(base)
        args = r3.parse_args(["--board", "r3proii", *base])
        self.assertEqual(args.profile, "parity")
        self.assertFalse(args.build)
        with self.assertRaises(SystemExit):
            r3.parse_args(["--board", "r3ii", *base])


if __name__ == "__main__":
    unittest.main()
