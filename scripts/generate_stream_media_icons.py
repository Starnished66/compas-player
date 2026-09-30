#!/usr/bin/env python3
"""Generate the enlarged native Stream Media row icons.

Source images can be supplied with the corresponding --*-source options.
The Qobuz/Tidal extraction intentionally keeps light logo detail only, making
the marks transparent over the app's dark row background.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets/theme2/stream_media"
SIZE = 192
SCALE = 4


def open_rgba(path: Path) -> Image.Image:
    return Image.open(path).convert("RGBA")


def fit_alpha_crop(im: Image.Image, *, max_size: int = 176) -> Image.Image:
    bbox = im.getchannel("A").getbbox()
    if not bbox:
        raise ValueError("source has no visible pixels")
    cropped = im.crop(bbox)
    factor = min(max_size / cropped.width, max_size / cropped.height)
    size = (max(1, round(cropped.width * factor)), max(1, round(cropped.height * factor)))
    cropped = cropped.resize(size, Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    canvas.alpha_composite(cropped, ((SIZE - cropped.width) // 2, (SIZE - cropped.height) // 2))
    return canvas


def source_brand(path: Path, bounds: tuple[int, int, int, int]) -> Image.Image:
    """Extract an antialiased white/gray logo from a stock dark tile."""
    src = open_rgba(path).crop(bounds)
    lum = src.convert("L")
    # Remove the charcoal tile and black logo backing while preserving soft
    # gray Qobuz shading and antialiased edges. White is the intended glyph.
    alpha = lum.point(lambda value: 0 if value <= 24 else min(255, round((value - 24) * 255 / 231)))
    glyph = Image.new("RGBA", src.size, (246, 248, 250, 0))
    glyph.putalpha(alpha)
    return fit_alpha_crop(glyph)


def podcast_glyph() -> Image.Image:
    """Draw a classic grille microphone with a compact broadcast wave pair."""
    s = SCALE
    art = Image.new("RGBA", (SIZE * s, SIZE * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(art)
    purple = (168, 113, 244, 255)
    lilac = (232, 216, 255, 255)
    cyan = (71, 213, 225, 255)

    # Short lateral broadcast arcs remain clearly separate from the microphone.
    def curve(points: tuple[tuple[float, float], tuple[float, float], tuple[float, float]], width: int) -> None:
        (x0, y0), (cx, cy), (x1, y1) = points
        samples = []
        for i in range(41):
            t = i / 40
            u = 1 - t
            x = u*u*x0 + 2*u*t*cx + t*t*x1
            y = u*u*y0 + 2*u*t*cy + t*t*y1
            samples.append((round(x*s), round(y*s)))
        d.line(samples, fill=cyan, width=width*s, joint="curve")

    curve(((65, 34), (48, 47), (65, 61)), 3)
    curve(((127, 34), (144, 47), (127, 61)), 3)

    # Purple rounded capsule with a fine cyan edge and four visible grille bars.
    d.rounded_rectangle((73*s, 19*s, 119*s, 101*s), radius=22*s,
                        fill=purple, outline=cyan, width=2*s)
    for y in (39, 51, 63, 75):
        d.line((82*s, y*s, 110*s, y*s), fill=lilac, width=3*s)

    # Distinct open U-shaped cradle, vertical stem and short horizontal foot.
    d.line((65*s, 64*s, 65*s, 76*s, 70*s, 87*s, 80*s, 94*s,
            96*s, 97*s, 112*s, 94*s, 122*s, 87*s, 127*s, 76*s, 127*s, 64*s),
           fill=cyan, width=4*s, joint="curve")
    d.line((96*s, 97*s, 96*s, 151*s), fill=cyan, width=5*s)
    d.line((74*s, 154*s, 118*s, 154*s), fill=cyan, width=5*s)
    return art.resize((SIZE, SIZE), Image.Resampling.LANCZOS)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-theme-dir", type=Path, required=True,
                        help="Original firmware theme2 directory, before icon normalization")
    parser.add_argument("--subsonic-source", type=Path)
    parser.add_argument("--radio-source", type=Path)
    parser.add_argument("--qobuz-source", type=Path)
    parser.add_argument("--tidal-source", type=Path)
    parser.add_argument("--output-dir", type=Path, default=OUT)
    args = parser.parse_args()
    args.subsonic_source = args.subsonic_source or args.source_theme_dir / "stream_media/subsonic.png"
    args.radio_source = args.radio_source or args.source_theme_dir / "category/net_radio.png"
    args.qobuz_source = args.qobuz_source or args.source_theme_dir / "stream_media/qobuz.png"
    args.tidal_source = args.tidal_source or args.source_theme_dir / "stream_media/tidal.png"

    output = args.output_dir
    output.mkdir(parents=True, exist_ok=True)
    normal = {
        "subsonic": fit_alpha_crop(open_rgba(args.subsonic_source)),
        "radio": fit_alpha_crop(open_rgba(args.radio_source)),
        "podcasts": fit_alpha_crop(podcast_glyph()),
        "qobuz": source_brand(args.qobuz_source, (71, 37, 140, 106)),
        "tidal": source_brand(args.tidal_source, (76, 53, 137, 93)),
    }
    for name, image in normal.items():
        image.save(output / f"{name}_row.png", optimize=True)
        image.save(output / f"{name}_row_s.png", optimize=True)


if __name__ == "__main__":
    main()
