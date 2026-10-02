#!/usr/bin/env python3
"""Regenerate embedded representative coordinates for supported IANA zones.

Coordinates come from the public-domain IANA tzdb zone.tab/zone1970.tab.
For historical compatibility IDs absent from those tables, only verified
geographic renames are mapped to a current IANA table entry. Rule-equivalent
aliases for different places are omitted. No network access or runtime
filesystem lookup is involved.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path


ZONE_RE = re.compile(r'^\s*\{\s*"([^"]+)"\s*,')
COORD_RE = re.compile(r'^([+-])(\d{4}(?:\d{2})?)([+-])(\d{5}(?:\d{2})?)$')
BEGIN = "    /* GENERATED DATA BEGIN */"
END = "    /* GENERATED DATA END */"


def decode_axis(sign: str, digits: str, degree_digits: int) -> int:
    degrees = int(digits[:degree_digits])
    minutes = int(digits[degree_digits:degree_digits + 2])
    seconds = int(digits[degree_digits + 2:degree_digits + 4] or "0")
    # tzdb coordinates are minute/second integral. Round to the nearest
    # millidegree while keeping the sign outside the magnitude calculation.
    millidegrees = (degrees * 3600 + minutes * 60 + seconds) * 1000
    millidegrees = (millidegrees + 1800) // 3600
    return -millidegrees if sign == "-" else millidegrees


def parse_coordinate(value: str) -> tuple[int, int] | None:
    match = COORD_RE.fullmatch(value)
    if not match:
        return None
    lat_sign, lat, lon_sign, lon = match.groups()
    return decode_axis(lat_sign, lat, 2), decode_axis(lon_sign, lon, 3)


def read_tables(paths: list[Path]) -> dict[str, tuple[int, int]]:
    locations: dict[str, tuple[int, int]] = {}
    for path in paths:
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) < 3:
                continue
            coords = parse_coordinate(fields[1])
            if coords is not None:
                locations.setdefault(fields[2], coords)
    return locations


def supported_ids(timezone_data: Path) -> list[str]:
    result = []
    for line in timezone_data.read_text(encoding="utf-8").splitlines():
        match = ZONE_RE.match(line)
        if match:
            result.append(match.group(1))
    if not result:
        raise RuntimeError(f"No timezone IDs found in {timezone_data}")
    return result


def generate(root: Path, zoneinfo: Path) -> str:
    timezone_data = root / "src/core/timezone_data.c"
    out_file = root / "src/core/timezone_location.c"
    zone_tab = read_tables([zoneinfo / "zone.tab"])
    zone1970_tab = read_tables([zoneinfo / "zone1970.tab"])
    location_tables = dict(zone1970_tab)
    location_tables.update(zone_tab)

    # Historical aliases verified to refer to the same named place. Link
    # records are deliberately not followed generically: tzdb also links
    # unrelated places that merely share post-1970 clock rules (for example,
    # Pacific/Truk -> Port_Moresby). The target IDs resolve to actual IANA
    # table entries, so coordinates still come only from zone.tab files.
    historical_locations = {
        "America/Indianapolis": "America/Indiana/Indianapolis",
        "America/Louisville": "America/Kentucky/Louisville",
        "America/Buenos_Aires": "America/Argentina/Buenos_Aires",
        "America/Catamarca": "America/Argentina/Catamarca",
        "America/Cordoba": "America/Argentina/Cordoba",
        "America/Jujuy": "America/Argentina/Jujuy",
        "America/Mendoza": "America/Argentina/Mendoza",
        "America/Godthab": "America/Nuuk",            # Greenland's capital was renamed
        "Atlantic/Faeroe": "Atlantic/Faroe",          # historical spelling
        "Asia/Calcutta": "Asia/Kolkata",              # city renamed
        "Asia/Katmandu": "Asia/Kathmandu",            # historical spelling
        "Pacific/Truk": "Pacific/Chuuk",              # zone.tab: “Chuuk/Truk, Yap”
        "Pacific/Ponape": "Pacific/Pohnpei",          # zone.tab: “Pohnpei/Ponape”
    }

    rows: dict[str, tuple[int, int]] = {}
    for zone_id in supported_ids(timezone_data):
        coords = location_tables.get(zone_id)
        if coords is None:
            target_id = historical_locations.get(zone_id)
            if target_id is not None:
                coords = location_tables.get(target_id)
        if coords is not None:
            rows[zone_id] = coords

    rendered = "\n".join(
        f'    {{ "{zone_id}", {lat}, {lon} }},'
        for zone_id, (lat, lon) in sorted(rows.items())
    )
    source = out_file.read_text(encoding="utf-8")
    if source.count(BEGIN) != 1 or source.count(END) != 1:
        raise RuntimeError(f"Expected one generated-data marker pair in {out_file}")
    before, remainder = source.split(BEGIN, 1)
    _, after = remainder.split(END, 1)
    return before + BEGIN + "\n" + rendered + "\n" + END + after


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--zoneinfo", type=Path, default=Path("/usr/share/zoneinfo"))
    args = parser.parse_args()
    output = generate(args.repo.resolve(), args.zoneinfo)
    target = args.repo.resolve() / "src/core/timezone_location.c"
    target.write_text(output, encoding="utf-8")
    total = sum(1 for line in output.splitlines() if line.lstrip().startswith('{ "'))
    print(f"Generated {total} timezone locations in {target}")


if __name__ == "__main__":
    main()
