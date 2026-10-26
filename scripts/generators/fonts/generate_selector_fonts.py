#!/usr/bin/env python3
"""Instantiate and subset Noto Sans for the bootloader selector.

Builds static Regular (wght=400) and Medium (wght=500) TrueType faces from
the pinned Noto Sans variable source, covering Basic Latin, Latin-1
Supplement, Latin Extended-A, Greek, Cyrillic, and the punctuation the
selector strings use. Outputs assets/fonts/NotoSans-*.ttf and
src/bootloader/selector/assets/fonts/selector_font_pin.h.

stb_truetype cannot instance a VF, so these static files are what the
selector loads from /usr/resource/fonts.
"""
from __future__ import annotations

import argparse
import filecmp
import hashlib
import os
import shutil
import sys
import tempfile
from pathlib import Path

import fontTools
from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

ROOT = Path(__file__).resolve().parents[3]
SOURCE_SHA256 = "fb53504de62e767a54695416123d68d67f71c9b69722175184ad56e7dd13547b"
FONTTOOLS_PIN = "4.65.0"
DEFAULT_SOURCE = Path("/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf")
OFL_TEMPLATE = ROOT / "assets" / "fonts" / "Noto-Sans-Arabic-OFL.txt"
OFL_NAME = "NotoSans-OFL.txt"
PIN_REL = Path("src") / "bootloader" / "selector" / "assets" / "fonts" / "selector_font_pin.h"
REGULAR_REL = Path("assets") / "fonts" / "NotoSans-Regular.ttf"
MEDIUM_REL = Path("assets") / "fonts" / "NotoSans-Medium.ttf"

UNICODES = set(range(0x20, 0x7F)) | set(range(0xA0, 0x100)) | set(range(0x100, 0x180))
UNICODES |= set(range(0x0370, 0x0400)) | set(range(0x0400, 0x0500))
UNICODES |= {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026}
REQUIRED = {0x20, 0x41, 0x61, 0xE1, 0x2026, 0x0391, 0x03B1, 0x0410, 0x0430}


def fail(message: str) -> None:
    raise SystemExit(message)


def require_fonttools() -> None:
    if fontTools.__version__ != FONTTOOLS_PIN:
        fail("fontTools %s required, got %s" % (FONTTOOLS_PIN, fontTools.__version__))


def crc32(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
    return crc ^ 0xFFFFFFFF


def set_names(font: TTFont, family: str, subfamily: str, ps: str) -> None:
    names = {
        1: family,
        2: subfamily,
        4: "%s %s" % (family, subfamily),
        6: ps,
        16: family,
        17: subfamily,
    }
    for record in font["name"].names:
        if record.nameID in names:
            record.string = names[record.nameID].encode(record.getEncoding(), errors="replace")


def build_face(source: Path, weight: int, subfamily: str, ps: str, output: Path) -> tuple[int, int]:
    font = TTFont(source)
    if "fvar" not in font or "wght" not in {axis.axisTag for axis in font["fvar"].axes}:
        fail("source font has no expected wght variable axis")
    instantiateVariableFont(font, {"wght": weight}, inplace=True, optimize=True)

    options = subset.Options()
    options.recalc_timestamp = False
    options.layout_features = []
    options.name_IDs = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 17]
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=UNICODES)
    subsetter.subset(font)

    cmap = {cp for table in font["cmap"].tables if table.isUnicode() for cp in table.cmap}
    missing = sorted(REQUIRED - cmap)
    if missing:
        fail("subset lacks " + ", ".join("U+%04X" % cp for cp in missing))

    set_names(font, "Compas UI", subfamily, ps)
    font["OS/2"].usWeightClass = weight
    font["head"].created = 2082844800
    font["head"].modified = 2082844800
    font.recalcTimestamp = False
    if "fvar" in font or "gvar" in font or "glyf" not in font or "CFF " in font:
        fail("output is not a static TrueType-outline font")

    output.parent.mkdir(parents=True, exist_ok=True)
    font.save(output, reorderTables=True)
    data = output.read_bytes()
    print("Wrote %s (%d bytes, %d mapped code points)" % (output, len(data), len(cmap)))
    return len(data), crc32(data)


