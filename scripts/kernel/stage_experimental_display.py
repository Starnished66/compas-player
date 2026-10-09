#!/usr/bin/env python3
"""Assemble a provenance-pinned, host-only full R1 display-module candidate."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shlex
import shutil
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from repack_driver_upt import REPLACEMENT_NAMES as REVIEWED_DRIVER_NAMES  # noqa: E402
import verify_panel_module  # noqa: E402


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
VENDOR_DISPLAY_MODULE_NAMES = {"axp2101", "cywdhd", "sau", "soc_i2c", "soc_msc", "soc_pwm"}
DISPLAY_MODULE_NAMES = REVIEWED_DRIVER_NAMES | VENDOR_DISPLAY_MODULE_NAMES | {"soc_fb"}
ABI_MODULE_COUNT = 29
BASE_PROVENANCE_REL = pathlib.Path("usr/share/compas/vendor-driver-provenance.json")
DISPLAY_SOURCE_PREFIX = "firmware/kernel/display-experimental/"


class DisplayStageError(Exception):
    """A candidate input failed its source, build, ABI, or loader pin."""


def digest(path: pathlib.Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def regular_file(path: pathlib.Path, label: str) -> None:
    if path.is_symlink() or not path.is_file():
        raise DisplayStageError(f"{label} must be a regular non-symlink file: {path}")


def checked_recorded_file(base: pathlib.Path, installed: dict[str, object],
                          relative: str) -> pathlib.Path:
    key = "/" + relative.as_posix()
    record = installed.get(key)
    if not isinstance(record, dict) or not isinstance(record.get("sha256"), str):
        raise DisplayStageError(f"base provenance has no hash for {key}")
    path = base / relative
    regular_file(path, f"base input {key}")
    if digest(path) != record["sha256"]:
        raise DisplayStageError(f"base input hash differs from provenance: {key}")
    if "size" in record and path.stat().st_size != record["size"]:
        raise DisplayStageError(f"base input size differs from provenance: {key}")
    return path


def validate_base(base: pathlib.Path, expected_archive_sha256: str) -> tuple[pathlib.Path, dict[str, object]]:
    if base.is_symlink() or not base.is_dir():
        raise DisplayStageError(f"base root must be a regular directory: {base}")
    provenance_path = base / BASE_PROVENANCE_REL
    regular_file(provenance_path, "base vendor-driver provenance")
    try:
        provenance = json.loads(provenance_path.read_text())
        installed = provenance["installed_files"]
    except (OSError, json.JSONDecodeError, KeyError, TypeError) as exc:
        raise DisplayStageError(f"malformed base vendor-driver provenance: {exc}") from exc
    if provenance.get("board") != "r1" or provenance.get("archive_sha256") != expected_archive_sha256:
        raise DisplayStageError("base archive SHA-256 does not match the explicit R1 base pin")
    if not isinstance(installed, dict):
        raise DisplayStageError("base provenance has no installed_files map")

    module_dir = base / "module_driver"
    if module_dir.is_symlink() or not module_dir.is_dir():
        raise DisplayStageError("base module_driver must be a regular directory")
    for path in module_dir.rglob("*"):
        if path.is_symlink():
            raise DisplayStageError(f"base module_driver contains a symlink: {path}")
        if path.is_file():
            relative = path.relative_to(base).as_posix()
            key = "/" + relative
            record = installed.get(key)
            if not isinstance(record, dict) or digest(path) != record.get("sha256"):
                raise DisplayStageError(f"base module_driver file is not pinned: {relative}")
    base_module_names = {path.stem for path in module_dir.glob("*.ko")}
    if base_module_names != DISPLAY_MODULE_NAMES:
        raise DisplayStageError("frozen base must contain the exact production 29-module display inventory")

    old_module = checked_recorded_file(base, installed, pathlib.Path("module_driver/soc_fb.ko"))
    loader = checked_recorded_file(base, installed, pathlib.Path("module_driver/soc_fb.sh"))
    startup = checked_recorded_file(base, installed,
                                    pathlib.Path("module_driver/driver_default_init_script.sh"))
    loader_text = loader.read_text()
    startup_text = startup.read_text()
    startup_scripts = re.findall(r"(?m)^\s*sh\s+(\S+)\s*$", startup_text)
    framebuffer_scripts = [name for name in startup_scripts
                           if "fb" in pathlib.PurePosixPath(name).stem.lower()]
    loads = []
    for line in loader_text.splitlines():
        tokens = shlex.split(line, comments=True)
        if tokens and tokens[0] == "insmod":
            loads.append(tokens)
    if (framebuffer_scripts != ["soc_fb.sh"] or len(loads) != 1 or len(loads[0]) < 2 or
            loads[0][1] != "soc_fb.ko"):
        raise DisplayStageError("R1 startup must invoke soc_fb.sh once and it must load soc_fb.ko once")
    return old_module, {
        "archive_sha256": provenance["archive_sha256"],
        "provenance_path": str(provenance_path.resolve()),
        "provenance_sha256": digest(provenance_path),
        "soc_fb_vendor_sha256": digest(old_module),
        "soc_fb_loader_sha256": digest(loader),
        "driver_init_sha256": digest(startup),
        "driver_init_soc_fb_invocations": 1,
        "loader_module": loads[0][1],
    }


def checked_preparation(path: pathlib.Path, built_modules: pathlib.Path,
                        repo: pathlib.Path) -> tuple[dict[str, object], dict[str, object]]:
    regular_file(path, "preparation manifest")
    try:
        preparation = json.loads(path.read_text())
        validation = preparation["build_validation"]
        upstream = preparation["upstream"]
    except (OSError, json.JSONDecodeError, KeyError, TypeError) as exc:
        raise DisplayStageError(f"malformed preparation manifest: {exc}") from exc
    if preparation.get("build_requested") is not True:
        raise DisplayStageError("display staging requires a completed fresh kernel build")
    if preparation.get("display_stack") != "compas" or preparation.get("wifi_stack") != "vendor":
        raise DisplayStageError("display staging requires --display-stack compas with vendor Wi-Fi")
    if not isinstance(upstream, dict) or preparation.get("upstream_head") != upstream.get("commit"):
        raise DisplayStageError("preparation manifest has inconsistent pinned kernel provenance")
    if not isinstance(validation, dict) or validation.get("display_stack") != "compas":
        raise DisplayStageError("preparation manifest lacks successful Compas display build validation")
    if not isinstance(built_modules, pathlib.Path) or built_modules.is_symlink() or not built_modules.is_dir():
        raise DisplayStageError("built module directory must be a regular directory")

    source_records = preparation.get("display_sources")
    source_hashes = validation.get("display_source_sha256")
    if not isinstance(source_records, list) or not source_records or not isinstance(source_hashes, dict):
        raise DisplayStageError("build provenance lacks Compas display source hashes")
    observed_hashes = {}
    source_names = set()
    for record in source_records:
        if not isinstance(record, dict):
            raise DisplayStageError("malformed display source record")
        original = record.get("original_path")
        copied_to = record.get("copied_to")
        expected = record.get("sha256")
        prepared_hash = record.get("prepared_sha256")
        if (not isinstance(original, str) or not original.startswith(DISPLAY_SOURCE_PREFIX) or
                not isinstance(copied_to, str) or not isinstance(expected, str) or
                not isinstance(prepared_hash, str) or expected != prepared_hash):
            raise DisplayStageError("malformed or inconsistent display source hash record")
        original_rel = pathlib.PurePosixPath(original)
        copied_rel = pathlib.PurePosixPath(copied_to)
        if original_rel.is_absolute() or ".." in original_rel.parts or copied_rel.is_absolute() or ".." in copied_rel.parts:
            raise DisplayStageError("unsafe display source path in preparation manifest")
        prefix_parts = len(pathlib.PurePosixPath(DISPLAY_SOURCE_PREFIX).parts)
        if ((original_rel.name in ("Makefile", "hiby.symvers") and len(original_rel.parts) != prefix_parts + 1) or
                (original_rel.name not in ("Makefile", "hiby.symvers") and original_rel.suffix not in (".c", ".h"))):
            raise DisplayStageError(f"unsupported display source path: {original}")
        source_path = repo.joinpath(*original_rel.parts)
        copied_path = path.parent.joinpath(*copied_rel.parts)
        try:
            source_path.resolve().relative_to(repo.resolve())
            copied_path.resolve().relative_to(path.parent.resolve())
        except ValueError as exc:
            raise DisplayStageError("display source path escapes its pinned workspace") from exc
        regular_file(source_path, "checked-in display source")
        regular_file(copied_path, "prepared display source")
        if digest(source_path) != expected or digest(copied_path) != prepared_hash:
            raise DisplayStageError(f"display source changed since build: {original}")
        if source_hashes.get(original) != expected:
            raise DisplayStageError(f"build validation source hash disagrees: {original}")
        observed_hashes[original] = expected
        source_names.add(pathlib.PurePosixPath(original).name)
    if (not any(name.endswith(".c") for name in source_names) or
            not any(name.endswith(".h") for name in source_names) or
            "Makefile" not in source_names or "hiby.symvers" not in source_names):
        raise DisplayStageError("display source manifest is incomplete")

    for path_key, hash_key in (("ximage_path", "ximage_sha256"),
                               ("config_path", "compiled_config_sha256"),
                               ("system_map_path", "system_map_sha256")):
        artifact_value = validation.get(path_key)
        hash_value = validation.get(hash_key)
        if not isinstance(artifact_value, str):
            raise DisplayStageError(f"build provenance lacks {path_key}")
        artifact = pathlib.Path(artifact_value)
        regular_file(artifact, f"fresh build {path_key}")
        if path_key == "system_map_path" and not isinstance(hash_value, str):
            hash_value = digest(artifact)
            validation[hash_key] = hash_value
        if not isinstance(hash_value, str):
            raise DisplayStageError(f"build provenance lacks {hash_key}")
        if digest(artifact) != hash_value:
            raise DisplayStageError(f"fresh build artifact changed: {path_key}")
    module_path_value = validation.get("soc_fb_module_path")
    module_hash = validation.get("soc_fb_module_sha256")
    if not isinstance(module_path_value, str) or not isinstance(module_hash, str):
        raise DisplayStageError("build provenance lacks soc_fb.ko hash")
    module_path = pathlib.Path(module_path_value)
    regular_file(module_path, "fresh source soc_fb.ko")
    try:
        module_path.resolve().relative_to(built_modules.resolve())
    except ValueError as exc:
        raise DisplayStageError("source soc_fb.ko is outside the fresh built module directory") from exc
    if digest(module_path) != module_hash:
        raise DisplayStageError("fresh source soc_fb.ko changed")
    return preparation, validation


def checked_abi_report(path: pathlib.Path, built_modules: pathlib.Path,
                       base: pathlib.Path, validation: dict[str, object],
                       module_count: int = ABI_MODULE_COUNT) -> tuple[dict[str, object], dict[str, dict[str, object]]]:
    regular_file(path, "module ABI report")
    try:
        report = json.loads(path.read_text())
        selected = report["selected_modules"]
    except (OSError, json.JSONDecodeError, KeyError, TypeError) as exc:
        raise DisplayStageError(f"malformed module ABI report: {exc}") from exc
    if (report.get("valid") is not True or report.get("selected_module_count") != module_count or
            not isinstance(selected, dict) or len(selected) != module_count or
            report.get("missing_imports_by_module") or report.get("vermagic_errors")):
        raise DisplayStageError(f"ABI report must contain exactly {module_count} valid selected modules")
    if set(selected) != DISPLAY_MODULE_NAMES:
        raise DisplayStageError("ABI report must select the exact 22 reviewed drivers, six vendor modules, and soc_fb")
    sysmap = pathlib.Path(str(validation["system_map_path"]))
    report_map = pathlib.Path(str(report.get("system_map", "")))
    if (report_map.resolve() != sysmap.resolve() or
            report.get("system_map_sha256") != validation.get("system_map_sha256")):
        raise DisplayStageError("ABI report is not pinned to the fresh build System.map")

    if ("soc_fb" not in selected or not isinstance(selected["soc_fb"], dict) or
            selected["soc_fb"].get("source") != "built"):
        raise DisplayStageError("ABI report must select source-built soc_fb.ko")
    module_path = pathlib.Path(str(validation["soc_fb_module_path"])).resolve()
    module_hash = str(validation["soc_fb_module_sha256"])
    if selected["soc_fb"].get("sha256") != module_hash:
        raise DisplayStageError("ABI report does not select the hash-pinned fresh soc_fb.ko")
    display_names = [name for name in selected if "fb" in name.lower()]
    if display_names != ["soc_fb"]:
        raise DisplayStageError("ABI report contains more than one framebuffer controller module")

    module_records = {}
    installed = json.loads((base / BASE_PROVENANCE_REL).read_text())["installed_files"]
    for name, record in selected.items():
        if not re.fullmatch(r"[A-Za-z0-9_]+", name) or not isinstance(record, dict):
            raise DisplayStageError(f"unsafe or malformed ABI module name: {name!r}")
        filename = name + ".ko"
        source = record.get("source")
        record_path = pathlib.Path(str(record.get("path", "")))
        regular_file(record_path, f"ABI selected module {filename}")
        if source == "built":
            actual_path = built_modules / filename
            regular_file(actual_path, f"fresh build module {filename}")
            if digest(actual_path) != record.get("sha256"):
                raise DisplayStageError(f"ABI module does not match fresh build output: {filename}")
        elif source == "vendor":
            if name not in VENDOR_DISPLAY_MODULE_NAMES:
                raise DisplayStageError(f"unexpected vendor ABI module: {filename}")
            expected_path = base / "module_driver" / filename
            provenance_record = installed.get("/module_driver/" + filename)
            if not isinstance(provenance_record, dict) or record.get("sha256") != provenance_record.get("sha256"):
                raise DisplayStageError(f"vendor ABI module is not pinned by base provenance: {filename}")
            # verify_module_abi.py selects modules from its vendor-modules input
            # directory. A deliberate copy excluding stock soc_fb.ko lets the
            # fresh source-built controller win name collisions. Trust the
            # vendor provenance hash, not that temporary scan path.
            checked_recorded_file(base, installed, pathlib.Path("module_driver") / filename)
        else:
            raise DisplayStageError(f"unknown ABI module source for {filename}: {source!r}")
        if digest(record_path) != record.get("sha256"):
            raise DisplayStageError(f"ABI module hash changed: {filename}")
        module_records[filename] = {
            "source": source,
            "sha256": record["sha256"],
            "vermagic": record.get("vermagic"),
            "path": str(record_path.resolve()),
        }
    return report, module_records


def stage(base_root: pathlib.Path, expected_base_archive_sha256: str,
          built_modules: pathlib.Path, build_manifest: pathlib.Path,
          abi_report_path: pathlib.Path, output: pathlib.Path,
          repo: pathlib.Path = REPO_ROOT) -> pathlib.Path:
    if not re.fullmatch(r"[0-9a-f]{64}", expected_base_archive_sha256):
        raise DisplayStageError("base archive SHA-256 must be an explicit lowercase digest")
    if output.is_symlink() or output.exists():
        raise DisplayStageError(f"candidate output must be fresh and not a symlink: {output}")
    base = base_root.resolve()
    modules = built_modules.resolve()
    provenance_path = base / BASE_PROVENANCE_REL
    regular_file(provenance_path, "base vendor-driver provenance")
    old_fb, base_info = validate_base(base, expected_base_archive_sha256)
    installed = json.loads((base / BASE_PROVENANCE_REL).read_text())["installed_files"]
    for name in VENDOR_DISPLAY_MODULE_NAMES:
        checked_recorded_file(base, installed, pathlib.Path("module_driver") / f"{name}.ko")
    preparation, validation = checked_preparation(build_manifest, modules, repo.resolve())
    report, module_records = checked_abi_report(abi_report_path, modules, base, validation)
    panel_module = modules / "lcd_lg35583.ko"
    if verify_panel_module.has_cleanup_module(panel_module):
        raise DisplayStageError("fresh lcd_lg35583.ko defines cleanup_module and cannot be unloaded safely")

    output.parent.mkdir(parents=True, exist_ok=True)
    temp = pathlib.Path(tempfile.mkdtemp(prefix=output.name + ".tmp.", dir=output.parent))
    root = temp / "rootfs"
    try:
        source_module_dir = base / "module_driver"
        target_module_dir = root / "module_driver"
        shutil.copytree(source_module_dir, target_module_dir)
        for module_file in target_module_dir.glob("*.ko"):
            if module_file.name not in module_records:
                module_file.unlink()
        for filename, record in module_records.items():
            if record["source"] == "built":
                source = pathlib.Path(record["path"])
                if not source.is_file() or source.is_symlink():
                    raise DisplayStageError(f"fresh build is missing {filename}")
            else:
                source = base / "module_driver" / filename
            shutil.copy2(source, target_module_dir / filename)
        staged_soc_fb = target_module_dir / "soc_fb.ko"
        if digest(staged_soc_fb) != module_records["soc_fb.ko"]["sha256"]:
            raise DisplayStageError("staged soc_fb.ko is not the source-built controller")
        if {path.stem for path in target_module_dir.glob("*.ko")} != DISPLAY_MODULE_NAMES:
            raise DisplayStageError("staged module_driver does not contain the exact production 29-module display set")
        if digest(target_module_dir / "soc_fb.sh") != base_info["soc_fb_loader_sha256"]:
            raise DisplayStageError("staged soc_fb.sh differs from pinned vendor loader")
        if digest(target_module_dir / "driver_default_init_script.sh") != base_info["driver_init_sha256"]:
            raise DisplayStageError("staged R1 startup script differs from its pin")

        source_hashes = {record["original_path"]: record["sha256"]
                         for record in preparation["display_sources"]}
        assembly = {
            "schema_version": 1,
            "status": "host-only candidate; not validated on device; not a flash/package artifact",
            "display_stack": "compas",
            "wifi_stack": "vendor",
            "exclusive_soc_fb_controller": True,
            "base": base_info,
            "build": {
                "preparation_path": str(build_manifest.resolve()),
                "preparation_sha256": digest(build_manifest),
                "upstream": preparation["upstream"],
                "stock_kernel": preparation["stock_kernel"],
                "ximage_sha256": validation["ximage_sha256"],
                "config_sha256": validation["compiled_config_sha256"],
                "system_map_sha256": validation["system_map_sha256"],
                "system_map_path": validation["system_map_path"],
                "soc_fb_module_sha256": validation["soc_fb_module_sha256"],
            },
            "abi_report": {
                "path": str(abi_report_path.resolve()),
                "sha256": digest(abi_report_path),
                "system_map_sha256": report["system_map_sha256"],
                "selected_module_count": ABI_MODULE_COUNT,
            },
            "display_sources": source_hashes,
            "modules": {filename[:-3]: record for filename, record in module_records.items()},
            "replaced_vendor_soc_fb_sha256": digest(old_fb),
            "loader": {
                "path": "module_driver/soc_fb.sh",
                "sha256": base_info["soc_fb_loader_sha256"],
                "module": "soc_fb.ko",
                "startup_invocations": 1,
            },
            "payload_root": "rootfs/module_driver",
        }
        (temp / "DISPLAY-CANDIDATE.json").write_text(
            json.dumps(assembly, indent=2, sort_keys=True) + "\n")
        os.replace(temp, output)
    except Exception:
        shutil.rmtree(temp, ignore_errors=True)
        raise
    return output


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-root", type=pathlib.Path, required=True,
                        help="explicitly extracted frozen R1 root tree")
    parser.add_argument("--base-archive-sha256", required=True,
                        help="explicit frozen vendor archive SHA-256")
    parser.add_argument("--built-modules", type=pathlib.Path, required=True,
                        help="modules-compas-r1 output from the fresh display build")
    parser.add_argument("--build-manifest", type=pathlib.Path, required=True,
                        help="preparation.json for the fresh Compas display kernel build")
    parser.add_argument("--abi-report", type=pathlib.Path, required=True,
                        help="29-module display-set verify_module_abi.py report")
    parser.add_argument("--output", type=pathlib.Path, required=True,
                        help="fresh host-only candidate directory")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        stage(args.base_root, args.base_archive_sha256, args.built_modules,
              args.build_manifest, args.abi_report, args.output)
    except DisplayStageError as exc:
        print(f"stage_experimental_display.py: {exc}", file=sys.stderr)
        return 2
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as exc:
        print(f"stage_experimental_display.py: malformed input: {exc}", file=sys.stderr)
        return 2
    print(f"Created host-only display candidate: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
