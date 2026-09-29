#!/usr/bin/env python3
"""Generate the original Car Mode pull-down toggle icon pair."""

from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "assets/theme2/pull_down"
LIGHT_OUTPUT = ROOT / "assets/theme1/pull_down"
SIZE = 84
SCALE = 4


def render(background: tuple[int, int, int], foreground: tuple[int, int, int]) -> Image.Image:
    n = SIZE * SCALE
    image = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    # Match the stock toggles' edge-to-edge circular button and transparent corners.
    draw.ellipse((0, 0, n - 1, n - 1), fill=(*background, 255))
    s = SCALE

    def pt(x: int, y: int) -> tuple[int, int]:
        return x * s, y * s

    ink = (*foreground, 255)
    cutout = (*background, 255)

    # Front-view car silhouette: sloped windshield, broad hood, two
    # headlights, bumper and tires underneath the body.
    draw.polygon([pt(29, 24), pt(55, 24), pt(61, 39), pt(23, 39)], fill=ink)
    draw.rounded_rectangle((*pt(20, 37), *pt(64, 57)), radius=5 * s, fill=ink)
    draw.rounded_rectangle((*pt(24, 52), *pt(31, 63)), radius=2 * s, fill=ink)
    draw.rounded_rectangle((*pt(53, 52), *pt(60, 63)), radius=2 * s, fill=ink)
    draw.rounded_rectangle((*pt(16, 36), *pt(23, 41)), radius=2 * s, fill=ink)
    draw.rounded_rectangle((*pt(61, 36), *pt(68, 41)), radius=2 * s, fill=ink)
    draw.polygon([pt(32, 28), pt(52, 28), pt(56, 37), pt(28, 37)], fill=cutout)
    draw.ellipse((*pt(25, 43), *pt(31, 49)), fill=cutout)
    draw.ellipse((*pt(53, 43), *pt(59, 49)), fill=cutout)
    draw.rounded_rectangle((*pt(36, 44), *pt(48, 47)), radius=s, fill=cutout)
    draw.rounded_rectangle((*pt(27, 52), *pt(57, 54)), radius=s, fill=cutout)

    return image.resize((SIZE, SIZE), Image.Resampling.LANCZOS)


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    render((28, 28, 30), (87, 87, 91)).save(OUTPUT / "car_mode.png", optimize=True)
    render((0, 159, 246), (255, 255, 255)).save(OUTPUT / "car_mode_s.png", optimize=True)
    LIGHT_OUTPUT.mkdir(parents=True, exist_ok=True)
    render((247, 249, 252), (50, 82, 108)).save(LIGHT_OUTPUT / "car_mode.png", optimize=True)
    render((0, 159, 246), (255, 255, 255)).save(LIGHT_OUTPUT / "car_mode_s.png", optimize=True)


if __name__ == "__main__":
    main()
