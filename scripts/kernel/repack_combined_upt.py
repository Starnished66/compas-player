#!/usr/bin/env python3
"""Pack the reviewed radio runtime with a fresh combined radio/display kernel.

This is a separate gate from repack_radio_upt: it accepts the frozen radio V3
runtime, but requires one fresh optimized brcmfmac+Compas-display build and a
new 31-module ABI report with source-built soc_fb. Output is host-only.
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
import struct
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import repack_radio_upt as radio  # noqa: E402
import verify_module_abi  # noqa: E402
import verify_panel_module  # noqa: E402
from repack_kernel_upt import LIMIT, PackError, package, parse_update, run, sha, unpack_image, validate_uimage  # noqa: E402
from repack_driver_upt import changed_paths, file_manifest  # noqa: E402

RADIO_PROVIDER_SHA = "4eeff488908afef806855f556cd2ba6c98c96895537108975e15b615686b7446"
RADIO_LIFETIME_PATCH_SHA = "4a39a5edb5e493bbb091f33f7dd25b19f0654a3c4c67f1f1d2cf397bb55b610d"
RADIO_MODULE_PATCHES = {
    "compas-radio-input-safety.patch": "8cfc06da68474f18913929eeaa648b00ce66d1cb81d29ce978094167fe074da3",
    "compas-radio-lifecycle.patch": "6d8ab2364bac070bc3a25f7448b35fd61b4cb70258c310904be7df3db38d9c86",
}
GENERAL_DRIVER_PATCH_SHA = "c4566a2061be9b87ab6caded1963cc46220894c5dd1d05ac40c400bfbbe7f8da"
DISPLAY_REVIEW_SHA = "f75988f0751daac56800d55c086e8b72bd5f759726b8aa6f618df035dcf5bad6"
DISPLAY_REVIEW_PATH = pathlib.Path("../compas-display-investigation-20261009/astra-display-adoption-final-review.json")
FAST_DISPLAY_REVIEW_SHA = "2a28f3f75b360299ffda4995c8e09eccb7e7ebe1221d3f6f44bf6c6272cbd598"
FAST_DISPLAY_REVIEW_PATH = pathlib.Path("../compas-display-investigation-20261009/astra-fast-adoption-final-review.json")
BOOT_LOGGING_REVIEW_SHA = "29f62e55f72eb3650b7c6fded8f804fef3f749dc73a737e354eb48dcb6115e92"
BOOT_LOGGING_MANIFEST_SHA = "55f2ec2adb6e34ec0f256cb4ed0e5ed65f54a8c0fb8e0c3954df48b75998b333"
BOOT_LOGGING_DIR = pathlib.Path("../compas-display-investigation-20261009/boot-logging-firmware")
BOOT_LOGGING_REVIEW_PATH = pathlib.Path("../compas-display-investigation-20261009/astra-boot-logging-final-review.json")
BOOT_LOGGING_SOURCE_PATHS = {
    "src/core/boot_trace.c", "src/core/boot_trace.h", "src/core/boot_trace_test.c",
    "src/main.c", "src/ui/gui.c", "src/bootloader/main.c", "Makefile",
}
BOOT_LOGGING_BINARY_HASHES = {
    "compas_player": "e9bc990425dca637114d38820024485adf46063185eef549d4669b55ae30afe5",
    "compas_bootloader": "10e152f25e98cdc747dd9742ed5148ebdf375f64a86cd75337a3274c274471da",
}
DISPLAY_MODULES = radio.BUILT_NORMAL | {"soc_fb"}
COMBINED_BUILT = radio.BUILT_DRIVER | radio.PRIVATE_MODULES | {"soc_fb"}
COMBINED_RETAINED = radio.RETAINED - {"soc_fb"}
COMBINED_STEMS = COMBINED_BUILT | COMBINED_RETAINED
MODULE_DRIVER_STEMS = radio.BUILT_DRIVER | {"soc_fb"} | COMBINED_RETAINED
REMOVED = radio.REMOVED_LEGACY
EXPECTED_UPSTREAM = "e1c5915290197ba01297e3f7c53f37197e0b1f9c"
EXPECTED_SDK_SHA256 = "3e8c101b7c12667dcfed888e2eda63cfa833c6e20f22569e2b646c40f8523641"
EXPECTED_SDK_SIZE = 1060590289


def regular(path: pathlib.Path, label: str) -> None:
    if path.is_symlink() or not path.is_file():
        raise PackError(f"{label} must be a regular non-symlink file: {path}")


def read_json(path: pathlib.Path, label: str) -> dict:
    regular(path, label)
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise PackError(f"invalid {label}: {exc}") from exc
    if not isinstance(data, dict):
        raise PackError(f"{label} must be a JSON object")
    return data


def json_sha(value: object) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def file_mode(path: pathlib.Path) -> int:
    return stat.S_IMODE(path.lstat().st_mode)


def validate_module_origins(selected: object) -> dict[str, dict]:
    if not isinstance(selected, dict) or set(selected) != COMBINED_STEMS:
        raise PackError("combined ABI selection is not the exact 26 built + five retained modules")
    for name, record in selected.items():
        expected = "built" if name in COMBINED_BUILT else "vendor"
        if not isinstance(record, dict) or record.get("source") != expected:
            raise PackError(f"combined ABI source class mismatch: {name}")
    return selected


def review_source_path(raw: object, repo: pathlib.Path) -> tuple[str, pathlib.Path]:
    """Normalize a reviewed source path and prove it stays in the checkout."""
    if not isinstance(raw, str) or not raw:
        raise PackError("reviewed display source path must be a nonempty string")
    raw_path = pathlib.Path(raw)
    if not raw_path.is_absolute():
        pure = pathlib.PurePosixPath(raw)
        if ".." in pure.parts:
            raise PackError(f"unsafe reviewed display source path: {raw}")
    source_path = raw_path if raw_path.is_absolute() else repo / raw_path
    if source_path.is_symlink() or not source_path.is_file():
        raise PackError(f"reviewed display source must be a regular non-symlink file: {raw}")
    try:
        resolved = source_path.resolve(strict=True)
    except OSError as exc:
        raise PackError(f"reviewed display source is missing: {raw}") from exc
    try:
        relative = resolved.relative_to(repo.resolve()).as_posix()
    except ValueError as exc:
        raise PackError(f"reviewed display source is outside this checkout: {raw}") from exc
    if not relative.startswith("firmware/kernel/display-experimental/"):
        raise PackError(f"reviewed display source is outside the display source tree: {raw}")
    regular(resolved, f"reviewed display source {relative}")
    return relative, resolved


def validate_display_review(prep: dict, validation: dict, workspace: pathlib.Path,
                            repo: pathlib.Path,
                            selected_review: pathlib.Path | None = None) -> tuple[dict[str, str], pathlib.Path]:
    selected_review = selected_review or DISPLAY_REVIEW_PATH
    report_path = (repo / selected_review).resolve(strict=True)
    legacy_report = (repo / DISPLAY_REVIEW_PATH).resolve(strict=True)
    fast_report = (repo / FAST_DISPLAY_REVIEW_PATH).resolve(strict=True)
    if report_path not in (legacy_report, fast_report):
        raise PackError("display review must use one of the two explicitly pinned Astra reports")
    regular(report_path, "final Astra display review")
    fast_profile = report_path == fast_report
    expected_review_sha = FAST_DISPLAY_REVIEW_SHA if fast_profile else DISPLAY_REVIEW_SHA
    if sha(report_path) != expected_review_sha:
        raise PackError("selected Astra display review report hash changed")
    review = read_json(report_path, "final Astra display review")
    expected_status = ("source_review_passed_controlled_experimental_host_scope"
                       if fast_profile else "source_static_review_passed")
    if review.get("status") != expected_status or review.get("model") != "gpt-6-astra":
        raise PackError("Compas display source requires the final Astra PASS review")
    source_map = review.get("source_sha256")
    if (not isinstance(source_map, dict) or len(source_map) != 12 or
            (not fast_profile and review.get("source_file_count") != 12) or
            (fast_profile and review.get("source_file_count") not in (None, 12))):
        raise PackError("Astra review must pin all 12 Compas display source files")
    reviewed: dict[str, str] = {}
    for raw, digest in source_map.items():
        try:
            relative, path = review_source_path(raw, repo)
        except OSError as exc:
            raise PackError(f"reviewed display source is missing: {raw}") from exc
        if relative in reviewed:
            raise PackError(f"duplicate normalized display source path: {relative}")
        if not isinstance(digest, str) or sha(path) != digest:
            raise PackError(f"reviewed display source changed: {relative}")
        reviewed[relative] = digest

    records = prep.get("display_sources")
    build_hashes = validation.get("display_source_sha256")
    if not isinstance(records, list) or not isinstance(build_hashes, dict):
        raise PackError("combined build lacks complete copied display source provenance")
    observed: dict[str, str] = {}
    for record in records:
        if not isinstance(record, dict):
            raise PackError("malformed preparation display source record")
        raw_rel = record.get("original_path")
        try:
            rel, _ = review_source_path(raw_rel, repo)
        except OSError as exc:
            raise PackError(f"combined build display source is missing: {raw_rel}") from exc
        digest = record.get("sha256")
        copied = record.get("copied_to")
        if rel not in reviewed or digest != reviewed.get(rel) or record.get("prepared_sha256") != digest:
            raise PackError(f"combined build display source differs from Astra-reviewed source: {rel}")
        normalized_build_hashes = {}
        for build_raw, build_digest in build_hashes.items():
            try:
                build_rel, _ = review_source_path(build_raw, repo)
            except OSError as exc:
                raise PackError(f"combined build display source is missing: {build_raw}") from exc
            if build_rel in normalized_build_hashes:
                raise PackError(f"duplicate normalized combined display source path: {build_rel}")
            normalized_build_hashes[build_rel] = build_digest
        if normalized_build_hashes.get(rel) != digest or not isinstance(copied, str):
            raise PackError(f"combined build validation does not pin display source: {rel}")
        pure = pathlib.PurePosixPath(copied)
        if pure.is_absolute() or ".." in pure.parts:
            raise PackError(f"unsafe prepared display source path: {copied}")
        prepared = workspace.joinpath(*pure.parts)
        regular(prepared, f"prepared display source {copied}")
        if sha(prepared) != digest:
            raise PackError(f"prepared display source changed: {copied}")
        observed[rel] = digest
    if observed != reviewed or normalized_build_hashes != reviewed:
        raise PackError("combined build and Astra review do not cover the same exact 12 display sources")
    return reviewed, report_path


def validate_mips_o32(path: pathlib.Path, label: str) -> None:
    regular(path, label)
    with path.open("rb") as stream:
        header = stream.read(40)
    if (len(header) < 40 or header[:4] != b"\x7fELF" or header[4] != 1 or
            header[5] not in (1, 2) or header[6] != 1):
        raise PackError(f"{label} must be a valid ELF32 executable")
    endian = ">" if header[5] == 2 else "<"
    elf_type, machine, version = struct.unpack(endian + "HHI", header[16:24])
    flags = struct.unpack(endian + "I", header[36:40])[0]
    abi = flags & 0xF000
    if machine != 8 or version != 1 or elf_type not in (2, 3) or flags & 0x20 or abi != 0x1000:
        raise PackError(f"{label} must be an ELF32 MIPS o32 executable/shared object")


def validate_boot_logging_binaries(binaries_dir: pathlib.Path,
                                   repo: pathlib.Path = REPO) -> dict[str, object]:
    expected_dir = (repo / BOOT_LOGGING_DIR).resolve(strict=True)
    if binaries_dir.resolve(strict=True) != expected_dir:
        raise PackError("boot logging binaries must come from the frozen reviewed artifact directory")
    manifest_path = expected_dir / "build-manifest.json"
    regular(manifest_path, "frozen boot logging build manifest")
    if sha(manifest_path) != BOOT_LOGGING_MANIFEST_SHA:
        raise PackError("frozen boot logging build manifest SHA-256 mismatch")
    manifest = read_json(manifest_path, "frozen boot logging build manifest")
    source_hashes = manifest.get("sources_sha256")
    binary_hashes = manifest.get("binaries_sha256")
    if (manifest.get("build_passed") is not True or manifest.get("device_installed") is not False or
            manifest.get("build") != "make -j4 target bootloader" or
            not isinstance(source_hashes, dict) or set(source_hashes) != BOOT_LOGGING_SOURCE_PATHS or
            not isinstance(binary_hashes, dict) or binary_hashes != BOOT_LOGGING_BINARY_HASHES):
        raise PackError("boot logging manifest does not pin the exact reviewed seven sources and two binaries")
    for relative, digest in source_hashes.items():
        pure = pathlib.PurePosixPath(relative)
        if pure.is_absolute() or ".." in pure.parts:
            raise PackError(f"unsafe boot logging source path: {relative}")
        source = repo.joinpath(*pure.parts)
        regular(source, f"boot logging source {relative}")
        if not isinstance(digest, str) or sha(source) != digest:
            raise PackError(f"boot logging source changed: {relative}")

    review_path = (repo / BOOT_LOGGING_REVIEW_PATH).resolve(strict=True)
    regular(review_path, "final Astra boot logging review")
    if sha(review_path) != BOOT_LOGGING_REVIEW_SHA:
        raise PackError("final Astra boot logging review hash changed")
    review = read_json(review_path, "final Astra boot logging review")
    if review.get("status") != "source_review_passed" or review.get("model") != "gpt-6-astra":
        raise PackError("boot logging binaries require the pinned final Astra source review")
    if review.get("source_sha256") != source_hashes:
        raise PackError("boot logging review and frozen build manifest source maps differ")

    paths = {"compas_player": expected_dir / "bin/compas_player",
             "compas_bootloader": expected_dir / "bin/compas_bootloader"}
    for name, path in paths.items():
        regular(path, f"frozen boot logging {name}")
        validate_mips_o32(path, f"frozen boot logging {name}")
        if sha(path) != binary_hashes[name]:
            raise PackError(f"frozen boot logging binary hash mismatch: {name}")
    return {"directory": expected_dir, "manifest_sha256": BOOT_LOGGING_MANIFEST_SHA,
            "review_sha256": BOOT_LOGGING_REVIEW_SHA, "source_sha256": source_hashes,
            "binary_sha256": binary_hashes, "player": paths["compas_player"],
            "bootloader": paths["compas_bootloader"]}


def require_exact_boot_logging_delta(before: dict, after: dict) -> None:
    expected = {"usr/bin/compas_player", "usr/bin/compas_bootloader"}
    changed = changed_paths(before, after)
    if changed != expected:
        raise PackError(f"boot logging overlay must change exactly {sorted(expected)}; found {sorted(changed)}")


def apply_boot_logging_overlay(root: pathlib.Path, boot_logging: dict[str, object]) -> None:
    before = file_manifest(root)
    shutil.copyfile(boot_logging["player"], root / "usr/bin/compas_player")
    shutil.copyfile(boot_logging["bootloader"], root / "usr/bin/compas_bootloader")
    after = file_manifest(root)
    require_exact_boot_logging_delta(before, after)
    for relative, name in (("usr/bin/compas_player", "compas_player"),
                           ("usr/bin/compas_bootloader", "compas_bootloader")):
        if after[relative].get("sha256") != boot_logging["binary_sha256"][name]:
            raise PackError(f"boot logging overlay binary hash mismatch: {relative}")


def load_frozen_radio_preparation(radio_assembly: dict) -> tuple[dict, dict]:
    """Load the hash-pinned radio provenance and the exact prep used by that build."""
    provenance_record = radio_assembly.get("provenance")
    if not isinstance(provenance_record, dict):
        raise PackError("radio candidate lacks frozen build provenance")
    provenance_path = pathlib.Path(str(provenance_record.get("path", "")))
    regular(provenance_path, "frozen radio provenance")
    if (provenance_record.get("sha256") != radio.PROVENANCE_SHA or
            sha(provenance_path) != radio.PROVENANCE_SHA):
        raise PackError("frozen radio provenance hash mismatch")
    provenance = read_json(provenance_path, "frozen radio provenance")
    build = provenance.get("build")
    if not isinstance(build, dict):
        raise PackError("frozen radio provenance lacks build inputs")
    prep_record = build.get("preparation_manifest")
    if not isinstance(prep_record, dict):
        raise PackError("frozen radio provenance lacks its preparation manifest pin")
    frozen_path = pathlib.Path(str(prep_record.get("path", "")))
    regular(frozen_path, "frozen radio preparation manifest")
    prep_hash = prep_record.get("sha256")
    if not isinstance(prep_hash, str) or sha(frozen_path) != prep_hash:
        raise PackError("frozen radio preparation manifest hash mismatch")
    frozen_prep = read_json(frozen_path, "frozen radio preparation manifest")
    return provenance, frozen_prep


def validate_local_patches(prep: dict, frozen_prep: dict,
                           workspace: pathlib.Path, repo: pathlib.Path) -> None:
    """Require the exact frozen ordered DMA/USB patch records and bytes."""
    frozen_records = frozen_prep.get("local_patches")
    records = prep.get("local_patches")
    if (not isinstance(frozen_records, list) or len(frozen_records) != 2 or
            not isinstance(records, list) or records != frozen_records):
        raise PackError("local DMA/USB patch records differ from frozen radio preparation")
    expected_names = ("compas-dma-signed-gaps.patch", "compas-usb-dac-safety.patch")
    for record, expected_name in zip(records, expected_names):
        if not isinstance(record, dict):
            raise PackError("malformed local DMA/USB patch record")
        original = record.get("original_path")
        copied = record.get("copied_to")
        digest = record.get("sha256")
        if (original != f"firmware/kernel/patches/{expected_name}" or
                copied != f"hiby-custom-kernel/patches/{expected_name}" or
                not isinstance(digest, str)):
            raise PackError("local DMA/USB patch order or path differs from frozen radio preparation")
        local_path = repo / original
        copied_path = workspace / copied
        regular(local_path, f"current local patch {expected_name}")
        regular(copied_path, f"copied local patch {expected_name}")
        if sha(local_path) != digest or sha(copied_path) != digest:
            raise PackError(f"local DMA/USB patch bytes differ from frozen radio preparation: {expected_name}")


def validate_frozen_sdk(prep: dict, frozen_provenance: dict) -> None:
    frozen_build = frozen_provenance.get("build")
    frozen_sdk = frozen_build.get("sdk") if isinstance(frozen_build, dict) else None
    upstream = prep.get("upstream")
    sdk = prep.get("sdk")
    if (not isinstance(frozen_sdk, dict) or frozen_sdk.get("sha256") != EXPECTED_SDK_SHA256 or
            frozen_sdk.get("size") != EXPECTED_SDK_SIZE or
            not isinstance(upstream, dict) or upstream.get("sdk_sha256") != EXPECTED_SDK_SHA256 or
            upstream.get("sdk_archive") != pathlib.Path(str(frozen_sdk.get("path", ""))).name or
            not isinstance(sdk, dict) or sdk.get("path") != frozen_sdk.get("path") or
            sdk.get("sha256") != EXPECTED_SDK_SHA256 or sdk.get("size") != EXPECTED_SDK_SIZE):
        raise PackError("combined build SDK does not match the frozen radio SDK pin")
    sdk_path = pathlib.Path(str(sdk["path"]))
    regular(sdk_path, "frozen SDK archive")
    if sdk_path.stat().st_size != EXPECTED_SDK_SIZE or sha(sdk_path) != EXPECTED_SDK_SHA256:
        raise PackError("combined build SDK archive bytes differ from the frozen radio SDK pin")


def validate_frozen_kernel_pins(validation: dict, radio_assembly: dict) -> None:
    kernel = radio_assembly.get("kernel")
    if not isinstance(kernel, dict):
        raise PackError("frozen radio assembly lacks kernel artifact pins")
    config = kernel.get("config")
    system_map = kernel.get("system_map")
    if (not isinstance(config, dict) or not isinstance(system_map, dict) or
            validation.get("compiled_config_sha256") != config.get("sha256") or
            validation.get("system_map_sha256") != system_map.get("sha256")):
        raise PackError("combined compiled config or System.map differs from frozen radio kernel pins")


def validate_preparation(workspace: pathlib.Path, repo: pathlib.Path,
                         radio_assembly: dict,
                         display_review_path: pathlib.Path | None = None) -> tuple[dict, dict, pathlib.Path, pathlib.Path, pathlib.Path, dict[str, str], pathlib.Path]:
    prep_path = workspace / "preparation.json"
    prep = read_json(prep_path, "combined preparation manifest")
    frozen_provenance, frozen_prep = load_frozen_radio_preparation(radio_assembly)
    if (prep.get("board") != "r1" or prep.get("build_requested") is not True or
            prep.get("profile") != "optimized" or prep.get("wifi_stack") != "brcmfmac" or
            prep.get("display_stack") != "compas" or prep.get("upstream_head") != EXPECTED_UPSTREAM or
            prep.get("upstream", {}).get("commit") != EXPECTED_UPSTREAM):
        raise PackError("fresh build must be optimized R1 brcmfmac + Compas display on the pinned kernel")
    validate_frozen_sdk(prep, frozen_provenance)
    validate_local_patches(prep, frozen_prep, workspace, repo)
    stock = prep.get("stock_kernel")
    if not isinstance(stock, dict) or stock.get("sha256") != "e3ed77e551afb061433b57c39ea10ebcfd862842436e51c6a7db66c737f4778a" or stock.get("size") != 3731520:
        raise PackError("combined build is not based on pinned R1 stock xImage")
    source = prep.get("provider_source")
    frozen_provider = radio_assembly.get("provider_source", {})
    if (not isinstance(source, dict) or source.get("sha256") != RADIO_PROVIDER_SHA or
            source.get("sha256") != frozen_provider.get("sha256") or
            source.get("path") != frozen_provider.get("path")):
        raise PackError("combined build provider source differs from reviewed radio provider")
    provider_path = pathlib.Path(str(source.get("path", "")))
    regular(provider_path, "authoritative provider source")
    if sha(provider_path) != RADIO_PROVIDER_SHA:
        raise PackError("authoritative provider source changed")
    prepared_rel = source.get("prepared_path")
    if not isinstance(prepared_rel, str) or pathlib.PurePosixPath(prepared_rel).is_absolute() or ".." in pathlib.PurePosixPath(prepared_rel).parts:
        raise PackError("unsafe prepared provider path")
    if prepared_rel != "hiby-custom-kernel/modules/bcm_wlbt_power/bcm_wlbt_power.c" or source.get("prepared_sha256") != RADIO_PROVIDER_SHA:
        raise PackError("prepared provider path/hash differs from the reviewed build input")
    prepared_provider = workspace.joinpath(*pathlib.PurePosixPath(prepared_rel).parts)
    regular(prepared_provider, "module-patch-produced provider source")
    if sha(prepared_provider) != RADIO_PROVIDER_SHA:
        raise PackError("combined build provider source is not the patch-produced reviewed input")

    patch_records = prep.get("module_patches")
    if not isinstance(patch_records, list):
        raise PackError("combined preparation lacks module source patch provenance")
    observed_patches: dict[str, str] = {}
    expected_paths = {name: f"firmware/kernel/module-patches/{name}" for name in
                      (*RADIO_MODULE_PATCHES, "compas-reconstructed-drivers-safety.patch")}
    for record in patch_records:
        if not isinstance(record, dict):
            raise PackError("malformed combined module patch record")
        name, digest = pathlib.PurePosixPath(str(record.get("original_path", ""))).name, record.get("sha256")
        if record.get("original_path") != expected_paths.get(name):
            raise PackError(f"unexpected module source patch path: {record.get('original_path')}")
        if name in observed_patches:
            raise PackError(f"duplicate module patch record: {name}")
        observed_patches[name] = digest
        local = repo / str(record.get("original_path", ""))
        regular(local, f"module source patch {name}")
        if sha(local) != digest:
            raise PackError(f"module source patch changed: {name}")
    expected_patches = {**RADIO_MODULE_PATCHES,
                        "compas-reconstructed-drivers-safety.patch": GENERAL_DRIVER_PATCH_SHA}
    patch_order = [pathlib.PurePosixPath(record["original_path"]).name for record in patch_records]
    if observed_patches != expected_patches or patch_order != [
            "compas-reconstructed-drivers-safety.patch", "compas-radio-input-safety.patch",
            "compas-radio-lifecycle.patch"]:
        raise PackError("combined module patches differ from the reviewed driver/radio patch set")
    for record in patch_records:
        copied = record.get("copied_to")
        patch_name = pathlib.PurePosixPath(str(record.get("original_path", ""))).name
        if copied != f"module-patches/{patch_name}":
            raise PackError(f"module patch copied to unexpected workspace path: {patch_name}")
        if not isinstance(copied, str) or pathlib.PurePosixPath(copied).is_absolute() or ".." in pathlib.PurePosixPath(copied).parts:
            raise PackError("unsafe copied module patch path")
        copy_path = workspace.joinpath(*pathlib.PurePosixPath(copied).parts)
        regular(copy_path, "copied module source patch")
        if sha(copy_path) != record["sha256"]:
            raise PackError(f"copied module source patch changed: {record['original_path']}")
    frozen_patch_records = radio_assembly.get("module_patches")
    if not isinstance(frozen_patch_records, list) or {
            pathlib.Path(record["path"]).name: record["sha256"] for record in frozen_patch_records
    } != RADIO_MODULE_PATCHES:
        raise PackError("radio frozen provenance does not pin the required ordered radio module patches")
    kernel_patch_records = prep.get("wifi_kernel_patches")
    if (not isinstance(kernel_patch_records, list) or len(kernel_patch_records) != 1 or
            kernel_patch_records[0].get("original_path") != "firmware/kernel/wifi-patches/compas-mmc-radio-lifetime.patch" or
            kernel_patch_records[0].get("sha256") != RADIO_LIFETIME_PATCH_SHA):
        raise PackError("combined build MMC patch differs from reviewed radio lifetime patch")
    kernel_patch = repo / kernel_patch_records[0]["original_path"]
    regular(kernel_patch, "MMC lifetime patch")
    if sha(kernel_patch) != RADIO_LIFETIME_PATCH_SHA:
        raise PackError("MMC lifetime patch bytes changed")
    if kernel_patch_records[0].get("copied_to") != "hiby-custom-kernel/patches/compas-mmc-radio-lifetime.patch":
        raise PackError("copied MMC patch is not in the pinned prepared-kit location")
    copied_kernel_patch = prep_path_parent(workspace) / kernel_patch_records[0]["copied_to"]
    regular(copied_kernel_patch, "copied MMC lifetime patch")
    if sha(copied_kernel_patch) != RADIO_LIFETIME_PATCH_SHA:
        raise PackError("copied MMC lifetime patch differs from reviewed radio patch")

    validation = prep.get("build_validation")
    if not isinstance(validation, dict) or validation.get("wifi_stack") not in (None, "brcmfmac") or validation.get("display_stack") != "compas":
        raise PackError("preparation has no successful combined build validation")
    validate_frozen_kernel_pins(validation, radio_assembly)
    config = pathlib.Path(str(validation.get("config_path", "")))
    system_map = pathlib.Path(str(validation.get("system_map_path", "")))
    ximage = pathlib.Path(str(validation.get("ximage_path", "")))
    out_dir = workspace / "hiby-custom-kernel/out"
    expected_artifacts = {
        config: out_dir / "config-compas-r1",
        system_map: out_dir / "System.map-compas-r1",
        ximage: out_dir / "xImage-compas-r1",
    }
    for path, label, key in ((config, "compiled combined config", "compiled_config_sha256"),
                             (system_map, "combined System.map", "system_map_sha256"),
                             (ximage, "combined xImage", "ximage_sha256")):
        if path.resolve() != expected_artifacts[path].resolve():
            raise PackError(f"combined build artifact is outside its pinned output location: {label}")
        regular(path, label)
        if not isinstance(validation.get(key), str) or sha(path) != validation[key]:
            raise PackError(f"combined build artifact does not match preparation: {key}")
    validate_uimage(ximage)
    config_text = config.read_text()
    for requested in ("CONFIG_BRCMFMAC=m", "CONFIG_BRCMUTIL=m", "# CONFIG_MODULE_FORCE_UNLOAD is not set"):
        if requested not in config_text:
            raise PackError(f"combined compiled config lacks required guarded radio setting: {requested}")
    if validation.get("compiled_config_sha256") is None:
        raise PackError("combined build does not pin its config hash")
    radio_kernel = radio_assembly.get("kernel", {})
    wifi_config = repo / "firmware/kernel/compas-r1-brcmfmac.config"
    regular(wifi_config, "reviewed brcmfmac config fragment")
    if sha(wifi_config) != radio_kernel.get("wifi_config_sha256"):
        raise PackError("brcmfmac config fragment differs from frozen radio candidate")
    if sha(workspace / "hiby-custom-kernel/configs/compas-r1-brcmfmac.config") != sha(wifi_config):
        raise PackError("prepared brcmfmac config fragment differs from frozen candidate")
    build_dir = workspace / "hiby-custom-kernel/out/modules-compas-r1"
    if build_dir.is_symlink() or not build_dir.is_dir():
        raise PackError("combined build module directory is missing or unsafe")
    actual_built = {p.stem for p in build_dir.glob("*.ko") if p.is_file() and not p.is_symlink()}
    expected_built = COMBINED_BUILT
    if actual_built != expected_built:
        raise PackError(f"combined build must emit exactly 26 modules; missing={sorted(expected_built-actual_built)}, extra={sorted(actual_built-expected_built)}")
    for p in build_dir.glob("*.ko"):
        regular(p, "combined built module")

    vermagic = validation.get("module_vermagic")
    expected_vermagic = "4.4.94+ preempt mod_unload MIPS32_R2 32BIT"
    if not isinstance(vermagic, dict) or set(vermagic) != expected_built or any(
            value != expected_vermagic for value in vermagic.values()):
        raise PackError("preparation does not record the exact combined module vermagic set")

    provider_hash = validation.get("provider_module_sha256")
    if (provider_hash != sha(build_dir / "bcm_wlbt_power.ko") or
            validation.get("provider_source_sha256") != RADIO_PROVIDER_SHA):
        raise PackError("combined provider module differs from recorded build validation")
    display_hashes, review_path = validate_display_review(prep, validation, workspace, repo, display_review_path)
    if (validation.get("soc_fb_module_path") != str((build_dir / "soc_fb.ko").resolve()) or
            validation.get("soc_fb_module_sha256") != sha(build_dir / "soc_fb.ko")):
        raise PackError("source-built soc_fb module differs from combined build validation")
    review = read_json(review_path, "final Astra display review")
    return prep, validation, build_dir, config, system_map, display_hashes, review_path


def build_abi_report(system_map: pathlib.Path, built_modules: pathlib.Path,
                     base_root: pathlib.Path, tmp: pathlib.Path,
                     candidate_assembly: dict) -> dict:
    vendor_dir = tmp / "retained-vendor-modules"
    vendor_dir.mkdir()
    module_hashes = candidate_assembly["modules"]
    source_base = pathlib.Path(candidate_assembly["base_root"]["path"])
    for name in sorted(COMBINED_RETAINED):
        source = base_root / "module_driver" / f"{name}.ko"
        stable_source = source_base / "module_driver" / f"{name}.ko"
        regular(source, f"retained vendor module {name}")
        regular(stable_source, f"pinned retained vendor module {name}")
        if sha(source) != module_hashes[f"{name}.ko"] or sha(stable_source) != module_hashes[f"{name}.ko"]:
            raise PackError(f"base retained vendor module differs from radio candidate pin: {name}")
        shutil.copyfile(source, vendor_dir / source.name)
    nm, modinfo = shutil.which("nm"), shutil.which("modinfo")
    if not nm or not modinfo:
        raise PackError("host nm and modinfo are required to verify the combined 31 modules")
    report = verify_module_abi.verify(system_map, built_modules, vendor_dir, nm, modinfo)
    if report.get("valid") is not True or report.get("selected_module_count") != radio.MODULE_TOTAL:
        raise PackError("combined mixed-module ABI verification failed")
    selected = validate_module_origins(report.get("selected_modules"))
    if report.get("missing_imports_by_module") or report.get("vermagic_errors"):
        raise PackError("combined ABI report has unresolved imports or vermagic errors")
    # The verifier used an isolated five-module temp directory so source-built
    # names win; point vendor rows back to the persistent pinned vendor files.
    for name in COMBINED_RETAINED:
        report["selected_modules"][name]["path"] = str(
            (source_base / "module_driver" / f"{name}.ko").resolve())
    for name, row in selected.items():
        expected_source = "built" if name in COMBINED_BUILT else "vendor"
        expected_file = built_modules / f"{name}.ko" if expected_source == "built" else base_root / "module_driver" / f"{name}.ko"
        if sha(expected_file) != row.get("sha256"):
            raise PackError(f"combined ABI module hash mismatch: {name}")
    return report


def check_soc_fb_loader(root: pathlib.Path, old_loader_sha: str,
                        old_startup_sha: str) -> dict[str, object]:
    module_dir = root / "module_driver"
    loader, startup = module_dir / "soc_fb.sh", module_dir / "driver_default_init_script.sh"
    regular(loader, "radio candidate soc_fb loader")
    regular(startup, "radio candidate driver startup")
    if sha(loader) != old_loader_sha or sha(startup) != old_startup_sha:
        raise PackError("radio candidate changed the pinned vendor soc_fb loader/startup")
    calls = re.findall(r"(?m)^\s*sh\s+(\S+)\s*$", startup.read_text())
    fb_calls = [call for call in calls if "fb" in pathlib.PurePosixPath(call).stem.lower()]
    loads = []
    for line in loader.read_text().splitlines():
        try:
            args = __import__("shlex").split(line, comments=True)
        except ValueError as exc:
            raise PackError(f"malformed soc_fb loader: {exc}") from exc
        if args and args[0] == "insmod":
            loads.append(args)
    if fb_calls != ["soc_fb.sh"] or len(loads) != 1 or len(loads[0]) < 2 or loads[0][1] != "soc_fb.ko":
        raise PackError("startup must invoke the single soc_fb.sh loader exactly once")
    return {"startup_invocations": 1, "loaded_module": "soc_fb.ko",
            "loader_sha256": old_loader_sha, "startup_sha256": old_startup_sha}


def replace_with_combined_modules(root: pathlib.Path, build_dir: pathlib.Path,
                                  candidate_assembly: dict,
                                  report: dict) -> dict[str, str]:
    module_dir = root / "module_driver"
    private_dir = root / "usr/lib/compas-radio/modules"
    selected = report["selected_modules"]
    final_hashes: dict[str, str] = {}
    for name in sorted(COMBINED_BUILT):
        src = build_dir / f"{name}.ko"
        destination = private_dir / src.name if name in radio.PRIVATE_MODULES else module_dir / src.name
        regular(src, f"combined build module {src.name}")
        expected = selected[name]
        if expected.get("source") != "built" or sha(src) != expected.get("sha256"):
            raise PackError(f"combined module is not selected by fresh ABI report: {name}")
        if file_mode(src) != 0o644:
            raise PackError(f"combined module has unexpected mode: {name}")
        shutil.copyfile(src, destination)
        destination.chmod(0o644)
        if sha(destination) != expected["sha256"]:
            raise PackError(f"copied combined module hash mismatch: {name}")
        final_hashes[name] = expected["sha256"]
    for name in sorted(COMBINED_RETAINED):
        path = module_dir / f"{name}.ko"
        row = selected[name]
        if row.get("source") != "vendor" or sha(path) != row.get("sha256"):
            raise PackError(f"retained vendor module changed: {name}")
        final_hashes[name] = row["sha256"]
    expected_driver = MODULE_DRIVER_STEMS
    actual_driver = {p.stem for p in module_dir.glob("*.ko")}
    if actual_driver != expected_driver or "cywdhd" in actual_driver:
        raise PackError("combined module_driver does not contain exactly 29 modules with source-built soc_fb")
    for name in radio.PRIVATE_MODULES:
        if sha(private_dir / f"{name}.ko") != final_hashes[name]:
            raise PackError(f"private radio module mismatch: {name}")
    if len(final_hashes) != radio.MODULE_TOTAL:
        raise PackError("combined payload does not account for all 31 ABI modules")
    old_soc = candidate_assembly["modules"]["soc_fb.ko"]
    return {"module_count": len(final_hashes), "source_built_count": len(COMBINED_BUILT),
            "retained_vendor_count": len(COMBINED_RETAINED), "module_sha256": final_hashes,
            "replaced_vendor_soc_fb_sha256": old_soc}


def create(base: pathlib.Path, radio_candidate: pathlib.Path, workspace: pathlib.Path,
           output: pathlib.Path, repo: pathlib.Path = REPO,
           display_review_path: pathlib.Path | None = None,
           boot_logging_binaries: pathlib.Path | None = None) -> pathlib.Path:
    abi_path = output.with_suffix(output.suffix + ".abi-report.json")
    if (output.exists() or output.is_symlink() or output.with_suffix(output.suffix + ".json").exists() or
            abi_path.exists() or abi_path.is_symlink()):
        raise PackError("combined output UPT, sidecar and ABI report must all be fresh")
    for tool in ("7z", "unsquashfs", "mksquashfs", "genisoimage"):
        if not shutil.which(tool):
            raise PackError(f"required package tool unavailable: {tool}")
    base = base.resolve(strict=True)
    radio_candidate = radio_candidate.resolve(strict=True)
    workspace = workspace.resolve(strict=True)
    candidate_assembly, radio_payload, radio_sources = radio.validate_candidate(radio_candidate)
    if boot_logging_binaries is not None:
        chosen_review = display_review_path or DISPLAY_REVIEW_PATH
        chosen_report = (repo / chosen_review).resolve(strict=True)
        if chosen_report != (repo / FAST_DISPLAY_REVIEW_PATH).resolve(strict=True):
            raise PackError("boot logging overlay requires explicit fast-adoption display review selection")
        boot_logging = validate_boot_logging_binaries(boot_logging_binaries, repo.resolve())
    else:
        boot_logging = None
    prep, validation, built_dir, config, system_map, display_sources, actual_display_review_path = validate_preparation(
        workspace, repo.resolve(), candidate_assembly, display_review_path)
    display_review_sha = sha(actual_display_review_path)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="repack-combined-", dir=output.parent) as tmp_name:
        work = pathlib.Path(tmp_name)
        base_root, ota, meta, base_rootfs_sha, base_sidecar = radio.unpack_base(base, work)
        # Build a fresh mixed ABI report from the exact combined module outputs
        # and only the five retained vendor modules. Keep its evidence beside UPT.
        report = build_abi_report(system_map, built_dir, base_root, work, candidate_assembly)
        report_path_tmp = work / "combined-31-module-abi-report.json"
        report_path_tmp.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        report_sha = sha(report_path_tmp)
        panel_path = built_dir / "lcd_lg35583.ko"
        if verify_panel_module.has_cleanup_module(panel_path):
            raise PackError("fresh lcd_lg35583.ko defines cleanup_module")
        panel_report = verify_panel_module.verify(base_root / "module_driver/lcd_lg35583.ko", panel_path)
        if not panel_report.get("valid"):
            raise PackError("fresh panel module does not match retained R1 panel ABI")

        before = file_manifest(base_root)
        root = work / "combined-root"
        shutil.copytree(base_root, root, symlinks=True)
        merged_change = radio.merge_root(root, radio_candidate, candidate_assembly, radio_payload, radio_sources)
        # The runtime/init script must continue loading the one controller using
        # the same vendor loader, now backed by the combined source-built module.
        base_vendor_soc = candidate_assembly["modules"]["soc_fb.ko"]
        if sha(root / "module_driver/soc_fb.ko") != base_vendor_soc:
            raise PackError("radio overlay did not retain the pinned vendor framebuffer module before replacement")
        # Radio v3 does not pin loader hashes directly; they are inherited byte
        # for byte from the exact V2 rootfs and checked by its outside-delta map.
        original_loader_sha = sha(root / "module_driver/soc_fb.sh")
        original_startup_sha = sha(root / "module_driver/driver_default_init_script.sh")
        loader = check_soc_fb_loader(root, original_loader_sha, original_startup_sha)
        combined_modules = replace_with_combined_modules(root, built_dir, candidate_assembly, report)
        if boot_logging is not None:
            apply_boot_logging_overlay(root, boot_logging)
        final_manifest = file_manifest(root)
        changed = changed_paths(before, final_manifest)
        new_dirs = {p for p, record in final_manifest.items() if record.get("type") == "directory" and p not in before}
        allowed = set(radio_payload) | set(radio.SYMLINKS) | set(REMOVED) | new_dirs | {"module_driver/soc_fb.ko"}
        if boot_logging is not None:
            allowed |= {"usr/bin/compas_player", "usr/bin/compas_bootloader"}
        changed = radio.ensure_allowed_delta(before, final_manifest, allowed)
        if not set(REMOVED).issubset(changed) or "module_driver/soc_fb.ko" not in changed:
            raise PackError("combined rootfs delta is missing the source-built soc_fb replacement or legacy removal")
        preserved_paths = sorted(set(before) - changed)
        preserved_manifest_sha = json_sha({p: before[p] for p in preserved_paths})
        for rel, digest in radio_payload.items():
            if boot_logging is not None and rel == radio.PLAYER_REL:
                continue
            if final_manifest.get(rel, {}).get("sha256") != digest:
                raise PackError(f"radio runtime payload/mode changed during combined assembly: {rel}")
        if final_manifest["module_driver/soc_fb.ko"]["sha256"] != report["selected_modules"]["soc_fb"]["sha256"]:
            raise PackError("rootfs does not contain the fresh source-built soc_fb module")
        for rel, target in radio.SYMLINKS.items():
            link = root.joinpath(*pathlib.PurePosixPath(rel).parts)
            if not link.is_symlink() or os.readlink(link) != target:
                raise PackError(f"radio firmware symlink changed: {rel}")
        driver_init = sha(root / "module_driver/driver_default_init_script.sh")
        if loader["startup_sha256"] != driver_init:
            raise PackError("radio startup script changed while assembling combined modules")

        iso = work / "base-iso"
        iso_manifest_before = file_manifest(iso)
        for prefix in ("xImage.", "ota_md5_xImage.", "rootfs.squashfs.", "ota_md5_rootfs.squashfs."):
            for path in list(ota.iterdir()):
                if path.name.startswith(prefix):
                    if path.is_symlink() or not path.is_file():
                        raise PackError(f"unsafe base OTA entry: {path.name}")
                    path.unlink()
        kernel_size, kernel_md5 = package(pathlib.Path(validation["ximage_path"]), ota, "xImage")
        new_rootfs = work / "combined-rootfs.squashfs"
        run(["mksquashfs", str(root), str(new_rootfs), "-comp", "lzo", "-all-root", "-noappend", "-no-xattrs", "-processors", "2"])
        rootfs_size, rootfs_md5 = package(new_rootfs, ota, "rootfs.squashfs")
        meta["kernel"].update(img_size=str(kernel_size), img_md5=kernel_md5)
        meta["rootfs"].update(img_size=str(rootfs_size), img_md5=rootfs_md5)
        metadata_lines = ["ota_version=0"]
        for kind in ("kernel", "rootfs"):
            row = meta[kind]
            metadata_lines.extend((f"img_type={kind}", f"img_name={row['img_name']}",
                                   f"img_size={row['img_size']}", f"img_md5={row['img_md5']}"))
        (ota / "ota_update.in").write_text("\n".join(metadata_lines) + "\n")
        iso_manifest_after = file_manifest(iso)
        iso_delta = changed_paths(iso_manifest_before, iso_manifest_after)
        allowed_iso_delta = {"ota_v0/ota_update.in"}
        for rel in iso_delta:
            if rel.startswith("ota_v0/xImage.") or rel.startswith("ota_v0/ota_md5_xImage.") or rel.startswith("ota_v0/rootfs.squashfs.") or rel.startswith("ota_v0/ota_md5_rootfs.squashfs."):
                allowed_iso_delta.add(rel)
        if iso_delta != allowed_iso_delta:
            raise PackError(f"ISO changed outside kernel/rootfs OTA payload metadata: {sorted(iso_delta ^ allowed_iso_delta)}")

        staged = work / "combined-radio-display.upt"
        run(["genisoimage", "-f", "-U", "-J", "-joliet-long", "-r", "-allow-lowercase", "-allow-multidot", "-o", str(staged), str(iso)])
        if staged.stat().st_size > LIMIT:
            raise PackError("combined candidate UPT exceeds 45 MiB")
        verify_iso = work / "verify-iso"
        verify_iso.mkdir()
        run(["7z", "x", "-y", str(staged), f"-o{verify_iso}"])
        final_ota = verify_iso / "ota_v0"
        final_meta = parse_update(final_ota / "ota_update.in")
        for kind, name in (("kernel", "xImage"), ("rootfs", "rootfs.squashfs")):
            row = final_meta[kind]
            unpack_image(final_ota, name, int(row["img_size"]), row["img_md5"], work / f"final-{name}")
        if sha(work / "final-xImage") != validation["ximage_sha256"]:
            raise PackError("round-trip kernel differs from fresh combined build")
        validate_uimage(work / "final-xImage")
        final_root = work / "final-root"
        final_root.mkdir()
        run(["unsquashfs", "-no-xattrs", "-d", str(final_root), str(work / "final-rootfs.squashfs")])
        final_files = file_manifest(final_root)
        if final_files != final_manifest:
            raise PackError("combined rootfs content, modes, symlinks, or inventory changed in squashfs round trip")
        if json_sha({p: final_files.get(p) for p in preserved_paths}) != preserved_manifest_sha:
            raise PackError("a rootfs path outside explicit combined changes was modified")
        final_driver = {p.stem for p in (final_root / "module_driver").glob("*.ko")}
        if final_driver != MODULE_DRIVER_STEMS:
            raise PackError("round-trip package does not contain exact combined module_driver inventory")
        final_abi_modules = report["selected_modules"]
        for name in COMBINED_STEMS:
            path = (final_root / "module_driver" / f"{name}.ko" if name in MODULE_DRIVER_STEMS else
                    final_root / "usr/lib/compas-radio/modules" / f"{name}.ko")
            if sha(path) != final_abi_modules[name]["sha256"]:
                raise PackError(f"round-trip module ABI hash mismatch: {name}")
        for rel, digest in radio_payload.items():
            if boot_logging is not None and rel == radio.PLAYER_REL:
                continue
            if final_files.get(rel, {}).get("sha256") != digest:
                raise PackError(f"round-trip frozen radio payload mismatch: {rel}")
        expected_player_sha = (boot_logging["binary_sha256"]["compas_player"]
                               if boot_logging is not None else radio.PLAYER_SHA)
        if final_files["usr/bin/compas_player"].get("sha256") != expected_player_sha:
            raise PackError("round-trip player differs from selected reviewed player")
        bootloader = final_files.get("usr/bin/compas_bootloader")
        if boot_logging is None and bootloader != before.get("usr/bin/compas_bootloader"):
            raise PackError("production bootloader changed")
        if boot_logging is not None and bootloader.get("sha256") != boot_logging["binary_sha256"]["compas_bootloader"]:
            raise PackError("round-trip bootloader differs from the reviewed boot logging binary")
        if json_sha({k: v for k, v in final_files.items() if "plugin" in k.lower() or "v1.1" in k.lower()}) != json_sha(
                {k: v for k, v in before.items() if "plugin" in k.lower() or "v1.1" in k.lower()}):
            raise PackError("plugin-named assets changed")
        if final_files.get("module_driver/cywdhd.ko") or final_files.get("module_driver/cywdhd.sh"):
            raise PackError("legacy cywdhd bypass survived the combined package")
        if check_soc_fb_loader(final_root, loader["loader_sha256"], loader["startup_sha256"]) != loader:
            raise PackError("round-trip soc_fb loader/startup contract changed")

        os.replace(staged, output)
        final_abi_path = output.with_suffix(output.suffix + ".abi-report.json")
        os.replace(report_path_tmp, final_abi_path)
        sidecar = output.with_suffix(output.suffix + ".json")
        sidecar.write_text(json.dumps({
            "schema_version": 1,
            "candidate_type": "host-only combined R1 radio/display UPT; not validated on device",
            "base_upt_sha256": radio.BASE_SHA, "base_sidecar_sha256": radio.BASE_SIDECAR_SHA,
            "base_kernel_sha256": radio.BASE_KERNEL_SHA, "base_rootfs_sha256": base_rootfs_sha,
            "radio_assembly_sha256": radio.ASSEMBLY_SHA,
            "radio_provenance_sha256": radio.PROVENANCE_SHA,
            "radio_player_sha256": radio.PLAYER_SHA,
            "radio_review_reports_sha256": radio.REVIEW_FILES,
            "display_review_model": "gpt-6-astra", "display_review_sha256": display_review_sha,
            "display_source_sha256": display_sources,
            "preparation_sha256": sha(workspace / "preparation.json"),
            "combined_kernel_sha256": validation["ximage_sha256"],
            "combined_config_sha256": validation["compiled_config_sha256"],
            "combined_system_map_sha256": validation["system_map_sha256"],
            "combined_mmc_patch_sha256": RADIO_LIFETIME_PATCH_SHA,
            "combined_provider_source_sha256": RADIO_PROVIDER_SHA,
            "combined_abi_report_sha256": sha(final_abi_path),
            "combined_abi_report_path": str(final_abi_path),
            "combined_module_sha256": combined_modules["module_sha256"],
            "module_count": radio.MODULE_TOTAL, "source_built_count": len(COMBINED_BUILT),
            "retained_vendor_count": len(COMBINED_RETAINED), "panel_abi": panel_report,
            "soc_fb_loader": loader, "rootfs_changed_paths": sorted(changed),
            "rootfs_preserved_manifest_sha256": preserved_manifest_sha,
            "rootfs_unchanged_outside_explicit_changes": True,
            "removed_legacy_wifi_paths_sha256": REMOVED,
            "production_bootloader_sha256": (before["usr/bin/compas_bootloader"]["sha256"]
                                               if boot_logging is not None else bootloader["sha256"]),
            "bootloader_sha256": bootloader["sha256"],
            "boot_logging_overlay": ({
                "review_sha256": boot_logging["review_sha256"],
                "build_manifest_sha256": boot_logging["manifest_sha256"],
                "source_sha256": boot_logging["source_sha256"],
                "binary_sha256": boot_logging["binary_sha256"],
                "changed_paths": ["usr/bin/compas_bootloader", "usr/bin/compas_player"],
                "rootfs_delta_exact": True,
            } if boot_logging is not None else None),
            "plugin_named_assets_manifest_sha256": json_sha({k: v for k, v in before.items() if "plugin" in k.lower() or "v1.1" in k.lower()}),
            "output_upt_sha256": sha(output), "output_upt_size": output.stat().st_size,
        }, indent=2, sort_keys=True) + "\n")
    return output


def prep_path_parent(workspace: pathlib.Path) -> pathlib.Path:
    return workspace


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-upt", type=pathlib.Path, required=True)
    parser.add_argument("--radio-candidate", type=pathlib.Path, required=True,
                        help="exact reviewed radio-overlay-ap6212a-host-final-v3")
    parser.add_argument("--kernel-workspace", type=pathlib.Path, required=True,
                        help="fresh optimized combined brcmfmac+Compas-display workspace")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--display-review", type=pathlib.Path,
                        help="explicitly select the pinned Astra display review (defaults to the legacy review)")
    parser.add_argument("--boot-logging-binaries", type=pathlib.Path,
                        help="opt in to the exact frozen Astra-reviewed player/bootloader artifact directory")
    args = parser.parse_args()
    try:
        print(create(args.base_upt, args.radio_candidate, args.kernel_workspace, args.output,
                     display_review_path=args.display_review,
                     boot_logging_binaries=args.boot_logging_binaries))
    except (OSError, KeyError, TypeError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"repack_combined_upt.py: {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
