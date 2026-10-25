#!/usr/bin/env python3
"""Report source file sizes and optionally enforce the maintained-source cap."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

SOURCE_SUFFIXES = {
    ".c", ".h", ".cc", ".hh", ".cpp", ".hpp", ".cxx", ".hxx",
    ".py", ".sh", ".bash", ".lua", ".js", ".mjs", ".cjs", ".jsx",
    ".ts", ".tsx",
}
VENDOR_DIRS = {"third_party", "stb_vorbis", "sonic", "tinfl"}
GENERATED_PATHS = {
    "src/ui/i18n_catalog.c",
    "src/network/ca_bundle.c",
    "src/network/remote_control_webapp.h",
    "src/ui/setup_world_map_data.h",
    "src/audio/wma_tables.h",
}


def classify(path: str) -> str:
    """Return maintained, vendored, or generated/data for a repository path."""
    if any(path == directory or path.startswith(directory + "/") for directory in VENDOR_DIRS):
        return "vendored"
    if path in GENERATED_PATHS or path.startswith("patches/lvgl_generated_fonts/"):
        return "generated/data"
    return "maintained"


def source_paths(root: Path) -> list[str]:
    """Return tracked and nonignored untracked source paths from Git."""
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    paths = {
        raw.decode("utf-8", errors="surrogateescape")
        for raw in result.stdout.split(b"\0") if raw
    }
    return sorted(path for path in paths
                  if Path(path).suffix.lower() in SOURCE_SUFFIXES and (root / path).is_file())


def line_count(path: Path) -> int:
    """Count physical lines, including a final unterminated line."""
    data = path.read_bytes()
    if not data:
        return 0
    return data.count(b"\n") + (not data.endswith(b"\n"))


def inventory(root: Path, limit: int) -> list[dict[str, object]]:
    rows = []
    for rel in source_paths(root):
        count = line_count(root / rel)
        rows.append({
            "path": rel,
            "lines": count,
            "classification": classify(rel),
            "over_limit": count > limit,
        })
    return rows


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd(), help="Git worktree root")
    parser.add_argument("--limit", type=int, default=500, help="maximum allowed physical lines")
    parser.add_argument("--check", action="store_true", help="fail for maintained files over the limit")
    parser.add_argument("--include-external", action="store_true",
                        help="also fail for vendored and generated/data files")
    parser.add_argument("--json", action="store_true", help="emit the inventory as JSON")
    args = parser.parse_args(argv)
    if args.limit <= 0:
        parser.error("--limit must be a positive integer")
    root = args.root.resolve()
    try:
        rows = inventory(root, args.limit)
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"check_module_sizes: {exc}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(rows, indent=2))
    else:
        for row in rows:
            mark = " OVER" if row["over_limit"] else ""
            print(f"{row['lines']:5}  {row['classification']:14} {row['path']}{mark}")
    checked = {"maintained"}
    if args.include_external:
        checked.update({"vendored", "generated/data"})
    offenders = [row for row in rows
                 if row["over_limit"] and row["classification"] in checked]
    if args.check and offenders:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
