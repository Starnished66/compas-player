import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from repack_driver_upt import (  # noqa: E402
    PackError, REPLACEMENT_NAMES, file_manifest, require_only_replacements,
)


class RootfsChangeGateTests(unittest.TestCase):
    def test_unexpected_rootfs_file_change_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp) / "root"
            modules = root / "module_driver"
            modules.mkdir(parents=True)
            for name in REPLACEMENT_NAMES:
                (modules / f"{name}.ko").write_bytes(b"vendor")
            (root / "etc").mkdir()
            config = root / "etc" / "boot.conf"
            config.write_bytes(b"unchanged")
            before = file_manifest(root)
            for name in REPLACEMENT_NAMES:
                (modules / f"{name}.ko").write_bytes(b"reviewed rebuilt module")
            after = file_manifest(root)
            require_only_replacements(before, after)
            config.write_bytes(b"unexpected edit")
            with self.assertRaisesRegex(PackError, "outside selected 22 modules"):
                require_only_replacements(before, file_manifest(root))


if __name__ == "__main__":
    unittest.main()
