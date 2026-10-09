#!/usr/bin/env python3
"""Build a host-only R1 UPT with an independently reviewed framebuffer module.

The output preserves the pinned production player/rootfs and replaces only the
kernel image plus the selected 29 module_driver/*.ko files. It never flashes a
device. The review manifest schema is described by ``reviewed_candidate``.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
from repack_kernel_upt import (  # noqa: E402
    LIMIT, PackError, package, parse_update, run, sha, unpack_image, validate_uimage,
)
from repack_driver_upt import changed_paths, file_manifest  # noqa: E402

BASE_UPT_SHA256 = "ba4816d812a79316b8b20d7c75f79b25804179eb2970ca134b20ae21c4f16806"
BASE_UPT_SIDECAR_SHA256 = "b04961b167385f6fce75f4828ff553d055444e4525a59749c14187869ebee480"
BASE_PACKAGE_SOURCE_SHA256 = "9c2460a7247607ca4eab57b72bd676adc87b675d36f90b0ad1d4d927d9a61e58"
BASE_DRIVER_REVIEW_SHA256 = "01268b2bea984f593ee4cc274da43dcc084d9ecda303b5c51400289723587a1e"
BASE_KERNEL_SHA256 = "02f646d746d11b3cc54e3f3f5bbbe578375ab1ca41076514c8306f930ef03490"
STOCK_KERNEL_SHA256 = "e3ed77e551afb061433b57c39ea10ebcfd862842436e51c6a7db66c737f4778a"
PINNED_KERNEL_COMMIT = "e1c5915290197ba01297e3f7c53f37197e0b1f9c"
DISPLAY_PREFIX = "firmware/kernel/display-experimental/"
from repack_driver_upt import REPLACEMENT_NAMES as REVIEWED_DRIVER_NAMES  # noqa: E402
import verify_panel_module  # noqa: E402

VENDOR_DISPLAY_MODULE_NAMES = {"axp2101", "cywdhd", "sau", "soc_i2c", "soc_msc", "soc_pwm"}
DISPLAY_MODULE_NAMES = REVIEWED_DRIVER_NAMES | VENDOR_DISPLAY_MODULE_NAMES | {"soc_fb"}
MODULE_COUNT = 29
REVIEW_MODELS = {"claude-opus-5-5", "gpt-6-astra"}
REVIEW_STATUS = "source_static_review_passed"


def regular(path: pathlib.Path, label: str) -> None:
    if path.is_symlink() or not path.is_file():
        raise PackError(f"{label} must be a regular non-symlink file: {path}")


def parse_json(path: pathlib.Path, label: str) -> dict[str, object]:
    regular(path, label)
    try:
        value = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise PackError(f"malformed {label}: {exc}") from exc
    if not isinstance(value, dict):
        raise PackError(f"{label} must be a JSON object")
    return value


def object_value(value: object, label: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise PackError(f"{label} must be a JSON object")
    return value


def sha_map(value: object, label: str) -> dict[str, str]:
    if not isinstance(value, dict) or not value:
        raise PackError(f"{label} must be a nonempty SHA-256 map")
    result: dict[str, str] = {}
    for name, digest in value.items():
        if not isinstance(name, str) or not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise PackError(f"invalid {label} entry: {name!r}")
        result[name] = digest
    return result


def module_name_map(value: object, label: str) -> dict[str, str]:
    result = sha_map(value, label)
    if any(not re.fullmatch(r"[A-Za-z0-9_]+", name) for name in result):
        raise PackError(f"unsafe module name in {label}")
    if len(result) != MODULE_COUNT:
        raise PackError(f"{label} must contain exactly {MODULE_COUNT} modules")
    return result


def checked_review(manifest_path: pathlib.Path, candidate: dict[str, object],
                   preparation: dict[str, object], abi_report: dict[str, object],
                   module_hashes: dict[str, str], repo: pathlib.Path) -> tuple[dict[str, str], str]:
    review = parse_json(manifest_path, "display review manifest")
    if review.get("status") != REVIEW_STATUS or review.get("model") not in REVIEW_MODELS:
        raise PackError("display review manifest must record an Opus/Astra source_static_review_passed result")
    report_path_value = review.get("report")
    report_sha = review.get("report_sha256")
    if not isinstance(report_path_value, str) or not isinstance(report_sha, str) or not re.fullmatch(r"[0-9a-f]{64}", report_sha):
        raise PackError("display review manifest lacks its pinned reviewer report")
    report_path = pathlib.Path(report_path_value)
    if not report_path.is_absolute():
        report_path = manifest_path.parent / report_path
    regular(report_path, "reviewer report")
    if sha(report_path) != report_sha:
        raise PackError("display reviewer report SHA-256 mismatch")

    source_hashes = sha_map(review.get("display_sources"), "reviewed display source hashes")
    if f"{DISPLAY_PREFIX}hiby.symvers" not in source_hashes:
        raise PackError("review manifest must pin the required display hiby.symvers")
    candidate_sources = candidate.get("display_sources")
    if not isinstance(candidate_sources, dict) or candidate_sources != source_hashes:
        raise PackError("reviewed display source hashes do not match staged candidate")
    for source, expected in source_hashes.items():
        pure = pathlib.PurePosixPath(source)
        if (pure.is_absolute() or ".." in pure.parts or not source.startswith(DISPLAY_PREFIX) or
                (pure.name in ("Makefile", "hiby.symvers") and
                 len(pure.parts) != len(pathlib.PurePosixPath(DISPLAY_PREFIX).parts) + 1) or
                (pure.name not in ("Makefile", "hiby.symvers") and pure.suffix not in (".c", ".h"))):
            raise PackError(f"unsafe reviewed display source path: {source}")
        source_path = repo.joinpath(*pure.parts)
        try:
            source_path.resolve().relative_to(repo.resolve())
        except ValueError as exc:
            raise PackError(f"reviewed source escapes checkout: {source}") from exc
        regular(source_path, "reviewed display source")
        if sha(source_path) != expected:
            raise PackError(f"reviewed display source changed: {source}")

    reviewed_modules = module_name_map(review.get("modules"), "reviewed module hashes")
    if reviewed_modules != module_hashes:
        raise PackError("reviewed module hashes do not match the complete staged module set")
    candidate_build = object_value(candidate.get("build"), "candidate build provenance")
    candidate_abi = object_value(candidate.get("abi_report"), "candidate ABI provenance")
    for key, expected in (("ximage_sha256", candidate_build.get("ximage_sha256")),
                          ("system_map_sha256", candidate_build.get("system_map_sha256")),
                          ("abi_report_sha256", candidate_abi.get("sha256")),
                          ("preparation_sha256", candidate_build.get("preparation_sha256"))):
        if review.get(key) != expected:
            raise PackError(f"review manifest {key} does not match the staged build")
    if review.get("base_upt_sha256") != BASE_UPT_SHA256:
        raise PackError("review manifest is not pinned to the approved production base UPT")
    return source_hashes, report_sha


def read_candidate(candidate_dir: pathlib.Path, review_path: pathlib.Path,
                   repo: pathlib.Path = REPO) -> tuple[dict[str, object], dict[str, object],
                                                         dict[str, object], dict[str, str],
                                                         pathlib.Path, pathlib.Path, str]:
    if candidate_dir.is_symlink() or not candidate_dir.is_dir():
        raise PackError("display candidate must be a regular directory")
    candidate_path = candidate_dir / "DISPLAY-CANDIDATE.json"
    candidate = parse_json(candidate_path, "display candidate manifest")
    candidate_build = object_value(candidate.get("build"), "candidate build provenance")
    candidate_abi = object_value(candidate.get("abi_report"), "candidate ABI provenance")
    if (candidate.get("status") != "host-only candidate; not validated on device; not a flash/package artifact" or
            candidate.get("display_stack") != "compas" or candidate.get("wifi_stack") != "vendor" or
            candidate.get("exclusive_soc_fb_controller") is not True):
        raise PackError("candidate is not an exclusive Compas display build with vendor Wi-Fi")
    if candidate_abi.get("selected_module_count") != MODULE_COUNT:
        raise PackError("candidate does not contain a verified 29-module ABI selection")

    prep_path_value = candidate_build.get("preparation_path")
    if not isinstance(prep_path_value, str):
        raise PackError("candidate build does not identify its preparation manifest")
    prep_path = pathlib.Path(prep_path_value)
    preparation = parse_json(prep_path, "fresh build preparation manifest")
    prep_hash = sha(prep_path)
    if candidate_build.get("preparation_sha256") != prep_hash:
        raise PackError("fresh build preparation manifest changed after staging")
    upstream = object_value(preparation.get("upstream"), "pinned kernel provenance")
    if (preparation.get("build_requested") is not True or preparation.get("profile") != "optimized" or
            preparation.get("display_stack") != "compas" or preparation.get("wifi_stack") != "vendor" or
            preparation.get("upstream_head") != PINNED_KERNEL_COMMIT or
            preparation.get("upstream_head") != upstream.get("commit")):
        raise PackError("display candidate must use a fresh pinned optimized Compas build and vendor Wi-Fi")
    stock = preparation.get("stock_kernel")
    if not isinstance(stock, dict) or stock.get("sha256") != STOCK_KERNEL_SHA256 or stock.get("size") != 3731520:
        raise PackError("display build is not based on the pinned R1 stock kernel")
    validation = preparation.get("build_validation")
    if not isinstance(validation, dict) or validation.get("display_stack") != "compas":
        raise PackError("fresh build manifest lacks Compas display validation")
    if not isinstance(validation.get("system_map_sha256"), str):
        map_value = validation.get("system_map_path")
        if isinstance(map_value, str):
            system_map_file = pathlib.Path(map_value)
            regular(system_map_file, "fresh optimized System.map")
            validation["system_map_sha256"] = sha(system_map_file)
    for source_key, candidate_key in (("ximage_sha256", "ximage_sha256"),
                                      ("system_map_sha256", "system_map_sha256")):
        if validation.get(source_key) != candidate_build.get(candidate_key):
            raise PackError(f"candidate {candidate_key} disagrees with fresh build validation")
    ximage_value = validation.get("ximage_path")
    if not isinstance(ximage_value, str):
        raise PackError("fresh build does not record its xImage path")
    ximage = pathlib.Path(ximage_value)
    regular(ximage, "fresh Compas kernel image")
    if sha(ximage) != validation.get("ximage_sha256"):
        raise PackError("fresh Compas kernel image hash changed")
    validate_uimage(ximage)

    abi_path_value = candidate_abi.get("path")
    abi_hash = candidate_abi.get("sha256")
    if not isinstance(abi_path_value, str) or not isinstance(abi_hash, str):
        raise PackError("candidate lacks its ABI report path/hash")
    abi_path = pathlib.Path(abi_path_value)
    abi_report = parse_json(abi_path, "fresh mixed-module ABI report")
    if sha(abi_path) != abi_hash:
        raise PackError("fresh mixed-module ABI report changed after staging")
    if (abi_report.get("valid") is not True or abi_report.get("selected_module_count") != MODULE_COUNT or
            not isinstance(abi_report.get("selected_modules"), dict) or
            len(abi_report["selected_modules"]) != MODULE_COUNT or abi_report.get("missing_imports_by_module") or
            abi_report.get("vermagic_errors")):
        raise PackError("fresh mixed-module ABI report is incomplete or invalid")
    if set(abi_report["selected_modules"]) != DISPLAY_MODULE_NAMES:
        raise PackError("fresh ABI selection is not the exact 22 reviewed drivers, six vendor modules, and soc_fb")
    if (pathlib.Path(str(abi_report.get("system_map", ""))).resolve() != pathlib.Path(
            str(validation.get("system_map_path", ""))).resolve() or
            abi_report.get("system_map_sha256") != validation.get("system_map_sha256")):
        raise PackError("fresh ABI report is not pinned to this optimized build System.map")
    system_map_path = pathlib.Path(str(validation.get("system_map_path", "")))
    regular(system_map_path, "fresh optimized System.map")
    if sha(system_map_path) != validation.get("system_map_sha256"):
        raise PackError("fresh optimized System.map hash changed")
    config_path_value = validation.get("config_path")
    config_hash = validation.get("compiled_config_sha256")
    if not isinstance(config_path_value, str) or not isinstance(config_hash, str):
        raise PackError("fresh optimized build lacks compiled config provenance")
    config_path = pathlib.Path(config_path_value)
    regular(config_path, "fresh optimized kernel config")
    if sha(config_path) != config_hash:
        raise PackError("fresh optimized kernel config hash changed")

    candidate_module_records = object_value(candidate.get("modules"), "candidate module records")
    if set(candidate_module_records) != DISPLAY_MODULE_NAMES:
        raise PackError("candidate does not identify the exact production 29-module inventory")
    module_hashes = module_name_map(
        {name: object_value(record, f"candidate module {name}").get("sha256")
         for name, record in candidate_module_records.items()}, "staged module hashes")
    selected = abi_report["selected_modules"]
    if set(selected) != set(module_hashes):
        raise PackError("ABI report and candidate select different module names")
    if {name for name, record in selected.items() if isinstance(record, dict) and
        record.get("source") == "built"} != REVIEWED_DRIVER_NAMES | {"soc_fb"}:
        raise PackError("ABI selection must use all 22 reviewed fresh modules and fresh soc_fb")
    soc_fb_build = pathlib.Path(str(validation.get("soc_fb_module_path", "")))
    if not soc_fb_build.is_file() or soc_fb_build.is_symlink():
        raise PackError("fresh build provenance lacks its soc_fb module")
    for name in REVIEWED_DRIVER_NAMES | {"soc_fb"}:
        fresh_module = soc_fb_build.parent / f"{name}.ko"
        regular(fresh_module, f"fresh compiled module {name}.ko")
        if sha(fresh_module) != module_hashes[name]:
            raise PackError(f"candidate {name}.ko does not match the fresh compiled module")
    if verify_panel_module.has_cleanup_module(soc_fb_build.parent / "lcd_lg35583.ko"):
        raise PackError("fresh lcd_lg35583.ko defines cleanup_module and cannot be unloaded safely")
    for name, expected in module_hashes.items():
        record = selected[name]
        if not isinstance(record, dict) or record.get("sha256") != expected:
            raise PackError(f"ABI report module hash differs from staged candidate: {name}")
    soc_fb_record = object_value(selected.get("soc_fb"), "ABI soc_fb module record")
    if soc_fb_record.get("source") != "built":
        raise PackError("fresh ABI selection must use the source-built soc_fb.ko")
    if [name for name in selected if "fb" in name.lower()] != ["soc_fb"]:
        raise PackError("fresh ABI report contains another framebuffer controller")

    source_hashes, review_hash = checked_review(review_path, candidate, preparation,
                                                abi_report, module_hashes, repo)
    module_dir = candidate_dir / "rootfs/module_driver"
    if module_dir.is_symlink() or not module_dir.is_dir():
        raise PackError("candidate rootfs/module_driver is missing or unsafe")
    actual_modules: dict[str, pathlib.Path] = {}
    for path in module_dir.iterdir():
        if path.suffix != ".ko":
            continue
        regular(path, "staged candidate module")
        actual_modules[path.stem] = path
    if set(actual_modules) != set(module_hashes):
        raise PackError("candidate payload does not contain exactly its reviewed 29 modules")
    for name, expected in module_hashes.items():
        if sha(actual_modules[name]) != expected:
            raise PackError(f"candidate module bytes changed: {name}")

    return candidate, preparation, abi_report, module_hashes, ximage, module_dir, review_hash


def check_loader(root: pathlib.Path, candidate_dir: pathlib.Path) -> dict[str, str]:
    module_dir = root / "module_driver"
    loader = module_dir / "soc_fb.sh"
    startup = module_dir / "driver_default_init_script.sh"
    regular(loader, "base soc_fb loader")
    regular(startup, "base driver startup script")
    loader_hash, startup_hash = sha(loader), sha(startup)
    candidate = parse_json(candidate_dir / "DISPLAY-CANDIDATE.json", "display candidate manifest")
    loader_info = candidate.get("loader")
    if not isinstance(loader_info, dict) or loader_hash != loader_info.get("sha256"):
        raise PackError("production-base soc_fb loader differs from reviewed candidate loader")
    startup_text = startup.read_text()
    commands = re.findall(r"(?m)^\s*sh\s+(\S+)\s*$", startup_text)
    framebuffer_scripts = [name for name in commands
                           if "fb" in pathlib.PurePosixPath(name).stem.lower()]
    loads = []
    for line in loader.read_text().splitlines():
        try:
            fields = shlex.split(line, comments=True)
        except ValueError as exc:
            raise PackError(f"malformed base framebuffer loader: {exc}") from exc
        if fields and fields[0] == "insmod":
            loads.append(fields)
    if framebuffer_scripts != ["soc_fb.sh"] or len(loads) != 1 or len(loads[0]) < 2 or loads[0][1] != "soc_fb.ko":
        raise PackError("production base must have one startup invocation of one soc_fb.ko loader")
    if loader_info.get("startup_invocations") != 1 or loader_info.get("module") != "soc_fb.ko":
        raise PackError("candidate loader metadata is not single-controller")
    return {"soc_fb_loader_sha256": loader_hash,
            "driver_init_sha256": startup_hash,
            "startup_invocations": 1,
            "loaded_module": "soc_fb.ko"}


def unpack_base(base_upt: pathlib.Path, expected_base_sha: str,
                work: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path, dict[str, dict[str, str]], str]:
    regular(base_upt, "base UPT")
    if sha(base_upt) != expected_base_sha:
        raise PackError("base UPT SHA-256 does not match approved current R1 v2 package")
    sidecar_path = base_upt.with_suffix(base_upt.suffix + ".json")
    sidecar = parse_json(sidecar_path, "base UPT sidecar")
    if (sha(sidecar_path) != BASE_UPT_SIDECAR_SHA256 or
            sidecar.get("output_upt_sha256") != BASE_UPT_SHA256 or
            sidecar.get("base_upt_sha256") != BASE_PACKAGE_SOURCE_SHA256 or
            sidecar.get("kernel_sha256") != BASE_KERNEL_SHA256 or
            sidecar.get("output_upt_size") != base_upt.stat().st_size or
            sidecar.get("rootfs_unchanged_except_replacements") is not True):
        raise PackError("base sidecar does not identify the pinned current R1 production image")
    base_manifest = sidecar.get("replacement_module_sha256")
    if (not isinstance(base_manifest, dict) or set(base_manifest) != REVIEWED_DRIVER_NAMES or
            any(not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value)
                for value in base_manifest.values())):
        raise PackError("base sidecar lacks the reviewed current 22-module replacement inventory")
    if sidecar.get("driver_review_sha256") != BASE_DRIVER_REVIEW_SHA256:
        raise PackError("base sidecar does not match the current approved driver review")
    run(["7z", "x", "-y", str(base_upt), f"-o{work / 'iso'}"])
    ota = work / "iso/ota_v0"
    if not ota.is_dir():
        raise PackError("base UPT has no ota_v0 directory")
    metadata = parse_update(ota / "ota_update.in")
    for kind, name in (("kernel", "xImage"), ("rootfs", "rootfs.squashfs")):
        item = metadata[kind]
        if item.get("img_name") != name:
            raise PackError(f"unexpected base UPT {kind} image name")
        try:
            size = int(item["img_size"])
            md5 = item["img_md5"].lower()
        except (KeyError, ValueError, AttributeError) as exc:
            raise PackError(f"invalid {kind} image metadata") from exc
        if size <= 0 or not re.fullmatch(r"[0-9a-f]{32}", md5):
            raise PackError(f"invalid base {kind} image metadata")
        unpack_image(ota, name, size, md5, work / name)
    if sha(work / "xImage") != BASE_KERNEL_SHA256:
        raise PackError("reconstructed base kernel does not match current UPT sidecar")
    validate_uimage(work / "xImage")
    if sidecar.get("rootfs_after_sha256") != sha(work / "rootfs.squashfs"):
        raise PackError("reconstructed base rootfs does not match current UPT sidecar")
    root = work / "rootfs"
    root.mkdir()
    run(["unsquashfs", "-no-xattrs", "-d", str(root), str(work / "rootfs.squashfs")])
    module_dir = root / "module_driver"
    base_modules = {path.stem: path for path in module_dir.glob("*.ko")}
    if set(base_modules) != DISPLAY_MODULE_NAMES:
        raise PackError("pinned base rootfs does not contain the exact production 29-module inventory")
    for name, expected in base_manifest.items():
        if sha(base_modules[name]) != expected:
            raise PackError(f"pinned base replacement module hash differs from sidecar: {name}")
    return root, ota, metadata, sha(work / "rootfs.squashfs")


def replace_modules(root: pathlib.Path, candidate_dir: pathlib.Path,
                    candidate_module_dir: pathlib.Path, module_hashes: dict[str, str]) -> dict[str, object]:
    driver_dir = root / "module_driver"
    if driver_dir.is_symlink() or not driver_dir.is_dir():
        raise PackError("pinned production rootfs has no safe module_driver directory")
    base_loader = check_loader(root, candidate_dir)
    base_modules = {path.stem: path for path in driver_dir.glob("*.ko")}
    if set(base_modules) != DISPLAY_MODULE_NAMES:
        raise PackError("production base module set differs from the exact 29-module inventory")
    for name in VENDOR_DISPLAY_MODULE_NAMES:
        if sha(base_modules[name]) != module_hashes.get(name):
            raise PackError(f"candidate did not retain pinned vendor module bytes: {name}")
    before = file_manifest(root)
    for path in driver_dir.glob("*.ko"):
        if path.is_symlink() or not path.is_file():
            raise PackError(f"unsafe base module entry: {path.name}")
        path.unlink()
    for name, expected in module_hashes.items():
        source = candidate_module_dir / f"{name}.ko"
        destination = driver_dir / f"{name}.ko"
        shutil.copyfile(source, destination)
        shutil.copystat(source, destination, follow_symlinks=True)
        if sha(destination) != expected:
            raise PackError(f"copied candidate module hash mismatch: {name}")
    staged_modules = {path.stem for path in driver_dir.glob("*.ko")}
    if staged_modules != set(module_hashes) or len(staged_modules) != MODULE_COUNT:
        raise PackError("rootfs does not contain exactly the reviewed 29-module inventory")
    after = file_manifest(root)
    changed = changed_paths(before, after)
    allowed = {path for path in changed
               if re.fullmatch(r"module_driver/[A-Za-z0-9_]+\.ko", path)}
    if changed != allowed:
        raise PackError(f"rootfs changed outside module_driver .ko files: {sorted(changed - allowed)}")
    return {"changed_paths": sorted(changed), "loader": base_loader,
            "rootfs_before_manifest_sha256": hashlib.sha256(json.dumps(before, sort_keys=True,
                separators=(",", ":")).encode()).hexdigest(),
            "rootfs_after_manifest_sha256": hashlib.sha256(json.dumps(after, sort_keys=True,
                separators=(",", ":")).encode()).hexdigest()}


def create(base_upt: pathlib.Path, candidate_dir: pathlib.Path,
           review_manifest: pathlib.Path, output: pathlib.Path,
           repo: pathlib.Path = REPO) -> pathlib.Path:
    if output.is_symlink() or output.exists() or output.with_suffix(output.suffix + ".json").exists():
        raise PackError("output UPT and sidecar must be fresh")
    for tool in ("7z", "unsquashfs", "mksquashfs", "genisoimage"):
        if not shutil.which(tool):
            raise PackError(f"required tool unavailable: {tool}")
    expected_base = BASE_UPT_SHA256
    base_upt = base_upt.resolve(strict=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    candidate, preparation, abi, module_hashes, ximage, candidate_modules, review_hash = read_candidate(
        candidate_dir.resolve(strict=True), review_manifest.resolve(strict=True), repo.resolve())
    with tempfile.TemporaryDirectory(prefix="repack-display-", dir=output.parent) as tmp_name:
        work = pathlib.Path(tmp_name)
        (work / "iso").mkdir()
        root, ota, metadata, base_rootfs_hash = unpack_base(base_upt, expected_base, work)
        module_delta = replace_modules(root, candidate_dir.resolve(strict=True), candidate_modules,
                                       module_hashes)
        player_paths = ("usr/bin/compas_player", "usr/bin/compas_bootloader")
        base_player_hashes = {}
        original_manifest = file_manifest(root)
        for relative in player_paths:
            record = original_manifest.get(relative)
            if not isinstance(record, dict) or record.get("type") != "file":
                raise PackError(f"pinned base image is missing production executable: /{relative}")
            base_player_hashes[relative] = record["sha256"]

        for path in list(ota.iterdir()):
            if path.name.startswith("rootfs.squashfs.") or path.name.startswith("ota_md5_rootfs.squashfs."):
                path.unlink()
            elif path.name.startswith("xImage.") or path.name.startswith("ota_md5_xImage."):
                path.unlink()
        kernel_size, kernel_md5 = package(ximage, ota, "xImage")
        new_rootfs = work / "new-rootfs.squashfs"
        run(["mksquashfs", str(root), str(new_rootfs), "-comp", "lzo", "-all-root",
             "-noappend", "-no-xattrs", "-processors", "2"])
        rootfs_size, rootfs_md5 = package(new_rootfs, ota, "rootfs.squashfs")
        metadata["kernel"] = {**metadata["kernel"], "img_size": str(kernel_size), "img_md5": kernel_md5}
        metadata["rootfs"] = {**metadata["rootfs"], "img_size": str(rootfs_size), "img_md5": rootfs_md5}
        lines = ["ota_version=0"]
        for kind in ("kernel", "rootfs"):
            item = metadata[kind]
            lines.extend((f"img_type={kind}", f"img_name={item['img_name']}",
                          f"img_size={item['img_size']}", f"img_md5={item['img_md5']}"))
        (ota / "ota_update.in").write_text("\n".join(lines) + "\n")
        staged = work / "candidate.upt"
        run(["genisoimage", "-f", "-U", "-J", "-joliet-long", "-r", "-allow-lowercase",
             "-allow-multidot", "-o", str(staged), str(work / "iso")])
        if staged.stat().st_size > LIMIT:
            raise PackError("candidate UPT exceeds 45 MiB")

        verify_iso = work / "verify-iso"
        verify_iso.mkdir()
        run(["7z", "x", "-y", str(staged), f"-o{verify_iso}"])
        final_ota = verify_iso / "ota_v0"
        final_meta = parse_update(final_ota / "ota_update.in")
        for kind, name in (("kernel", "xImage"), ("rootfs", "rootfs.squashfs")):
            item = final_meta[kind]
            unpack_image(final_ota, name, int(item["img_size"]), item["img_md5"], work / f"final-{name}")
        if sha(work / "final-xImage") != candidate["build"]["ximage_sha256"]:
            raise PackError("packaged kernel does not match the reviewed optimized build")
        validate_uimage(work / "final-xImage")
        final_root = work / "final-root"
        final_root.mkdir()
        run(["unsquashfs", "-no-xattrs", "-d", str(final_root), str(work / "final-rootfs.squashfs")])
        final_manifest = file_manifest(final_root)
        base_manifest = file_manifest(root)
        if changed_paths(base_manifest, final_manifest):
            raise PackError("repacked rootfs content or modes changed during squashfs packaging")
        final_module_dir = final_root / "module_driver"
        if {path.stem for path in final_module_dir.glob("*.ko")} != set(module_hashes):
            raise PackError("final package does not contain exactly the 29 selected modules")
        for name, expected in module_hashes.items():
            if sha(final_module_dir / f"{name}.ko") != expected:
                raise PackError(f"final package module hash mismatch: {name}")
        final_abi = abi["selected_modules"]
        if len(final_abi) != MODULE_COUNT or final_abi["soc_fb"].get("source") != "built":
            raise PackError("final package is missing the fresh 29-module ABI selection")
        for relative, expected in base_player_hashes.items():
            if sha(final_root / relative) != expected:
                raise PackError(f"production player changed during packaging: /{relative}")
        final_loader = check_loader(final_root, candidate_dir.resolve(strict=True))
        if final_loader != module_delta["loader"]:
            raise PackError("display module loader/startup changed during package assembly")
        # Persist only after complete round-trip verification.
        os.replace(staged, output)
        sidecar = output.with_suffix(output.suffix + ".json")
        sidecar.write_text(json.dumps({
            "schema_version": 1,
            "candidate_type": "host-only R1 display UPT; not flashed or device validated",
            "base_upt_sha256": expected_base,
            "base_kernel_sha256": BASE_KERNEL_SHA256,
            "base_rootfs_sha256": base_rootfs_hash,
            "candidate_kernel_sha256": candidate["build"]["ximage_sha256"],
            "candidate_preparation_sha256": candidate["build"]["preparation_sha256"],
            "candidate_system_map_sha256": candidate["build"]["system_map_sha256"],
            "candidate_abi_report_sha256": candidate["abi_report"]["sha256"],
            "candidate_display_source_sha256": candidate["display_sources"],
            "candidate_module_sha256": module_hashes,
            "review_model": parse_json(review_manifest, "display review manifest").get("model"),
            "review_report_sha256": review_hash,
            "production_executables_sha256": base_player_hashes,
            "rootfs_changed_only_selected_module_files": True,
            "rootfs_change_manifest": module_delta,
            "loader": final_loader,
            "module_count": MODULE_COUNT,
            "wifi_stack": "vendor",
            "output_upt_sha256": sha(output),
            "output_upt_size": output.stat().st_size,
        }, indent=2, sort_keys=True) + "\n")
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-upt", type=pathlib.Path, required=True,
                        help="pinned current R1 custom-drivers v2 UPT")
    parser.add_argument("--candidate", type=pathlib.Path, required=True,
                        help="host-only output directory from stage_experimental_display.py")
    parser.add_argument("--review-manifest", type=pathlib.Path, required=True,
                        help="Opus/Astra report and exact source/kernel/module/ABI hash pins")
    parser.add_argument("--output", type=pathlib.Path, required=True,
                        help="fresh output path for an unflashed candidate UPT")
    args = parser.parse_args()
    try:
        result = create(args.base_upt, args.candidate, args.review_manifest, args.output)
    except (OSError, KeyError, TypeError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"repack_display_upt.py: {exc}\n")
    print(f"Created host-only display UPT candidate: {result}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
