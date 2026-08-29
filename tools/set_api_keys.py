#!/usr/bin/env python3
"""Merge AirportDB / AeroDataBox / CARTO API keys into FlightLevel314 config.json.

Secrets are never typed on the touchscreen. This script is the supported
SSH/kiosk path for putting apt_tok / adbox_key / carto_key onto the Pi.

Default target (systemd kiosk):
  /opt/flightlevel314/.config/flightlevel314/config.json

Examples:
  sudo python3 tools/set_api_keys.py --apt-tok TOKEN --adbox-key KEY
  sudo python3 tools/set_api_keys.py --carto-key KEY
  sudo python3 tools/set_api_keys.py --config /path/to/config.json --apt-tok TOKEN
  python3 tools/set_api_keys.py --print-path

After writing keys, open Settings → API KEYS (VALID / ENABLE) or restart:
  sudo systemctl restart flightlevel314

CARTO: free key from https://carto.com/basemaps/apikey — required for
dark / voyager raster basemap styles (otherwise tiles are watermarked
"API key required"). Then VIEW → Basemap → Rebuild map (or wait for
cache miss) so watermarked mosaics are replaced.
"""

from __future__ import annotations

import argparse
import json
import os
import pwd
import sys
from pathlib import Path


DEFAULT_KIOSK_CONFIG = Path(
    "/opt/flightlevel314/.config/flightlevel314/config.json"
)


def resolve_config_path(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit).expanduser().resolve()
    # Prefer the systemd kiosk file when it (or its parent install) exists.
    if DEFAULT_KIOSK_CONFIG.parent.is_dir() or Path("/opt/flightlevel314").is_dir():
        return DEFAULT_KIOSK_CONFIG
    xdg = os.environ.get("XDG_CONFIG_HOME")
    if xdg:
        return Path(xdg) / "flightlevel314" / "config.json"
    home = os.environ.get("HOME") or str(Path.home())
    return Path(home) / ".config" / "flightlevel314" / "config.json"


def load_json(path: Path) -> dict:
    if not path.is_file():
        return {}
    try:
        with path.open("r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, dict):
            return data
    except (OSError, json.JSONDecodeError) as e:
        print(f"error: cannot parse {path}: {e}", file=sys.stderr)
        sys.exit(1)
    print(f"error: {path} is not a JSON object", file=sys.stderr)
    sys.exit(1)


def maybe_chown_kiosk(path: Path) -> None:
    if os.geteuid() != 0:
        return
    try:
        pw = pwd.getpwnam("flightlevel314")
    except KeyError:
        return
    # Own config dir + file so the service can rewrite on Save.
    for p in (path.parent, path):
        if p.exists():
            os.chown(p, pw.pw_uid, pw.pw_gid)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Set FlightLevel314 API keys in config.json (SSH/kiosk)."
    )
    ap.add_argument(
        "--config",
        help="Path to config.json (default: kiosk path if present, else ~/.config/...)",
    )
    ap.add_argument("--apt-tok", help="AirportDB.io token → apt_tok")
    ap.add_argument("--adbox-key", help="AeroDataBox API key → adbox_key")
    ap.add_argument(
        "--carto-key",
        help="CARTO basemap API key → carto_key (free: carto.com/basemaps/apikey)",
    )
    ap.add_argument(
        "--adbox-prov",
        type=int,
        choices=(0, 1, 2),
        help="AeroDataBox gateway: 0=RapidAPI, 1=API.Market, 2=Direct",
    )
    ap.add_argument(
        "--print-path",
        action="store_true",
        help="Print the resolved config path and exit",
    )
    ap.add_argument(
        "--show",
        action="store_true",
        help="Show whether keys are present (not the values) and exit",
    )
    args = ap.parse_args()

    path = resolve_config_path(args.config)
    if args.print_path:
        print(path)
        return 0

    doc = load_json(path)
    if args.show:
        apt = bool(doc.get("apt_tok"))
        adb = bool(doc.get("adbox_key"))
        carto = bool(doc.get("carto_key"))
        print(f"{path}")
        print(f"  apt_tok:   {'present' if apt else 'missing'}")
        print(f"  adbox_key: {'present' if adb else 'missing'}")
        print(f"  carto_key: {'present' if carto else 'missing'}")
        if "adbox_prov" in doc:
            print(f"  adbox_prov: {doc.get('adbox_prov')}")
        return 0

    if (
        args.apt_tok is None
        and args.adbox_key is None
        and args.adbox_prov is None
        and args.carto_key is None
    ):
        ap.error(
            "pass --apt-tok / --adbox-key / --carto-key (and optional --adbox-prov), "
            "or use --show / --print-path"
        )

    if args.apt_tok is not None:
        doc["apt_tok"] = args.apt_tok.strip()
    if args.adbox_key is not None:
        doc["adbox_key"] = args.adbox_key.strip()
    if args.carto_key is not None:
        doc["carto_key"] = args.carto_key.strip()
    if args.adbox_prov is not None:
        doc["adbox_prov"] = args.adbox_prov

    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".json.tmp")
    with tmp.open("w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    tmp.replace(path)
    maybe_chown_kiosk(path)

    print(f"Updated {path}")
    print(f"  apt_tok:   {'set' if doc.get('apt_tok') else 'empty'}")
    print(f"  adbox_key: {'set' if doc.get('adbox_key') else 'empty'}")
    print(f"  carto_key: {'set' if doc.get('carto_key') else 'empty'}")
    print("Next: open Settings → API KEYS (VALID / ENABLE), or:")
    print("  sudo systemctl restart flightlevel314")
    if doc.get("carto_key"):
        print("For basemap: VIEW → Basemap → Rebuild map (clears watermarked cache).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
