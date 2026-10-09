import hashlib
import pathlib
import struct
import tempfile
import unittest
import zlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from repack_kernel_upt import PackError, require_stock_kernel, validate_uimage


class StockKernelGateTests(unittest.TestCase):
    def test_valid_uimage_with_wrong_stock_hash_is_rejected(self) -> None:
        payload = b"valid payload"
        fields = [0x27051956, 0, 0, len(payload), 0x80F00000, 0x80F00000,
                  zlib.crc32(payload) & 0xffffffff]
        tail = bytes((5, 5, 2, 0)) + b"test" + b"\0" * 28
        header = bytearray(struct.pack(">7I", *fields) + tail)
        fields[1] = zlib.crc32(header) & 0xffffffff
        header = struct.pack(">7I", *fields) + tail
        with tempfile.TemporaryDirectory() as temp:
            image = pathlib.Path(temp) / "valid-xImage"
            image.write_bytes(header + payload)
            validate_uimage(image)
            wrong_hash = hashlib.sha256(b"different stock kernel").hexdigest()
            with self.assertRaisesRegex(PackError, "base kernel SHA-256 mismatch"):
                require_stock_kernel(image, wrong_hash)


if __name__ == "__main__":
    unittest.main()
