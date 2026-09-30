#!/usr/bin/env python3
"""Render the coral Genres submenu icon from its tracked Phosphor SVG source.

The path is from @phosphor-icons/core@2.1.1 (MIT). Requires ffmpeg with
librsvg and Pillow; render at 176px before Lanczos downsampling to 44px.
"""
from pathlib import Path
import subprocess
import tempfile

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "scripts/assets/submenu_icons/genres.svg"
OUTPUTS = (
    ROOT / "assets/theme2/submenu/genres.png",
    ROOT / "assets/r3ii_2025/theme2/submenu/genres.png",
    ROOT / "assets/r3proii/theme2/submenu/genres.png",
)

with tempfile.TemporaryDirectory(prefix="submenu-genres-") as temp_dir:
    high_res = Path(temp_dir) / "genres-176.png"
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
         "-i", str(SOURCE), "-frames:v", "1", "-vf", "scale=176:176",
         str(high_res)],
        check=True,
    )
    with Image.open(high_res) as rendered:
        icon = rendered.convert("RGBA").resize((44, 44), Image.Resampling.LANCZOS)
    for output in OUTPUTS:
        output.parent.mkdir(parents=True, exist_ok=True)
        icon.save(output, format="PNG", optimize=True)
        print(output)
