#!/usr/bin/env python3
"""Prepare a pinned, host-only R3 kernel compatibility candidate.

This first port deliberately builds only xImage/System.map/config. It keeps the
selected R3 OEM module set for static ABI checks and does not build replacement
modules, package an update, or communicate with a device.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import prepare_r1_kernel as common  # noqa: E402
import repack_kernel_upt  # noqa: E402
import verify_module_abi  # noqa: E402

BOARD_NAMES = ("r3proii", "r3ii_2025")
REGISTRY_PATH = REPO / "firmware/kernel/r3-boards.json"
# Filled with the repository-owned immutable registry digest.
REGISTRY_SHA256 = "3ecae49bd405fbe91386b3ed68c83cc45a338effa8cb3f6b6e775aae2c952297"
SDK_SHA256 = "3e8c101b7c12667dcfed888e2eda63cfa833c6e20f22569e2b646c40f8523641"
EXPECTED_VERMAGIC = "4.4.94+ preempt mod_unload MIPS32_R2 32BIT"
R3II_KALLSYMS_LAYOUT = {
    "count_offset": 0x62A0E0, "names_offset": 0x62A0F0,
    "markers_offset": 0x6AA190, "token_table_offset": 0x6AA450,
    "token_index_offset": 0x6AA7E0,
}
EXPECTED_SCREEN = {
    "r3proii": {"width": 480, "height": 720, "framebuffer_format": 1, "lcd_mode": 0,
                 "refresh_hz": 62, "panel_descriptor_bytes": 164, "rotation_callback_at_160": False},
    "r3ii_2025": {"width": 320, "height": 480, "framebuffer_format": 1, "lcd_mode": 4,
                   "refresh_hz": 45, "panel_descriptor_bytes": 164, "rotation_callback_at_160": True},
}
LOCAL_PATCHES = {"compas-dma-signed-gaps.patch", "compas-usb-dac-safety.patch"}


class R3PreparationError(Exception):
    """An R3 input or host build artifact failed its pin or validation."""


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def validation_code_pins() -> list[dict[str, str]]:
    """Bind a preparation record to the exact code used for its validation."""
    paths = [HERE / "prepare_r3_kernel.py", HERE / "prepare_r1_kernel.py",
             HERE / "verify_module_abi.py", HERE / "repack_kernel_upt.py"]
    return [{"path": path.relative_to(REPO).as_posix(), "sha256": sha256(path)}
            for path in paths]


def decode_kallsyms(data: bytes, layout: dict[str, int]) -> list[str]:
    """Decode Linux 4.4 MIPS kallsyms compressed names at a pinned layout."""
    count_offset = layout["count_offset"]
    names_offset = layout["names_offset"]
    markers_offset = layout["markers_offset"]
    tokens_offset = layout["token_table_offset"]
    index_offset = layout["token_index_offset"]
    if not (0 <= count_offset <= names_offset < markers_offset <= tokens_offset < index_offset):
        raise R3PreparationError("Invalid pinned stock kallsyms offsets")
    if index_offset + 512 > len(data) or count_offset + 4 > len(data):
        raise R3PreparationError("Stock kallsyms layout extends beyond vmlinux input")
    count = int.from_bytes(data[count_offset:count_offset + 4], "little")
    if count < 1 or count > 200000:
        raise R3PreparationError("Stock kallsyms symbol count is outside the supported range")
    token_bytes = data[tokens_offset:index_offset]
    token_offsets = [int.from_bytes(data[index_offset + i * 2:index_offset + i * 2 + 2], "little")
                     for i in range(256)]
    if token_offsets[0] != 0 or any(a >= b for a, b in zip(token_offsets, token_offsets[1:])):
        raise R3PreparationError("Stock kallsyms token offsets are not the audited monotonic table")
    decoded_tokens: list[str] = []
    for offset in token_offsets:
        if offset >= len(token_bytes):
            raise R3PreparationError("Stock kallsyms token index escapes its pinned token table")
        end = token_bytes.find(b"\0", offset)
        if end < 0:
            raise R3PreparationError("Stock kallsyms token is not NUL terminated")
        decoded_tokens.append(token_bytes[offset:end].decode("latin-1"))
    cursor = names_offset
    symbols: list[str] = []
    for _ in range(count):
        if cursor >= markers_offset:
            raise R3PreparationError("Stock kallsyms names end before the pinned marker table")
        number = len(symbols)
        if number % 256 == 0:
            marker_offset = markers_offset + (number // 256) * 4
            if marker_offset + 4 > tokens_offset:
                raise R3PreparationError("Stock kallsyms marker table is truncated")
            marker = int.from_bytes(data[marker_offset:marker_offset + 4], "little")
            if marker != cursor - names_offset:
                raise R3PreparationError("Stock kallsyms marker does not match compressed name stream")
        length = data[cursor]
        cursor += 1
        if length == 0 or length >= 128 or cursor + length > markers_offset:
            raise R3PreparationError("Stock kallsyms contains an invalid compressed name length")
        expanded = "".join(decoded_tokens[token] for token in data[cursor:cursor + length])
        cursor += length
        if len(expanded) < 2 or not expanded[0].isalpha():
            raise R3PreparationError("Stock kallsyms contains a truncated typed symbol")
        symbols.append(expanded)  # retain type for the evidence hash
    aligned_cursor = (cursor + 15) & ~15
    marker_count = (count + 255) // 256
    marker_end = markers_offset + marker_count * 4
    if aligned_cursor != markers_offset or data[cursor:markers_offset] != bytes(markers_offset - cursor):
        raise R3PreparationError("Stock kallsyms name table alignment/padding differs from audited image")
    if ((marker_end + 15) & ~15) != tokens_offset or data[marker_end:tokens_offset] != bytes(tokens_offset - marker_end):
        raise R3PreparationError("Stock kallsyms marker table alignment/padding differs from audited image")
    return symbols


def r3ii_symbol_evidence(vmlinux: pathlib.Path, record: dict[str, object]) -> dict[str, object]:
    typed_symbols = decode_kallsyms(vmlinux.read_bytes(), R3II_KALLSYMS_LAYOUT)
    symbols = [symbol[1:] for symbol in typed_symbols]
    names = set(symbols)
    typed_names_sha = hashlib.sha256(("\n".join(typed_symbols) + "\n").encode("ascii")).hexdigest()
    pinned = record.get("stock_symbol_evidence")
    if not isinstance(pinned, dict):
        raise R3PreparationError("R3II registry lacks pinned stock kallsyms evidence")
    evidence = {
        "symbol_count": len(typed_symbols), "layout": dict(R3II_KALLSYMS_LAYOUT),
        "typed_names_sha256": typed_names_sha,
        "gpio_pca953x": {"present": "pca953x_probe" in names and "pca953x_irq_handler" in names,
                          "symbols": sorted(name for name in names if name.startswith("pca953x_"))},
        "debugfs": {"present": all(name in names for name in (
            "debugfs_create_dir", "debugfs_create_file", "debugfs_remove_recursive"))},
        "slab": {"present": all(name in names for name in (
            "kmalloc_slab", "kmalloc_caches", "slabinfo_write", "create_kmalloc_caches"))},
        "typec_controller": {"present": any(name.startswith((
            "typec_", "tcpm_register", "tcpm_unregister", "fusb302")) for name in names)},
        "compaction": {"present": any("compact" in name.lower() for name in names)},
        "cma_dma": {"present": any(name.startswith(("cma_", "dma_contiguous")) for name in names)},
        "source_sha256": sha256(vmlinux),
        "decoder_sha256": pinned.get("decoder_sha256"),
        "architecture_review_sha256": pinned.get("architecture_review_sha256"),
        "evidence_json_sha256": pinned.get("evidence_json_sha256"),
    }
    if (len(typed_symbols) != pinned.get("symbol_count") or typed_names_sha != pinned.get("typed_names_sha256") or
            pinned.get("layout") != R3II_KALLSYMS_LAYOUT or pinned.get("all_markers_verified") is not True or
            not isinstance(pinned.get("decoder_sha256"), str) or
            not isinstance(pinned.get("architecture_review_sha256"), str) or
            not all(evidence[key]["present"] for key in ("gpio_pca953x", "debugfs", "slab"))):
        raise R3PreparationError("R3II stock kallsyms differs from the audited full symbol table")
    if any(evidence[key]["present"] for key in ("typec_controller", "compaction", "cma_dma")):
        raise R3PreparationError("R3II stock kallsyms contradicts the pinned Type-C/allocator config evidence")
    return evidence


def regular_file(path: pathlib.Path, label: str) -> None:
    if path.is_symlink() or not path.is_file():
        raise R3PreparationError(f"{label} must be a regular non-symlink file: {path}")


def load_registry(path: pathlib.Path | None = None) -> dict[str, object]:
    path = path or REGISTRY_PATH
    regular_file(path, "R3 board registry")
    expected = REGISTRY_SHA256
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise R3PreparationError("R3 board registry SHA-256 pin is not configured")
    if sha256(path) != expected:
        raise R3PreparationError("R3 board registry hash differs from its source pin")
    try:
        registry = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise R3PreparationError(f"Cannot parse R3 board registry: {exc}") from exc
    if (not isinstance(registry, dict) or registry.get("schema_version") != 1 or
            not isinstance(registry.get("boards"), dict) or
            set(registry["boards"]) != set(BOARD_NAMES)):
        raise R3PreparationError("R3 board registry must pin exactly r3proii and r3ii_2025")
    if any(not isinstance(row, dict) for row in registry["boards"].values()):
        raise R3PreparationError("R3 board registry contains a malformed board record")
    return registry


def selected_board(registry: dict[str, object], board: str) -> dict[str, object]:
    if board not in BOARD_NAMES:
        raise R3PreparationError(f"Unsupported R3 board {board!r}; choose one of {', '.join(BOARD_NAMES)}")
    boards = registry["boards"]
    record = boards.get(board) if isinstance(boards, dict) else None
    if not isinstance(record, dict):
        raise R3PreparationError(f"R3 registry lacks board {board}")
    if record.get("expected_vermagic") != EXPECTED_VERMAGIC:
        raise R3PreparationError(f"{board} registry has an unexpected OEM vermagic pin")
    for field in ("stock_upt_sha256", "stock_kernel_sha256", "stock_dtb_sha256", "stock_vmlinux_sha256"):
        if not isinstance(record.get(field), str) or not re.fullmatch(r"[0-9a-f]{64}", record[field]):
            raise R3PreparationError(f"{board} registry has a malformed {field}")
    if not isinstance(record.get("source_archive_filename"), str):
        raise R3PreparationError(f"{board} registry lacks its source archive filename")
    if record.get("screen") != EXPECTED_SCREEN[board]:
        raise R3PreparationError(f"{board} screen geometry/format does not match its board-specific audit")
    return record


def validate_stock_inputs(board: str, record: dict[str, object], stock_upt: pathlib.Path,
                          stock_kernel: pathlib.Path, stock_modules: pathlib.Path,
                          stock_vmlinux: pathlib.Path,
                          registry: dict[str, object]) -> dict[str, object]:
    """Validate full archive, stock kernels, DTB, and exact OEM module inventory."""
    for path, label in ((stock_upt, "stock R3 UPT"), (stock_kernel, "stock R3 xImage")):
        regular_file(path, label)
    regular_file(stock_vmlinux, "stock R3 vmlinux symbol image")
    if stock_upt.name != record.get("source_archive_filename"):
        raise R3PreparationError(f"stock UPT filename does not match the pinned {board} source archive")
    if sha256(stock_upt) != record.get("stock_upt_sha256"):
        raise R3PreparationError(f"stock UPT does not match the pinned {board} archive")
    if sha256(stock_vmlinux) != record.get("stock_vmlinux_sha256"):
        raise R3PreparationError(f"stock vmlinux does not match the pinned {board} symbol image")
    if sha256(stock_kernel) != record.get("stock_kernel_sha256"):
        for other, other_record in registry["boards"].items():
            if other != board and sha256(stock_kernel) == other_record.get("stock_kernel_sha256"):
                raise R3PreparationError(f"stock xImage belongs to {other}, not {board}")
        raise R3PreparationError(f"stock xImage does not match the pinned {board} kernel")
    dtb = common.embedded_dtb(stock_kernel)
    if dtb is None or hashlib.sha256(dtb).hexdigest() != record.get("stock_dtb_sha256"):
        for other, other_record in registry["boards"].items():
            if other != board and dtb is not None and hashlib.sha256(dtb).hexdigest() == other_record.get("stock_dtb_sha256"):
                raise R3PreparationError(f"embedded stock DTB belongs to {other}, not {board}")
        raise R3PreparationError(f"stock xImage DTB does not match the pinned {board} DTB")
    if stock_modules.is_symlink() or not stock_modules.is_dir():
        raise R3PreparationError("OEM module directory must be a regular non-symlink directory")
    expected = record.get("stock_modules")
    if not isinstance(expected, dict) or not expected or any(
            not isinstance(name, str) or not name.endswith(".ko") or
            not re.fullmatch(r"[A-Za-z0-9_.+-]+\.ko", name) or
            not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest)
            for name, digest in expected.items()):
        raise R3PreparationError(f"{board} OEM module pin is malformed")
    for path in stock_modules.rglob("*"):
        if path.is_symlink():
            raise R3PreparationError(f"OEM module input contains a symlink: {path}")
    module_paths = [path for path in stock_modules.rglob("*.ko") if path.is_file()]
    nested_modules = [path for path in module_paths if path.parent != stock_modules]
    actual = {path.name for path in module_paths if path.parent == stock_modules}
    if nested_modules or actual != set(expected):
        raise R3PreparationError(
            f"OEM module inventory differs for {board}: missing={sorted(set(expected)-actual)}, "
            f"extra={sorted(actual-set(expected))}, nested={sorted(p.relative_to(stock_modules).as_posix() for p in nested_modules)}")
    for name, digest in expected.items():
        path = stock_modules / name
        regular_file(path, f"OEM module {name}")
        if sha256(path) != digest:
            raise R3PreparationError(f"OEM module hash differs from the {board} pin: {name}")
    result = {"stock_upt_sha256": sha256(stock_upt),
            "stock_kernel_sha256": sha256(stock_kernel),
            "stock_vmlinux_sha256": sha256(stock_vmlinux),
            "stock_dtb_sha256": hashlib.sha256(dtb).hexdigest(),
            "stock_dtb_size": len(dtb),
            "stock_module_count": len(expected),
            "stock_modules": dict(sorted(expected.items()))}
    if board == "r3ii_2025":
        result["stock_symbol_evidence"] = r3ii_symbol_evidence(stock_vmlinux, record)
    return result


def validate_upstream(upstream: pathlib.Path, pin: dict[str, str], board: str) -> str:
    if upstream.is_symlink() or not upstream.is_dir():
        raise R3PreparationError(f"Pinned upstream must be a regular directory: {upstream}")
    try:
        commit = subprocess.run(["git", "-C", str(upstream), "rev-parse", "HEAD"],
                                check=True, capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(upstream), "status", "--porcelain", "--untracked-files=all"],
                               check=True, capture_output=True, text=True).stdout
        origin = subprocess.run(["git", "-C", str(upstream), "remote", "get-url", "origin"],
                                check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise R3PreparationError(f"Cannot verify pinned upstream checkout: {exc}") from exc
    if commit != pin["commit"]:
        raise R3PreparationError(f"Upstream HEAD must be pinned commit {pin['commit']}, got {commit}")
    normalize = lambda value: value.removesuffix(".git").rstrip("/")
    if normalize(origin) != normalize(pin["source_repo"]):
        raise R3PreparationError(f"Upstream origin must be {pin['source_repo']}, got {origin}")
    if dirty:
        raise R3PreparationError("Upstream checkout must be clean (tracked and untracked files)")
    required = ["build.sh", "tools/extract-stock.py"]
    if board == "r3proii":
        required.append("boards/r3proii")
    for path in required:
        if not (upstream / path).exists():
            raise R3PreparationError(f"Pinned upstream is missing R3 input {path}")
    return commit


def config_source(repo: pathlib.Path, record: dict[str, object], key: str) -> tuple[pathlib.Path, str]:
    rel = record.get(key)
    digest = record.get(f"{key}_sha256")
    if not isinstance(rel, str) or not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise R3PreparationError(f"R3 registry lacks a hash-pinned {key}")
    pure = pathlib.PurePosixPath(rel)
    if pure.is_absolute() or ".." in pure.parts:
        raise R3PreparationError(f"Unsafe R3 config path in registry: {rel}")
    path = repo.joinpath(*pure.parts)
    regular_file(path, f"R3 {key}")
    if sha256(path) != digest:
        raise R3PreparationError(f"R3 config changed from registry pin: {rel}")
    return path, digest


def kernel_only_build_script(path: pathlib.Path) -> tuple[str, str]:
    """Truncate a copied upstream build script immediately before module builds."""
    original = path.read_text()
    lines = original.splitlines(keepends=True)
    markers = [index for index, line in enumerate(lines)
               if line.lstrip().startswith("# Modules:")]
    if len(markers) != 1:
        raise R3PreparationError("Pinned build.sh has no unique kernel/modules boundary")
    marker = markers[0]
    modified = "".join(lines[:marker]) + (
        'cp include/config/kernel.release "$OUT/kernel.release-$NAME"\n'
        'echo "KERNEL_ONLY_BUILD_OK: module build intentionally omitted"\n')
    path.write_text(modified)
    return hashlib.sha256(original.encode()).hexdigest(), sha256(path)


def build_command(runtime: str, workspace: pathlib.Path, sdk: pathlib.Path | None,
                  sdk_sha: str | None, board: str, profile: str,
                  patches: list[str], jobs: int) -> list[str]:
    if profile != "parity":
        raise R3PreparationError("Only the initial R3 parity plus Compas compatibility profile is available")
    command = common.container_command(runtime, workspace, sdk, sdk_sha, "parity", jobs, patches)
    for index, token in enumerate(command[:-1]):
        if token == "--model":
            command[index + 1] = board
        elif token == "--name":
            command[index + 1] = f"compas-{board}"
    # Replace the two R1-only fragments generated by the shared command helper.
    filtered: list[str] = []
    skip = False
    for index, token in enumerate(command):
        if skip:
            skip = False
            continue
        if token == "--fragment":
            skip = True
            continue
        filtered.append(token)
    filtered.extend(["--fragment", f"{board}-parity.config"])
    filtered.extend(["--fragment", f"compas-{board}.config"])
    return filtered


def validate_kernel_build(kit: pathlib.Path, board: str, profile: str,
                          stock_kernel: pathlib.Path, stock_dtb: bytes,
                          record: dict[str, object], stock_modules: pathlib.Path,
                          stock_manifest: dict[str, object]) -> tuple[dict[str, object], dict]:
    out = kit / "out"
    name = f"compas-{board}"
    image, config, system_map, release = (
        out / f"xImage-{name}", out / f"config-{name}",
        out / f"System.map-{name}", out / f"kernel.release-{name}")
    if (out / f"modules-{name}").exists():
        raise R3PreparationError("R3 initial port must not build or retain replacement kernel modules")
    for path, label in ((image, "kernel xImage"), (config, "compiled kernel config"),
                        (system_map, "kernel System.map"), (release, "compiled kernel release")):
        regular_file(path, f"built {label}")
    if release.read_text().strip() != "4.4.94+":
        raise R3PreparationError("Built R3 kernel release must be exactly 4.4.94+ for pinned OEM modules")
    if image.stat().st_size == 0 or image.stat().st_size > stock_kernel.stat().st_size:
        raise R3PreparationError("Built R3 xImage is empty or larger than the pinned stock xImage")
    try:
        repack_kernel_upt.validate_uimage(image)
    except Exception as exc:
        raise R3PreparationError(f"Built R3 xImage failed MIPS uImage/CRC validation: {exc}") from exc
    built_dtb = common.embedded_dtb(image)
    if built_dtb != stock_dtb:
        raise R3PreparationError("Built R3 xImage does not embed the exact selected board stock DTB")
    if hashlib.sha256(built_dtb).hexdigest() != record["stock_dtb_sha256"]:
        raise R3PreparationError("Built R3 xImage DTB hash differs from the selected board pin")

    config_targets = [kit / f"configs/{board}-required.config", kit / f"configs/{board}-parity.config"]
    config_targets.append(kit / f"configs/compas-{board}.config")
    requested = common.config_requests(*config_targets)
    actual = common.parse_kernel_config(config)
    differences = [f"{symbol}: requested {value}, built {actual.get(symbol, 'n')}"
                   for symbol, value in requested.items() if actual.get(symbol, "n") != value]
    if differences:
        raise R3PreparationError("R3 compiled config lost requested options: " + "; ".join(differences))
    if actual.get("CONFIG_NLS_CODEPAGE_936") != "y":
        raise R3PreparationError("R3 compiled config must retain CONFIG_NLS_CODEPAGE_936=y")
    if actual.get("CONFIG_MODVERSIONS", "n") != "n":
        raise R3PreparationError("R3 kernel candidate must disable CONFIG_MODVERSIONS for pinned OEM modules")
    if actual.get("CONFIG_SMP", "n") != "n":
        raise R3PreparationError("R3 OEM modules require a non-SMP kernel candidate")
    for symbol in ("CONFIG_PREEMPT", "CONFIG_MODULE_UNLOAD", "CONFIG_CPU_MIPS32_R2", "CONFIG_32BIT"):
        if actual.get(symbol) != "y":
            raise R3PreparationError(f"R3 compiled config lacks required OEM ABI setting: {symbol}=y")

    with tempfile.TemporaryDirectory(prefix="r3-empty-built-modules-") as empty_name:
        report = verify_module_abi.verify(system_map, pathlib.Path(empty_name), stock_modules,
                                          shutil.which("nm") or "nm", shutil.which("modinfo") or "modinfo",
                                          vendor_only=True)
    if not report.get("valid") or report.get("selected_module_count") != stock_manifest["stock_module_count"]:
        raise R3PreparationError("Selected R3 OEM module import/export/vermagic ABI check failed")
    if report.get("missing_imports_by_module") or report.get("vermagic_errors"):
        raise R3PreparationError("Selected R3 OEM modules have unresolved imports or vermagic errors")
    result = {
        "ximage_path": str(image.resolve()), "ximage_sha256": sha256(image),
        "ximage_size": image.stat().st_size, "config_path": str(config.resolve()),
        "compiled_config_sha256": sha256(config), "kernel_release_path": str(release.resolve()),
        "kernel_release": release.read_text().strip(), "kernel_release_sha256": sha256(release),
        "system_map_path": str(system_map.resolve()),
        "system_map_sha256": sha256(system_map), "stock_dtb_sha256": hashlib.sha256(built_dtb).hexdigest(),
        "kernel_export_count": report["kernel_export_count"],
        "required_module_count": stock_manifest["stock_module_count"],
        "selected_module_count": report["selected_module_count"],
        "module_abi_report": report,
        "compiled_config_options": {name: actual.get(name, "n") for name in (
            "CONFIG_NLS_CODEPAGE_936", "CONFIG_PREEMPT", "CONFIG_MODULE_UNLOAD",
            "CONFIG_MODVERSIONS", "CONFIG_SMP", "CONFIG_CPU_MIPS32_R2", "CONFIG_32BIT")},
    }
    return result, report


def prepare(args: argparse.Namespace) -> pathlib.Path:
    registry = load_registry()
    record = selected_board(registry, args.board)
    # Keep the final path component intact so regular_file()/module-directory
    # checks can reject symlink inputs instead of silently following them.
    stock_upt = args.stock_upt.absolute()
    stock_kernel = args.stock_kernel.absolute()
    stock_modules = args.stock_modules.absolute()
    stock_vmlinux = args.stock_vmlinux.absolute()
    stock_manifest = validate_stock_inputs(args.board, record, stock_upt, stock_kernel,
                                           stock_modules, stock_vmlinux, registry)

    pin = common.load_pin()
    upstream = args.upstream.absolute()
    commit = validate_upstream(upstream, pin, args.board)
    repo = REPO.resolve()
    required_path, required_sha = config_source(repo, record, "required_config")
    parity_path, parity_sha = config_source(repo, record, "parity_config")
    profile_path, profile_sha = config_source(repo, record, "profile_config")
    patches = common.local_patches(upstream)
    patch_names = [path.name for path in patches]
    if set(patch_names) != LOCAL_PATCHES or len(patch_names) != len(LOCAL_PATCHES):
        raise R3PreparationError("R3 preparation accepts only the reviewed DMA and USB audio kernel patches")
    for path in (stock_upt, stock_kernel, stock_modules, stock_vmlinux, upstream):
        resolved = path.resolve()
        if args.workspace.resolve() == resolved or resolved in args.workspace.resolve().parents:
            raise R3PreparationError("R3 workspace must be outside all pinned input trees")
    workspace = args.workspace.resolve()
    if workspace == repo or repo in workspace.parents:
        raise R3PreparationError("R3 workspace must be outside the Compás source checkout")
    if workspace == upstream or upstream in workspace.parents:
        raise R3PreparationError("R3 workspace must be outside the pinned upstream checkout")
    if workspace.exists() or args.workspace.is_symlink():
        raise R3PreparationError(f"R3 workspace must be a fresh, non-symlink path: {workspace}")

    runtime = common.select_container_runtime(args.container_runtime)
    sdk = common.validate_sdk(args.sdk.resolve() if args.sdk else None, args.build, runtime)
    sdk_sha = sha256(sdk) if sdk else None
    if sdk and sdk_sha != SDK_SHA256:
        raise R3PreparationError("SDK bytes differ from pinned X1600 SDK SHA-256")
    command = build_command(runtime, workspace, sdk, sdk_sha, args.board, args.profile,
                            patch_names, args.jobs)
    workspace.mkdir(parents=True)
    kit = workspace / "hiby-custom-kernel"
    try:
        common.copy_tracked_checkout(upstream, kit)
        board_dir = kit / "boards" / args.board
        board_dir.mkdir(parents=True, exist_ok=True)
        embedded = common.embedded_dtb(stock_kernel)
        if embedded is None:
            raise R3PreparationError("Pinned R3 xImage has no embedded stock DTB")
        (board_dir / "stock.dtb").write_bytes(embedded)
        config_root = kit / "configs"
        config_root.mkdir(parents=True, exist_ok=True)
        config_records = []
        for source, dest, label, digest in (
                (required_path, f"{args.board}-required.config", "required_config", required_sha),
                (parity_path, f"{args.board}-parity.config", "parity_config", parity_sha),
                (profile_path, f"compas-{args.board}.config", "profile_config", profile_sha)):
            shutil.copy2(source, config_root / dest)
            config_records.append({"name": label, "source_path": str(source), "source_sha256": digest,
                                  "copied_to": f"hiby-custom-kernel/configs/{dest}",
                                  "copied_sha256": sha256(config_root / dest)})
        patch_dir = kit / "patches"
        patch_dir.mkdir(parents=True, exist_ok=True)
        patch_records = []
        for patch in patches:
            copied = patch_dir / patch.name
            shutil.copy2(patch, copied)
            patch_records.append({"original_path": patch.relative_to(repo).as_posix(),
                                  "sha256": sha256(patch),
                                  "copied_to": f"hiby-custom-kernel/patches/{patch.name}",
                                  "copied_sha256": sha256(copied)})
        original_script_sha, kernel_only_script_sha = kernel_only_build_script(kit / "build.sh")
        manifest = {
            "schema_version": 1, "status": "host-only R3 kernel compatibility candidate; OEM modules retained",
            "board": args.board, "profile": args.profile,
            "upstream": pin, "upstream_head": commit,
            "stock_inputs": {**stock_manifest, "stock_upt_path": str(stock_upt),
                             "stock_kernel_path": str(stock_kernel), "stock_modules_path": str(stock_modules),
                             "stock_vmlinux_path": str(stock_vmlinux),
                             "stock_version": record.get("stock_version"),
                             "source_archive_filename": record.get("source_archive_filename"),
                             "source_class": record.get("source_class"),
                             "screen": record["screen"]},
            "registry_path": str(REGISTRY_PATH.resolve()), "registry_sha256": REGISTRY_SHA256,
            "validation_code": validation_code_pins(),
            "configs": config_records,
            "local_kernel_patches": patch_records,
            "build_script": {"upstream_sha256": original_script_sha,
                             "kernel_only_sha256": kernel_only_script_sha,
                             "modules_section_removed_before": "# Modules:"},
            "sdk": ({"path": str(sdk), "sha256": sdk_sha, "size": sdk.stat().st_size} if sdk else None),
            "build_requested": bool(args.build), "build_command": command,
            "build_command_shell": common.shlex.join(command), "jobs": args.jobs,
            "module_policy": "all exact selected-board OEM modules retained; no replacement modules built",
        }
        (board_dir / "stock.dtb").chmod(0o644)
        manifest["stock_inputs"]["prepared_dtb_path"] = "hiby-custom-kernel/boards/" + args.board + "/stock.dtb"
        manifest["stock_inputs"]["prepared_dtb_sha256"] = sha256(board_dir / "stock.dtb")
        (workspace / "preparation.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    except Exception:
        if not args.build:
            shutil.rmtree(workspace, ignore_errors=True)
        raise
    if args.build:
        try:
            subprocess.run(command, cwd=workspace, check=True)
        except (OSError, subprocess.CalledProcessError) as exc:
            raise R3PreparationError(f"R3 kernel-only container build failed; workspace preserved: {exc}") from exc
        result, report = validate_kernel_build(kit, args.board, args.profile, stock_kernel,
                                               embedded, record, stock_modules, stock_manifest)
        manifest = json.loads((workspace / "preparation.json").read_text())
        manifest["build_validation"] = result
        manifest["module_abi_report"] = {"selected_module_count": report["selected_module_count"],
                                         "system_map_sha256": result["system_map_sha256"],
                                         "valid": report["valid"]}
        manifest["build_validation"]["build_script_sha256"] = kernel_only_script_sha
        manifest["build_validation"]["stock_dtb_roundtrip_sha256"] = result["stock_dtb_sha256"]
        (workspace / "preparation.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        report_path = kit / "out" / f"abi-oem-{args.board}.json"
        report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    return workspace


def positive_int(value: str) -> int:
    try:
        number = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a positive integer") from exc
    if number < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return number


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", choices=BOARD_NAMES, required=True)
    parser.add_argument("--upstream", type=pathlib.Path, required=True)
    parser.add_argument("--stock-upt", type=pathlib.Path, required=True)
    parser.add_argument("--stock-kernel", type=pathlib.Path, required=True)
    parser.add_argument("--stock-vmlinux", type=pathlib.Path, required=True)
    parser.add_argument("--stock-modules", type=pathlib.Path, required=True)
    parser.add_argument("--workspace", type=pathlib.Path, required=True)
    parser.add_argument("--profile", choices=("parity",), default="parity",
                        help="initial R3 profile combines the board parity and Compas compatibility fragments")
    parser.add_argument("--sdk", type=pathlib.Path)
    parser.add_argument("--container-runtime", choices=("auto", "docker", "podman"), default="auto")
    parser.add_argument("--jobs", type=positive_int, default=2)
    parser.add_argument("--build", action="store_true", help="build only the kernel image, System.map and config")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    try:
        args = parse_args(argv)
        workspace = prepare(args)
    except (R3PreparationError, common.PreparationError, verify_module_abi.VerificationError,
            repack_kernel_upt.PackError, OSError, KeyError, TypeError, ValueError,
            subprocess.CalledProcessError) as exc:
        print(f"prepare_r3_kernel.py: {exc}", file=sys.stderr)
        return 1
    print(f"Prepared isolated {args.board} kernel-only compatibility workspace: {workspace}")
    if not args.build:
        print("No build was run. preparation.json pins all host inputs and the future kernel-only command.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
