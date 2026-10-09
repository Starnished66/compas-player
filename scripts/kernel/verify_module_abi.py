#!/usr/bin/env python3
"""Verify kernel/module symbol compatibility and vermagic for a firmware set.

Example::

    scripts/kernel/verify_module_abi.py --system-map /path/to/System.map-compas-r1 \
      --built-modules /path/to/modules-compas-r1 \
      --vendor-modules /path/to/stock/module_driver --output /path/to/abi-report.json

By default, vendor modules take precedence and non-colliding built modules are
included. Pass ``--vendor-only`` when the firmware retains every vendor module
and ships none of the reconstructed modules.

This is a static symbol/vermagic check. It cannot prove struct-layout
compatibility, module load order, or runtime behavior on the player.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys


EXPECTED_RELEASE = "4.4.94+"
REQUIRED_VERMAGIC = {EXPECTED_RELEASE, "preempt", "mod_unload", "MIPS32_R2", "32BIT"}
KSYM_PREFIX = "__ksymtab_"


class VerificationError(Exception):
    """Input artifacts or the host inspection tools could not be used."""


def file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def module_files(directory: pathlib.Path, *, allow_empty: bool = False) -> dict[str, pathlib.Path]:
    if not directory.is_dir():
        raise VerificationError(f"Module directory does not exist: {directory}")
    files = {path.stem: path for path in sorted(directory.glob("*.ko")) if path.is_file()}
    if not files and not allow_empty:
        raise VerificationError(f"No .ko modules found in {directory}")
    return files


def run_tool(command: list[str], description: str) -> str:
    try:
        result = subprocess.run(command, check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise VerificationError(f"{description} failed ({' '.join(command)}): {exc}") from exc
    return result.stdout


def parse_nm_undefined(output: str) -> tuple[set[str], set[str]]:
    strong: set[str] = set()
    weak: set[str] = set()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 2:
            continue
        symbol_type, symbol = fields[-2], fields[-1]
        if symbol_type == "U":
            strong.add(symbol)
        elif symbol_type in ("w", "v"):
            weak.add(symbol)
    return strong, weak


def parse_nm_exports(output: str) -> set[str]:
    exports: set[str] = set()
    for line in output.splitlines():
        fields = line.split()
        if not fields:
            continue
        symbol = fields[-1]
        if symbol.startswith(KSYM_PREFIX) and len(symbol) > len(KSYM_PREFIX):
            exports.add(symbol[len(KSYM_PREFIX):])
    return exports


def parse_kernel_exports(system_map: pathlib.Path) -> set[str]:
    exports: set[str] = set()
    try:
        lines = system_map.read_text(errors="replace").splitlines()
    except OSError as exc:
        raise VerificationError(f"Cannot read built System.map {system_map}: {exc}") from exc
    for line in lines:
        fields = line.split()
        if len(fields) >= 3 and fields[2].startswith(KSYM_PREFIX):
            exports.add(fields[2][len(KSYM_PREFIX):])
    if not exports:
        raise VerificationError(f"No exported kernel symbols found in {system_map}")
    return exports


def verify(system_map: pathlib.Path, built_dir: pathlib.Path, vendor_dir: pathlib.Path,
           nm: str, modinfo: str, *, vendor_only: bool = False) -> dict[str, object]:
    built = module_files(built_dir, allow_empty=True)
    vendor = module_files(vendor_dir)
    duplicates = sorted(built.keys() & vendor.keys())
    selected: dict[str, tuple[pathlib.Path, str]] = {
        name: (path, "vendor") for name, path in vendor.items()
    }
    if not vendor_only:
        selected.update({name: (path, "built") for name, path in built.items() if name not in vendor})

    kernel_exports = parse_kernel_exports(system_map)
    module_exports: dict[str, set[str]] = {}
    module_imports: dict[str, tuple[set[str], set[str]]] = {}
    module_vermagic: dict[str, str] = {}
    module_records: dict[str, dict[str, object]] = {}
    observed_built_modules: dict[str, dict[str, str]] = {}

    for name, (path, source) in sorted(selected.items()):
        exports_out = run_tool([nm, "--defined-only", str(path)], f"nm exports for {path}")
        undefined_out = run_tool([nm, "-u", str(path)], f"nm imports for {path}")
        exports = parse_nm_exports(exports_out)
        strong, weak = parse_nm_undefined(undefined_out)
        module_exports[name] = exports
        module_imports[name] = (strong, weak)
        value = run_tool([modinfo, "-F", "vermagic", str(path)], f"modinfo vermagic for {path}").strip()
        if not value:
            raise VerificationError(f"{path} has empty vermagic")
        module_vermagic[name] = value
        module_records[name] = {
            "path": str(path),
            "source": source,
            "sha256": file_sha256(path),
            "vermagic": value,
            "exports": sorted(exports),
            "undefined_strong": sorted(strong),
            "undefined_weak": sorted(weak),
        }

    for name, path in sorted(built.items()):
        if name in selected and selected[name][0] == path:
            continue
        value = run_tool([modinfo, "-F", "vermagic", str(path)], f"modinfo vermagic for {path}").strip()
        if not value:
            raise VerificationError(f"{path} has empty vermagic")
        module_vermagic[f"built:{name}"] = value
        observed_built_modules[name] = {
            "path": str(path),
            "sha256": file_sha256(path),
            "vermagic": value,
        }

    vermagic_values = sorted(set(module_vermagic.values()))
    vermagic_errors: list[str] = []
    if len(vermagic_values) != 1:
        vermagic_errors.append("Selected modules do not share one full vermagic string")
    for name, value in module_vermagic.items():
        fields = value.split()
        tokens = set(fields)
        release = fields[0] if fields else "<empty>"
        if release != EXPECTED_RELEASE:
            vermagic_errors.append(
                f"{name}.ko vermagic release is {release!r}; expected {EXPECTED_RELEASE!r}"
            )
        missing = sorted(REQUIRED_VERMAGIC - tokens)
        if missing:
            vermagic_errors.append(f"{name}.ko vermagic lacks required tokens: {', '.join(missing)}")
        if "modversions" in tokens:
            vermagic_errors.append(f"{name}.ko vermagic enables modversions; expected MODVERSIONS off")

    providers = kernel_exports | set().union(*module_exports.values())
    missing_by_module = {
        name: sorted(strong - providers)
        for name, (strong, _weak) in module_imports.items()
        if strong - providers
    }

    return {
        "schema_version": 1,
        "valid": not vermagic_errors and not missing_by_module,
        "system_map": str(system_map),
        "system_map_sha256": file_sha256(system_map),
        "kernel_export_count": len(kernel_exports),
        "kernel_exports": sorted(kernel_exports),
        "selected_module_count": len(selected),
        "selected_modules": module_records,
        "vendor_only": vendor_only,
        "observed_built_modules_not_selected": observed_built_modules,
        "built_duplicates_skipped_in_favor_of_vendor": duplicates,
        "vermagic_values": vermagic_values,
        "vermagic_errors": vermagic_errors,
        "missing_imports_by_module": missing_by_module,
        "limitations": [
            "Static symbol and vermagic checks cannot prove struct-layout compatibility, module load order, or runtime behavior."
        ],
    }


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--system-map", type=pathlib.Path, required=True)
    parser.add_argument("--built-modules", type=pathlib.Path, required=True)
    parser.add_argument("--vendor-modules", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--nm", default=shutil.which("nm"), help="host nm supporting target MIPS objects")
    parser.add_argument("--modinfo", default=shutil.which("modinfo"), help="host modinfo executable")
    parser.add_argument("--vendor-only", action="store_true",
                        help="verify vendor modules only; built modules provide no symbols")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if not args.nm:
            raise VerificationError("Host nm is unavailable; pass --nm with a MIPS-capable nm")
        if not args.modinfo:
            raise VerificationError("Host modinfo is unavailable")
        report = verify(args.system_map, args.built_modules, args.vendor_modules, args.nm, args.modinfo,
                        vendor_only=args.vendor_only)
    except VerificationError as exc:
        print(f"verify_module_abi.py: {exc}", file=sys.stderr)
        return 2

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(f"ABI report: {args.output}")
    if not report["valid"]:
        print("Module ABI verification failed; see report for vermagic and missing imports", file=sys.stderr)
        return 1
    print(f"Verified {report['selected_module_count']} selected modules against kernel and module exports")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
