import pathlib
import json
import shutil
import sys
import tempfile
import unittest
import hashlib
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import repack_radio_upt as radio


class RadioPackerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.candidate = (pathlib.Path(__file__).resolve().parents[2] / ".." /
                         "compas-driver-candidates-20261009/phase2-bootstrap-corrected/"
                         "radio-overlay-ap6212a-host-final-v3").resolve()

    def copy_candidate(self, directory):
        target = pathlib.Path(directory) / "candidate"
        shutil.copytree(self.candidate, target, symlinks=True)
        assembly_path = target / "CANDIDATE-ASSEMBLY.json"
        original_bytes = assembly_path.read_bytes()
        self.assertEqual(hashlib.sha256(original_bytes).hexdigest(), radio.ASSEMBLY_SHA)
        assembly = json.loads(original_bytes)
        frozen_player = target / radio.PLAYER_REL
        self.assertEqual(hashlib.sha256(frozen_player.read_bytes()).hexdigest(), radio.PLAYER_SHA)
        # The immutable candidate embeds its reviewed player. Its provenance
        # points at a mutable build output that unrelated app builds replace.
        # Relocate only this copied fixture's path to the pinned embedded bytes.
        assembly["player"]["path"] = str(frozen_player.resolve())
        assembly_path.write_text(json.dumps(assembly))
        return target

    def validate_fixture(self, candidate, *, abi_sha=None):
        assembly_hash = hashlib.sha256((candidate / "CANDIDATE-ASSEMBLY.json").read_bytes()).hexdigest()
        with mock.patch.object(radio, "ASSEMBLY_SHA", assembly_hash):
            if abi_sha is None:
                return radio.validate_candidate(candidate)
            with mock.patch.object(radio, "ABI_SHA", abi_sha):
                return radio.validate_candidate(candidate)

    def test_delta_allows_only_explicit_radio_paths(self):
        before = {
            "module_driver/cywdhd.ko": {"type": "file", "sha256": "old"},
            "usr/bin/compas_player": {"type": "file", "sha256": "old-player"},
            "etc/keep": {"type": "file", "sha256": "same"},
        }
        after = {
            "usr/bin/compas_player": {"type": "file", "sha256": "pinned-player"},
            "etc/keep": {"type": "file", "sha256": "same"},
            "usr/bin/compas-radio": {"type": "file", "sha256": "pinned-script"},
        }
        changed = radio.ensure_allowed_delta(before, after, {
            "module_driver/cywdhd.ko", "usr/bin/compas_player", "usr/bin/compas-radio",
        })
        self.assertEqual(changed, {
            "module_driver/cywdhd.ko", "usr/bin/compas_player", "usr/bin/compas-radio",
        })

    def test_delta_rejects_unlisted_rootfs_mutation(self):
        before = {"usr/lib/plugin.lua": {"type": "file", "sha256": "original"}}
        after = {"usr/lib/plugin.lua": {"type": "file", "sha256": "changed"}}
        with self.assertRaisesRegex(radio.PackError, "outside the approved radio overlay"):
            radio.ensure_allowed_delta(before, after, {"usr/bin/compas-radio"})

    def test_output_must_be_fresh(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = pathlib.Path(temporary) / "existing.upt"
            output.touch()
            with self.assertRaisesRegex(radio.PackError, "must be fresh"):
                radio.create(pathlib.Path("missing.upt"), pathlib.Path("missing-candidate"), output)

    def test_candidate_is_exact_frozen_v3_and_full_abi_set(self):
        if not self.candidate.is_dir():
            self.skipTest("external frozen candidate workspace is unavailable")
        with tempfile.TemporaryDirectory() as temporary:
            candidate = self.copy_candidate(temporary)
            assembly, payload, sources = self.validate_fixture(candidate)
        self.assertEqual(len(assembly["modules"]), 31)
        self.assertEqual(len(payload), 52)
        self.assertEqual(len(sources), 54)
        self.assertTrue(set(payload) <= set(sources))
        self.assertEqual(len(radio.frozen_payload_metadata(self.candidate)), 70)

    def test_runtime_executable_mode_tampering_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            candidate = self.copy_candidate(temporary)
            player = candidate / radio.PLAYER_REL
            player.chmod(0o644)
            with self.assertRaisesRegex(radio.PackError, "types/modes differ"):
                self.validate_fixture(candidate)

    def test_payload_hash_tampering_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            candidate = self.copy_candidate(temporary)
            manifest = candidate / "EXPERIMENTAL-RADIO-MANIFEST.txt"
            manifest.write_text(manifest.read_text() + "tampered\n")
            with self.assertRaisesRegex(radio.PackError, "payload hash mismatch"):
                self.validate_fixture(candidate)

    def test_extra_file_and_symlink_payloads_are_rejected(self):
        for kind in ("file", "symlink"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary:
                candidate = self.copy_candidate(temporary)
                extra = candidate / "usr/bin/unreviewed-extra"
                if kind == "file":
                    extra.write_text("unexpected payload\n")
                else:
                    extra.symlink_to("compas_player")
                with self.assertRaisesRegex(radio.PackError, "types/modes differ"):
                    self.validate_fixture(candidate)

    def test_abi_wrong_source_is_rejected_even_with_updated_hash_pins(self):
        with tempfile.TemporaryDirectory() as temporary:
            candidate = self.copy_candidate(temporary)
            assembly_path = candidate / "CANDIDATE-ASSEMBLY.json"
            assembly = json.loads(assembly_path.read_text())
            abi_path = pathlib.Path(temporary) / "altered-abi.json"
            abi = json.loads(pathlib.Path(assembly["module_abi_report"]["path"]).read_text())
            abi["selected_modules"]["bcm_wlbt_power"]["source"] = "vendor"
            abi_path.write_text(json.dumps(abi))
            abi_hash = hashlib.sha256(abi_path.read_bytes()).hexdigest()
            assembly["module_abi_report"]["path"] = str(abi_path)
            assembly["module_abi_report"]["sha256"] = abi_hash
            assembly_path.write_text(json.dumps(assembly))
            with self.assertRaisesRegex(radio.PackError, "ABI source/hash mismatch for bcm_wlbt_power.ko"):
                self.validate_fixture(candidate, abi_sha=abi_hash)

    def test_bad_base_and_review_pins_are_rejected(self):
        base = (pathlib.Path(__file__).resolve().parents[2] / ".." /
                "compas-r1-custom-drivers-20261008/flash/r1-custom-drivers-v2.upt").resolve()
        if not base.is_file():
            self.skipTest("pinned production V2 base is unavailable")
        with tempfile.TemporaryDirectory() as temporary:
            with mock.patch.object(radio, "BASE_SHA", "0" * 64):
                with self.assertRaisesRegex(radio.PackError, "not the pinned production V2"):
                    radio.unpack_base(base, pathlib.Path(temporary))
        with mock.patch.dict(radio.REVIEW_FILES, {"opus-radio-r2-review.json": "0" * 64}):
            with self.assertRaisesRegex(radio.PackError, "review report changed"):
                with tempfile.TemporaryDirectory() as temporary:
                    candidate = self.copy_candidate(temporary)
                    self.validate_fixture(candidate)


if __name__ == "__main__":
    unittest.main()
