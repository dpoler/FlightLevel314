# ADS-B Radar Display

Real-time aircraft tracker on an ESP32-P4 with a 1024x600 touchscreen. Pulls live ADS-B data from [adsb.lol](https://api.adsb.lol) and displays aircraft on four swipeable views.

![Views](https://img.shields.io/badge/views-Map%20%7C%20Radar%20%7C%20Arrivals%20%7C%20Stats-blue)
![Platform](https://img.shields.io/badge/platform-ESP32--P4-green)
![License](https://img.shields.io/badge/license-MIT-brightgreen)

## Views

- **Map** — Top-down map with aircraft icons (airliner/jet/GA/heli), altitude-colored trails, and static pre-rendered OSM backgrounds at 6 zoom levels
- **Radar** — Rotating sweep with phosphor-style blips, paint-detail zone showing callsign/route/altitude as the sweep passes each aircraft
- **Arrivals** — Split-flap departure board with animated character flips, showing callsign, route, type, altitude, speed, distance, and status
- **Stats** — System health dashboard (heap, PSRAM, temperature, FPS, RTOS tasks, LVGL objects, flash), network stats (IP, fetch/enrich counts, latency, RSSI), and session tracking (unique aircraft, peak count, altitude/speed distributions, top airlines)

All views support:
- Tap any aircraft to open a scrollable detail card with enriched data (operator, registration, type, route, photo credits)
- Filter toggles: COM / MIL / EMG / HELI / FAST / SLOW / ODD
- Adjustable range: 150 / 100 / 50 / 20 / 5 / 1 nm
- Auto-cycle between views with configurable interval and touch-pause

## Hardware

**Board:** JC1060P470C (ESP32-P4 RISC-V, 32MB PSRAM, 16MB flash)
- 1024x600 MIPI-DSI display (JD9165 controller)
- GT911 capacitive touchscreen
- Built-in 100Mbps Ethernet (IP101 PHY)
- ESP32-C6 WiFi module (SDIO hosted)

Any ESP32-P4 board with a MIPI-DSI display should work with pin adjustments in `src/pins_config.h`.

## Setup

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- Vendor HAL files in `components/vendor_hal/` (board-specific, not included)

### Configuration

1. Copy the config template:
   ```bash
   cp src/config.h.example src/config.h
   ```

2. Edit `src/config.h` with your settings:
   ```c
   // Network: uncomment ONE
   #define USE_ETHERNET      // Built-in Ethernet
   // #define USE_WIFI       // WiFi via ESP32-C6

   // WiFi credentials (if using WiFi)
   #define WIFI_SSID "your_ssid"
   #define WIFI_PASS "your_password"

   // Your location (used as radar center)
   #define HOME_LAT 40.7128
   #define HOME_LON -74.0060
   ```

### Static Map Backgrounds (Optional)

Generate pre-rendered map tiles for the map view background:

```bash
pip install requests Pillow
python tools/generate_static_map.py --lat YOUR_LAT --lon YOUR_LON
```

This creates `src/ui/static_map_data.h` with 6 zoom levels. The map view works without it (shows a plain background).

### Build & Flash

```bash
pio run -e jc1060
pio run -e jc1060 -t upload --upload-port /dev/ttyACM0
pio device monitor -p /dev/ttyACM0
```

## Settings

Tap the gear icon in the status bar to open the settings panel. All settings persist to NVS (non-volatile storage):

- **WiFi SSID/Password** — for WiFi mode
- **Home Location** — latitude/longitude (radar center point)
- **Default Radius** — 5-150nm
- **Metric Units** — toggle km/knots display
- **Aircraft Trails** — toggle trail rendering on map and radar
- **Trail Length** — 10-60 points
- **Auto-Cycle** — automatically rotate through views
- **Cycle Interval** — 15-120 seconds per view

## Data Sources

| Source | Purpose |
|--------|---------|
| [api.adsb.lol/v2/point](https://api.adsb.lol) | Live aircraft positions (bulk, no API key) |
| [api.adsbdb.com](https://www.adsbdb.com) | Aircraft registration, type, operator enrichment |
| [Planespotters.net](https://www.planespotters.net) | Aircraft photo credits |

## Architecture

```
src/
  main.cpp              — Hardware init, LVGL setup, boot sequence
  config.h              — User configuration (gitignored)
  pins_config.h         — GPIO pin definitions
  hal/                  — Display (JD9165) and touch (GT911) drivers
  data/
    aircraft.h          — Aircraft struct, AircraftList with FreeRTOS mutex
    fetcher.cpp         — Bulk API fetch, network init (WiFi/Ethernet)
    enrichment.cpp      — Per-aircraft 3-stage enrichment (route, details, photo)
    http_mutex.h        — Global HTTP request serialization
    storage.h           — NVS persistent settings
  ui/
    views.cpp           — Tileview manager, auto-cycle timer
    map_view.cpp        — Map with static background, rotated aircraft icons, trails
    radar_view.cpp      — Radar sweep with phosphor blips and trail dots
    arrivals_view.cpp   — Split-flap board with character-flip animation
    stats_view.cpp      — System health + network + session stats
    detail_card.cpp     — Scrollable aircraft detail overlay
    settings.cpp        — Settings panel with NVS persistence
    filters.cpp         — Shared filter state (COM/MIL/EMG/HELI/FAST/SLOW/ODD)
    range.cpp           — Shared zoom range state
    alerts.cpp          — Alert overlay for emergencies/military
    status_bar.cpp      — Top bar: nav dots, connection indicator, gear icon
tools/
  generate_static_map.py — OSM tile fetcher for map backgrounds
```

## Memory Usage

- **Flash:** ~75% of 4MB app partition (custom `partitions.csv`)
- **Internal RAM:** ~12% (58KB of 512KB)
- **PSRAM:** Two 307KB render buffers + heap for HTTP responses
- **LVGL objects:** ~1,540 across all views
- **FreeRTOS tasks:** 3 on core 1 (adsb_fetch 32KB, route_enrich 16KB, enrich 10KB transient)

## Known Limitations

- **No aircraft photos rendered** — PSRAM-sourced images corrupt on ESP32-P4 due to cache coherency. Photo credit text is displayed in detail cards instead.
- **USB CDC serial** can be unreliable on some boards. Use UART0 if serial output is needed for debugging.
- **Tile cache disabled** — `lv_draw_image` has rendering issues on ESP32-P4 PPA. Static pre-rendered maps are used instead.

## License

[MIT](LICENSE)
