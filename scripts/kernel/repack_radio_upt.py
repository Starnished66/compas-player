#!/usr/bin/env python3
"""Create the pinned, host-only AP6212A radio candidate UPT.

This deliberately accepts one reviewed V3 overlay and one production V2 base.
It never writes to or communicates with a device. The output is an experimental
package and still requires the separately documented on-device validation.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from repack_kernel_upt import LIMIT, PackError, package, parse_update, run, sha, unpack_image, validate_uimage  # noqa: E402
from repack_driver_upt import changed_paths, file_manifest  # noqa: E402

BASE_SHA = "ba4816d812a79316b8b20d7c75f79b25804179eb2970ca134b20ae21c4f16806"
BASE_SIDECAR_SHA = "b04961b167385f6fce75f4828ff553d055444e4525a59749c14187869ebee480"
BASE_SOURCE_SHA = "9c2460a7247607ca4eab57b72bd676adc87b675d36f90b0ad1d4d927d9a61e58"
BASE_KERNEL_SHA = "02f646d746d11b3cc54e3f3f5bbbe578375ab1ca41076514c8306f930ef03490"
BASE_ROOTFS_SHA = "3944a4202678c50cf63324ffff1b4b7024944538cde02788c064c60e52701c45"
ASSEMBLY_SHA = "db52bdb0157e993b350a5d6c9c56186d0a01e4b11d8ceb930537c7bc770a06b9"
PROVENANCE_SHA = "f5183ddca22ce1b74a566175b290b33f69fcbb5a0ed27cfb23c31c82c9aaac3a"
ABI_SHA = "d318888fb7e625904c89a11af8f950e9aa85ec6ab33b855f827feb03b2664488"
PLAYER_SHA = "a1d84b5479c234ec4a119954ed6b6cfcbf42c5144cb8bc1df839340eb355d1d2"
MANIFEST_SHA = "449b87e2f41e5acba6559980c0fec31f1aa36d1ab428059d328c3180a6af1c69"
FIRMWARE_MANIFEST_SHA = "90a3159b0a43d5449ff61006f997eee838beb52d89a284e53930ad08292384d7"
REVIEW_FILES = {
    "opus-radio-r2-review.json": "925c123e1e767991fcf5a1a8c8b09f3b9372ea6b2ede8bb6cf26bebbdfe5df76",
    "opus-radio-provenance-review.json": "d3bdc4a4f1c576d56d58e11cd134b3fac4090462d7df9fae6db749416bf2dc60",
    "opus-radio-metadata-review.json": "cfd5466f70a6cefb33ab417c6f4ffc98edec09c88f8cf716fd544013f34c9d88",
}
PLAYER_REL = "usr/bin/compas_player"
REMOVED_LEGACY = {
    "module_driver/cywdhd.ko": "249ead31451fece8de2ffcb93cda8bd316a59295989a10e065d3446cc5b9bdb5",
    "module_driver/cywdhd.sh": "99c378ed142de72ca49756964d5a3cbff9e48ac94b957834a76745b636a5e3d9",
}
RETAINED = {"axp2101", "sau", "soc_fb", "soc_i2c", "soc_msc", "soc_pwm"}
BUILT_NORMAL = {
    "codec_cs43131", "cst8xx_touch", "cw2015", "i2c_gpio_add", "keyboard_adc_multifunc",
    "keyboard_gpio_add", "lcd_lg35583", "leds_pwm_add", "pwm_backlight", "rmem_manager",
    "sa_config_module", "sa_earpods_adc", "sa_hgl_dma", "sa_sound_switch", "soc_adc",
    "soc_aic", "soc_efuse", "soc_gpio", "soc_utils", "tcs1421_add", "utils",
    "x1600_hiby_r1_sound_card",
}
BUILT_DRIVER = BUILT_NORMAL | {"bcm_wlbt_power"}
PRIVATE_MODULES = {"brcmfmac", "brcmutil"}
MODULE_TOTAL = 31
SYMLINKS = {
    "lib/firmware/brcm/brcmfmac43430-sdio.bin": "/usr/data/.compas/wifi-firmware/selected.bin",
    "lib/firmware/brcm/brcmfmac43430-sdio.txt": "/usr/data/.compas/wifi-firmware/selected.txt",
}
FROZEN_DIRS_0755 = frozenset("""
etc etc/init.d lib lib/firmware lib/firmware/brcm lib/firmware/bt_bcm
lib/firmware/wifi_bcm module_driver usr usr/bin usr/lib usr/lib/compas-radio
usr/lib/compas-radio/modules usr/libexec usr/libexec/compas
usr/libexec/compas/radio-bt
""".split())
FROZEN_FILES_0755 = frozenset("""
etc/init.d/S43wifi_bcm_init_config
lib/firmware/bt_bcm/BCM4343A1_001.002.009.0122.0538.hcd
lib/firmware/wifi_bcm/cyw43438-7.46.58.35.bin
lib/firmware/wifi_bcm/fw_bcm43438a1.bin
lib/firmware/wifi_bcm/nvram_ap6212a.txt
module_driver/bcm_wlbt_power.sh module_driver/driver_default_init_script.sh
usr/bin/bt_enable_bsa.sh usr/bin/bt_init usr/bin/bt_resume usr/bin/bt_suspend
usr/bin/compas-radio usr/bin/compas-wifi-mac usr/bin/compas_player
usr/bin/wifi_down.sh usr/bin/wifi_off.sh usr/bin/wifi_on.sh usr/bin/wifi_up.sh
usr/libexec/compas/radio-bt/bt_init.vendor.sh
usr/libexec/compas/radio-bt/bt_resume.vendor.sh
usr/libexec/compas/radio-bt/bt_suspend.vendor.sh
usr/libexec/compas/wifi_off.brcmfmac.vendor.sh
usr/libexec/compas/wifi_on.brcmfmac.vendor.sh
""".split())
FROZEN_FILES_0644 = frozenset("""
EXPERIMENTAL-RADIO-MANIFEST.txt
lib/firmware/bt_bcm/BCM4343A1_001.002.009.1010.1030.hcd
lib/firmware/wifi_bcm/nvram_azw372.txt
module_driver/bcm_wlbt_power.ko module_driver/codec_cs43131.ko
module_driver/cst8xx_touch.ko module_driver/cw2015.ko
module_driver/i2c_gpio_add.ko module_driver/keyboard_adc_multifunc.ko
module_driver/keyboard_gpio_add.ko module_driver/lcd_lg35583.ko
module_driver/leds_pwm_add.ko module_driver/pwm_backlight.ko
module_driver/rmem_manager.ko module_driver/sa_config_module.ko
module_driver/sa_earpods_adc.ko module_driver/sa_hgl_dma.ko
module_driver/sa_sound_switch.ko module_driver/soc_adc.ko
module_driver/soc_aic.ko module_driver/soc_efuse.ko module_driver/soc_gpio.ko
module_driver/soc_utils.ko module_driver/tcs1421_add.ko module_driver/utils.ko
module_driver/x1600_hiby_r1_sound_card.ko
usr/lib/compas-radio/firmware-sha256
usr/lib/compas-radio/modules/brcmfmac.ko
usr/lib/compas-radio/modules/brcmutil.ko
""".split())


def frozen_payload_metadata(candidate_dir: pathlib.Path) -> dict[str, tuple[str, int, str | None]]:
    """Return the path/type/mode inventory that the reviewed V3 tree pins."""
    expected: dict[str, tuple[str, int, str | None]] = {}
    for rel in FROZEN_DIRS_0755:
        expected[rel] = ("directory", 0o755, None)
    for rel in FROZEN_FILES_0755:
        expected[rel] = ("file", 0o755, None)
    for rel in FROZEN_FILES_0644:
        expected[rel] = ("file", 0o644, None)
    for rel, target in SYMLINKS.items():
        expected[rel] = ("symlink", 0o777, target)
    if candidate_dir.is_symlink() or stat.S_IMODE(candidate_dir.lstat().st_mode) != 0o755:
        raise PackError("candidate root directory mode differs from the frozen V3 payload")
    actual: dict[str, tuple[str, int, str | None]] = {}
    for path in candidate_dir.rglob("*"):
        rel = path.relative_to(candidate_dir).as_posix()
        if rel == "CANDIDATE-ASSEMBLY.json":
            continue
        mode = stat.S_IMODE(path.lstat().st_mode)
        if path.is_symlink():
            actual[rel] = ("symlink", mode, os.readlink(path))
        elif path.is_dir():
            actual[rel] = ("directory", mode, None)
        elif path.is_file():
            actual[rel] = ("file", mode, None)
        else:
            raise PackError(f"candidate contains a special payload path: {rel}")
    if actual != expected:
        missing = sorted(set(expected) - set(actual))
        extra = sorted(set(actual) - set(expected))
        changed = sorted(path for path in set(expected) & set(actual)
                         if expected[path] != actual[path])
        raise PackError(f"candidate payload types/modes differ from frozen V3 inventory; "
                        f"missing={missing}, extra={extra}, changed={changed}")
    return actual


def regular(path: pathlib.Path, label: str) -> None:
    if path.is_symlink() or not path.is_file():
        raise PackError(f"{label} must be a regular file: {path}")


def read_json(path: pathlib.Path, label: str) -> dict:
    regular(path, label)
    try:
        result = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise PackError(f"invalid {label}: {exc}") from exc
    if not isinstance(result, dict):
        raise PackError(f"{label} must be an object")
    return result


def digest_json(value: object) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def ensure_allowed_delta(before: dict, after: dict, allowed: set[str]) -> set[str]:
    changed = changed_paths(before, after)
    unexpected = changed - allowed
    if unexpected:
        raise PackError(f"rootfs changed outside the approved radio overlay: {sorted(unexpected)}")
    return changed


def validate_candidate(candidate_dir: pathlib.Path) -> tuple[dict, dict[str, str], dict[str, pathlib.Path]]:
    if candidate_dir.is_symlink() or not candidate_dir.is_dir():
        raise PackError("candidate must be a non-symlink directory")
    frozen_payload_metadata(candidate_dir)
    assembly_path = candidate_dir / "CANDIDATE-ASSEMBLY.json"
    regular(assembly_path, "candidate assembly")
    if sha(assembly_path) != ASSEMBLY_SHA:
        raise PackError("candidate assembly is not the reviewed frozen V3 assembly")
    a = read_json(assembly_path, "candidate assembly")
    if (a.get("schema_version") != 1 or a.get("status") != "host-only candidate; not validated on device; not a flash/package artifact" or
            a.get("board") != "R1" or a.get("wifi_profile") != "ap6212a"):
        raise PackError("candidate is not the frozen host-only R1 AP6212A overlay")
    if a.get("provenance", {}).get("sha256") != PROVENANCE_SHA:
        raise PackError("candidate frozen build provenance hash mismatch")
    provenance = pathlib.Path(a["provenance"]["path"])
    regular(provenance, "frozen radio build provenance")
    if sha(provenance) != PROVENANCE_SHA:
        raise PackError("frozen radio build provenance changed")
    modules = a.get("modules")
    expected_module_files = {f"{n}.ko" for n in BUILT_DRIVER | PRIVATE_MODULES | RETAINED}
    if not isinstance(modules, dict) or len(modules) != MODULE_TOTAL or set(modules) != expected_module_files:
        raise PackError("candidate must pin exactly the reviewed 31-module ABI inventory")
    for name, value in modules.items():
        if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value):
            raise PackError(f"invalid candidate module digest: {name}")
    if a.get("retained_vendor_modules") != {f"{n}.ko": modules[f"{n}.ko"] for n in sorted(RETAINED)}:
        raise PackError("retained vendor module inventory mismatch")
    if a.get("module_abi_report", {}).get("sha256") != ABI_SHA or a["module_abi_report"].get("selected_count") != MODULE_TOTAL:
        raise PackError("candidate mixed-module ABI report pin mismatch")
    abi_path = pathlib.Path(a["module_abi_report"]["path"])
    abi = read_json(abi_path, "31-module ABI report")
    if sha(abi_path) != ABI_SHA or abi.get("valid") is not True or abi.get("selected_module_count") != MODULE_TOTAL:
        raise PackError("candidate ABI report is not valid or has changed")
    selected = abi.get("selected_modules")
    if not isinstance(selected, dict) or set(selected) != {name[:-3] for name in modules}:
        raise PackError("ABI report does not select the exact candidate 31-module set")
    if abi.get("missing_imports_by_module") or abi.get("vermagic_errors"):
        raise PackError("candidate ABI report contains unresolved imports or vermagic errors")
    for name, expected in modules.items():
        stem = name[:-3]
        row = selected[stem]
        expected_source = "vendor" if stem in RETAINED else "built"
        if not isinstance(row, dict) or row.get("source") != expected_source or row.get("sha256") != expected:
            raise PackError(f"ABI source/hash mismatch for {name}")
    if a.get("kernel", {}).get("ximage", {}).get("sha256") != "509ad6555602aedcb08451e004be10daf032fe1f3c13fbef6cf122d4d4d13428":
        raise PackError("candidate kernel hash mismatch")
    kernel = pathlib.Path(a["kernel"]["ximage"]["path"])
    regular(kernel, "candidate xImage")
    if sha(kernel) != a["kernel"]["ximage"]["sha256"]:
        raise PackError("candidate xImage bytes changed")
    validate_uimage(kernel)
    player = a.get("player", {})
    player_path = pathlib.Path(player.get("path", ""))
    regular(player_path, "reviewed candidate player")
    if player.get("overlay_path") != PLAYER_REL or player.get("sha256") != PLAYER_SHA or sha(player_path) != PLAYER_SHA:
        raise PackError("candidate player is not the one reviewed frozen binary")
    if a.get("payload_metadata") != {"EXPERIMENTAL-RADIO-MANIFEST.txt": MANIFEST_SHA,
                                      "usr/lib/compas-radio/firmware-sha256": FIRMWARE_MANIFEST_SHA}:
        raise PackError("candidate metadata files are not the reviewed frozen payload")
    for filename, expected in REVIEW_FILES.items():
        path = pathlib.Path(__file__).resolve().parents[2] / ".." / "compas-driver-candidates-20261009" / filename
        regular(path, "Opus host-only review report")
        if sha(path) != expected:
            raise PackError(f"review report changed: {filename}")
    payload_hashes: dict[str, str] = {}
    payload_hashes.update(a.get("runtime_files", {}))
    payload_hashes.update(a.get("payload_metadata", {}))
    payload_hashes.update({f"module_driver/{n}": h for n, h in modules.items() if n[:-3] in BUILT_DRIVER})
    payload_hashes.update({f"usr/lib/compas-radio/modules/{n}.ko": modules[f"{n}.ko"] for n in PRIVATE_MODULES})
    payload_hashes.update(a.get("base_root", {}).get("firmware_files", {}))
    payload_hashes[PLAYER_REL] = PLAYER_SHA
    if set(payload_hashes) != (set(a.get("runtime_files", {})) | set(a.get("payload_metadata", {})) |
            {f"module_driver/{n}.ko" for n in BUILT_DRIVER} |
            {f"usr/lib/compas-radio/modules/{n}.ko" for n in PRIVATE_MODULES} |
            set(a.get("base_root", {}).get("firmware_files", {})) | {PLAYER_REL}):
        raise PackError("candidate payload inventory calculation mismatch")
    sources: dict[str, pathlib.Path] = {}
    for rel, expected in payload_hashes.items():
        if pathlib.PurePosixPath(rel).is_absolute() or ".." in pathlib.PurePosixPath(rel).parts:
            raise PackError(f"unsafe candidate payload path: {rel}")
        src = candidate_dir.joinpath(*pathlib.PurePosixPath(rel).parts)
        regular(src, f"candidate payload {rel}")
        if sha(src) != expected:
            raise PackError(f"candidate payload hash mismatch: {rel}")
        sources[rel] = src
    for rel, target in SYMLINKS.items():
        path = candidate_dir.joinpath(*pathlib.PurePosixPath(rel).parts)
        if not path.is_symlink() or os.readlink(path) != target:
            raise PackError(f"candidate firmware symlink mismatch: {rel}")
        sources[rel] = path
    actual = {p.relative_to(candidate_dir).as_posix() for p in candidate_dir.rglob("*") if not p.is_dir() and p.name != "CANDIDATE-ASSEMBLY.json"}
    if actual != set(sources):
        raise PackError("candidate contains unexpected or missing payload paths")
    return a, payload_hashes, sources


def unpack_base(base: pathlib.Path, work: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path, dict, str, dict]:
    regular(base, "production base UPT")
    if sha(base) != BASE_SHA:
        raise PackError("base UPT is not the pinned production V2 package")
    side_path = base.with_suffix(base.suffix + ".json")
    side = read_json(side_path, "production base UPT sidecar")
    if sha(side_path) != BASE_SIDECAR_SHA or side.get("output_upt_sha256") != BASE_SHA or side.get("base_upt_sha256") != BASE_SOURCE_SHA or side.get("kernel_sha256") != BASE_KERNEL_SHA or side.get("rootfs_after_sha256") != BASE_ROOTFS_SHA or side.get("rootfs_unchanged_except_replacements") is not True:
        raise PackError("base sidecar does not identify the reviewed V2 package")
    iso = work / "base-iso"
    iso.mkdir()
    run(["7z", "x", "-y", str(base), f"-o{iso}"])
    ota = iso / "ota_v0"
    if not ota.is_dir():
        raise PackError("base UPT lacks ota_v0")
    meta = parse_update(ota / "ota_update.in")
    for kind, image in (("kernel", "xImage"), ("rootfs", "rootfs.squashfs")):
        row = meta[kind]
        if row.get("img_name") != image or int(row.get("img_size", "0")) <= 0 or not re.fullmatch(r"[0-9a-f]{32}", row.get("img_md5", "")):
            raise PackError(f"invalid base {kind} OTA metadata")
        unpack_image(ota, image, int(row["img_size"]), row["img_md5"], work / image)
    if sha(work / "xImage") != BASE_KERNEL_SHA:
        raise PackError("base kernel payload hash mismatch")
    validate_uimage(work / "xImage")
    if sha(work / "rootfs.squashfs") != BASE_ROOTFS_SHA:
        raise PackError("base compressed rootfs hash mismatch")
    root = work / "base-root"
    root.mkdir()
    run(["unsquashfs", "-no-xattrs", "-d", str(root), str(work / "rootfs.squashfs")])
    return root, ota, meta, sha(work / "rootfs.squashfs"), side


def merge_root(root: pathlib.Path, candidate: pathlib.Path, a: dict, payload: dict[str, str], sources: dict[str, pathlib.Path]) -> dict:
    before = file_manifest(root)
    candidate_dirs = {p.relative_to(candidate).as_posix(): p for p in candidate.rglob("*") if p.is_dir()}
    new_dirs: set[str] = set()
    for rel, source in sorted(candidate_dirs.items(), key=lambda pair: (pair[0].count("/"), pair[0])):
        destination = root.joinpath(*pathlib.PurePosixPath(rel).parts)
        if not destination.exists():
            destination.mkdir(parents=True, exist_ok=True)
            new_dirs.add(rel)
            os.chmod(destination, stat.S_IMODE(source.stat().st_mode))
    for rel, expected in REMOVED_LEGACY.items():
        path = root.joinpath(*pathlib.PurePosixPath(rel).parts)
        regular(path, f"legacy Wi-Fi bypass {rel}")
        if sha(path) != expected:
            raise PackError(f"legacy Wi-Fi bypass differs from pinned base: {rel}")
        path.unlink()
    # The candidate player is the only non-radio executable allowed to change.
    for rel, src in sources.items():
        dst = root.joinpath(*pathlib.PurePosixPath(rel).parts)
        dst.parent.mkdir(parents=True, exist_ok=True)
        if src.is_symlink():
            if dst.exists() or dst.is_symlink():
                dst.unlink()
            dst.symlink_to(os.readlink(src))
        else:
            shutil.copyfile(src, dst)
            shutil.copystat(src, dst, follow_symlinks=True)
    after = file_manifest(root)
    changed = changed_paths(before, after)
    allowed = set(payload) | set(REMOVED_LEGACY) | set(SYMLINKS) | new_dirs
    # Existing paths whose candidate bytes are identical do not count as changes.
    changed = ensure_allowed_delta(before, after, allowed)
    for rel, digest in payload.items():
        row = after.get(rel)
        if not isinstance(row, dict) or row.get("type") != "file" or row.get("sha256") != digest:
            raise PackError(f"merged rootfs payload mismatch: {rel}")
    module_dir = root / "module_driver"
    names = {p.stem for p in module_dir.glob("*.ko")}
    expected_driver = BUILT_DRIVER | RETAINED
    if names != expected_driver or "cywdhd" in names:
        raise PackError("merged module_driver must contain exactly 29 candidate modules and no legacy cywdhd")
    for name, digest in a["modules"].items():
        if name[:-3] in BUILT_DRIVER:
            path = module_dir / name
        elif name[:-3] in PRIVATE_MODULES:
            path = root / "usr/lib/compas-radio/modules" / name
        else:
            path = module_dir / name
        if not path.is_file() or sha(path) != digest:
            raise PackError(f"module set is not complete at final payload path: {name}")
    for rel, digest in a["base_root"]["firmware_files"].items():
        # These are already shipped by the pinned base and must remain byte-identical.
        if sha(root.joinpath(*pathlib.PurePosixPath(rel).parts)) != digest:
            raise PackError(f"pinned firmware file changed: {rel}")
    preserved_paths = sorted(set(before) - changed)
    preserved_manifest = {path: before[path] for path in preserved_paths}
    return {"changed_paths": sorted(changed), "before_manifest_sha256": digest_json(before),
            "after_manifest_sha256": digest_json(after), "removed_legacy_paths": REMOVED_LEGACY.copy(),
            "preserved_paths": preserved_paths,
            "preserved_paths_manifest_sha256": digest_json(preserved_manifest)}


def create(base: pathlib.Path, candidate: pathlib.Path, output: pathlib.Path) -> pathlib.Path:
    if output.exists() or output.is_symlink() or output.with_suffix(output.suffix + ".json").exists():
        raise PackError("output UPT and sidecar must be fresh")
    for tool in ("7z", "unsquashfs", "mksquashfs", "genisoimage"):
        if not shutil.which(tool):
            raise PackError(f"required tool unavailable: {tool}")
    base = base.resolve(strict=True)
    candidate = candidate.resolve(strict=True)
    a, payload_hashes, payload_sources = validate_candidate(candidate)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="repack-radio-", dir=output.parent) as tmp:
        work = pathlib.Path(tmp)
        root, ota, meta, base_root_hash, side = unpack_base(base, work)
        original = file_manifest(root)
        change = merge_root(root, candidate, a, payload_hashes, payload_sources)
        preserved = {"usr/bin/compas_bootloader": original.get("usr/bin/compas_bootloader"),
                     "plugin_named_assets_manifest_sha256": digest_json({k: v for k, v in original.items() if "v1.1" in k.lower() or "plugin" in k.lower()})}
        if not isinstance(preserved["usr/bin/compas_bootloader"], dict):
            raise PackError("base package missing bootloader executable")
        # Remove only validated old payload chunk chains.
        for prefix in ("xImage.", "ota_md5_xImage.", "rootfs.squashfs.", "ota_md5_rootfs.squashfs."):
            for path in list(ota.iterdir()):
                if path.name.startswith(prefix):
                    if path.is_symlink() or not path.is_file():
                        raise PackError(f"unsafe OTA chunk entry: {path.name}")
                    path.unlink()
        kernel_size, kernel_md5 = package(pathlib.Path(a["kernel"]["ximage"]["path"]), ota, "xImage")
        new_rootfs = work / "radio-rootfs.squashfs"
        run(["mksquashfs", str(root), str(new_rootfs), "-comp", "lzo", "-all-root", "-noappend", "-no-xattrs", "-processors", "2"])
        rootfs_size, rootfs_md5 = package(new_rootfs, ota, "rootfs.squashfs")
        meta["kernel"].update(img_size=str(kernel_size), img_md5=kernel_md5)
        meta["rootfs"].update(img_size=str(rootfs_size), img_md5=rootfs_md5)
        lines = ["ota_version=0"]
        for kind in ("kernel", "rootfs"):
            row = meta[kind]
            lines += [f"img_type={kind}", f"img_name={row['img_name']}", f"img_size={row['img_size']}", f"img_md5={row['img_md5']}"]
        (ota / "ota_update.in").write_text("\n".join(lines) + "\n")
        staged = work / "radio-candidate.upt"
        run(["genisoimage", "-f", "-U", "-J", "-joliet-long", "-r", "-allow-lowercase", "-allow-multidot", "-o", str(staged), str(work / "base-iso")])
        if staged.stat().st_size > LIMIT:
            raise PackError("radio candidate UPT exceeds 45 MiB")
        verify_iso = work / "verify-iso"
        verify_iso.mkdir()
        run(["7z", "x", "-y", str(staged), f"-o{verify_iso}"])
        final_ota = verify_iso / "ota_v0"
        final_meta = parse_update(final_ota / "ota_update.in")
        for kind, name in (("kernel", "xImage"), ("rootfs", "rootfs.squashfs")):
            row = final_meta[kind]
            unpack_image(final_ota, name, int(row["img_size"]), row["img_md5"], work / f"final-{name}")
        if sha(work / "final-xImage") != a["kernel"]["ximage"]["sha256"]:
            raise PackError("round-trip kernel does not match candidate xImage")
        validate_uimage(work / "final-xImage")
        final_root = work / "final-root"
        final_root.mkdir()
        run(["unsquashfs", "-no-xattrs", "-d", str(final_root), str(work / "final-rootfs.squashfs")])
        if changed_paths(file_manifest(root), file_manifest(final_root)):
            raise PackError("rootfs content, symlink targets, or modes changed in squashfs round-trip")
        final_files = file_manifest(final_root)
        preserved_paths = change["preserved_paths"]
        preserved_after = {path: final_files.get(path) for path in preserved_paths}
        if digest_json(preserved_after) != change["preserved_paths_manifest_sha256"]:
            raise PackError("a rootfs path outside the explicit radio changes was modified")
        if final_files.get("usr/bin/compas_player", {}).get("sha256") != PLAYER_SHA:
            raise PackError("round-trip player hash mismatch")
        if final_files.get("usr/bin/compas_bootloader") != preserved["usr/bin/compas_bootloader"]:
            raise PackError("production bootloader changed")
        if digest_json({k: v for k, v in final_files.items() if "v1.1" in k.lower() or "plugin" in k.lower()}) != preserved["plugin_named_assets_manifest_sha256"]:
            raise PackError("plugin-named rootfs assets changed")
        if final_files.get("module_driver/cywdhd.ko") is not None or final_files.get("module_driver/cywdhd.sh") is not None:
            raise PackError("legacy cywdhd module/bypass survived package assembly")
        for rel, digest in payload_hashes.items():
            if final_files.get(rel, {}).get("sha256") != digest:
                raise PackError(f"round-trip overlay payload mismatch: {rel}")
        for rel, target in SYMLINKS.items():
            path = final_root.joinpath(*pathlib.PurePosixPath(rel).parts)
            if not path.is_symlink() or os.readlink(path) != target:
                raise PackError(f"round-trip firmware symlink mismatch: {rel}")
        os.replace(staged, output)
        sidecar = output.with_suffix(output.suffix + ".json")
        sidecar.write_text(json.dumps({
            "schema_version": 1,
            "candidate_type": "host-only experimental R1 AP6212A radio UPT; not device validated",
            "base_upt_sha256": BASE_SHA, "base_sidecar_sha256": BASE_SIDECAR_SHA,
            "base_kernel_sha256": BASE_KERNEL_SHA, "base_rootfs_sha256": base_root_hash,
            "candidate_assembly_sha256": ASSEMBLY_SHA, "candidate_provenance_sha256": PROVENANCE_SHA,
            "candidate_kernel_sha256": a["kernel"]["ximage"]["sha256"],
            "candidate_abi_report_sha256": ABI_SHA, "candidate_module_sha256": a["modules"],
            "candidate_provider_source_sha256": a["provider_source"]["sha256"],
            "candidate_kernel_lifetime_patch_sha256": a["kernel"]["lifetime_patch"]["sha256"],
            "candidate_player_sha256": PLAYER_SHA, "review_reports_sha256": REVIEW_FILES,
            "review_model": "claude-opus-5-5",
            "rootfs_changes": {k: v for k, v in change.items() if k != "preserved_paths"},
            "rootfs_unchanged_outside_explicit_radio_changes": True,
            "preserved_production_payload": preserved,
            "removed_legacy_wifi_bypass_sha256": REMOVED_LEGACY,
            "module_count": MODULE_TOTAL, "wifi_profile": "ap6212a",
            "output_upt_sha256": sha(output), "output_upt_size": output.stat().st_size,
        }, indent=2, sort_keys=True) + "\n")
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-upt", type=pathlib.Path, required=True)
    parser.add_argument("--candidate", type=pathlib.Path, required=True,
                        help="the exact reviewed radio-overlay-ap6212a-host-final-v3 directory")
    parser.add_argument("--output", type=pathlib.Path, required=True,
                        help="fresh output path; package remains host-only and unvalidated")
    args = parser.parse_args()
    try:
        print(create(args.base_upt, args.candidate, args.output))
    except (OSError, KeyError, TypeError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"repack_radio_upt.py: {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
