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


def module_patches() -> list[pathlib.Path]:
    patch_dir = REPO_ROOT / "firmware/kernel/module-patches"
    if patch_dir.is_symlink():
        raise PreparationError(f"Module source patch path must be a regular directory: {patch_dir}")
    if not patch_dir.exists():
        return []
    if not patch_dir.is_dir():
        raise PreparationError(f"Module source patch path must be a regular directory: {patch_dir}")
    patches = sorted(patch_dir.glob("*.patch"), key=lambda path: path.name)
    for patch in patches:
        if patch.is_symlink() or not patch.is_file():
            raise PreparationError(f"Module source patch must be a regular non-symlink file: {patch}")
        validate_module_patch_paths(patch)
    return patches


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
                      patches: list[str] | None = None) -> list[str]:
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
    for patch in patches or []:
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
                match = re.match(r"\s*obj-m\s*:?=\s*(.*?)\s*(?:#.*)?$", line)
                if not match:
                    continue
                expected.update(pathlib.Path(item).stem for item in match.group(1).split() if item.endswith(".o"))
    if not expected:
        raise PreparationError("Pinned R1 kernel kit declares no expected external modules")
    return expected


def validate_build(kit: pathlib.Path, stock_kernel: pathlib.Path) -> dict[str, object]:
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

    fragment_paths = (kit / "configs/r1-required.config", kit / "configs/r1-parity.config",
                      kit / "configs/compas-r1.config")
    requested = config_requests(*fragment_paths)
    actual_config = parse_kernel_config(config)
    differences = [f"{symbol}: requested {value}, built {actual_config.get(symbol, 'n')}"
                   for symbol, value in requested.items() if actual_config.get(symbol, "n") != value]
    if differences:
        raise PreparationError("Compiled config did not retain requested profile options: " + "; ".join(differences))

    map_symbols = {parts[2] for line in system_map.read_text(errors="replace").splitlines()
                   if len(parts := line.split()) >= 3}
    needed_path = kit / "boards/r1/modules-need.txt"
    needed = [line.strip() for line in needed_path.read_text().splitlines() if line.strip()]
    missing = [symbol for symbol in needed if "__ksymtab_" + symbol not in map_symbols]
    if missing:
        raise PreparationError(
            f"Built kernel is missing {len(missing)} exported symbols required by R1 modules: "
            + ", ".join(missing[:12])
        )

    expected_modules = expected_r1_modules(kit)
    actual_modules = {path.stem: path for path in modules_dir.glob("*.ko") if path.is_file()}
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
        if not value.split() or value.split()[0] != "4.4.94+":
            raise PreparationError(f"{module}.ko has unexpected vermagic {value!r}; expected 4.4.94+")
        vermagic[module] = value

    return {
        "ximage_sha256": sha256(image),
        "ximage_size": image.stat().st_size,
        "stock_kernel_size": stock_kernel.stat().st_size,
        "stock_dtb_sha256": hashlib.sha256(actual_dtb).hexdigest(),
        "compiled_config_sha256": sha256(config),
        "required_export_count": len(needed),
        "required_module_count": len(expected_modules),
        "module_vermagic": vermagic,
    }


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
    source_patches = module_patches()
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
    command = container_command(runtime, workspace, sdk, sdk_sha256, args.profile, jobs, patch_names)

    workspace.mkdir(parents=True)
    try:
        copy_tracked_checkout(upstream, kit_destination)
        copied_patch_dir = kit_destination / "patches"
        if patches:
            copied_patch_dir.mkdir(parents=True, exist_ok=True)
            for patch in patches:
                shutil.copy2(patch, copied_patch_dir / patch.name)
        applied_module_patches = apply_module_patches(kit_destination, workspace, source_patches)
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
        manifest = {
            "schema_version": 1,
            "board": "r1",
            "profile": args.profile,
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
            "module_patches": applied_module_patches,
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
        validation = validate_build(kit_destination, stock)
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
