#!/usr/bin/env python3
"""Generate static map background C arrays for ADS-B display.

Downloads OSM tiles for a given location at multiple zoom levels, stitches
them, crops to viewport, converts to RGB565, and outputs a C header file.

Usage:
    python3 tools/generate_static_map.py [--lat LAT] [--lon LON] [--output PATH]

Defaults to HOME_LAT/HOME_LON from src/config.h if not specified.
Tile source: CartoDB dark_all (dark basemap matching the UI theme).
"""

import argparse
import io
import math
import struct
import sys
import time
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow required. Install with: pip3 install Pillow", file=sys.stderr)
    sys.exit(1)

try:
    import requests
except ImportError:
    print("ERROR: requests required. Install with: pip3 install requests", file=sys.stderr)
    sys.exit(1)

# Display dimensions (must match firmware)
CANVAS_W = 1024
CANVAS_H = 570
TILE_PX = 256

# All range levels (must match range.h)
RANGE_LEVELS = [150, 100, 50, 20, 5, 1]

TILE_URL = "https://basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png"
USER_AGENT = "ADS-B-Display-Static-Map/1.0"


def osm_lon_to_x(lon: float, z: int) -> int:
    return int(math.floor((lon + 180.0) / 360.0 * (1 << z)))


def osm_lat_to_y(lat: float, z: int) -> int:
    lat_rad = math.radians(lat)
    return int(math.floor((1.0 - math.log(math.tan(lat_rad) + 1.0 / math.cos(lat_rad)) / math.pi) / 2.0 * (1 << z)))


def osm_zoom_for_radius(radius_nm: float, screen_h: int, center_lat: float) -> int:
    """Match firmware's osm_zoom_for_radius() exactly."""
    our_ppd = screen_h / (radius_nm * 2.0) * 60.0
    cos_lat = math.cos(math.radians(center_lat))
    best_z = 4
    best_diff = 1e9
    for z in range(4, 16):
        osm_ppd = 256.0 * (1 << z) / 360.0 * cos_lat
        diff = abs(math.log(osm_ppd / our_ppd))
        if diff < best_diff:
            best_diff = diff
            best_z = z
    return best_z


def pixel_of_coord(lat: float, lon: float, z: int) -> tuple[float, float]:
    """Convert lat/lon to absolute pixel coordinates at zoom z."""
    n = 1 << z
    px = (lon + 180.0) / 360.0 * n * TILE_PX
    lat_rad = math.radians(lat)
    py = (1.0 - math.log(math.tan(lat_rad) + 1.0 / math.cos(lat_rad)) / math.pi) / 2.0 * n * TILE_PX
    return px, py


def download_tile(z: int, x: int, y: int, max_retries: int = 3) -> Image.Image:
    """Download a single OSM tile with retries."""
    url = TILE_URL.format(z=z, x=x, y=y)
    for attempt in range(max_retries):
        try:
            resp = requests.get(url, headers={"User-Agent": USER_AGENT}, timeout=10)
            if resp.status_code == 200:
                return Image.open(io.BytesIO(resp.content)).convert("RGB")
            elif resp.status_code == 404:
                return Image.new("RGB", (TILE_PX, TILE_PX), (10, 10, 26))
            else:
                print(f"  HTTP {resp.status_code} for tile {z}/{x}/{y}, retry {attempt+1}")
        except Exception as e:
            print(f"  Error fetching tile {z}/{x}/{y}: {e}, retry {attempt+1}")
        time.sleep(0.5 * (attempt + 1))
    print(f"  WARNING: Failed to fetch tile {z}/{x}/{y}, using blank")
    return Image.new("RGB", (TILE_PX, TILE_PX), (10, 10, 26))


def rgb888_to_rgb565(r: int, g: int, b: int) -> int:
    """Convert RGB888 to RGB565."""
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def generate_one_zoom(lat: float, lon: float, radius_nm: int, out_w: int, out_h: int) -> tuple[int, bytearray]:
    """Generate map data for one zoom level. Returns (osm_zoom, rgb565_bytes)."""
    z = osm_zoom_for_radius(radius_nm, CANVAS_H, lat)
    print(f"\n--- {radius_nm}nm (OSM zoom {z}, output {out_w}x{out_h}) ---")

    cx, cy = pixel_of_coord(lat, lon, z)

    # Viewport in absolute pixels (at full canvas size for correct centering)
    vp_left = cx - CANVAS_W / 2
    vp_top = cy - CANVAS_H / 2
    vp_right = vp_left + CANVAS_W
    vp_bottom = vp_top + CANVAS_H

    tx_min = int(math.floor(vp_left / TILE_PX))
    tx_max = int(math.floor((vp_right - 1) / TILE_PX))
    ty_min = int(math.floor(vp_top / TILE_PX))
    ty_max = int(math.floor((vp_bottom - 1) / TILE_PX))

    n_tiles = (tx_max - tx_min + 1) * (ty_max - ty_min + 1)
    print(f"Tiles: {tx_max - tx_min + 1}x{ty_max - ty_min + 1} = {n_tiles}")

    stitch_w = (tx_max - tx_min + 1) * TILE_PX
    stitch_h = (ty_max - ty_min + 1) * TILE_PX
    stitched = Image.new("RGB", (stitch_w, stitch_h))

    count = 0
    n = 1 << z
    for ty in range(ty_min, ty_max + 1):
        for tx in range(tx_min, tx_max + 1):
            count += 1
            wrapped_tx = tx % n
            if wrapped_tx < 0:
                wrapped_tx += n
            print(f"  [{count}/{n_tiles}] tile {z}/{wrapped_tx}/{ty}")
            tile_img = download_tile(z, wrapped_tx, ty)
            paste_x = (tx - tx_min) * TILE_PX
            paste_y = (ty - ty_min) * TILE_PX
            stitched.paste(tile_img, (paste_x, paste_y))
            time.sleep(0.05)

    # Crop to full canvas viewport
    crop_left = int(vp_left - tx_min * TILE_PX)
    crop_top = int(vp_top - ty_min * TILE_PX)
    cropped = stitched.crop((crop_left, crop_top, crop_left + CANVAS_W, crop_top + CANVAS_H))

    # Resize to output dimensions
    if out_w != CANVAS_W or out_h != CANVAS_H:
        cropped = cropped.resize((out_w, out_h), Image.LANCZOS)

    # Convert to RGB565
    pixels = list(cropped.getdata())
    rgb565_data = bytearray(out_w * out_h * 2)
    for i, (r, g, b) in enumerate(pixels):
        val = rgb888_to_rgb565(r, g, b)
        rgb565_data[i * 2] = val & 0xFF
        rgb565_data[i * 2 + 1] = (val >> 8) & 0xFF

    print(f"  Data: {len(rgb565_data)} bytes ({len(rgb565_data)/1024:.1f} KB)")
    return z, rgb565_data


