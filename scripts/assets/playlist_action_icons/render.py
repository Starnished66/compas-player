#!/usr/bin/env python3
"""Render sourced Phosphor playlist action SVGs as 44px white RGBA icons."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SOURCE_DIR = Path(__file__).resolve().parent
OUTPUT_DIR = ROOT / "assets/theme2/playlist_actions"

for name in ("trash", "list-checks"):
    source = SOURCE_DIR / f"{name}.svg"
    svg = source.read_text(encoding="utf-8").replace("currentColor", "#ffffff")
    with tempfile.TemporaryDirectory(prefix="playlist-icons-") as temp_dir:
        rendered_source = Path(temp_dir) / source.name
        rendered_source.write_text(svg, encoding="utf-8")
        subprocess.run(
            ["ffmpeg", "-loglevel", "error", "-y", "-i", str(rendered_source),
             "-vf", "scale=44:44:flags=lanczos", "-frames:v", "1", "-pix_fmt", "rgba",
             str(OUTPUT_DIR / f"{name}.png")],
            check=True,
        )
