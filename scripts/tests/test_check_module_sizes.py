#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "check_module_sizes.py"


class ModuleSizeCheckerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="module-size-")
        self.root = Path(self.temp.name)
        self.git("init", "-q")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "user.name", "Test")

    def tearDown(self):
        self.temp.cleanup()

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.root), *args], check=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def put(self, rel, data):
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def run_checker(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.root), *args],
                              text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_inventory_includes_tracked_and_nonignored_untracked_once(self):
        self.put("tracked.c", b"a\n")
        self.put("untracked.tsx", b"b")
        self.put("ignored.py", b"x\n" * 600)
        self.put(".gitignore", b"ignored.py\n")
        self.git("add", "tracked.c", ".gitignore")
        self.git("commit", "-qm", "fixture")
        result = self.run_checker("--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = json.loads(result.stdout)
        self.assertEqual([row["path"] for row in rows], ["tracked.c", "untracked.tsx"])
        self.assertEqual(rows[0]["lines"], 1)
        self.assertEqual(rows[1]["lines"], 1)

    def test_deleted_tracked_source_is_skipped(self):
        source = self.put("moving.c", b"old\n")
        self.git("add", "moving.c")
        self.git("commit", "-qm", "tracked source")
        source.unlink()
        result = self.run_checker("--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), [])

    def test_physical_lines_count_unterminated_final_line(self):
        self.put("plain.lua", b"one\ntwo")
        self.assertEqual(json.loads(self.run_checker("--json").stdout)[0]["lines"], 2)

    def test_classification_is_reported_and_external_only_fails_when_requested(self):
        paths = {
            "src/main.c": b"x\n" * 501,
            "third_party/lib/code.cpp": b"x\n" * 501,
            "src/ui/sonic/local.c": b"x\n",
            "src/ui/i18n_catalog.c": b"x\n" * 501,
            "patches/lvgl_generated_fonts/font.h": b"x\n" * 501,
        }
        for rel, data in paths.items():
            self.put(rel, data)
        result = self.run_checker("--json", "--check")
        self.assertEqual(result.returncode, 1)
        rows = {row["path"]: row for row in json.loads(result.stdout)}
        self.assertEqual(rows["src/main.c"]["classification"], "maintained")
        self.assertEqual(rows["third_party/lib/code.cpp"]["classification"], "vendored")
        self.assertEqual(rows["src/ui/sonic/local.c"]["classification"], "maintained")
        self.assertEqual(rows["src/ui/i18n_catalog.c"]["classification"], "generated/data")
        self.assertEqual(rows["patches/lvgl_generated_fonts/font.h"]["classification"], "generated/data")
        self.assertEqual(self.run_checker("--check", "--include-external").returncode, 1)

    def test_external_only_offenders_are_skipped_by_default_check(self):
        self.put("sonic/upstream.c", b"x\n" * 501)
        self.put("src/ui/i18n_catalog.c", b"x\n" * 501)
        self.assertEqual(self.run_checker("--check").returncode, 0)
        self.assertEqual(self.run_checker("--check", "--include-external").returncode, 1)

    def test_exact_limit_passes_and_one_over_fails(self):
        self.put("exact.sh", b"x\n" * 500)
        self.assertEqual(self.run_checker("--check").returncode, 0)
        self.put("over.sh", b"x\n" * 500 + b"last")
        self.assertEqual(self.run_checker("--check").returncode, 1)

    def test_nonpositive_limit_is_rejected(self):
        result = self.run_checker("--limit", "0")
        self.assertEqual(result.returncode, 2)
        self.assertIn("positive", result.stderr)


if __name__ == "__main__":
    unittest.main()
