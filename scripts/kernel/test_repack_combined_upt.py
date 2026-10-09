import pathlib
import json
import hashlib
import struct
import shutil
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import repack_combined_upt as combined
import repack_radio_upt as radio


class CombinedPackerTests(unittest.TestCase):
    def relocated_radio_candidate(self, directory):
        source = (pathlib.Path(__file__).resolve().parents[2] / ".." /
                  "compas-driver-candidates-20261009/phase2-bootstrap-corrected/"
                  "radio-overlay-ap6212a-host-final-v3").resolve()
        candidate = pathlib.Path(directory) / "radio-candidate"
        shutil.copytree(source, candidate, symlinks=True)
        assembly_path = candidate / "CANDIDATE-ASSEMBLY.json"
        original = assembly_path.read_bytes()
        self.assertEqual(combined.sha(assembly_path), radio.ASSEMBLY_SHA)
        assembly = json.loads(original)
        player = candidate / radio.PLAYER_REL
        self.assertEqual(combined.sha(player), radio.PLAYER_SHA)
        # Use the player embedded in the immutable candidate. The assembly's
        # original absolute path names mutable compas_player_target build output.
        assembly["player"]["path"] = str(player.resolve())
        assembly_path.write_text(json.dumps(assembly))
        return candidate, combined.sha(assembly_path)

    def valid_origins(self):
        return {name: {"source": "built" if name in combined.COMBINED_BUILT else "vendor"}
                for name in combined.COMBINED_STEMS}

    def test_exact_26_built_5_vendor_origins(self):
        selected = combined.validate_module_origins(self.valid_origins())
        self.assertEqual(len(selected), 31)
        self.assertEqual(sum(row["source"] == "built" for row in selected.values()), 26)
        self.assertEqual(sum(row["source"] == "vendor" for row in selected.values()), 5)

    def test_source_built_soc_fb_is_required(self):
        selected = self.valid_origins()
        selected["soc_fb"]["source"] = "vendor"
        with self.assertRaisesRegex(combined.PackError, "source class mismatch: soc_fb"):
            combined.validate_module_origins(selected)

    def test_abi_inventory_rejects_missing_or_legacy_module(self):
        selected = self.valid_origins()
        del selected["soc_fb"]
        selected["cywdhd"] = {"source": "vendor"}
        with self.assertRaisesRegex(combined.PackError, r"exact 26 built \+ five retained"):
            combined.validate_module_origins(selected)

    def test_outside_allowlist_mutation_is_rejected(self):
        before = {"usr/lib/plugin.lua": {"type": "file", "sha256": "old"}}
        after = {"usr/lib/plugin.lua": {"type": "file", "sha256": "new"}}
        with self.assertRaisesRegex(radio.PackError, "outside the approved radio overlay"):
            radio.ensure_allowed_delta(before, after, {"module_driver/soc_fb.ko"})

    def test_radio_candidate_and_display_review_are_frozen(self):
        source_candidate = (pathlib.Path(__file__).resolve().parents[2] / ".." /
                            "compas-driver-candidates-20261009/phase2-bootstrap-corrected/"
                            "radio-overlay-ap6212a-host-final-v3")
        if not source_candidate.is_dir():
            self.skipTest("external frozen radio workspace unavailable")
        with tempfile.TemporaryDirectory() as temporary:
            candidate, assembly_hash = self.relocated_radio_candidate(temporary)
            with mock.patch.object(radio, "ASSEMBLY_SHA", assembly_hash):
                assembly, _, _ = radio.validate_candidate(candidate.resolve())
            workspace = (pathlib.Path(__file__).resolve().parents[2] / ".." /
                         "compas-r1-combined-build-v1-20261009")
            if not (workspace / "preparation.json").is_file():
                self.skipTest("fresh combined build workspace unavailable")
            repo = pathlib.Path(__file__).resolve().parents[2]
            review_path = (repo / combined.DISPLAY_REVIEW_PATH).resolve()
            self.assertEqual(combined.sha(review_path), combined.DISPLAY_REVIEW_SHA)
            reviewed_sources = json.loads(review_path.read_text())["source_sha256"]
            lifetime_patch = repo / "firmware/kernel/wifi-patches/compas-mmc-radio-lifetime.patch"
            if combined.sha(lifetime_patch) != combined.RADIO_LIFETIME_PATCH_SHA:
                # A new kernel fix must obtain fresh clearance instead of
                # silently reusing the historical radio candidate's review.
                with self.assertRaisesRegex(combined.PackError, "MMC lifetime patch bytes changed"):
                    combined.validate_preparation(workspace.resolve(), repo, assembly)
                return
            if any(combined.sha(pathlib.Path(path)) != digest
                   for path, digest in reviewed_sources.items()):
                # A later optimization must not silently reuse the older build's
                # clearance. The immutable candidate remains pinned to its review.
                with self.assertRaisesRegex(combined.PackError, "reviewed display source changed"):
                    combined.validate_preparation(workspace.resolve(), repo, assembly)
                return
            prep, validation, _, _, _, display_sources, review_path = combined.validate_preparation(
                workspace.resolve(), repo, assembly)
            self.assertEqual(len(display_sources), 12)
            self.assertEqual(combined.sha(review_path), combined.DISPLAY_REVIEW_SHA)
            self.assertEqual(validation["required_module_count"], 26)

    def test_output_must_be_fresh(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = pathlib.Path(temporary) / "candidate.upt"
            output.touch()
            with self.assertRaisesRegex(combined.PackError, "must all be fresh"):
                combined.create(pathlib.Path("missing.upt"), pathlib.Path("missing-radio"),
                                pathlib.Path("missing-build"), output)

    def test_new_fast_display_review_uses_normalized_relative_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            repo = root / "repo"
            workspace = root / "workspace"
            repo.mkdir(); workspace.mkdir()
            source_names = ["Makefile", "hiby.symvers"] + [f"source_{i}.c" for i in range(10)]
            source_map = {}
            records = []
            copied_hashes = {}
            for name in source_names:
                relative = f"firmware/kernel/display-experimental/{name}"
                local = repo / relative
                local.parent.mkdir(parents=True, exist_ok=True)
                local.write_bytes(("reviewed:" + name).encode())
                digest = combined.sha(local)
                copied = workspace / "display-sources" / name
                copied.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(local, copied)
                source_map[relative] = digest
                copied_hashes[relative] = digest
                records.append({"original_path": relative, "sha256": digest,
                                "prepared_sha256": digest, "copied_to": f"display-sources/{name}"})
            report = {"status": "source_review_passed_controlled_experimental_host_scope",
                      "model": "gpt-6-astra",
                      "source_sha256": source_map}
            report_path = repo / "fast-review.json"
            report_path.write_text(json.dumps(report))
            (repo / "legacy-review.json").write_text("{}")
            prep = {"display_sources": records}
            validation = {"display_source_sha256": copied_hashes}
            with mock.patch.object(combined, "DISPLAY_REVIEW_PATH", pathlib.Path("legacy-review.json")), \
                 mock.patch.object(combined, "FAST_DISPLAY_REVIEW_PATH", pathlib.Path("fast-review.json")), \
                 mock.patch.object(combined, "FAST_DISPLAY_REVIEW_SHA", combined.sha(report_path)):
                reviewed, selected_path = combined.validate_display_review(
                    prep, validation, workspace, repo, pathlib.Path("fast-review.json"))
            self.assertEqual(reviewed, source_map)
            self.assertEqual(selected_path, report_path.resolve())

    def test_review_source_paths_reject_traversal_and_external_absolute_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            repo = pathlib.Path(temporary) / "repo"
            source = repo / "firmware/kernel/display-experimental/Makefile"
            source.parent.mkdir(parents=True)
            source.write_text("source")
            self.assertEqual(combined.review_source_path(
                "firmware/kernel/display-experimental/Makefile", repo)[0],
                "firmware/kernel/display-experimental/Makefile")
            with self.assertRaisesRegex(combined.PackError, "unsafe reviewed display source"):
                combined.review_source_path("firmware/kernel/display-experimental/../../outside", repo)
            outside = pathlib.Path(temporary) / "outside.c"
            outside.write_text("outside")
            with self.assertRaisesRegex(combined.PackError, "outside this checkout"):
                combined.review_source_path(str(outside), repo)
            link = repo / "firmware/kernel/display-experimental/link.c"
            link.symlink_to(source.name)
            with self.assertRaisesRegex(combined.PackError, "non-symlink"):
                combined.review_source_path("firmware/kernel/display-experimental/link.c", repo)

    @staticmethod
    def elf32_mips(flags=0x1000):
        data = bytearray(40)
        data[:7] = b"\x7fELF\x01\x02\x01"
        data[16:24] = struct.pack(">HHI", 2, 8, 1)
        data[36:40] = struct.pack(">I", flags)
        return bytes(data)

    def test_mips_o32_binary_gate_rejects_random_and_n32_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            valid = root / "valid"
            valid.write_bytes(self.elf32_mips())
            combined.validate_mips_o32(valid, "fixture")
            random = root / "random"
            random.write_bytes(b"not ELF")
            with self.assertRaisesRegex(combined.PackError, "ELF32"):
                combined.validate_mips_o32(random, "fixture")
            n32 = root / "n32"
            n32.write_bytes(self.elf32_mips(0x1020))
            with self.assertRaisesRegex(combined.PackError, "o32"):
                combined.validate_mips_o32(n32, "fixture")

    def test_boot_logging_requires_matching_review_build_sources_and_binary_hashes(self):
        with tempfile.TemporaryDirectory() as temporary:
            repo = pathlib.Path(temporary)
            artifact = repo / "boot-artifact"
            artifact.mkdir()
            sources = {}
            for relative in combined.BOOT_LOGGING_SOURCE_PATHS:
                path = repo / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(("source:" + relative).encode())
                sources[relative] = combined.sha(path)
            binaries = {"compas_player": self.elf32_mips(),
                        "compas_bootloader": self.elf32_mips() + b"bootloader"}
            binary_hashes = {}
            for name, data in binaries.items():
                (artifact / "bin").mkdir(exist_ok=True)
                path = artifact / "bin" / name
                path.write_bytes(data)
                binary_hashes[name] = combined.sha(path)
            manifest = {"sources_sha256": sources, "binaries_sha256": binary_hashes,
                        "build_passed": True, "device_installed": False,
                        "build": "make -j4 target bootloader"}
            manifest_path = artifact / "build-manifest.json"
            manifest_path.write_text(json.dumps(manifest, sort_keys=True))
            review = {"source_sha256": sources, "status": "source_review_passed", "model": "gpt-6-astra"}
            review_path = artifact / "review.json"
            review_path.write_text(json.dumps(review, sort_keys=True))
            with mock.patch.object(combined, "BOOT_LOGGING_DIR", pathlib.Path("boot-artifact")), \
                 mock.patch.object(combined, "BOOT_LOGGING_REVIEW_PATH", pathlib.Path("boot-artifact/review.json")), \
                 mock.patch.object(combined, "BOOT_LOGGING_MANIFEST_SHA", combined.sha(manifest_path)), \
                 mock.patch.object(combined, "BOOT_LOGGING_REVIEW_SHA", combined.sha(review_path)), \
                 mock.patch.object(combined, "BOOT_LOGGING_BINARY_HASHES", binary_hashes):
                result = combined.validate_boot_logging_binaries(artifact, repo)
                self.assertEqual(result["binary_sha256"], binary_hashes)
                self.assertEqual(result["source_sha256"], sources)
                bad_dir = repo / "other-artifact"
                bad_dir.mkdir()
                with self.assertRaisesRegex(combined.PackError, "frozen reviewed artifact directory"):
                    combined.validate_boot_logging_binaries(bad_dir, repo)

    def test_boot_logging_overlay_changes_exactly_the_two_executables(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            target = root / "rootfs"
            (target / "usr/bin").mkdir(parents=True)
            (target / "usr/bin/compas_player").write_bytes(b"old player")
            (target / "usr/bin/compas_bootloader").write_bytes(b"old bootloader")
            player = root / "new-player"
            bootloader = root / "new-bootloader"
            player.write_bytes(b"new player")
            bootloader.write_bytes(b"new bootloader")
            pins = {"compas_player": combined.sha(player), "compas_bootloader": combined.sha(bootloader)}
            base = combined.file_manifest(target)
            combined.apply_boot_logging_overlay(target, {
                "player": player, "bootloader": bootloader, "binary_sha256": pins,
            })
            after = combined.file_manifest(target)
            combined.require_exact_boot_logging_delta(base, after)
            self.assertEqual(after["usr/bin/compas_player"]["sha256"], pins["compas_player"])
            self.assertEqual(after["usr/bin/compas_bootloader"]["sha256"], pins["compas_bootloader"])

            unexpected = dict(after)
            unexpected["usr/bin/extra"] = {"type": "file", "mode": 0o755, "sha256": "0" * 64}
            with self.assertRaisesRegex(combined.PackError, "must change exactly"):
                combined.require_exact_boot_logging_delta(base, unexpected)


class CombinedFrozenInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = pathlib.Path(__file__).resolve().parents[2]
        radio_dir = cls.repo.parent / "compas-driver-candidates-20261009/phase2-bootstrap-corrected"
        cls.assembly = json.loads((radio_dir / "radio-overlay-ap6212a-host-final-v3/CANDIDATE-ASSEMBLY.json").read_text())
        cls.provenance, cls.frozen_prep = combined.load_frozen_radio_preparation(cls.assembly)
        cls.prep = json.loads((cls.repo.parent / "compas-r1-combined-build-v1-20261009/preparation.json").read_text())
        cls.validation = cls.prep["build_validation"]

    def fixture_paths(self, temporary):
        root = pathlib.Path(temporary)
        workspace = root / "workspace"
        repo = root / "repo"
        workspace.mkdir()
        repo.mkdir()
        for record in self.frozen_prep["local_patches"]:
            relative = pathlib.Path(record["original_path"])
            local = repo / relative
            local.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.repo / relative, local)
            copied = workspace / record["copied_to"]
            copied.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(self.repo / relative, copied)
        return workspace, repo

    def test_frozen_sdk_local_patches_and_kernel_hashes_match(self):
        combined.validate_frozen_sdk(self.prep, self.provenance)
        with tempfile.TemporaryDirectory() as temporary:
            workspace, repo = self.fixture_paths(temporary)
            combined.validate_local_patches(self.prep, self.frozen_prep, workspace, repo)
        combined.validate_frozen_kernel_pins(self.validation, self.assembly)

    def test_sdk_hash_tampering_is_rejected(self):
        prep = json.loads(json.dumps(self.prep))
        prep["upstream"]["sdk_sha256"] = "0" * 64
        with self.assertRaisesRegex(combined.PackError, "SDK does not match"):
            combined.validate_frozen_sdk(prep, self.provenance)
        prep = json.loads(json.dumps(self.prep))
        prep["sdk"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(combined.PackError, "SDK does not match"):
            combined.validate_frozen_sdk(prep, self.provenance)

    def test_local_patch_record_order_and_hash_tampering_are_rejected(self):
        prep = json.loads(json.dumps(self.prep))
        prep["local_patches"].reverse()
        with tempfile.TemporaryDirectory() as temporary:
            workspace, repo = self.fixture_paths(temporary)
            with self.assertRaisesRegex(combined.PackError, "records differ"):
                combined.validate_local_patches(prep, self.frozen_prep, workspace, repo)
        prep = json.loads(json.dumps(self.prep))
        prep["local_patches"][0]["sha256"] = "0" * 64
        with tempfile.TemporaryDirectory() as temporary:
            workspace, repo = self.fixture_paths(temporary)
            with self.assertRaisesRegex(combined.PackError, "records differ"):
                combined.validate_local_patches(prep, self.frozen_prep, workspace, repo)

    def test_current_or_copied_local_patch_byte_tampering_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            workspace, repo = self.fixture_paths(temporary)
            local = repo / self.frozen_prep["local_patches"][0]["original_path"]
            local.write_bytes(local.read_bytes() + b"tampered")
            with self.assertRaisesRegex(combined.PackError, "patch bytes differ"):
                combined.validate_local_patches(self.prep, self.frozen_prep, workspace, repo)
        with tempfile.TemporaryDirectory() as temporary:
            workspace, repo = self.fixture_paths(temporary)
            copied = workspace / self.frozen_prep["local_patches"][1]["copied_to"]
            copied.write_bytes(copied.read_bytes() + b"tampered")
            with self.assertRaisesRegex(combined.PackError, "patch bytes differ"):
                combined.validate_local_patches(self.prep, self.frozen_prep, workspace, repo)

    def test_config_and_system_map_hash_tampering_are_rejected(self):
        validation = dict(self.validation)
        validation["compiled_config_sha256"] = "0" * 64
        with self.assertRaisesRegex(combined.PackError, "config or System.map"):
            combined.validate_frozen_kernel_pins(validation, self.assembly)
        validation = dict(self.validation)
        validation["system_map_sha256"] = "0" * 64
        with self.assertRaisesRegex(combined.PackError, "config or System.map"):
            combined.validate_frozen_kernel_pins(validation, self.assembly)


if __name__ == "__main__":
    unittest.main()
