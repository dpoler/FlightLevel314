# FlightLevel314 — Agent Knowledge Dump

> historical archive in [`project-knowledge.md`](project-knowledge.md)
> (ESP32 / CrowPanel / early Pi port). Prefer **this file** for current
> product facts; verify against code before trusting any “done” claim.

**Owner:** Dan (dpoler). Prefers working directly in code; builds himself
unless asked. Cut/paste from agent chat can mangle commands — keep them
on separate lines.

---

## 1. What this is

**FlightLevel314** — live ADS-B aircraft display for a Raspberry Pi
touchscreen (Waveshare 10.1″ DSI, **1280×800**). Name: Flight Level 314… π.

Views: **MAP** | **RADAR** | **LIST** | **INFO**, plus gear → **Settings**,
status-bar range chip, **VIEW** overlay menu, location picker, detail card.

Traffic: [adsb.lol](https://adsb.lol) (default) or [adsb.fi](https://adsb.fi).
Optional: AirportDB.io runways, AeroDataBox O/D, CARTO basemap key,
Planespotters photos, OpenTravelData airline names, METAR/ATIS on INFO.

---

## 2. Repos & lineage

| Repo | Role |
|------|------|
| **https://github.com/dpoler/FlightLevel314** | Canonical. Pi / Linux only. `master` |
| **https://github.com/dpoler/adsb** | Historical dual-target (ESP32 jc1060 + Pi). **Paused** for day-to-day |

Forked from `dpoler/adsb` Pi port **2026-08-08**. ESP32 / PlatformIO / jc1060
are **not** first-class targets here. Cherry-picks back to adsb are fine later.

Day-to-day agent rules: root [`AGENTS.md`](../AGENTS.md).

---

## 3. Hardware & runtime

| Item | Notes |
|------|--------|
| Display | Waveshare 10.1″ DSI capacitive (1280×800) |
| Board | Pi 5 / 4B typical |
| OS (kiosk) | Raspberry Pi OS Lite; app owns DRM/KMS |
| Binary | `build/pi/flightlevel314` → install `/opt/flightlevel314/flightlevel314` |
| Config (dev) | `~/.config/flightlevel314/{config.json,locations.json}` |
| Config (kiosk) | `/opt/flightlevel314/.config/flightlevel314/` (`User=flightlevel314`, `HOME=/opt/…`) |
| Service | `pi/flightlevel314.service` |

**Footgun:** editing login-user `~/.config/…` does **not** change the kiosk
service config under `/opt/…`.

Brightness: Settings → Display slider → `/sys/class/backlight/*/brightness`
(needs udev write perms on Pi; no-op on SDL/HDMI without backlight sysfs).

---

## 4. Build & cloud env

```bash
# SDL simulator (Mac / Linux / Cloud)
cmake -S . -B build -DPI_DISPLAY_BACKEND=SDL
cmake --build build -j$(nproc)
./build/pi/flightlevel314

# Pi DRM kiosk (real panel; out of scope for Cloud VMs)
cmake -S . -B build -DPI_DISPLAY_BACKEND=DRM
```

Deps: `cmake`, `libcurl4-openssl-dev`, `libsdl2-dev` (SDL); plus
`libdrm-dev`, `libinput-dev` (DRM).

an SDL build. No C++ unit tests / lint; CI builds SDL on version tags.
Verification signal = clean CMake SDL build (+ UI when Computer Use / display
available).

**Standing preference:** user handles builds unless asked — prefer writing
code over drive-by rebuilds.

LVGL **v9.5.0** via FetchContent. CMake runs patches:
- `pi/patches/apply_lvgl_drm_patch.py` — DRM atomic hang fix
- `pi/patches/apply_lvgl_decoder_assert_patch.py` — image decoder assert soft-fail

---

## 5. Tree layout (Pi-only)

```
pi/                  # Linux entry: main, display_sdl/drm, input, basemap, weather,
                     # settings_pi.cpp, backlight, platform_linux/*, patches, service
src/ui/              # Shared LVGL UI (map, radar, list/arrivals, stats/info,
                     # detail_card, view_menu, location_picker, filters, …)
src/data/            # Headers + some .cpp (enrichment API, error_log, storage shape,
                     # fetcher stats contracts, ota, metar, atis, airlines, …)
src/platform/        # platform.h abstractions
tools/               # set_api_keys.py, generate_airports_db.py, generate_static_map.py
docs/                # project-knowledge.md (archive), screenshots/, this dump
```

`pi/settings_pi.cpp` is a **Pi-specific** Settings implementation of
`src/ui/settings.h` — deliberately **not** a port of ESP32 `settings.cpp`.

Codegen (gitignored headers under `src/ui/`):
- `generate_airports_db.py` → `airports_db.h` (CMake also `--ensure`s)
- `generate_static_map.py` → `static_map_data.h`

---

## 6. Views & intentional UX rules

| View | Behavior |
|------|----------|
| **MAP** | Full rectangular canvas; aircraft drawable/tappable beyond range radius to edges. No bullseye rings. Airports/runways. Basemap + optional weather. |
| **RADAR** | Circular clip to range; rings + sweep + blips. No airport/runway drawing. |
| **LIST** | Sortable traffic board (`arrivals_view`) |
| **INFO** | Session / METAR / ATIS layout (airport mode) |

**Do not** add Map radius clip or loosen Radar’s circular clip — deliberate.

Filters: category COM/GA/HELI/MIL/EMG = OR; VERT/HIGH/LOW = AND with category;
GND = separate hide (defaults hidden on fresh config).

Commercial traffic / airliner icon / ADB O/D eligibility share
`is_commercial_traffic`: airline callsign **and** emitter A2–A6; empty
category → not commercial / no O/D.

VIEW menu: trails, tags, basemap style, weather, rebuild map, etc.
(Map VIEW is two-column on Pi.)

---

## 7. Settings UI (current — merged PR #38, 2026-09-23)

Three tabs, narrow panel (~**560×660**), footer **Cancel / Save** only.

| Tab | Contents |
|-----|----------|
| **Display** | Range presets (nm), metric toggle, brightness slider |
| **Services** | Traffic provider; AirportDB / AeroDataBox / CARTO presence·valid·enable; top line “API keys are set with set_api_keys.py” |
| **System** | OTA check, HOST info (hw/OS/hostname/arch/sys uptime), diagnostics, Clear caches \| Reset to defaults |

### Draft / live-preview semantics
- Most fields draft until **Save**; **Cancel** restores `_cfg_at_open`.
- Live-preview (Cancel reverts): **brightness**, **traffic provider**,
  **ADB gateway**, **Enable** switches for AirportDB / AeroDataBox.
- Settings cannot dismiss without Save or Cancel (no backdrop tap-dismiss).

### Enable switches (important bug fixed in #38)
Opening Settings used to force Enable Off while key verify was `Checking`,
clearing draft enable but leaving `g_config.*_enabled` true → APIs still
ran. Fix: keep draft during Checking; live-apply Enable to `g_config`;
Missing/Invalid force live Off. Enable cannot turn on unless VALID = yes.

### Help `?` dialogs
Custom dark compact dialog (not LVGL default white/blue msgbox). ASCII-only
copy (montserrat tofu on em dash / middle-dot). Keys not explained in each
popup — one top-of-Services line points at `set_api_keys.py`.

### System diagnostics labels
- **ADS-B POLLS** — traffic feed ok/err counts (`FetcherStats`)
- **LAST POLL** — last successful ADS-B fetch duration
- **APP UPTIME** — process uptime
- **APP ERRORS** — ring buffer for airlines load / airport add / runway
  refresh (not the ADS-B err counter)

Tab bleed fix: tab pages stacked + hidden (not sideways-scrolled LVGL
tabview content).

---

## 8. Config & API keys

Keys are **never typed on the touchscreen**. Provision with:

```bash
sudo python3 tools/set_api_keys.py \
  --apt-tok '…' --adbox-key '…' --carto-key '…'
# --adbox-prov 0|1|2   RapidAPI | API.Market | Direct
# --adbox-renew-day N  billing anniversary day for USAGE line
# --show
```

Config JSON keys (among others): `apt_tok` / `apt_en`, `adbox_key` /
`adbox_en` / `adbox_prov` / usage + soft-cap fields, `carto_key`,
`traffic_prov`, radius presets, view filters, basemap/weather opacities, …

### AeroDataBox cost notes (from 2026-09 work)
- Settings key verify uses **FREE** `/health/services/feeds/FlightSchedules`
  (0 units) — PR #37.
- Live O/D flight status is Tier‑2 class (≥ **2 units** per lookup).
- Marketplace Basic often **600 units/month**; USAGE prefers header
  `x-ratelimit-api-units-*` when present; soft-cap / 429 / 0 remaining
  auto-disable Enable.
- Billing month = signup anniversary (`--adbox-renew-day`), not header
  “requests reset”.

### CARTO
Free key from carto.com/basemaps/apikey. Without it, tiles watermark
“API key required”. After setting key: **VIEW → Rebuild map** (or restart).

---

## 9. Enrichment / O/D

Pi: `pi/platform_linux/enrichment_linux.cpp`. Stages include adsbdb-ish
identity, Planespotters photo, AeroDataBox route when `adbox_allowed()`.

Route pick prefers live-looking flights (EnRoute/Approaching/…) vs first
row with airports. CallSign-first search; commercial filter gates API.

Open hygiene backlog (display-only): hide implausible low-altitude O/D near
an airport that isn’t on the published route (see project-knowledge §8).

---

## 10. Basemap & weather

`pi/basemap.cpp` — mosaic for one `(lat, lon, range, style)`, not a slippy
map. Rebuild on location/range/style change; cache on disk.

Weather radar overlay optional (`pi/weather.cpp`); intensity floor tweaks
have landed in recent PRs.

**Follow Mode** (not built): tracking a flight would fight mosaic-recenter
vs ADS-B query-center — design notes in project-knowledge §7.1; **hold**.

---

## 11. OTA

GitHub Releases check / Settings “Check for update” / install + systemd
restart on Pi (`ota` / `ota_linux`). Dev builds report `v0.0.0-dev` so they
never claim “up to date” falsely.

---

## 12. Open backlog (do not start unless Dan asks)

Highest-signal leftovers (verify §7.1 in project-knowledge before acting):

- **Follow Mode** — hold; design notes only
- Detail card: STD/ATD/STA/ATA + diverted; reclaim blank telemetry rows
- Enrichment O/D low-altitude hygiene near airports (display filter)
- Satellite basemap style (Esri/Mapbox; key OK)
- Optional fresher README LIST/INFO screenshots
- QoL themes / font size

Deferred: small airports in static DB; airframes.io ACARS O/D.

Recently closed highlights: ADB free verify (#37); Settings three tabs (#38);
commercial traffic definition; ADB USAGE headers + renew day; bullseye
declutter; INFO METAR/ATIS; DRM hang patch; Map hang decoder soft-fail;
adsb.fi provider; CARTO key path; etc.

---

## 13. Cross-cutting lessons

- Map ≠ Radar visibility — intentional.
- Don’t guess pixel layout from source alone — measure (ruler / photo).
- Montserrat font: avoid Unicode middle-dot / em dash → tofu rectangles.
- Kiosk HOME vs login HOME for config.
- Cloud agent from wrong GitHub App install cannot push the other repo.
- LVGL: delete-from-event-handler → `lv_obj_delete_async`; switch hit-test
  vs `lv_obj_set_size` desync; tabview sideways peek → stack/hide pages.
- Prefer draft-until-Save Settings; live-preview only where Cancel can
  restore (brightness, traffic, enable, ADB provider).

---

## 14. This agent session (2026-09-20 → 2026-09-23) — what landed

1. **PR #37** — AeroDataBox Settings verify via free health endpoint (merged).
2. **PR #38** — Settings Display | Services | System (merged to `master`
   as `fe6bdd2`):
   - Narrow panel; Device → System; Clear/Reset on System (side-by-side)
   - Tab bleed fixed; help dialogs restyled dark/compact
   - Enable live-gate bugfix; Services spacing; diagnostics labels
   - AirportDB help shortened; CARTO em-dash tofu fixed


---

## 15. Quick commands cheat-sheet

```bash
# Build + run SDL
cmake -S . -B build -DPI_DISPLAY_BACKEND=SDL && cmake --build build -j$(nproc)
./build/pi/flightlevel314

# Keys (kiosk)
sudo python3 tools/set_api_keys.py --show
sudo python3 tools/set_api_keys.py --adbox-key '…' --adbox-renew-day 9

# Service
journalctl -u flightlevel314 -f
sudo systemctl restart flightlevel314

# Airports DB regen
python3 tools/generate_airports_db.py
```

---

*End of 2026-09-23 dump. For ESP32 / CrowPanel / early port archaeology,
see [`project-knowledge.md`](project-knowledge.md).*