def write_ofl(ofl_out: Path) -> None:
    body = OFL_TEMPLATE.read_text(encoding="utf-8")
    marker = "This Font Software is licensed under the SIL Open Font License"
    idx = body.find(marker)
    if idx < 0:
        fail("cannot split OFL template")
    header = (
        "Copyright 2022 The Noto Project Authors "
        "(https://github.com/notofonts/latin-greek-cyrillic)\n\n"
        "This Font Software is licensed under the SIL Open Font License, Version 1.1.\n"
        "This license is copied below, and is also available with a FAQ at:\n"
        "https://openfontlicense.org\n\n\n"
    )
    rest = body[idx:]
    rest = rest[rest.find("-----------------------------------------------------------"):]
    ofl_out.parent.mkdir(parents=True, exist_ok=True)
    ofl_out.write_text(header + rest, encoding="utf-8")
    print("Wrote %s" % ofl_out)


def write_pin(pin_out: Path, regular_size: int, regular_crc: int,
              medium_size: int, medium_crc: int) -> None:
    pin_out.parent.mkdir(parents=True, exist_ok=True)
    pin_out.write_text(
        "/* Generated by scripts/generators/fonts/generate_selector_fonts.py. Do not edit. */\n"
        "#ifndef BOOTLOADER_SELECTOR_FONT_PIN_H\n"
        "#define BOOTLOADER_SELECTOR_FONT_PIN_H\n\n"
        "#define NOTO_SANS_REGULAR_BYTES %uu\n"
        "#define NOTO_SANS_REGULAR_CRC32 0x%08Xu\n"
        "#define NOTO_SANS_MEDIUM_BYTES %uu\n"
        "#define NOTO_SANS_MEDIUM_CRC32 0x%08Xu\n\n"
        "#endif /* BOOTLOADER_SELECTOR_FONT_PIN_H */\n"
        % (regular_size, regular_crc, medium_size, medium_crc),
        encoding="utf-8",
    )
    print("Wrote %s" % pin_out)


def generate(source: Path, out_root: Path) -> None:
    require_fonttools()
    try:
        source_bytes = source.read_bytes()
    except OSError as error:
        fail("cannot read source font %s: %s" % (source, error))
    actual_hash = hashlib.sha256(source_bytes).hexdigest()
    if actual_hash != SOURCE_SHA256:
        fail("source SHA-256 mismatch: expected %s, got %s" % (SOURCE_SHA256, actual_hash))

    regular_out = out_root / REGULAR_REL
    medium_out = out_root / MEDIUM_REL
    ofl_out = out_root / "assets" / "fonts" / OFL_NAME
    pin_out = out_root / PIN_REL
    regular_size, regular_crc = build_face(
        source, 400, "Regular", "CompasUI-Regular", regular_out)
    medium_size, medium_crc = build_face(
        source, 500, "Medium", "CompasUI-Medium", medium_out)
    write_ofl(ofl_out)
    write_pin(pin_out, regular_size, regular_crc, medium_size, medium_crc)
    print("Pinned source SHA-256: %s" % actual_hash)


def verify(source: Path) -> None:
    tmp = tempfile.mkdtemp(prefix="selector-fonts-")
    try:
        generate(source, Path(tmp))
        stale = []
        for rel in (REGULAR_REL, MEDIUM_REL, Path("assets") / "fonts" / OFL_NAME, PIN_REL):
            expected = os.path.join(tmp, str(rel))
            actual = os.path.join(ROOT, str(rel))
            if not os.path.isfile(actual) or not filecmp.cmp(expected, actual, shallow=False):
                stale.append(str(rel))
        if stale:
            sys.stderr.write("selector fonts are stale; run python3 scripts/generators/fonts/generate_selector_fonts.py\n")
            for path in stale:
                sys.stderr.write("  %s\n" % path)
            sys.exit(1)
        print("selector fonts match scripts/generators/fonts/generate_selector_fonts.py")
    finally:
        shutil.rmtree(tmp)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--verify", action="store_true",
                        help="fail if committed fonts and pin do not match a fresh subset")
    args = parser.parse_args()
    os.chdir(ROOT)
    if args.verify:
        verify(args.source)
    else:
        generate(args.source, ROOT)


if __name__ == "__main__":
    main()
