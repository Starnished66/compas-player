#!/usr/bin/env python3
"""Check that repack rejects malformed early-splash ELF inputs pre-unpack."""
import pathlib
import struct
import subprocess
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[4]
REPACK = REPO / "scripts/firmware/package/repack_upt.sh"


def elf(*, flags=0x70001000, phnum=1, phentsize=32, p_type=1,
        p_offset=84, p_filesz=16, p_memsz=16):
    data = bytearray(100)
    data[:16] = b"\x7fELF" + bytes((1, 1, 1, 0)) + bytes(8)
    struct.pack_into("<HHIIIIIHHHHHH", data, 16,
                     2, 8, 1, 0x10000, 52, 0, flags,
                     52, phentsize, phnum, 0, 0, 0)
    if phnum and phentsize >= 32 and 52 + phentsize <= len(data):
        struct.pack_into("<8I", data, 52, p_type, p_offset, 0x10000,
                         0x10000, p_filesz, p_memsz, 5, 4)
    return bytes(data)


def main():
    cases = {
        "no-program-headers": (elf(phnum=0), "must be a valid MIPS ET_EXEC"),
        "wrong-phdr-size": (elf(phentsize=36), "must be a valid MIPS ET_EXEC"),
        "no-load-segment": (elf(p_type=4), "has no PT_LOAD"),
        "invalid-mips-abi": (elf(flags=0x70002000), "supported MIPS O32 ABI and ISA"),
        "unsupported-mips-arch": (elf(flags=0x90001000), "supported MIPS O32 ABI and ISA"),
        "micro-mips": (elf(flags=0x71001000), "supported MIPS O32 ABI and ISA"),
        "segment-file-out-of-bounds": (elf(p_offset=96, p_filesz=16), "out-of-bounds"),
        "load-filesz-exceeds-memsz": (elf(p_filesz=16, p_memsz=8), "invalid PT_LOAD size"),
        "dynamic": (elf(p_type=2), "statically linked"),
        "interpreter": (elf(p_type=3), "statically linked"),
    }
    with tempfile.TemporaryDirectory(prefix="repack-early-splash-") as temp:
        root = pathlib.Path(temp)
        dummy = root / "dummy"
        dummy.write_bytes(b"present")
        marker = root / "7z-was-called"
        fake_bin = root / "bin"
        fake_bin.mkdir()
        for command in ("unsquashfs", "mksquashfs", "genisoimage", "md5sum",
                        "sha256sum", "split", "file"):
            stub = fake_bin / command
            stub.write_text("#!/bin/sh\nexit 99\n")
            stub.chmod(0o755)
        fake_7z = fake_bin / "7z"
        fake_7z.write_text(f"#!/bin/sh\nprintf called > '{marker}'\nexit 87\n")
        fake_7z.chmod(0o755)
        env = dict(__import__("os").environ)
        env["PATH"] = str(fake_bin) + ":" + env.get("PATH", "")

        def invoke(helper):
            return subprocess.run(
                [str(REPACK), "--board", "r1", "--early-splash", str(helper),
                 str(dummy), str(dummy), str(dummy), str(root / "out.upt")],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False, env=env,
            )

        for name, (image, expected_reason) in cases.items():
            helper = root / name
            helper.write_bytes(image)
            result = invoke(helper)
            if result.returncode == 0 or expected_reason not in result.stdout:
                raise AssertionError(f"{name}: repacker proceeded past ELF validation:\n{result.stdout}")
            if marker.exists():
                raise AssertionError(f"{name}: repacker invoked 7z before rejection")

        valid = root / "valid-static-mips-o32"
        valid.write_bytes(elf())
        result = invoke(valid)
        if result.returncode != 87 or not marker.exists():
            raise AssertionError(f"valid control did not reach unpack step:\n{result.stdout}")
    print("early splash ELF validation tests passed")


if __name__ == "__main__":
    main()
