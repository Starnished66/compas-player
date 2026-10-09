#!/usr/bin/env python3
"""Replace only the kernel payload in a validated R1 UPT package."""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import shutil
import struct
import subprocess
import tempfile
import zlib

LIMIT = 45 * 1024 * 1024
CHUNK = 512 * 1024
REVIEW_SHA = "fa8135760f41473e752ad8a533b15e6ed99b141fff9fa928789c88304c29aae5"
STOCK_SHA = "e3ed77e551afb061433b57c39ea10ebcfd862842436e51c6a7db66c737f4778a"
KERNEL_SHA = "02f646d746d11b3cc54e3f3f5bbbe578375ab1ca41076514c8306f930ef03490"


class PackError(ValueError):
    pass


def sha(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def run(args: list[str]) -> None:
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)


def validate_uimage(path: pathlib.Path) -> None:
    data = path.read_bytes()
    if len(data) < 64:
        raise PackError("kernel image is shorter than a legacy uImage header")
    fields = struct.unpack(">7I4B32s", data[:64])
    magic, hcrc, timestamp, size, load, entry, dcrc, os_id, arch, typ, comp, name = fields
    header = bytearray(data[:64]); header[4:8] = b"\0" * 4
    if magic != 0x27051956 or (zlib.crc32(header) & 0xffffffff) != hcrc:
        raise PackError("kernel uImage magic/header CRC invalid")
    if size != len(data) - 64 or (zlib.crc32(data[64:]) & 0xffffffff) != dcrc:
        raise PackError("kernel uImage payload length/data CRC invalid")
    if (os_id, arch, typ) != (5, 5, 2):
        raise PackError("kernel uImage must be Linux/MIPS/kernel")


def require_stock_kernel(path: pathlib.Path, expected: str = STOCK_SHA) -> str:
    actual = sha(path)
    if actual != expected:
        raise PackError(f"base kernel SHA-256 mismatch: expected {expected}, got {actual}")
    return actual


def parse_update(path: pathlib.Path) -> dict[str, dict[str, str]]:
    result: dict[str, dict[str, str]] = {}
    current: dict[str, str] = {}
    for line in path.read_text().splitlines() + [""]:
        if line.startswith("img_type=") and current.get("img_type"):
            kind = current["img_type"]
            if kind in result: raise PackError(f"duplicate OTA image type {kind}")
            result[kind] = current; current = {}
        if not line.strip():
            if current:
                kind = current.get("img_type", "")
                if kind in result:
                    raise PackError(f"duplicate OTA image type {kind}")
                result[kind] = current
                current = {}
        elif "=" in line:
            k, v = line.split("=", 1); current[k.strip()] = v.strip()
    if set(result) != {"kernel", "rootfs"}:
        raise PackError("OTA metadata must contain kernel and rootfs")
    return result


def unpack_image(ota: pathlib.Path, name: str, expected_size: int, expected_md5: str, dest: pathlib.Path) -> None:
    chain_path = ota / f"ota_md5_{name}.{expected_md5}"
    if not chain_path.is_file():
        raise PackError(f"missing {name} md5 chain")
    hashes = chain_path.read_text().splitlines()
    if not hashes or any(not re.fullmatch(r"[0-9a-f]{32}", x) for x in hashes):
        raise PackError(f"invalid {name} md5 chain")
    matches = sorted(ota.glob(f"{name}.[0-9][0-9][0-9][0-9].*") , key=lambda p: p.name)
    if len(matches) != len(hashes):
        raise PackError(f"{name} chunk count does not match chain")
    previous = expected_md5
    md = hashlib.md5(); size = 0
    with dest.open("wb") as out:
        for i, p in enumerate(matches):
            m = re.fullmatch(re.escape(name) + r"\.(\d{4})\.([0-9a-f]{32})", p.name)
            if not m or int(m.group(1)) != i or m.group(2) != previous or not p.is_file():
                raise PackError(f"invalid {name} chunk name/chain at {p.name}")
            b = p.read_bytes(); digest = hashlib.md5(b).hexdigest()
            if digest != hashes[i]: raise PackError(f"{name} chunk MD5 mismatch at {p.name}")
            previous = digest; size += len(b); md.update(b); out.write(b)
    if size != expected_size or md.hexdigest() != expected_md5:
        raise PackError(f"{name} reconstructed size or MD5 mismatch")


def package(image: pathlib.Path, ota: pathlib.Path, name: str) -> tuple[int, str]:
    digest = hashlib.md5(image.read_bytes()).hexdigest(); size = image.stat().st_size
    chain = ota / f"ota_md5_{name}.{digest}"
    hashes: list[str] = []; previous = digest
    with image.open("rb") as src:
        i = 0
        while block := src.read(CHUNK):
            part_hash = hashlib.md5(block).hexdigest(); hashes.append(part_hash)
            (ota / f"{name}.{i:04d}.{previous}").write_bytes(block)
            previous = part_hash; i += 1
    chain.write_text("".join(x + "\n" for x in hashes))
    return size, digest


