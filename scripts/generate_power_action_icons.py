#!/usr/bin/env python3
"""Render Power Hub icons from the pinned Lucide 0.468.0 SVG sources.

Requires ffmpeg with librsvg and Pillow. White strokes are recolored with
the current accent by LVGL. Sources and ISC license are kept alongside.
"""
from pathlib import Path
import subprocess
import tempfile

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "scripts/assets/power_action_icons"
OUTPUT_DIRS = (
    ROOT / "assets/theme2/power_action",
    ROOT / "assets/theme1/power_action",
    ROOT / "assets/r3proii/theme2/power_action",
    ROOT / "assets/r3proii/theme1/power_action",
    ROOT / "assets/r3ii_2025/theme2/power_action",
    ROOT / "assets/r3ii_2025/theme1/power_action",
)

with tempfile.TemporaryDirectory(prefix="power-icons-") as temp:
    temp = Path(temp)
    for name in ("power", "reboot", "screen_off", "sleep_timer"):
        source = temp / f"{name}.svg"
        source.write_text((SOURCE_DIR / f"{name}.svg").read_text().replace(
            'stroke="currentColor"', 'stroke="#ffffff"'))
        rendered = temp / f"{name}.png"
        subprocess.run([
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
            "-i", str(source), "-frames:v", "1", "-vf", "scale=384:384",
            str(rendered),
        ], check=True)
        with Image.open(rendered) as image:
            icon = image.convert("RGBA").resize((96, 96), Image.Resampling.LANCZOS)
        for output_dir in OUTPUT_DIRS:
            output_dir.mkdir(parents=True, exist_ok=True)
            icon.save(output_dir / f"{name}.png", optimize=True)
    for output_dir in OUTPUT_DIRS:
        (output_dir / "LICENSE.txt").write_text((SOURCE_DIR / "LICENSE").read_text())
        (output_dir / "SOURCES.txt").write_text(
            "Lucide 0.468.0 (ISC); white strokes, recolored by the live accent.\n"
            "power: https://unpkg.com/lucide-static@0.468.0/icons/power.svg\n"
            "reboot: https://unpkg.com/lucide-static@0.468.0/icons/rotate-cw.svg\n"
            "screen_off: https://unpkg.com/lucide-static@0.468.0/icons/monitor-off.svg\n"
            "sleep_timer: https://unpkg.com/lucide-static@0.468.0/icons/timer.svg\n"
            "Rebuild: python3 scripts/generate_power_action_icons.py\n")