def generate_all(lat: float, lon: float, out_w: int, out_h: int, output_path: str):
    """Generate static maps for all zoom levels."""
    zoom_data = []  # (radius_nm, osm_zoom, rgb565_bytes)

    for radius in RANGE_LEVELS:
        z, data = generate_one_zoom(lat, lon, radius, out_w, out_h)
        zoom_data.append((radius, z, data))

    total_bytes = sum(len(d) for _, _, d in zoom_data)
    print(f"\nTotal data: {total_bytes} bytes ({total_bytes/1024:.1f} KB, {total_bytes/1024/1024:.2f} MB)")

    # Write C header
    with open(output_path, "w") as f:
        f.write(f"// Auto-generated static map backgrounds for ({lat}, {lon})\n")
        f.write(f"// Tile source: CartoDB dark_all\n")
        f.write(f"// Generated by tools/generate_static_map.py\n")
        f.write(f"// Total size: {total_bytes} bytes ({total_bytes/1024:.1f} KB)\n")
        f.write(f"#pragma once\n")
        f.write(f"#include <cstdint>\n\n")
        f.write(f"#define STATIC_MAP_W {out_w}\n")
        f.write(f"#define STATIC_MAP_H {out_h}\n")
        f.write(f"#define STATIC_MAP_STRIDE ({out_w} * 2)\n")
        f.write(f"#define STATIC_MAP_COUNT {len(zoom_data)}\n\n")

        # Each zoom level's data array
        for radius, osm_z, data in zoom_data:
            var_name = f"static_map_{radius}nm_data"
            f.write(f"// {radius}nm — OSM zoom {osm_z}, {len(data)} bytes\n")
            f.write(f"static const uint8_t {var_name}[{len(data)}] PROGMEM = {{\n")
            for row_start in range(0, len(data), 32):
                chunk = data[row_start:row_start + 32]
                hex_vals = ", ".join(f"0x{b:02x}" for b in chunk)
                f.write(f"    {hex_vals},\n")
            f.write(f"}};\n\n")

        # Lookup table: radius_nm → data pointer
        f.write(f"struct StaticMapEntry {{\n")
        f.write(f"    int radius_nm;\n")
        f.write(f"    const uint8_t *data;\n")
        f.write(f"    uint32_t data_size;\n")
        f.write(f"}};\n\n")
        f.write(f"static const StaticMapEntry static_maps[{len(zoom_data)}] = {{\n")
        for radius, osm_z, data in zoom_data:
            f.write(f"    {{ {radius}, static_map_{radius}nm_data, {len(data)} }},\n")
        f.write(f"}};\n")

    print(f"\nWritten to: {output_path}")


def parse_config_h(config_path: str) -> tuple[float, float]:
    """Extract HOME_LAT and HOME_LON from config.h."""
    lat, lon = None, None
    with open(config_path) as f:
        for line in f:
            if "#define HOME_LAT" in line:
                lat = float(line.split()[-1].rstrip("f"))
            elif "#define HOME_LON" in line:
                lon = float(line.split()[-1].rstrip("f"))
    if lat is None or lon is None:
        raise ValueError(f"Could not find HOME_LAT/HOME_LON in {config_path}")
    return lat, lon


def main():
    parser = argparse.ArgumentParser(description="Generate static map backgrounds for ADS-B display")
    parser.add_argument("--lat", type=float, help="Center latitude (default: from config.h)")
    parser.add_argument("--lon", type=float, help="Center longitude (default: from config.h)")
    parser.add_argument("--width", type=int, default=512, help="Output width per map (default: 512)")
    parser.add_argument("--height", type=int, default=285, help="Output height per map (default: 285)")
    parser.add_argument("--output", type=str, default=None,
                        help="Output path (default: src/ui/static_map_data.h)")
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parent

    if args.lat is None or args.lon is None:
        config_path = repo_root / "src" / "config.h"
        lat, lon = parse_config_h(str(config_path))
        if args.lat is not None:
            lat = args.lat
        if args.lon is not None:
            lon = args.lon
        print(f"Using location from config.h: ({lat}, {lon})")
    else:
        lat, lon = args.lat, args.lon

    output = args.output or str(repo_root / "src" / "ui" / "static_map_data.h")

    generate_all(lat, lon, args.width, args.height, output)


if __name__ == "__main__":
    main()