def create(base: pathlib.Path, workspace: pathlib.Path, output: pathlib.Path) -> pathlib.Path:
    if output.exists(): raise PackError(f"output must not exist: {output}")
    if output.with_suffix(output.suffix + ".json").exists(): raise PackError("output sidecar already exists")
    for tool in ("7z", "genisoimage"):
        if not shutil.which(tool): raise PackError(f"required tool unavailable: {tool}")
    prep_path = workspace / "preparation.json"
    prep = json.loads(prep_path.read_text())
    review = prep["astra_review"]
    report = workspace / review["report"]
    if review.get("status") != "source_static_review_passed" or review.get("model") != "gpt-6-astra" or sha(report) != REVIEW_SHA or review.get("sha256") != REVIEW_SHA:
        raise PackError("workspace does not have the required final Astra PASS review")
    validation = prep.get("build_validation", {})
    kernel = workspace / "hiby-custom-kernel/out/xImage-compas-r1"
    if validation.get("ximage_sha256") != KERNEL_SHA or sha(kernel) != KERNEL_SHA:
        raise PackError("built kernel does not match reviewed preparation build_validation")
    stock = prep.get("stock_kernel", {})
    if stock.get("sha256") != STOCK_SHA or stock.get("size") != 3731520:
        raise PackError("preparation stock kernel identity mismatch")
    validate_uimage(kernel)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="repack-kernel-") as tmp:
        root = pathlib.Path(tmp); extracted = root / "base"; extracted.mkdir()
        run(["7z", "x", "-y", str(base), f"-o{extracted}"])
        ota = extracted / "ota_v0"
        if not ota.is_dir(): raise PackError("base UPT has no ota_v0")
        meta = parse_update(ota / "ota_update.in")
        for kind, name in (("kernel", "xImage"), ("rootfs", "rootfs.squashfs")):
            item = meta[kind]
            if item.get("img_name") != name: raise PackError(f"unexpected {kind} image name")
            size = int(item["img_size"]); md = item["img_md5"].lower()
            if size <= 0 or not re.fullmatch(r"[0-9a-f]{32}", md): raise PackError(f"invalid {kind} metadata")
            unpack_image(ota, name, size, md, root / name)
        base_kernel_hash = require_stock_kernel(root / "xImage", stock["sha256"])
        validate_uimage(root / "xImage")
        root_hash = sha(root / "rootfs.squashfs")
        # Keep every extracted entry except old kernel chunks and its chain.
        for p in list(ota.iterdir()):
            if p.name.startswith("xImage.") or p.name.startswith("ota_md5_xImage."):
                p.unlink()
        ksize, kmd = package(kernel, ota, "xImage")
        old = meta["kernel"]; meta["kernel"] = {**old, "img_size": str(ksize), "img_md5": kmd}
        lines = ["ota_version=0"]
        for kind in ("kernel", "rootfs"):
            item = meta[kind]
            lines.extend((f"img_type={kind}", f"img_name={item['img_name']}", f"img_size={item['img_size']}", f"img_md5={item['img_md5']}"))
        (ota / "ota_update.in").write_text("\n".join(lines) + "\n")
        # Ensure rootfs bytes and vendor payload files survive extraction/reassembly.
        root_check = root / "root-check.squashfs"
        unpack_image(ota, "rootfs.squashfs", int(meta["rootfs"]["img_size"]), meta["rootfs"]["img_md5"], root_check)
        if sha(root_check) != root_hash: raise PackError("rootfs compressed bytes changed")
        run(["genisoimage", "-f", "-U", "-J", "-joliet-long", "-r", "-allow-lowercase", "-allow-multidot", "-o", str(output), str(extracted)])
        if output.stat().st_size > LIMIT: raise PackError("packed UPT exceeds 45 MiB")
        verify = root / "verify"; verify.mkdir(); run(["7z", "x", "-y", str(output), f"-o{verify}"])
        verify_ota = verify / "ota_v0"; vmeta = parse_update(verify_ota / "ota_update.in")
        unpack_image(verify_ota, "xImage", int(vmeta["kernel"]["img_size"]), vmeta["kernel"]["img_md5"], root / "final-kernel")
        unpack_image(verify_ota, "rootfs.squashfs", int(vmeta["rootfs"]["img_size"]), vmeta["rootfs"]["img_md5"], root / "final-rootfs")
        if sha(root / "final-kernel") != KERNEL_SHA or sha(root / "final-rootfs") != root_hash:
            raise PackError("final ISO payload verification failed")
        validate_uimage(root / "final-kernel")
        sidecar = output.with_suffix(output.suffix + ".json")
        sidecar.write_text(json.dumps({"base_upt_sha256": sha(base), "base_kernel_sha256": base_kernel_hash,
            "kernel_sha256": KERNEL_SHA, "rootfs_sha256": root_hash, "rootfs_unchanged": True,
            "review_sha256": REVIEW_SHA, "output_upt_sha256": sha(output),
            "output_upt_size": output.stat().st_size}, indent=2) + "\n")
    return output


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--base-upt", type=pathlib.Path, required=True)
    p.add_argument("--kernel-workspace", type=pathlib.Path, required=True)
    p.add_argument("--output", type=pathlib.Path, required=True)
    a = p.parse_args()
    try:
        result = create(a.base_upt, a.kernel_workspace, a.output)
    except (OSError, KeyError, ValueError, subprocess.CalledProcessError) as exc:
        p.exit(1, f"repack_kernel_upt: {exc}\n")
    print(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
