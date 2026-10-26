#!/usr/bin/env python3
"""Host-only tests for module ABI selection and symbol verification."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import subprocess
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).with_name("verify_module_abi.py")
SPEC = importlib.util.spec_from_file_location("verify_module_abi", SCRIPT)
assert SPEC and SPEC.loader
verify_module_abi = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify_module_abi)


class VerifyModuleAbiTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="verify-module-abi-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.built = self.root / "built"
        self.vendor = self.root / "vendor"
        self.built.mkdir()
        self.vendor.mkdir()
        self.map = self.root / "System.map"
        self.map.write_text(
            "00000001 T __ksymtab_kernel_api\n"
            "00000002 R not_an_export\n"
        )
        self.nm = self.root / "nm"
        self.nm.write_text(
            "#!/bin/sh\n"
            "last=\n"
            "for arg in \"$@\"; do last=$arg; done\n"
            "case \" $* \" in\n"
            "  *' -u '*) cat \"$last.undef\" ;;\n"
            "  *) cat \"$last.defs\" ;;\n"
            "esac\n"
        )
        self.nm.chmod(0o755)
        self.modinfo = self.root / "modinfo"
        self.modinfo.write_text("#!/bin/sh\ncat \"$3.vermagic\"\n")
        self.modinfo.chmod(0o755)
        self.vermagic = "4.4.94+ SMP preempt mod_unload MIPS32_R2 32BIT"

    def make_module(self, directory: pathlib.Path, name: str, *, defs: str = "", undef: str = "",
                    vermagic: str | None = None) -> pathlib.Path:
        module = directory / f"{name}.ko"
        module.write_bytes(f"fixture:{name}".encode())
        pathlib.Path(f"{module}.defs").write_text(defs)
        pathlib.Path(f"{module}.undef").write_text(undef)
        pathlib.Path(f"{module}.vermagic").write_text(vermagic or self.vermagic)
        return module

    def run_verify(self):
        return verify_module_abi.verify(self.map, self.built, self.vendor,
                                        str(self.nm), str(self.modinfo))

    def test_vendor_only_excludes_built_exports_as_providers(self) -> None:
        self.make_module(self.built, "helper", defs="00000001 T __ksymtab_built_api\n")
        self.make_module(self.vendor, "stock", undef="         U built_api\n")

        report = verify_module_abi.verify(self.map, self.built, self.vendor,
                                          str(self.nm), str(self.modinfo), vendor_only=True)

        self.assertFalse(report["valid"])
        self.assertTrue(report["vendor_only"])
        self.assertEqual(set(report["selected_modules"]), {"stock"})
        self.assertEqual(set(report["observed_built_modules_not_selected"]), {"helper"})
        self.assertEqual(report["missing_imports_by_module"], {"stock": ["built_api"]})

    def test_vendor_only_still_gates_observed_built_vermagic(self) -> None:
        helper = self.make_module(self.built, "helper")
        self.make_module(self.vendor, "stock")
        pathlib.Path(f"{helper}.vermagic").write_text("4.4.94+ SMP mod_unload MIPS32_R2 32BIT")

        report = verify_module_abi.verify(self.map, self.built, self.vendor,
                                          str(self.nm), str(self.modinfo), vendor_only=True)

        self.assertFalse(report["valid"])
        self.assertTrue(any("do not share one full vermagic" in error
                            for error in report["vermagic_errors"]))

    def test_vendor_versions_win_duplicates_and_kernel_only_additions_are_selected(self) -> None:
        self.make_module(self.built, "shared", defs="00000001 T __ksymtab_built_api\n")
        self.make_module(self.vendor, "shared", defs="00000001 T __ksymtab_vendor_api\n")
        self.make_module(self.built, "extra", undef="         U kernel_api\n         U local_api\n         w optional_api\n")
        self.make_module(self.vendor, "stock", defs="00000004 r __ksymtab_local_api\n",
                         undef="         U vendor_api\n")
        (self.map).write_text(self.map.read_text() + "00000003 T __ksymtab_vendor_api\n")

        report = self.run_verify()

        self.assertTrue(report["valid"])
        self.assertEqual(report["built_duplicates_skipped_in_favor_of_vendor"], ["shared"])
        self.assertEqual(set(report["selected_modules"]), {"shared", "extra", "stock"})
        self.assertEqual(report["selected_modules"]["shared"]["source"], "vendor")
        self.assertEqual(report["selected_modules"]["extra"]["undefined_weak"], ["optional_api"])
        self.assertIn("local_api", report["selected_modules"]["stock"]["exports"])
        self.assertEqual(report["system_map_sha256"], verify_module_abi.file_sha256(self.map))
        self.assertIn("kernel_api", report["kernel_exports"])

    def test_strong_unresolved_import_fails(self) -> None:
        self.make_module(self.vendor, "stock", undef="         U absent_api\n")
        report = self.run_verify()
        self.assertFalse(report["valid"])
        self.assertEqual(report["missing_imports_by_module"], {"stock": ["absent_api"]})

    def test_import_requires_exported_kernel_symbol_not_plain_definition(self) -> None:
        self.map.write_text("00000001 T __ksymtab_kernel_api\n00000002 T plain_definition\n")
        self.make_module(self.vendor, "stock", undef="         U plain_definition\n")
        report = self.run_verify()
        self.assertFalse(report["valid"])
        self.assertEqual(report["missing_imports_by_module"], {"stock": ["plain_definition"]})

    def test_full_vermagic_must_match_and_include_abi_tokens(self) -> None:
        self.make_module(self.vendor, "one")
        self.make_module(self.built, "two", vermagic="4.4.94+ SMP mod_unload MIPS32_R2 32BIT")
        report = self.run_verify()
        self.assertFalse(report["valid"])
        self.assertTrue(any("do not share one full vermagic" in error for error in report["vermagic_errors"]))
        self.assertTrue(any("preempt" in error for error in report["vermagic_errors"]))

    def test_vermagic_release_must_be_first_token(self) -> None:
        self.make_module(self.vendor, "stock", vermagic="4.4.94+other SMP preempt mod_unload MIPS32_R2 32BIT 4.4.94+")
        report = self.run_verify()
        self.assertFalse(report["valid"])
        self.assertTrue(any("vermagic release is '4.4.94+other'" in error for error in report["vermagic_errors"]))

    def test_vermagic_rejects_modversions(self) -> None:
        self.make_module(self.vendor, "stock", vermagic=self.vermagic + " modversions")
        report = self.run_verify()
        self.assertFalse(report["valid"])
        self.assertTrue(any("expected MODVERSIONS off" in error for error in report["vermagic_errors"]))

    def test_missing_module_directories_and_empty_dirs_are_rejected(self) -> None:
        with self.assertRaisesRegex(verify_module_abi.VerificationError, r"No \.ko modules"):
            self.run_verify()

    def test_cli_writes_json_and_returns_failure_on_missing_import(self) -> None:
        self.make_module(self.vendor, "stock", undef="         U absent_api\n")
        output = self.root / "nested/report.json"
        result = subprocess.run([
            "python3", str(SCRIPT), "--system-map", str(self.map),
            "--built-modules", str(self.built), "--vendor-modules", str(self.vendor),
            "--output", str(output), "--nm", str(self.nm), "--modinfo", str(self.modinfo),
        ], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(json.loads(output.read_text())["valid"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
