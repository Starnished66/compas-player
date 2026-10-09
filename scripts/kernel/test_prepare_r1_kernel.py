#!/usr/bin/env python3
"""Focused preparation tests; these do not invoke Docker or touch a device."""

from __future__ import annotations

import gzip
import importlib.util
import json
import os
import pathlib
import struct
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).with_name("prepare_r1_kernel.py")
SPEC = importlib.util.spec_from_file_location("prepare_r1_kernel", SCRIPT)
assert SPEC and SPEC.loader
prepare_r1_kernel = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prepare_r1_kernel)


class PrepareR1KernelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.upstream = SCRIPT.parents[2].parent / "compas-kernel-investigation-20261008"
        if not cls.upstream.is_dir():
            raise unittest.SkipTest("pinned upstream checkout is not available beside the Compás repository")

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="prepare-r1-kernel-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.profile_root = self.root / "repo/firmware/kernel"
        self.profile_root.mkdir(parents=True)
        self.pin_path = self.profile_root / "upstream.json"
        self.pin_path.write_text(json.dumps({
            "source_repo": prepare_r1_kernel.UPSTREAM_REPO_URL,
            "commit": prepare_r1_kernel.EXPECTED_COMMIT,
            "sdk_archive": "ingenic-linux-kernel4.4.94-x1600-v6.0-20240606.tar.bz2",
            "sdk_sha256": "a" * 64,
        }))
        for profile in ("parity", "optimized"):
            (self.profile_root / f"r1-{profile}.config").write_text(
                "CONFIG_NLS_CODEPAGE_936=y\nCONFIG_COMPAS_TEST=y\n"
            )
        self.old_repo_root = prepare_r1_kernel.REPO_ROOT
        self.old_pin_path = prepare_r1_kernel.PIN_PATH
        prepare_r1_kernel.REPO_ROOT = self.root / "repo"
        prepare_r1_kernel.PIN_PATH = self.pin_path
        self.addCleanup(setattr, prepare_r1_kernel, "REPO_ROOT", self.old_repo_root)
        self.addCleanup(setattr, prepare_r1_kernel, "PIN_PATH", self.old_pin_path)

        self.stock = self.root / "xImage"
        dtb = bytearray(1200)
        dtb[:4] = b"\xd0\x0d\xfe\xed"
        struct.pack_into(">I", dtb, 4, len(dtb))
        self.stock.write_bytes(b"uImage-header" + gzip.compress(b"kernel bytes" + dtb))
        self.workspace = self.root / "prepared"

    def args(self, **overrides):
        values = {
            "upstream": self.upstream,
            "stock_kernel": self.stock,
            "workspace": self.workspace,
            "profile": "parity",
            "sdk": None,
            "build": False,
            "container_runtime": "auto",
            "jobs": 2,
        }
        values.update(overrides)
        return prepare_r1_kernel.argparse.Namespace(**values)

    def test_prepare_copies_pinned_tree_extracts_dtb_and_preserves_nls936(self) -> None:
        fake_tools = self.root / "fake-tools"
        fake_tools.mkdir()
        finder = fake_tools / "kallsyms-finder"
        finder.write_text("#!/bin/sh\nprintf '00000000 T _text\\n'\n")
        finder.chmod(0o755)
        with mock.patch.dict(os.environ, {"PATH": f"{fake_tools}:{os.environ['PATH']}"}):
            result = prepare_r1_kernel.prepare(self.args())
        kit = result / "hiby-custom-kernel"
        self.assertEqual((kit / "boards/r1/stock.dtb").stat().st_size, 1200)
        self.assertTrue((kit / "boards/r1/vmlinux-stock").is_file())
        self.assertTrue((kit / "boards/r1/stock.kallsyms").is_file())
        profile = (kit / "configs/compas-r1.config").read_text()
        self.assertIn("CONFIG_NLS_CODEPAGE_936=y", profile.splitlines())
        metadata = json.loads((result / "preparation.json").read_text())
        self.assertEqual(metadata["upstream_head"], prepare_r1_kernel.EXPECTED_COMMIT)
        self.assertFalse(metadata["build_requested"])
        self.assertIn("--fragment compas-r1.config", metadata["build_command_shell"])
        self.assertIn("--jobs 2", metadata["build_command_shell"])
        self.assertEqual(metadata["jobs"], 2)
        self.assertEqual(metadata["local_patches"], [])

    def test_prepare_copies_and_records_local_patches_in_sorted_build_order(self) -> None:
        patch_dir = self.profile_root / "patches"
        patch_dir.mkdir()
        (patch_dir / "compas-usb-fix.patch").write_text("usb fix\n")
        (patch_dir / "compas-audio-fix.patch").write_text("audio fix\n")
        with mock.patch.object(prepare_r1_kernel, "REPO_ROOT", self.root / "repo"):
            with mock.patch.dict(os.environ, {"PATH": os.environ["PATH"]}):
                result = prepare_r1_kernel.prepare(self.args())
        kit = result / "hiby-custom-kernel"
        metadata = json.loads((result / "preparation.json").read_text())
        entries = metadata["local_patches"]
        self.assertEqual([item["original_path"] for item in entries], [
            "firmware/kernel/patches/compas-audio-fix.patch",
            "firmware/kernel/patches/compas-usb-fix.patch",
        ])
        for item in entries:
            copied = kit / "patches" / pathlib.Path(item["copied_to"]).name
            self.assertTrue(copied.is_file())
            self.assertEqual(prepare_r1_kernel.sha256(copied), item["sha256"])
        command = metadata["build_command"]
        patch_args = [command[index + 1] for index, value in enumerate(command[:-1]) if value == "--patch"]
        self.assertEqual(patch_args, ["compas-audio-fix.patch", "compas-usb-fix.patch"])

    def test_rejects_symlink_local_patch_before_creating_workspace(self) -> None:
        patch_dir = self.profile_root / "patches"
        patch_dir.mkdir()
        external = self.root / "outside.patch"
        external.write_text("outside\n")
        (patch_dir / "compas-bad.patch").symlink_to(external)
        with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "regular non-symlink file"):
            prepare_r1_kernel.prepare(self.args())
        self.assertFalse(self.workspace.exists())

    def test_rejects_zero_prefixed_local_patch_before_creating_workspace(self) -> None:
        patch_dir = self.profile_root / "patches"
        patch_dir.mkdir()
        (patch_dir / "0001-double-apply.patch").write_text("duplicate\n")
        with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "must not start with 0"):
            prepare_r1_kernel.prepare(self.args())
        self.assertFalse(self.workspace.exists())

    def test_rejects_local_patch_name_colliding_with_upstream(self) -> None:
        patch_dir = self.profile_root / "patches"
        patch_dir.mkdir()
        (patch_dir / "compas-collision.patch").write_text("collision\n")
        completed = mock.Mock(stdout=b"patches/compas-collision.patch\0")
        with mock.patch.object(prepare_r1_kernel.subprocess, "run", return_value=completed):
            with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "collides with an upstream"):
                prepare_r1_kernel.local_patches(self.upstream)

    def test_rejects_dangling_symlink_patch_directory(self) -> None:
        patch_dir = self.profile_root / "patches"
        patch_dir.symlink_to(self.root / "missing-patches")
        with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "regular directory"):
            prepare_r1_kernel.local_patches(self.upstream)

    def test_applies_module_patch_to_fresh_kit_and_records_hash(self) -> None:
        patch_dir = self.profile_root / "module-patches"
        patch_dir.mkdir()
        patch_file = patch_dir / "compas-driver-fix.patch"
        patch_file.write_text(
            "--- a/modules/example/driver.c\n"
            "+++ b/modules/example/driver.c\n"
            "@@ -1 +1 @@\n"
            "-old value\n"
            "+new value\n"
        )
        kit = self.root / "kit"
        source = kit / "modules/example/driver.c"
        source.parent.mkdir(parents=True)
        source.write_text("old value\n")
        self.workspace.mkdir()

        patches = prepare_r1_kernel.module_patches()
        result = prepare_r1_kernel.apply_module_patches(kit, self.workspace, patches)

        self.assertEqual(source.read_text(), "new value\n")
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]["sha256"], prepare_r1_kernel.sha256(patch_file))
        self.assertEqual(result[0]["copied_to"], "module-patches/compas-driver-fix.patch")

    def test_rejects_module_patch_paths_outside_module_trees(self) -> None:
        patch_file = self.root / "outside.patch"
        patch_file.write_text(
            "--- a/etc/init.d/player\n"
            "+++ b/etc/init.d/player\n"
            "@@ -1 +1 @@\n"
            "-old\n"
            "+new\n"
        )
        with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "outside modules"):
            prepare_r1_kernel.validate_module_patch_paths(patch_file)

    def test_rejects_changed_pin_before_creating_workspace(self) -> None:
        self.pin_path.write_text(json.dumps({
            "source_repo": prepare_r1_kernel.UPSTREAM_REPO_URL,
            "commit": "0" * 40,
            "sdk_archive": "ingenic-linux-kernel4.4.94-x1600-v6.0-20240606.tar.bz2",
        }))
        with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "Kernel source pin must be"):
            prepare_r1_kernel.prepare(self.args())
        self.assertFalse(self.workspace.exists())

    def test_build_requires_sdk_archive_before_workspace_creation(self) -> None:
        with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "--build requires --sdk"):
            prepare_r1_kernel.prepare(self.args(build=True))
        self.assertFalse(self.workspace.exists())

    def test_rejects_existing_workspace(self) -> None:
        self.workspace.mkdir()
        with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "fresh, non-existing"):
            prepare_r1_kernel.prepare(self.args())

    def test_rejects_profile_that_disables_nls936(self) -> None:
        profile = self.profile_root / "r1-optimized.config"
        profile.write_text("# CONFIG_NLS_CODEPAGE_936 is not set\n")
        with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "CONFIG_NLS_CODEPAGE_936=y"):
            prepare_r1_kernel.prepare(self.args(profile="optimized"))
        self.assertFalse(self.workspace.exists())

    def test_profile_cli_rejects_unknown_value(self) -> None:
        with self.assertRaises(SystemExit):
            prepare_r1_kernel.parse_args([
                "--upstream", str(self.upstream), "--stock-kernel", str(self.stock),
                "--workspace", str(self.workspace), "--profile", "unsupported-profile",
            ])

    def build_fixture(self):
        kit = self.root / "fake-kit"
        (kit / "boards/r1/modules").mkdir(parents=True)
        (kit / "modules/shared/Makefile").parent.mkdir(parents=True)
        (kit / "boards/r1/modules/board").mkdir()
        (kit / "modules/shared/Makefile").write_text("obj-m := shared_mod.o\n")
        (kit / "boards/r1/modules/board/Makefile").write_text("obj-m := board_mod.o\n")
        (kit / "boards/r1/modules-need.txt").write_text("export_one\nexport_two\n")
        dtb = bytearray(1200)
        dtb[:4] = b"\xd0\x0d\xfe\xed"
        struct.pack_into(">I", dtb, 4, len(dtb))
        (kit / "boards/r1/stock.dtb").write_bytes(dtb)
        (kit / "configs").mkdir()
        (kit / "configs/r1-required.config").write_text("CONFIG_REQUIRED=y\n")
        (kit / "configs/r1-parity.config").write_text(
            "CONFIG_PARITY=y\nCONFIG_NLS_CODEPAGE_936=y\nCONFIG_PROFILE=n\n"
        )
        (kit / "configs/compas-r1.config").write_text("CONFIG_PROFILE=y\n")
        out = kit / "out"
        (out / "modules-compas-r1").mkdir(parents=True)
        raw = b"kernel" + dtb
        (out / "xImage-compas-r1").write_bytes(b"uImage" + gzip.compress(raw))
        (out / "config-compas-r1").write_text(
            "CONFIG_REQUIRED=y\nCONFIG_PARITY=y\nCONFIG_NLS_CODEPAGE_936=y\nCONFIG_PROFILE=y\n"
        )
        (out / "System.map-compas-r1").write_text(
            "00000001 T __ksymtab_export_one\n00000002 T __ksymtab_export_two\n"
        )
        for name in ("shared_mod", "board_mod"):
            (out / "modules-compas-r1" / f"{name}.ko").write_text("fake ELF module")
        self.stock.write_bytes(b"S" * 10000)
        modinfo = self.root / "bin/modinfo"
        modinfo.parent.mkdir(parents=True)
        modinfo.write_text("#!/bin/sh\necho '4.4.94+ SMP preempt mod_unload MIPS32_R2 32BIT'\n")
        modinfo.chmod(0o755)
        return kit, out, modinfo

    def validate_fixture(self, kit, modinfo):
        with mock.patch.object(prepare_r1_kernel.shutil, "which", return_value=str(modinfo)):
            return prepare_r1_kernel.validate_build(kit, self.stock)

    def test_post_build_validation_accepts_complete_artifacts(self) -> None:
        kit, _, modinfo = self.build_fixture()
        result = self.validate_fixture(kit, modinfo)
        self.assertEqual(result["required_export_count"], 2)
        self.assertEqual(result["required_module_count"], 2)

    def test_post_build_validation_rejects_missing_export(self) -> None:
        kit, out, modinfo = self.build_fixture()
        (out / "System.map-compas-r1").write_text("00000001 T __ksymtab_export_one\n")
        with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "missing 1 exported symbols"):
            self.validate_fixture(kit, modinfo)

    def test_post_build_validation_rejects_mismatched_dtb(self) -> None:
        kit, out, modinfo = self.build_fixture()
        other_dtb = bytearray(1200)
        other_dtb[:4] = b"\xd0\x0d\xfe\xed"
        struct.pack_into(">I", other_dtb, 4, len(other_dtb))
        other_dtb[12] = 1
        (out / "xImage-compas-r1").write_bytes(b"uImage" + gzip.compress(b"kernel" + other_dtb))
        with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "DTB differs"):
            self.validate_fixture(kit, modinfo)

    def test_post_build_validation_rejects_kernel_larger_than_stock(self) -> None:
        kit, _, modinfo = self.build_fixture()
        self.stock.write_bytes(b"small")
        with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "larger than base kernel"):
            self.validate_fixture(kit, modinfo)

    def test_post_build_validation_rejects_compiled_config_drift(self) -> None:
        kit, out, modinfo = self.build_fixture()
        (out / "config-compas-r1").write_text(
            "CONFIG_PARITY=y\nCONFIG_NLS_CODEPAGE_936=y\nCONFIG_PROFILE=n\n"
        )
        with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "requested profile options"):
            self.validate_fixture(kit, modinfo)

    def test_post_build_validation_rejects_missing_expected_module(self) -> None:
        kit, out, modinfo = self.build_fixture()
        (out / "modules-compas-r1/board_mod.ko").unlink()
        with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "missing expected R1 modules"):
            self.validate_fixture(kit, modinfo)

    def test_post_build_validation_rejects_wrong_module_vermagic(self) -> None:
        kit, _, modinfo = self.build_fixture()
        modinfo.write_text("#!/bin/sh\necho '6.1.0 SMP'\n")
        with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "unexpected vermagic"):
            self.validate_fixture(kit, modinfo)

    def test_post_build_validation_rejects_missing_required_config(self) -> None:
        kit, out, modinfo = self.build_fixture()
        config = out / "config-compas-r1"
        config.write_text(config.read_text().replace("CONFIG_REQUIRED=y\n", ""))
        with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "CONFIG_REQUIRED"):
            self.validate_fixture(kit, modinfo)

    def test_post_build_validation_rejects_release_suffix(self) -> None:
        kit, _, modinfo = self.build_fixture()
        modinfo.write_text("#!/bin/sh\necho '4.4.94+other preempt'\n")
        with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "unexpected vermagic"):
            self.validate_fixture(kit, modinfo)

    def test_docker_volume_isolated_by_workspace_and_sdk_hash(self) -> None:
        volume = prepare_r1_kernel.docker_volume_name(self.workspace, "a" * 64)
        self.assertNotEqual(volume, prepare_r1_kernel.docker_volume_name(self.workspace, "b" * 64))
        self.assertNotEqual(volume, prepare_r1_kernel.docker_volume_name(self.root / "other", "a" * 64))

    def test_auto_runtime_prefers_docker_when_both_are_available(self) -> None:
        with mock.patch.object(prepare_r1_kernel.shutil, "which", side_effect=lambda name: f"/bin/{name}"):
            self.assertEqual(prepare_r1_kernel.select_container_runtime("auto"), "docker")

    def test_auto_runtime_falls_back_to_podman(self) -> None:
        with mock.patch.object(prepare_r1_kernel.shutil, "which",
                               side_effect=lambda name: "/bin/podman" if name == "podman" else None):
            self.assertEqual(prepare_r1_kernel.select_container_runtime("auto"), "podman")

    def test_explicit_missing_runtime_fails_before_build(self) -> None:
        sdk = self.root / "ingenic-linux-kernel4.4.94-x1600-v6.0-20240606.tar.bz2"
        sdk.write_bytes(b"sdk")
        pin = json.loads(self.pin_path.read_text())
        pin["sdk_sha256"] = prepare_r1_kernel.sha256(sdk)
        self.pin_path.write_text(json.dumps(pin))

        def which(name):
            return "/bin/modinfo" if name == "modinfo" else None

        with mock.patch.object(prepare_r1_kernel.shutil, "which", side_effect=which):
            with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "requires podman"):
                prepare_r1_kernel.validate_sdk(sdk, True, "podman")

    def test_podman_command_uses_private_selinux_mounts_and_explicit_selection(self) -> None:
        sdk = self.root / "ingenic-linux-kernel4.4.94-x1600-v6.0-20240606.tar.bz2"
        command = prepare_r1_kernel.container_command("podman", self.workspace, sdk, "a" * 64, "optimized")
        self.assertEqual(command[0], "podman")
        self.assertIn("TAR_OPTIONS=--no-same-owner", command)
        self.assertIn(f"{self.workspace}:/work:Z", command)
        self.assertIn(f"{sdk}:/work/{sdk.name}:ro,Z", command)
        self.assertIn("--fragment", command)
        self.assertIn("--jobs", command)
        self.assertEqual(command[command.index("--jobs") + 1], "2")
        custom_jobs = prepare_r1_kernel.container_command(
            "podman", self.workspace, sdk, "a" * 64, "optimized", jobs=5
        )
        self.assertEqual(custom_jobs[custom_jobs.index("--jobs") + 1], "5")

    def test_cli_accepts_explicit_runtime(self) -> None:
        args = prepare_r1_kernel.parse_args([
            "--upstream", str(self.upstream), "--stock-kernel", str(self.stock),
            "--workspace", str(self.workspace), "--profile", "parity", "--container-runtime", "podman",
        ])
        self.assertEqual(args.container_runtime, "podman")

    def test_cli_jobs_default_and_custom_value(self) -> None:
        base = ["--upstream", str(self.upstream), "--stock-kernel", str(self.stock),
                "--workspace", str(self.workspace), "--profile", "parity"]
        self.assertEqual(prepare_r1_kernel.parse_args(base).jobs, 2)
        self.assertEqual(prepare_r1_kernel.parse_args(base + ["--jobs", "5"]).jobs, 5)

    def test_cli_rejects_nonpositive_jobs(self) -> None:
        base = ["--upstream", str(self.upstream), "--stock-kernel", str(self.stock),
                "--workspace", str(self.workspace), "--profile", "parity"]
        with self.assertRaises(SystemExit):
            prepare_r1_kernel.parse_args(base + ["--jobs", "0"])
        with self.assertRaises(SystemExit):
            prepare_r1_kernel.parse_args(base + ["--jobs", "-1"])

    def test_sdk_digest_must_match_pin(self) -> None:
        sdk = self.root / "ingenic-linux-kernel4.4.94-x1600-v6.0-20240606.tar.bz2"
        sdk.write_bytes(b"wrong SDK contents")
        with self.assertRaisesRegex(prepare_r1_kernel.PreparationError, "SHA-256 does not match"):
            prepare_r1_kernel.validate_sdk(sdk, False, "podman")
        self.assertFalse(self.workspace.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
