#!/usr/bin/env python3
"""Replace reviewed R1 driver modules in a custom-kernel UPT image.

The review manifest must contain ``status``, ``model``, ``report``,
``report_sha256``, ``modules`` (exactly the 22 selected module-name to SHA-256
entries), and ``sources`` (source-path to SHA-256 entries). The report itself
must be an external, hash-pinned review artifact. No package is made until all
reviewed source and binary hashes match.
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
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
from repack_kernel_upt import (  # noqa: E402
    LIMIT, PackError, package, parse_update, run, sha, unpack_image, validate_uimage,
)

BASE_SHA256 = "9c2460a7247607ca4eab57b72bd676adc87b675d36f90b0ad1d4d927d9a61e58"
KERNEL_SHA256 = "02f646d746d11b3cc54e3f3f5bbbe578375ab1ca41076514c8306f930ef03490"
REPLACEMENT_NAMES = {
    "codec_cs43131", "cst8xx_touch", "cw2015", "i2c_gpio_add",
    "keyboard_adc_multifunc", "keyboard_gpio_add", "lcd_lg35583",
    "leds_pwm_add", "pwm_backlight", "rmem_manager", "sa_config_module",
    "sa_earpods_adc", "sa_hgl_dma", "sa_sound_switch", "soc_adc", "soc_aic",
    "soc_efuse", "soc_gpio", "soc_utils", "tcs1421_add", "utils",
    "x1600_hiby_r1_sound_card",
}
RETAIN_NAMES = {"axp2101", "cywdhd", "sau", "soc_fb", "soc_i2c", "soc_msc", "soc_pwm"}


def file_manifest(root: pathlib.Path) -> dict[str, dict[str, object]]:
    result: dict[str, dict[str, object]] = {}
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root).as_posix()
        mode = stat.S_IMODE(path.lstat().st_mode)
        if path.is_symlink():
            result[rel] = {"type": "symlink", "mode": mode, "target": os.readlink(path)}
        elif path.is_dir():
            result[rel] = {"type": "directory", "mode": mode}
        elif path.is_file():
            result[rel] = {"type": "file", "mode": mode, "sha256": sha(path)}
        else:
            raise PackError(f"unsupported special file in rootfs: {rel}")
    return result


def changed_paths(before: dict[str, dict[str, object]], after: dict[str, dict[str, object]]) -> set[str]:
    return {p for p in before.keys() | after.keys() if before.get(p) != after.get(p)}


def require_only_replacements(before: dict[str, dict[str, object]], after: dict[str, dict[str, object]]) -> None:
    allowed = {f"module_driver/{name}.ko" for name in REPLACEMENT_NAMES}
    changed = changed_paths(before, after)
    if changed != allowed:
        raise PackError(f"rootfs tree changed outside selected 22 modules: {sorted(changed ^ allowed)}")


def reviewed_files(manifest_path: pathlib.Path, workspace: pathlib.Path) -> tuple[dict[str, str], dict[str, str], str]:
    review = json.loads(manifest_path.read_text())
    if review.get("status") != "source_static_review_passed" or review.get("model") != "gpt-6-astra":
        raise PackError("driver review manifest must record Astra source_static_review_passed")
    report_path = pathlib.Path(review["report"])
    if not report_path.is_absolute():
        report_path = manifest_path.parent / report_path
    report_hash = str(review["report_sha256"]).lower()
    if not re.fullmatch(r"[0-9a-f]{64}", report_hash) or sha(report_path) != report_hash:
        raise PackError("driver review report SHA-256 mismatch")
    modules = review.get("modules")
    sources = review.get("sources")
    if not isinstance(modules, dict) or set(modules) != REPLACEMENT_NAMES:
        raise PackError("review manifest must hash exactly the selected 22 modules")
    if not isinstance(sources, dict) or not sources:
        raise PackError("review manifest must include source path hashes")
    module_hashes: dict[str, str] = {}
    for name, digest in modules.items():
        digest = str(digest).lower()
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise PackError(f"invalid reviewed module hash for {name}")
        module_hashes[name] = digest
    source_hashes: dict[str, str] = {}
    for raw_path, raw_hash in sources.items():
        path = pathlib.Path(raw_path)
        if not path.is_absolute(): path = workspace / path
        path = path.resolve(strict=True)
        digest = str(raw_hash).lower()
        if not path.is_file() or not re.fullmatch(r"[0-9a-f]{64}", digest) or sha(path) != digest:
            raise PackError(f"reviewed source hash mismatch: {raw_path}")
        source_hashes[str(path)] = digest
    return module_hashes, source_hashes, report_hash


def verify_module_set(kernel_workspace: pathlib.Path, module_dir: pathlib.Path, empty_dir: pathlib.Path,
                      report_path: pathlib.Path) -> dict[str, object]:
    sys.path.insert(0, str(HERE))
    import verify_module_abi
    result = verify_module_abi.verify(
        kernel_workspace / "hiby-custom-kernel/out/System.map-compas-r1",
        empty_dir, module_dir, shutil.which("nm") or "nm",
        shutil.which("modinfo") or "modinfo", vendor_only=True,
    )
    report_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    if not result.get("valid") or result.get("selected_module_count") != 29:
        raise PackError("mixed 29-module set failed kernel symbol/vermagic verification")
    actual = result.get("selected_modules", {})
    for name in REPLACEMENT_NAMES:
        if actual.get(name, {}).get("sha256") != sha(module_dir / f"{name}.ko"):
            raise PackError(f"ABI report did not verify selected replacement {name}")
    return result


def create(base: pathlib.Path, workspace: pathlib.Path, vendor_root: pathlib.Path,
           review_manifest: pathlib.Path, output: pathlib.Path,
           driver_modules: pathlib.Path | None = None) -> pathlib.Path:
    if output.exists() or output.with_suffix(output.suffix + ".json").exists():
        raise PackError("output and sidecar must not already exist")
    for tool in ("7z", "unsquashfs", "mksquashfs", "genisoimage"):
        if not shutil.which(tool): raise PackError(f"required tool unavailable: {tool}")
    base = base.resolve(strict=True); workspace = workspace.resolve(strict=True)
    vendor_root = vendor_root.resolve(strict=True)
    if driver_modules is None:
        driver_modules = workspace / "hiby-custom-kernel/out/modules-compas-r1"
    driver_modules = driver_modules.resolve(strict=True)
    if not driver_modules.is_dir(): raise PackError("driver module source must be a directory")
    if sha(base) != BASE_SHA256: raise PackError("base UPT SHA-256 does not match approved custom-kernel package")
    base_sidecar = base.with_suffix(base.suffix + ".json")
    base_info = json.loads(base_sidecar.read_text())
    if base_info.get("output_upt_sha256") != BASE_SHA256 or base_info.get("kernel_sha256") != KERNEL_SHA256:
        raise PackError("base UPT sidecar does not identify approved custom kernel")
    module_hashes, source_hashes, review_hash = reviewed_files(review_manifest, workspace)
    built_dir = driver_modules
    actual_built = {p.stem: p for p in built_dir.glob("*.ko")}
    if not REPLACEMENT_NAMES.issubset(actual_built) or set(actual_built) != REPLACEMENT_NAMES | {"bcm_wlbt_power"}:
        raise PackError("build output module set no longer matches reviewed 23-module inventory")
    for name in REPLACEMENT_NAMES:
        if sha(actual_built[name]) != module_hashes[name]:
            raise PackError(f"built module hash differs from reviewed module: {name}")
    vendor_modules = {p.stem: p for p in (vendor_root / "module_driver").glob("*.ko")}
    if set(vendor_modules) != REPLACEMENT_NAMES | RETAIN_NAMES:
        raise PackError("base rootfs module inventory changed; refusing unexpected driver set")
    from verify_panel_module import verify as verify_panel
    panel_abi = verify_panel(vendor_modules["lcd_lg35583"], actual_built["lcd_lg35583"])
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="repack-driver-", dir=output.parent) as tmp_name:
        tmp = pathlib.Path(tmp_name); iso = tmp / "iso"; iso.mkdir()
        run(["7z", "x", "-y", str(base), f"-o{iso}"])
        ota = iso / "ota_v0"
        metadata = parse_update(ota / "ota_update.in")
        for kind, name in (("kernel", "xImage"), ("rootfs", "rootfs.squashfs")):
            entry = metadata[kind]
            if entry.get("img_name") != name: raise PackError(f"unexpected OTA {kind} image")
            unpack_image(ota, name, int(entry["img_size"]), entry["img_md5"], tmp / name)
        if sha(tmp / "xImage") != KERNEL_SHA256: raise PackError("base UPT kernel payload changed")
        validate_uimage(tmp / "xImage")
        root = tmp / "root"; root.mkdir()
        run(["unsquashfs", "-no-xattrs", "-d", str(root), str(tmp / "rootfs.squashfs")])
        driver_dir = root / "module_driver"
        before = file_manifest(root)
        for name in REPLACEMENT_NAMES:
            destination = driver_dir / f"{name}.ko"
            shutil.copyfile(actual_built[name], destination)
            shutil.copystat(actual_built[name], destination, follow_symlinks=True)
        expected = file_manifest(root)
        allowed = {f"module_driver/{name}.ko" for name in REPLACEMENT_NAMES}
        require_only_replacements(before, expected)

        mixed_dir = tmp / "mixed-modules"; mixed_dir.mkdir()
        for p in driver_dir.glob("*.ko"): shutil.copy2(p, mixed_dir / p.name)
        if len(list(mixed_dir.glob("*.ko"))) != 29: raise PackError("mixed module directory must contain 29 modules")
        empty_built = tmp / "empty-built"; empty_built.mkdir()
        abi = verify_module_set(workspace, mixed_dir, empty_built, tmp / "module-abi.json")

        new_rootfs = tmp / "new-rootfs.squashfs"
        run(["mksquashfs", str(root), str(new_rootfs), "-comp", "lzo", "-all-root", "-noappend",
             "-no-xattrs", "-processors", "2"])
        rebuilt = tmp / "rebuilt-root"; rebuilt.mkdir()
        run(["unsquashfs", "-no-xattrs", "-d", str(rebuilt), str(new_rootfs)])
        actual = file_manifest(rebuilt)
        changed_after = changed_paths(expected, actual)
        if changed_after: raise PackError(f"repacked rootfs content/modes differ: {sorted(changed_after)[:30]}")
        for name, digest in module_hashes.items():
            if sha(rebuilt / "module_driver" / f"{name}.ko") != digest:
                raise PackError(f"repacked replacement hash mismatch: {name}")

        for p in list(ota.iterdir()):
            if p.name.startswith("rootfs.squashfs.") or p.name.startswith("ota_md5_rootfs.squashfs."):
                p.unlink()
        root_size, root_md5 = package(new_rootfs, ota, "rootfs.squashfs")
        metadata["rootfs"] = {**metadata["rootfs"], "img_size": str(root_size), "img_md5": root_md5}
        lines = ["ota_version=0"]
        for kind in ("kernel", "rootfs"):
            item = metadata[kind]
            lines.extend((f"img_type={kind}", f"img_name={item['img_name']}",
                          f"img_size={item['img_size']}", f"img_md5={item['img_md5']}"))
        (ota / "ota_update.in").write_text("\n".join(lines) + "\n")
        staged = tmp / "candidate.upt"
        run(["genisoimage", "-f", "-U", "-J", "-joliet-long", "-r", "-allow-lowercase",
             "-allow-multidot", "-o", str(staged), str(iso)])
        if staged.stat().st_size > LIMIT: raise PackError("candidate UPT exceeds 45 MiB")
        check_iso = tmp / "check-iso"; check_iso.mkdir(); run(["7z", "x", "-y", str(staged), f"-o{check_iso}"])
        check_ota = check_iso / "ota_v0"; check_meta = parse_update(check_ota / "ota_update.in")
        unpack_image(check_ota, "xImage", int(check_meta["kernel"]["img_size"]), check_meta["kernel"]["img_md5"], tmp / "check-kernel")
        unpack_image(check_ota, "rootfs.squashfs", int(check_meta["rootfs"]["img_size"]), check_meta["rootfs"]["img_md5"], tmp / "check-rootfs.squashfs")
        if sha(tmp / "check-kernel") != KERNEL_SHA256: raise PackError("final package changed kernel payload")
        verify_root = tmp / "verify-root"; verify_root.mkdir()
        run(["unsquashfs", "-no-xattrs", "-d", str(verify_root), str(tmp / "check-rootfs.squashfs")])
        final_manifest = file_manifest(verify_root)
        final_diff = changed_paths(before, final_manifest)
        if final_diff != allowed: raise PackError(f"final rootfs differs outside selected modules: {sorted(final_diff ^ allowed)}")
        for name, digest in module_hashes.items():
            if sha(verify_root / "module_driver" / f"{name}.ko") != digest:
                raise PackError(f"final package module verification failed: {name}")
        os.replace(staged, output)
        sidecar = output.with_suffix(output.suffix + ".json")
        sidecar.write_text(json.dumps({
            "base_upt_sha256": BASE_SHA256, "kernel_sha256": KERNEL_SHA256,
            "driver_review_sha256": review_hash, "reviewed_source_sha256": source_hashes,
            "panel_binary_abi": panel_abi,
            "replacement_module_sha256": module_hashes,
            "retained_vendor_modules": sorted(RETAIN_NAMES), "mixed_module_abi_report": abi,
            "rootfs_before_sha256": sha(tmp / "rootfs.squashfs"),
            "rootfs_after_sha256": sha(tmp / "check-rootfs.squashfs"),
            "rootfs_unchanged_except_replacements": True,
            "output_upt_sha256": sha(output), "output_upt_size": output.stat().st_size,
        }, indent=2, sort_keys=True) + "\n")
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-upt", type=pathlib.Path, required=True)
    parser.add_argument("--kernel-workspace", type=pathlib.Path, required=True)
    parser.add_argument("--vendor-root", type=pathlib.Path, required=True,
                        help="extracted matching stock rootfs containing module_driver")
    parser.add_argument("--driver-review-manifest", type=pathlib.Path, required=True)
    parser.add_argument("--driver-modules", type=pathlib.Path,
                        help="directory with the reviewed replacement .ko files (defaults to kernel workspace out/modules-compas-r1)")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        result = create(args.base_upt, args.kernel_workspace, args.vendor_root,
                        args.driver_review_manifest, args.output, args.driver_modules)
    except (OSError, KeyError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"repack_driver_upt.py: {exc}\n")
    print(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
