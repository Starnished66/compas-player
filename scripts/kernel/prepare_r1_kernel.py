#!/usr/bin/env python3
"""Prepare an isolated, pinned R1 custom-kernel build workspace.

Preparation extracts the stock device tree and records all input hashes. It
never flashes a device and does not build unless ``--build`` is supplied.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import pathlib
import shlex
import shutil
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
PIN_PATH = REPO_ROOT / "firmware/kernel/upstream.json"
EXPECTED_COMMIT = "e1c5915290197ba01297e3f7c53f37197e0b1f9c"
UPSTREAM_REPO_URL = "https://github.com/Jepl4r/hiby-custom-kernel"
SDK_GLOB_PREFIX = "ingenic-linux-kernel4.4.94-x1600-"
DOCKER_IMAGE = "hiby-custom-kernel"
WIFI_FRAGMENTS = {"brcmfmac": "compas-r1-brcmfmac.config"}
WIFI_MODULE_PATCHES = {
    "brcmfmac": ("compas-radio-input-safety.patch", "compas-radio-lifecycle.patch"),
}
WIFI_KERNEL_PATCHES = {"brcmfmac": "compas-mmc-radio-lifetime.patch"}
DISPLAY_STACKS = ("vendor", "compas")
DISPLAY_SOURCE_DIR = pathlib.Path("firmware/kernel/display-experimental")
DISPLAY_REQUIRED_SYMVERS = {
    "rmem_alloc_aligned": "rmem_manager",
    "rmem_free": "rmem_manager",
    "gpio_port_set_func": "utils",
}
RADIO_GUARD_EXPORTS = (
    "mmc_compas_sdio_bootstrap_off",
    "mmc_compas_sdio_begin",
    "mmc_compas_sdio_identity",
    "mmc_compas_sdio_power",
    "mmc_compas_sdio_set_off",
    "mmc_compas_sdio_end",
)
EXPECTED_VERMAGIC = "4.4.94+ preempt mod_unload MIPS32_R2 32BIT"


class PreparationError(Exception):
    """An input or safety check failed before a usable workspace was made."""


def positive_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a positive integer") from exc
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def local_patches(upstream: pathlib.Path) -> list[pathlib.Path]:
    patch_dir = REPO_ROOT / "firmware/kernel/patches"
    if patch_dir.is_symlink():
        raise PreparationError(f"Local kernel patch path must be a regular directory: {patch_dir}")
    if not patch_dir.exists():
        return []
    if not patch_dir.is_dir():
        raise PreparationError(f"Local kernel patch path must be a regular directory: {patch_dir}")
    try:
        tracked = subprocess.run(
            ["git", "-C", str(upstream), "ls-files", "-z", "--", "patches/*.patch"],
            check=True, capture_output=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise PreparationError(f"Cannot list upstream kernel patches: {exc}") from exc
    upstream_names = {pathlib.PurePosixPath(os.fsdecode(path)).name
                      for path in tracked.split(b"\0") if path}
    patches = sorted(patch_dir.glob("*.patch"), key=lambda path: path.name)
    for patch in patches:
        if patch.is_symlink() or not patch.is_file():
            raise PreparationError(f"Local kernel patch must be a regular non-symlink file: {patch}")
        if patch.name.startswith("0"):
            raise PreparationError(
                f"Local kernel patch filename must not start with 0 because upstream applies those automatically: {patch.name}"
            )
        if patch.name in upstream_names:
            raise PreparationError(f"Local kernel patch collides with an upstream patch filename: {patch.name}")
    return patches


def wifi_kernel_patches(wifi_stack: str = "vendor") -> list[pathlib.Path]:
    if wifi_stack not in ("vendor", *WIFI_KERNEL_PATCHES):
        raise PreparationError(f"Unsupported Wi-Fi stack {wifi_stack!r}; choose vendor or brcmfmac")
    if wifi_stack == "vendor":
        return []
    patch_dir = REPO_ROOT / "firmware/kernel/wifi-patches"
    if patch_dir.is_symlink() or not patch_dir.is_dir():
        raise PreparationError(f"brcmfmac Wi-Fi build requires a regular kernel patch directory: {patch_dir}")
    required = WIFI_KERNEL_PATCHES[wifi_stack]
    patch = patch_dir / required
    if patch.is_symlink() or not patch.is_file():
        raise PreparationError(f"brcmfmac Wi-Fi build requires kernel patch: {required}")
    return [patch]


def module_patches(wifi_stack: str = "vendor") -> list[pathlib.Path]:
    if wifi_stack not in ("vendor", *WIFI_FRAGMENTS):
        raise PreparationError(f"Unsupported Wi-Fi stack {wifi_stack!r}; choose vendor or brcmfmac")
    patch_dir = REPO_ROOT / "firmware/kernel/module-patches"
    if patch_dir.is_symlink():
        raise PreparationError(f"Module source patch path must be a regular directory: {patch_dir}")
    if not patch_dir.exists():
        if wifi_stack == "brcmfmac":
            required = WIFI_MODULE_PATCHES[wifi_stack][0]
            raise PreparationError(f"brcmfmac Wi-Fi build requires module source patch: {required}")
        return []
    if not patch_dir.is_dir():
        raise PreparationError(f"Module source patch path must be a regular directory: {patch_dir}")
    wifi_patch_names = {name for names in WIFI_MODULE_PATCHES.values() for name in names}
    patches = sorted((path for path in patch_dir.glob("*.patch") if path.name not in wifi_patch_names),
                     key=lambda path: path.name)
    if wifi_stack == "brcmfmac":
        required = WIFI_MODULE_PATCHES[wifi_stack]
        available = {path.name: path for path in patch_dir.glob("*.patch")}
        for name in required:
            if name not in available:
                raise PreparationError(f"brcmfmac Wi-Fi build requires module source patch: {name}")
        patches.extend(available[name] for name in required)
    for patch in patches:
        if patch.is_symlink() or not patch.is_file():
            raise PreparationError(f"Module source patch must be a regular non-symlink file: {patch}")
        validate_module_patch_paths(patch)
    return patches


def display_source_files(display_stack: str = "vendor") -> list[pathlib.Path]:
    if display_stack not in DISPLAY_STACKS:
        raise PreparationError(
            f"Unsupported display stack {display_stack!r}; choose vendor or compas"
        )
    if display_stack == "vendor":
        return []
    source_dir = REPO_ROOT / DISPLAY_SOURCE_DIR
    if source_dir.is_symlink() or not source_dir.is_dir():
        raise PreparationError(f"Compas display build requires source directory: {source_dir}")
    files = sorted((path for path in source_dir.rglob("*")
                    if path.is_file() and (path.suffix in (".c", ".h") or
                                           path.name in ("Makefile", "hiby.symvers"))),
                   key=lambda path: path.relative_to(source_dir).as_posix())
    tree_paths = list(source_dir.rglob("*"))
    for path in tree_paths:
        if path.is_symlink():
            raise PreparationError(f"Display source tree must not contain symlinks: {path}")
    unexpected = [path for path in tree_paths if path.is_file() and path not in files]
    if unexpected:
        raise PreparationError(f"Unsupported file in Compas display source tree: {unexpected[0]}")
    makefiles = [path for path in files if path.name == "Makefile"]
    if len(makefiles) != 1 or makefiles[0].parent != source_dir:
        raise PreparationError("Compas display source requires one top-level Makefile")
    symvers = [path for path in files if path.name == "hiby.symvers"]
    if len(symvers) != 1 or symvers[0].parent != source_dir:
        raise PreparationError("Compas display Makefile requires top-level hiby.symvers")
    if not any(path.suffix == ".c" for path in files) or not any(path.suffix == ".h" for path in files):
        raise PreparationError("Compas display source requires C and header files")
    makefile_text = makefiles[0].read_text()
    if not re.search(r"(?m)^\s*obj-m\s*[+:]?=\s*.*\bsoc_fb\.o\b", makefile_text):
        raise PreparationError("Compas display Makefile must build soc_fb.o")
    kbuild_lines = [line for line in makefile_text.splitlines()
                    if re.match(r"\s*(?:(?:export|override|private)\s+)*KBUILD_EXTRA_SYMBOLS\b", line)]
    if (len(kbuild_lines) != 1 or not re.fullmatch(
            r"\s*KBUILD_EXTRA_SYMBOLS\s*\+=\s*\$\(src\)/hiby\.symvers\s*(?:#.*)?",
            kbuild_lines[0])):
        raise PreparationError("Compas display Makefile must load only $(src)/hiby.symvers")
    symvers_entries = {}
    symvers_valid = True
    for line in symvers[0].read_text().splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r"(0x[0-9a-f]{8})\t([A-Za-z_][A-Za-z0-9_]*)\t"
                             r"([A-Za-z_][A-Za-z0-9_]*)\t(EXPORT_SYMBOL)", line)
        if not match:
            symvers_valid = False
            break
        crc, symbol, module, _export_type = match.groups()
        if symbol in symvers_entries:
            symvers_valid = False
            break
        symvers_entries[symbol] = (crc, module)
    expected_entries = {name: ("0x00000000", module)
                        for name, module in DISPLAY_REQUIRED_SYMVERS.items()}
    if not symvers_valid or symvers_entries != expected_entries:
        raise PreparationError(
            "Compas display hiby.symvers must contain only the pinned rmem/GPIO exports"
        )
    for line in makefile_text.splitlines():
        match = re.match(r"\s*soc_fb-(?:y|objs)\s*[+:]?=\s*(.*?)\s*(?:#.*)?$", line)
        if not match:
            continue
        for obj in match.group(1).split():
            if not obj.endswith(".o"):
                continue
            source = source_dir / (obj[:-2] + ".c")
            if not source.is_file() or source.is_symlink():
                raise PreparationError(f"Compas display Makefile references missing source: {source}")
    return files


def copy_display_sources(kit: pathlib.Path, workspace: pathlib.Path,
                         sources: list[pathlib.Path]) -> list[dict[str, str]]:
    if not sources:
        return []
    target_dir = kit / "modules/soc_fb"
    if target_dir.exists() or target_dir.is_symlink():
        raise PreparationError(f"Pinned kit already contains modules/soc_fb: {target_dir}")
    source_dir = REPO_ROOT / DISPLAY_SOURCE_DIR
    records = []
    for source in sources:
        relative = source.relative_to(source_dir)
        target = target_dir / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        records.append({
            "original_path": (DISPLAY_SOURCE_DIR / relative).as_posix(),
            "sha256": sha256(source),
            "copied_to": target.relative_to(workspace).as_posix(),
            "prepared_sha256": sha256(target),
        })
    if any(item["sha256"] != item["prepared_sha256"] for item in records):
        raise PreparationError("Copied Compas display sources differ from checked-in source hashes")
    return records


def validate_prepared_display_sources(kit: pathlib.Path,
                                      records: list[dict[str, str]]) -> None:
    for record in records:
        relative = pathlib.PurePosixPath(record["copied_to"])
        if relative.is_absolute() or ".." in relative.parts:
            raise PreparationError(f"Unsafe prepared display source path: {record['copied_to']}")
        if relative.parts and relative.parts[0] == kit.name:
            relative = pathlib.PurePosixPath(*relative.parts[1:])
        path = kit.joinpath(*relative.parts)
        if path.is_symlink() or not path.is_file() or sha256(path) != record["prepared_sha256"]:
            raise PreparationError(f"Prepared Compas display source changed: {record['copied_to']}")


def validate_module_patch_paths(patch: pathlib.Path) -> None:
    try:
        lines = patch.read_text().splitlines()
    except (OSError, UnicodeError) as exc:
        raise PreparationError(f"Cannot read module source patch {patch}: {exc}") from exc

    def checked_path(header: str, prefix: str) -> str:
        if not header.startswith(prefix):
            raise PreparationError(f"Malformed file path header in module patch {patch}: {header!r}")
        value = header[len(prefix):].split("\t", 1)[0]
        side = "a/" if prefix == "--- " else "b/"
        if not value.startswith(side):
            raise PreparationError(f"Module patch paths must use {side!r} prefixes: {value!r}")
        value = value[len(side):]
        if value.startswith("/") or "\\" in value:
            raise PreparationError(f"Unsafe absolute module patch path: {value!r}")
        path = pathlib.PurePosixPath(value)
        if path.is_absolute() or not path.parts or any(part in ("", ".", "..") for part in value.split("/")):
            raise PreparationError(f"Unsafe module patch path: {value!r}")
        allowed = (path.parts[0] == "modules" and len(path.parts) >= 2) or (
            path.parts[:3] == ("boards", "r1", "modules") and len(path.parts) >= 4
        )
        if not allowed:
            raise PreparationError(f"Module patch path is outside modules or boards/r1/modules: {value!r}")
        return value

    changed_files = 0
    index = 0
    while index < len(lines):
        line = lines[index]
        if line.startswith("--- "):
            if index + 1 >= len(lines) or not lines[index + 1].startswith("+++ "):
                raise PreparationError(f"Unpaired unified-diff headers in module patch {patch}")
            old_path = checked_path(line, "--- ")
            new_path = checked_path(lines[index + 1], "+++ ")
            if old_path != new_path:
                raise PreparationError(f"Module patch renames are not allowed: {old_path!r} to {new_path!r}")
            changed_files += 1
            index += 2
            continue
        if line.startswith("+++ "):
            raise PreparationError(f"Unpaired unified-diff headers in module patch {patch}")
        index += 1
    if not changed_files:
        raise PreparationError(f"Module patch contains no validated file changes: {patch}")


def apply_module_patches(kit: pathlib.Path, workspace: pathlib.Path,
                         patches: list[pathlib.Path]) -> list[dict[str, str]]:
    if not patches:
        return []
    if not shutil.which("patch"):
        raise PreparationError("Applying module source patches requires the host patch utility")
    copied_dir = workspace / "module-patches"
    copied_dir.mkdir()
    manifest: list[dict[str, str]] = []
    for patch in patches:
        copied = copied_dir / patch.name
        shutil.copy2(patch, copied)
        try:
            subprocess.run(
                ["patch", "--batch", "--forward", "--fuzz=0", "-p1", "-i", str(copied)],
                cwd=kit, check=True, capture_output=True, text=True,
            )
        except (OSError, subprocess.CalledProcessError) as exc:
            output = getattr(exc, "stderr", "") or getattr(exc, "stdout", "") or str(exc)
            raise PreparationError(f"Cannot apply module source patch {patch.name}: {output.strip()}") from exc
        manifest.append({
            "original_path": patch.relative_to(REPO_ROOT).as_posix(),
            "sha256": sha256(patch),
            "copied_to": copied.relative_to(workspace).as_posix(),
        })
    return manifest


def load_pin(pin_path: pathlib.Path | None = None) -> dict[str, str]:
    pin_path = pin_path or PIN_PATH
    try:
        pin = json.loads(pin_path.read_text())
        source_repo = pin["source_repo"]
        commit = pin["commit"]
    except (OSError, json.JSONDecodeError, KeyError, TypeError) as exc:
        raise PreparationError(f"Cannot read kernel source pin {pin_path}: {exc}") from exc
    if not isinstance(source_repo, str) or not source_repo.startswith(("https://", "ssh://", "git@")):
        raise PreparationError("Kernel source pin must contain a valid source_repo URL")
    if commit != EXPECTED_COMMIT:
        raise PreparationError(f"Kernel source pin must be {EXPECTED_COMMIT}, got {commit!r}")
    sdk_archive = pin.get("sdk_archive")
    if not isinstance(sdk_archive, str) or not sdk_archive.startswith(SDK_GLOB_PREFIX) or not sdk_archive.endswith(".tar.bz2"):
        raise PreparationError("Kernel source pin must contain the expected sdk_archive filename")
    result = {"source_repo": source_repo, "commit": commit, "sdk_archive": sdk_archive}
    sdk_sha256 = pin.get("sdk_sha256")
    if sdk_sha256 is not None:
        if not isinstance(sdk_sha256, str) or not re.fullmatch(r"[0-9a-f]{64}", sdk_sha256):
            raise PreparationError("Kernel source pin sdk_sha256 must be a lowercase SHA-256 digest")
        result["sdk_sha256"] = sdk_sha256
    return result


def validate_upstream(upstream: pathlib.Path, pin: dict[str, str]) -> str:
    if not upstream.is_dir():
        raise PreparationError(f"Pinned upstream checkout is not a directory: {upstream}")
    try:
        commit = subprocess.run(
            ["git", "-C", str(upstream), "rev-parse", "HEAD"],
            check=True, capture_output=True, text=True,
        ).stdout.strip()
        dirty = subprocess.run(
            ["git", "-C", str(upstream), "status", "--porcelain", "--untracked-files=all"],
            check=True, capture_output=True, text=True,
        ).stdout
        origin = subprocess.run(
            ["git", "-C", str(upstream), "remote", "get-url", "origin"],
            check=True, capture_output=True, text=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise PreparationError(f"Cannot verify upstream git checkout at {upstream}: {exc}") from exc
    if commit != pin["commit"]:
        raise PreparationError(f"Upstream HEAD must be pinned commit {pin['commit']}, got {commit}")
    normalize_url = lambda url: url.removesuffix(".git").rstrip("/")
    if normalize_url(origin) != normalize_url(pin["source_repo"]):
        raise PreparationError(f"Upstream origin must be {pin['source_repo']}, got {origin}")
    if dirty:
        raise PreparationError("Upstream checkout must be clean (tracked and untracked files)")
    for required in ("build.sh", "tools/extract-stock.py", "boards/r1", "configs/r1-required.config",
                     "configs/r1-parity.config"):
        if not (upstream / required).exists():
            raise PreparationError(f"Pinned upstream checkout is missing {required}")
    return commit


def select_container_runtime(requested: str) -> str:
    if requested not in ("auto", "docker", "podman"):
        raise PreparationError(f"Unsupported container runtime {requested!r}; choose docker, podman, or auto")
    if requested != "auto":
        return requested
    if shutil.which("docker"):
        return "docker"
    if shutil.which("podman"):
        return "podman"
    # Prepare-only mode can still create a workspace and print the command to
    # run after the user installs a container runtime.
    return "docker"


def validate_sdk(sdk: pathlib.Path | None, build: bool, runtime: str) -> pathlib.Path | None:
    if sdk is None:
        if build:
            raise PreparationError(
                "--build requires --sdk pointing to the Ingenic SDK archive "
                f"({SDK_GLOB_PREFIX}*.tar.bz2); the upstream build cannot run without it"
            )
        return None
    if not sdk.is_file() or sdk.is_symlink() or sdk.stat().st_size == 0:
        raise PreparationError(f"SDK archive must be a non-empty regular file: {sdk}")
    expected = load_pin()["sdk_archive"]
    if sdk.name != expected:
        raise PreparationError(f"SDK archive filename must match the pinned {expected}: {sdk.name}")
    pin = load_pin()
    if pin.get("sdk_sha256"):
        actual = sha256(sdk)
        if actual != pin["sdk_sha256"]:
            raise PreparationError(
                f"SDK archive SHA-256 does not match the pinned digest: expected {pin['sdk_sha256']}, got {actual}"
            )
    if build:
        if not shutil.which("modinfo"):
            raise PreparationError("--build requires host modinfo to validate every built module's vermagic")
        runtime_path = shutil.which(runtime)
        if not runtime_path:
            raise PreparationError(
                f"--build requires {runtime}; install/start the selected container runtime "
                "or choose an installed runtime with --container-runtime (this tool will not install one)"
            )
        try:
            subprocess.run([runtime_path, "image", "inspect", DOCKER_IMAGE], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except (OSError, subprocess.CalledProcessError) as exc:
            raise PreparationError(
                f"--build requires the existing {runtime} image {DOCKER_IMAGE!r}; "
                f"build it from the pinned upstream docker/ directory first, for example: "
                f"cd <pinned-upstream> && {runtime} build --platform linux/amd64 "
        f"-t {DOCKER_IMAGE} docker/"
            ) from exc
    return sdk


def copy_tracked_checkout(upstream: pathlib.Path, destination: pathlib.Path) -> None:
    try:
        listing = subprocess.run(
            ["git", "-C", str(upstream), "ls-files", "-z"],
            check=True, capture_output=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise PreparationError(f"Cannot list pinned upstream source files: {exc}") from exc
    for raw in listing.split(b"\0"):
        if not raw:
            continue
        relative = pathlib.PurePosixPath(os.fsdecode(raw))
        if relative.is_absolute() or ".." in relative.parts:
            raise PreparationError(f"Unsafe tracked path in upstream checkout: {relative}")
        source = upstream.joinpath(*relative.parts)
        target = destination.joinpath(*relative.parts)
        if source.is_symlink():
            resolved = source.resolve(strict=True)
            try:
                resolved.relative_to(upstream.resolve())
            except ValueError as exc:
                raise PreparationError(f"Upstream symlink escapes checkout: {relative}") from exc
            target.parent.mkdir(parents=True, exist_ok=True)
            target.symlink_to(os.readlink(source))
        elif source.is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        else:
            raise PreparationError(f"Tracked upstream file is missing or unsupported: {relative}")


def docker_volume_name(workspace: pathlib.Path, sdk_sha256: str | None) -> str:
    workspace_id = hashlib.sha256(str(workspace).encode()).hexdigest()[:12]
    sdk_id = (sdk_sha256 or "no-sdk")[:12]
    return f"compas-kbuild-{workspace_id}-{sdk_id}"


def container_command(runtime: str, workspace: pathlib.Path, sdk: pathlib.Path | None,
                      sdk_sha256: str | None, profile: str, jobs: int = 2,
                      patches: list[str] | None = None,
                      wifi_stack: str = "vendor",
                      wifi_patches: list[str] | None = None) -> list[str]:
    kit = workspace / "hiby-custom-kernel"
    volume = docker_volume_name(workspace, sdk_sha256)
    command = [
        runtime, "run", "--platform", "linux/amd64", "--rm",
    ]
    if runtime == "podman":
        command.extend(["-v", f"{workspace}:/work:Z", "-v", f"{volume}:/build"])
    else:
        command.extend(["-v", f"{workspace}:/work", "-v", f"{volume}:/build"])
    if sdk:
        mount_options = ":ro,Z" if runtime == "podman" else ":ro"
        command.extend(["-v", f"{sdk}:/work/{sdk.name}{mount_options}"])
    command.extend([
        "-e", "TAR_OPTIONS=--no-same-owner",
        DOCKER_IMAGE, "bash", f"/work/{kit.name}/build.sh", "--model", "r1",
        "--name", "compas-r1", "--jobs", str(jobs), "--fragment", "r1-parity.config",
        "--fragment", "compas-r1.config",
    ])
    wifi_fragment = WIFI_FRAGMENTS.get(wifi_stack)
    if wifi_fragment:
        command.extend(["--fragment", wifi_fragment])
    for patch in [*(patches or []), *(wifi_patches or [])]:
        command.extend(["--patch", patch])
    return command


def config_requests(*paths: pathlib.Path) -> dict[str, str]:
    requested: dict[str, str] = {}
    for path in paths:
        for line in path.read_text().splitlines():
            match = re.fullmatch(r"(CONFIG_[A-Za-z0-9_]+)=(.*)", line)
            if match:
                requested[match.group(1)] = match.group(2)
                continue
            match = re.fullmatch(r"# (CONFIG_[A-Za-z0-9_]+) is not set", line)
            if match:
                requested[match.group(1)] = "n"
    return requested


def parse_kernel_config(path: pathlib.Path) -> dict[str, str]:
    return config_requests(path)


def embedded_dtb(ximage: pathlib.Path) -> bytes | None:
    import struct
    import zlib

    data = ximage.read_bytes()
    offset = data.find(b"\x1f\x8b\x08")
    if offset < 0:
        return None
    try:
        raw = zlib.decompressobj(31).decompress(data[offset:])
    except zlib.error:
        return None
    for match in re.finditer(b"\xd0\x0d\xfe\xed", raw):
        size = struct.unpack(">I", raw[match.start() + 4:match.start() + 8])[0]
        if 1000 < size < 200000:
            return raw[match.start():match.start() + size]
    return None


def expected_r1_modules(kit: pathlib.Path) -> set[str]:
    expected: set[str] = set()
    for directory in (kit / "modules", kit / "boards/r1/modules"):
        for makefile in sorted(directory.glob("*/Makefile")):
            for line in makefile.read_text(errors="replace").splitlines():
                match = re.match(r"\s*obj-m\s*[+:]?=\s*(.*?)\s*(?:#.*)?$", line)
                if not match:
                    continue
                expected.update(pathlib.Path(item).stem for item in match.group(1).split() if item.endswith(".o"))
    if not expected:
        raise PreparationError("Pinned R1 kernel kit declares no expected external modules")
    return expected


def validate_build(kit: pathlib.Path, stock_kernel: pathlib.Path,
                   wifi_stack: str = "vendor",
                   provider_source_sha256: str | None = None,
                   display_stack: str = "vendor",
                   display_sources: list[dict[str, str]] | None = None) -> dict[str, object]:
    if wifi_stack not in ("vendor", *WIFI_FRAGMENTS):
        raise PreparationError(f"Unsupported Wi-Fi stack {wifi_stack!r}; choose vendor or brcmfmac")
    if display_stack not in DISPLAY_STACKS:
        raise PreparationError(f"Unsupported display stack {display_stack!r}; choose vendor or compas")
    display_sources = display_sources or []
    if display_stack == "compas" and not display_sources:
        raise PreparationError("Compas display validation requires copied C/header/Makefile/hiby.symvers")
    if display_stack == "vendor" and display_sources:
        raise PreparationError("Vendor display validation cannot include Compas display sources")
    out = kit / "out"
    name = "compas-r1"
    image = out / f"xImage-{name}"
    config = out / f"config-{name}"
    system_map = out / f"System.map-{name}"
    modules_dir = out / f"modules-{name}"
    for path in (image, config, system_map, modules_dir):
        if not path.exists():
            raise PreparationError(f"Build output is missing required artifact: {path}")
    if not image.is_file() or image.stat().st_size == 0:
        raise PreparationError("Built R1 xImage is empty")
    if image.stat().st_size > stock_kernel.stat().st_size:
        raise PreparationError(
            f"Built R1 xImage is {image.stat().st_size} bytes, larger than base kernel "
            f"({stock_kernel.stat().st_size} bytes)"
        )

    stock_dtb_path = kit / "boards/r1/stock.dtb"
    actual_dtb = embedded_dtb(image)
    if actual_dtb is None:
        raise PreparationError("Built xImage has no recognizable embedded DTB")
    expected_dtb = stock_dtb_path.read_bytes()
    if actual_dtb != expected_dtb:
        raise PreparationError("Built xImage DTB differs from extracted stock DTB")

    fragment_paths = [kit / "configs/r1-required.config", kit / "configs/r1-parity.config",
                      kit / "configs/compas-r1.config"]
    wifi_fragment = WIFI_FRAGMENTS.get(wifi_stack)
    if wifi_fragment:
        fragment_paths.append(kit / "configs" / wifi_fragment)
    requested = config_requests(*fragment_paths)
    actual_config = parse_kernel_config(config)
    if wifi_stack == "brcmfmac" and actual_config.get("CONFIG_MODULE_FORCE_UNLOAD", "n") != "n":
        raise PreparationError("brcmfmac candidate must disable CONFIG_MODULE_FORCE_UNLOAD")
    differences = [f"{symbol}: requested {value}, built {actual_config.get(symbol, 'n')}"
                   for symbol, value in requested.items() if actual_config.get(symbol, "n") != value]
    if differences:
        raise PreparationError("Compiled config did not retain requested profile options: " + "; ".join(differences))

    map_symbols = {parts[2] for line in system_map.read_text(errors="replace").splitlines()
                   if len(parts := line.split()) >= 3}
    needed_path = kit / "boards/r1/modules-need.txt"
    needed = [line.strip() for line in needed_path.read_text().splitlines() if line.strip()]
    missing = [symbol for symbol in needed if "__ksymtab_" + symbol not in map_symbols]
    if wifi_stack == "brcmfmac":
        missing_guard = [symbol for symbol in RADIO_GUARD_EXPORTS
                         if "__ksymtab_" + symbol not in map_symbols]
        if missing_guard:
            raise PreparationError("Built kernel is missing exported MMC lifetime helpers: "
                                   + ", ".join(missing_guard))
    if missing:
        raise PreparationError(
            f"Built kernel is missing {len(missing)} exported symbols required by R1 modules: "
            + ", ".join(missing[:12])
        )

    expected_modules = expected_r1_modules(kit)
    actual_modules = {path.stem: path for path in modules_dir.glob("*.ko") if path.is_file()}
    if display_stack == "compas":
        if "soc_fb" not in expected_modules:
            raise PreparationError("Compas display kit does not declare the soc_fb module")
        validate_prepared_display_sources(kit, display_sources)
    elif "soc_fb" in actual_modules:
        raise PreparationError("Vendor display build unexpectedly contains a replacement soc_fb.ko")
    wifi_modules = {"brcmfmac", "brcmutil"}
    if wifi_stack == "vendor":
        unexpected_wifi = sorted(wifi_modules & actual_modules.keys())
        if unexpected_wifi:
            raise PreparationError(
                "Vendor Wi-Fi build unexpectedly contains candidate modules: " + ", ".join(unexpected_wifi)
            )
    else:
        expected_modules.update(wifi_modules)
    absent = sorted(expected_modules - actual_modules.keys())
    if absent:
        raise PreparationError("Build is missing expected R1 modules: " + ", ".join(absent))
    modinfo = shutil.which("modinfo")
    if not modinfo:
        raise PreparationError("Cannot validate module vermagic: host modinfo is unavailable")
    vermagic: dict[str, str] = {}
    for module in sorted(expected_modules):
        try:
            result = subprocess.run([modinfo, "-F", "vermagic", str(actual_modules[module])],
                                    check=True, capture_output=True, text=True)
        except (OSError, subprocess.CalledProcessError) as exc:
            raise PreparationError(f"Cannot read vermagic from {actual_modules[module]}: {exc}") from exc
        value = result.stdout.strip()
        if value != EXPECTED_VERMAGIC:
            raise PreparationError(
                f"{module}.ko has unexpected vermagic {value!r}; expected {EXPECTED_VERMAGIC!r}"
            )
        vermagic[module] = value

    result = {
        "ximage_sha256": sha256(image),
        "ximage_size": image.stat().st_size,
        "stock_kernel_size": stock_kernel.stat().st_size,
        "stock_dtb_sha256": hashlib.sha256(actual_dtb).hexdigest(),
        "compiled_config_sha256": sha256(config),
        "required_export_count": len(needed),
        "required_module_count": len(expected_modules),
        "module_vermagic": vermagic,
        "system_map_path": str(system_map.resolve()),
        "system_map_sha256": sha256(system_map),
        "config_path": str(config.resolve()),
        "ximage_path": str(image.resolve()),
        "display_stack": display_stack,
    }
    if display_stack == "compas":
        source_hashes = {item["original_path"]: item["sha256"] for item in display_sources}
        result["display_source_sha256"] = source_hashes
        result["soc_fb_module_path"] = str(actual_modules["soc_fb"].resolve())
        result["soc_fb_module_sha256"] = sha256(actual_modules["soc_fb"])
    if wifi_stack == "brcmfmac":
        provider_source = kit / "modules/bcm_wlbt_power/bcm_wlbt_power.c"
        provider_module = actual_modules.get("bcm_wlbt_power")
        if provider_source_sha256 is None:
            raise PreparationError("brcmfmac validation requires the recorded provider source SHA-256")
        if not provider_source.is_file() or provider_source.is_symlink():
            raise PreparationError(f"Prepared provider source is missing or unsafe: {provider_source}")
        actual_source_sha256 = sha256(provider_source)
        if actual_source_sha256 != provider_source_sha256:
            raise PreparationError("Prepared provider source changed after its recorded source hash")
        if provider_module is None:
            raise PreparationError("Build is missing bcm_wlbt_power.ko")
        result["provider_source_sha256"] = actual_source_sha256
        result["provider_module_sha256"] = sha256(provider_module)
    return result


def prepare(args: argparse.Namespace) -> pathlib.Path:
    pin = load_pin()
    upstream = args.upstream.resolve()
    commit = validate_upstream(upstream, pin)
    if args.stock_kernel.is_symlink():
        raise PreparationError(f"Stock kernel must not be a symlink: {args.stock_kernel}")
    stock = args.stock_kernel.resolve()
    if not stock.is_file() or stock.stat().st_size == 0:
        raise PreparationError(f"Stock kernel must be a non-empty regular file: {stock}")
    profile_path = REPO_ROOT / f"firmware/kernel/r1-{args.profile}.config"
    if not profile_path.is_file() or profile_path.is_symlink():
        raise PreparationError(f"Missing R1 kernel profile: {profile_path}")
    profile_text = profile_path.read_text()
    if "CONFIG_NLS_CODEPAGE_936=y" not in profile_text.splitlines():
        raise PreparationError(
            f"{profile_path} must keep CONFIG_NLS_CODEPAGE_936=y; Compás mounts FAT media with codepage 936"
        )
    patches = local_patches(upstream)
    wifi_stack = getattr(args, "wifi_stack", "vendor")
    if wifi_stack not in ("vendor", *WIFI_FRAGMENTS):
        raise PreparationError(f"Unsupported Wi-Fi stack {wifi_stack!r}; choose vendor or brcmfmac")
    provider_source_arg = getattr(args, "provider_source", None)
    if wifi_stack == "brcmfmac":
        if provider_source_arg is None:
            raise PreparationError("brcmfmac Wi-Fi build requires --provider-source with the reviewed provider C source")
        if provider_source_arg.is_symlink() or not provider_source_arg.is_file():
            raise PreparationError(f"Provider source must be a regular non-symlink file: {provider_source_arg}")
        provider_source_path = provider_source_arg.resolve()
        provider_source_sha256 = sha256(provider_source_path)
    else:
        if provider_source_arg is not None:
            raise PreparationError("--provider-source is only valid with --wifi-stack brcmfmac")
        provider_source_path = None
        provider_source_sha256 = None
    display_stack = getattr(args, "display_stack", "vendor")
    display_sources = display_source_files(display_stack)
    source_patches = module_patches(wifi_stack)
    experimental_kernel_patches = wifi_kernel_patches(wifi_stack)
    wifi_fragment_name = WIFI_FRAGMENTS.get(wifi_stack)
    wifi_fragment_path = REPO_ROOT / "firmware/kernel" / wifi_fragment_name if wifi_fragment_name else None
    if wifi_fragment_path and (wifi_fragment_path.is_symlink() or not wifi_fragment_path.is_file()):
        raise PreparationError(f"Missing regular Wi-Fi config fragment: {wifi_fragment_path}")
    if args.sdk and args.sdk.is_symlink():
        raise PreparationError(f"SDK archive must not be a symlink: {args.sdk}")
    runtime = select_container_runtime(getattr(args, "container_runtime", "auto"))
    sdk = validate_sdk(args.sdk.resolve() if args.sdk else None, args.build, runtime)
    sdk_sha256 = sha256(sdk) if sdk else None

    if args.workspace.is_symlink():
        raise PreparationError(f"Workspace path must not be a symlink: {args.workspace}")
    workspace = args.workspace.resolve()
    upstream_root = upstream.resolve()
    repo_root = REPO_ROOT.resolve()
    if workspace == upstream_root or upstream_root in workspace.parents:
        raise PreparationError("Workspace must be outside the pinned upstream checkout")
    if workspace == repo_root or repo_root in workspace.parents:
        raise PreparationError("Workspace must be outside the Compás source checkout")
    if workspace.exists():
        raise PreparationError(f"Workspace must be a fresh, non-existing path: {workspace}")
    kit_destination = workspace / "hiby-custom-kernel"
    jobs = getattr(args, "jobs", 2)
    patch_names = [patch.name for patch in patches]
    wifi_patch_names = [patch.name for patch in experimental_kernel_patches]
    command = container_command(runtime, workspace, sdk, sdk_sha256, args.profile, jobs, patch_names,
                                wifi_stack, wifi_patch_names)

    workspace.mkdir(parents=True)
    try:
        copy_tracked_checkout(upstream, kit_destination)
        copied_patch_dir = kit_destination / "patches"
        if patches or experimental_kernel_patches:
            copied_patch_dir.mkdir(parents=True, exist_ok=True)
            for patch in [*patches, *experimental_kernel_patches]:
                shutil.copy2(patch, copied_patch_dir / patch.name)
        applied_module_patches = apply_module_patches(kit_destination, workspace, source_patches)
        display_source_records = copy_display_sources(kit_destination, workspace, display_sources)
        if provider_source_path:
            prepared_provider = kit_destination / "modules/bcm_wlbt_power/bcm_wlbt_power.c"
            if not prepared_provider.is_file() or prepared_provider.is_symlink():
                raise PreparationError("Patched kit is missing a regular bcm_wlbt_power.c source")
            prepared_hash = sha256(prepared_provider)
            if prepared_hash != provider_source_sha256:
                raise PreparationError(
                    "Authoritative --provider-source does not match the module-patch-produced provider source; "
                    "update the reviewed patch instead of overriding its result"
                )
        stock_dtb_dir = kit_destination / "boards/r1"
        stock_dtb_dir.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [sys.executable, str(kit_destination / "tools/extract-stock.py"), "r1", str(stock)],
            check=True,
        )
        dtb = stock_dtb_dir / "stock.dtb"
        if not dtb.is_file() or dtb.stat().st_size == 0:
            raise PreparationError("Pinned stock extractor did not produce boards/r1/stock.dtb")
        shutil.copy2(profile_path, kit_destination / "configs/compas-r1.config")
        if wifi_fragment_path:
            shutil.copy2(wifi_fragment_path, kit_destination / "configs" / wifi_fragment_name)
        manifest = {
            "schema_version": 1,
            "board": "r1",
            "profile": args.profile,
            "wifi_stack": wifi_stack,
            "display_stack": display_stack,
            "container_runtime": runtime,
            "jobs": jobs,
            "upstream": pin,
            "upstream_head": commit,
            "stock_kernel": {"path": str(stock), "sha256": sha256(stock), "size": stock.stat().st_size},
            "stock_dtb": {"sha256": sha256(dtb), "size": dtb.stat().st_size},
            "profile_config": {
                "path": str(profile_path),
                "sha256": sha256(profile_path),
                "copied_to": "hiby-custom-kernel/configs/compas-r1.config",
            },
            "local_patches": [
                {
                    "original_path": patch.relative_to(REPO_ROOT).as_posix(),
                    "sha256": sha256(patch),
                    "copied_to": f"hiby-custom-kernel/patches/{patch.name}",
                }
                for patch in patches
            ],
            "wifi_kernel_patches": [
                {
                    "original_path": patch.relative_to(REPO_ROOT).as_posix(),
                    "sha256": sha256(patch),
                    "copied_to": f"hiby-custom-kernel/patches/{patch.name}",
                }
                for patch in experimental_kernel_patches
            ],
            "module_patches": applied_module_patches,
            "display_sources": display_source_records,
            "provider_source": ({
                "path": str(provider_source_path),
                "sha256": provider_source_sha256,
                "prepared_path": "hiby-custom-kernel/modules/bcm_wlbt_power/bcm_wlbt_power.c",
                "prepared_sha256": prepared_hash,
            } if provider_source_path else None),
            "wifi_config_fragment": ({
                "path": str(wifi_fragment_path),
                "sha256": sha256(wifi_fragment_path),
                "copied_to": f"hiby-custom-kernel/configs/{wifi_fragment_name}",
            } if wifi_fragment_path else None),
            "build_command": command,
            "build_command_shell": shlex.join(command),
            "sdk_expected_in_workspace_as": None if sdk else pin["sdk_archive"],
            "build_requested": bool(args.build),
        }
        if sdk:
            manifest["sdk"] = {"path": str(sdk), "sha256": sdk_sha256, "size": sdk.stat().st_size}
        (workspace / "preparation.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    except Exception:
        if not args.build:
            shutil.rmtree(workspace, ignore_errors=True)
        raise

    if args.build:
        try:
            subprocess.run(command, cwd=workspace, check=True)
        except (OSError, subprocess.CalledProcessError) as exc:
            raise PreparationError(f"Container kernel build failed; prepared workspace is preserved: {exc}") from exc
        validation = validate_build(kit_destination, stock, wifi_stack, provider_source_sha256,
                                     display_stack, display_source_records)
        manifest_path = workspace / "preparation.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["build_validation"] = validation
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return workspace


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", type=pathlib.Path, required=True, help="clean pinned kernel kit checkout")
    parser.add_argument("--stock-kernel", type=pathlib.Path, required=True, help="base firmware's extracted xImage")
    parser.add_argument("--workspace", type=pathlib.Path, required=True, help="fresh destination directory")
    parser.add_argument("--profile", choices=("parity", "optimized"), required=True)
    parser.add_argument("--wifi-stack", choices=("vendor", "brcmfmac"), default="vendor",
                        help="Wi-Fi build profile; brcmfmac builds candidate modules only and does not "
                        "migrate rootfs loading or activate the radio (default: existing vendor stack)")
    parser.add_argument("--display-stack", choices=DISPLAY_STACKS, default="vendor",
                        help="display module source; compas builds an isolated soc_fb replacement, "
                        "vendor preserves the existing module stack (default: vendor)")
    parser.add_argument("--provider-source", type=pathlib.Path,
                        help="reviewed bcm_wlbt_power.c source required for --wifi-stack brcmfmac")
    parser.add_argument("--sdk", type=pathlib.Path, help="optional Ingenic SDK tarball")
    parser.add_argument("--container-runtime", choices=("auto", "docker", "podman"), default="auto",
                        help="container runtime (default: prefer Docker, then Podman)")
    parser.add_argument("--jobs", type=positive_int, default=2,
                        help="parallel kernel build jobs (default: 2)")
    parser.add_argument("--build", action="store_true", help="run the container build after preparation")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    try:
        workspace = prepare(parse_args(argv))
    except PreparationError as exc:
        print(f"prepare_r1_kernel.py: {exc}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as exc:
        print(f"prepare_r1_kernel.py: stock DTB extraction failed ({exc})", file=sys.stderr)
        return 1
    print(f"Prepared isolated R1 kernel workspace: {workspace}")
    if not any(arg == "--build" for arg in (argv if argv is not None else sys.argv[1:])):
        print("No build was run. See preparation.json for the pinned inputs and container command.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
