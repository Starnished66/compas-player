#!/usr/bin/env python3
"""Render the three added main Settings category icons from tracked SVG sources.

The paths come from @phosphor-icons/core@2.1.1 regular icons (MIT), available
at https://github.com/phosphor-icons/core/tree/main/assets/regular. The SVGs
in scripts/assets/settings_category_icons/ retain those paths and add only the
matching Compas pastel tint. Requires ffmpeg with librsvg and Pillow.
"""
from pathlib import Path
import subprocess
import tempfile

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "scripts/assets/settings_category_icons"
OUTPUT_DIRS = (
    ROOT / "assets/theme2/settings",
    ROOT / "assets/r3ii_2025/theme2/settings",
    ROOT / "assets/r3proii/theme2/settings",
)
ICONS = ("sound", "playback", "library")

with tempfile.TemporaryDirectory(prefix="settings-icons-") as temp_dir:
    temp_dir = Path(temp_dir)
    for name in ICONS:
        source = SOURCE_DIR / f"{name}.svg"
        high_res = temp_dir / f"{name}-176.png"
        subprocess.run(
            [
                "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                "-i", str(source), "-frames:v", "1", "-vf", "scale=176:176",
                str(high_res),
            ],
            check=True,
        )
        with Image.open(high_res) as rendered:
            icon = rendered.convert("RGBA").resize((44, 44), Image.Resampling.LANCZOS)
        for output_dir in OUTPUT_DIRS:
            output_dir.mkdir(parents=True, exist_ok=True)
            output = output_dir / f"{name}.png"
            icon.save(output, format="PNG", optimize=True)
            print(output)
