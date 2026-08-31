#!/usr/bin/env python3
"""Generate a static large/medium airport glyph database for ADS-B display.

Downloads OurAirports' airports.csv, filters to large_airport and
medium_airport types, and emits a compact C array of
{icao, alias, name, municipality, lat, lon, large} for Map view glyphs and
ICAO↔name lookup (detail-card routes, location picker). Airports the user
has saved as a Location (with runway data from airportdb.io) are drawn with
full runway geometry instead — see draw_saved_airports() in map_view.cpp,
which skips any static-DB entry whose ICAO matches a saved Location.

Primary `icao` prefers OurAirports `icao_code` when present (what flight
APIs return, e.g. SPJC). `ident` is kept as `alias` when it differs
(e.g. SPIM) so lookups still resolve historical / local codes without
drawing a second map glyph.

`municipality` is the city / locality from OurAirports (search + future UI).
Detail-card FROM/TO place labels use abbreviate + cut-Airport/Aerodrome, with
optional ICAO overrides in `airport_display_names.h` — not municipality.

Usage:
    python3 tools/generate_airports_db.py [--output PATH]
"""

import argparse
import csv
import io
import sys
import unicodedata
import urllib.request
from pathlib import Path

AIRPORTS_CSV_URL = "https://davidmegginson.github.io/ourairports-data/airports.csv"
INCLUDED_TYPES = {"large_airport", "medium_airport"}
# OurAirports names run long ("John F. Kennedy International Airport" = 37).
# 63 (+NUL → name[64]) covers ~99.7% of large/medium entries fully; the few
# outliers still get a word-boundary truncate with "...". Wide enough for the
# location-picker label at montserrat_16 on a ~540px panel.
NAME_MAX = 63  # + NUL in char name[64]
# Municipalities are short (p99 ~27, observed max ~42 among large/medium).
MUNI_MAX = 47  # + NUL in char municipality[48]

# Markers that must appear in a current airports_db.h. CMake --ensure and
# airports_db_include.h both key off these after schema bumps.
SCHEMA_MARKERS = (
    "AIRPORTS_DB_HAS_ALIAS",
    "AIRPORTS_DB_HAS_MUNICIPALITY",
    "char alias[5]",
    "char name[64]",
    "char municipality[48]",
)


def header_is_current(path: Path) -> bool:
    if not path.is_file():
        return False
    try:
        head = path.read_text(encoding="utf-8", errors="replace")[:1600]
    except OSError:
        return False
    return all(m in head for m in SCHEMA_MARKERS)


def download_csv(url: str) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": "ADS-B-Display-Airports-DB/1.0"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return resp.read().decode("utf-8")


def c_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def to_ascii(s: str) -> str:
    # Montserrat has no Arabic/CJK/etc.; fold accents then drop non-ASCII.
    # OurAirports often glues tokens with '/' or a Unicode dash (U+2013)
    # and no spaces — e.g. "São Paulo/Guarulhos–Governor …". Stripping the
    # dash without a replacement yields "GuarulhosGovernor". Normalize those
    # separators to spaced ASCII first so truncate and UI wrap can break.
    s = s or ""
    for ch in ("\u2013", "\u2014", "\u2212", "\u2010", "\u2011"):
        s = s.replace(ch, " - ")
    s = s.replace("/", " / ")
    nk = unicodedata.normalize("NFKD", s)
    ascii_s = "".join(c for c in nk if 32 <= ord(c) < 127)
    return " ".join(ascii_s.split())


def truncate_field(name: str, max_len: int) -> str:
    name = to_ascii(name)  # already whitespace-collapsed
    if len(name) <= max_len:
        return name
    budget = max_len - 3  # room for "..."
    head = name[:budget]
    # Prefer " / " (city/airport pairs) then a plain space.
    cut = None
    for sep in (" / ", " "):
        if sep in head:
            cand = head.rsplit(sep, 1)[0]
            if len(cand) >= 12:
                cut = cand
                break
    if cut is None:
        cut = head
    return cut + "..."


def truncate_name(name: str) -> str:
    return truncate_field(name, NAME_MAX)


def truncate_muni(name: str) -> str:
    return truncate_field(name, MUNI_MAX)


def valid_code(code: str) -> bool:
    """OurAirports ICAO / local idents used as icao[5] keys (len ≤ 4)."""
    return bool(code) and len(code) <= 4 and code.isalnum()


def parse_airports(csv_text: str):
    airports = []
    seen_primary = set()
    reader = csv.DictReader(io.StringIO(csv_text))
    for row in reader:
        if row.get("type") not in INCLUDED_TYPES:
            continue
        ident = (row.get("ident") or "").strip().upper()
        icao_code = (row.get("icao_code") or "").strip().upper()

        # Prefer official ICAO (flight APIs / AeroDataBox). Fall back to ident
        # when icao_code is blank. Keep ident as alias when both exist and differ
        # so SPJC and SPIM both resolve without a duplicate map glyph.
        if valid_code(icao_code):
            primary = icao_code
            alias = ident if valid_code(ident) and ident != icao_code else ""
        elif valid_code(ident):
            primary = ident
            alias = ""
        else:
            continue  # skip non-ICAO idents (AU-0539, IN-0276, …) with no icao_code

        if primary in seen_primary:
            continue
        seen_primary.add(primary)

        try:
            lat = float(row["latitude_deg"])
            lon = float(row["longitude_deg"])
        except (KeyError, ValueError):
            continue
        # Prefer official name; fall back to municipality if blank.
        raw_muni = (row.get("municipality") or "").strip()
        raw_name = (row.get("name") or "").strip() or raw_muni or primary
        airports.append({
            "icao": primary,
            "alias": alias,
            "name": truncate_name(raw_name),
            "municipality": truncate_muni(raw_muni),
            "lat": lat,
            "lon": lon,
            "large": 1 if row["type"] == "large_airport" else 0,
        })
    return airports


def write_header(airports, output_path: str):
    with open(output_path, "w") as f:
        f.write("// Auto-generated static airport glyph + name DB\n")
        f.write(f"// Source: {AIRPORTS_CSV_URL}\n")
        f.write("// Generated by tools/generate_airports_db.py\n")
        f.write(f"// {len(airports)} airports (large_airport + medium_airport)\n")
        f.write("#pragma once\n\n")
        f.write("// Schema flags — callers reject older gitignored headers.\n")
        f.write("#define AIRPORTS_DB_HAS_ALIAS 1\n")
        f.write("#define AIRPORTS_DB_HAS_MUNICIPALITY 1\n\n")
        f.write("struct StaticAirport {\n")
        f.write("    char icao[5];  // preferred OurAirports icao_code (else ident)\n")
        f.write("    char alias[5]; // alternate ident when it differs from icao\n")
        f.write("    char name[64]; // ASCII-folded OurAirports name (may end in ...)\n")
        f.write("    char municipality[48]; // city/locality for compact UI (may be empty)\n")
        f.write("    float lat, lon;\n")
        f.write("    unsigned char large; // 1 = large_airport, 0 = medium_airport\n")
        f.write("};\n\n")
        f.write(f"#define AIRPORTS_DB_COUNT {len(airports)}\n\n")
        f.write(f"static const StaticAirport airports_db[AIRPORTS_DB_COUNT] = {{\n")
        for ap in airports:
            f.write(
                f'    {{"{ap["icao"]}", "{ap["alias"]}", "{c_escape(ap["name"])}", '
                f'"{c_escape(ap["municipality"])}", '
                f'{ap["lat"]:.6f}f, {ap["lon"]:.6f}f, {ap["large"]}}},\n'
            )
        f.write("};\n")

    # icao[5]+alias[5]+name[64]+muni[48]+floats+large+pad ≈ 136 bytes/entry
    size_estimate = len(airports) * 136
    print(f"Wrote {len(airports)} airports to {output_path} (~{size_estimate/1024:.1f} KB)")


def main():
    parser = argparse.ArgumentParser(description="Generate static airport glyph DB for ADS-B display")
    parser.add_argument("--output", type=str, default=None,
                        help="Output path (default: src/ui/airports_db.h)")
    parser.add_argument(
        "--ensure",
        action="store_true",
        help="Only download/regenerate when the header is missing or schema-stale",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    output = Path(args.output or (repo_root / "src" / "ui" / "airports_db.h"))

    if args.ensure and header_is_current(output):
        print(f"airports_db.h schema OK ({output})")
        return

    print(f"Downloading {AIRPORTS_CSV_URL} ...")
    try:
        csv_text = download_csv(AIRPORTS_CSV_URL)
    except Exception as e:
        print(f"ERROR: failed to download airports.csv: {e}", file=sys.stderr)
        sys.exit(1)

    airports = parse_airports(csv_text)
    if not airports:
        print("ERROR: no airports parsed — check the CSV format/URL", file=sys.stderr)
        sys.exit(1)

    write_header(airports, str(output))


if __name__ == "__main__":
    main()
