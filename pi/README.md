# ADS-B Radar Display — Raspberry Pi port

A parallel build target for the same app as the repo root, targeting a
Raspberry Pi + DSI touchscreen instead of the ESP32-P4 (jc1060) board.
Shares most of `src/data/` and `src/ui/` with the ESP32 build — see each
file's own comments for the `#if defined(ARDUINO)` forks where platform
behavior genuinely differs, and `src/platform/platform.h` for the
mutex/time/HTTP/config-storage seam that makes the sharing possible.

**Status**: Map/Radar/Arrivals/Stats all render live adsb.lol traffic in a
real swipeable tileview. Not yet ported: Settings (needs a scoped-down
design — see below), the status bar (nav dots/gear icon — there's a blank
~48px strip at the top for now), alerts (military/emergency toasts),
locations (Home + saved airports — the active location is a fixed
Seattle-Tacoma coordinate stub in `app_stubs.cpp`), metar/airlines/
enrichment (detail card falls back to the aircraft's own fields fine
without these). Real DSI hardware bring-up (DRM/KMS + libinput) hasn't
been tested against physical hardware yet.

## Hardware target

Waveshare 10.1" DSI capacitive touch panel (1280×800) on a Raspberry Pi
3B, running Raspberry Pi OS Lite (no desktop) — the app runs directly
over DRM/KMS as a systemd kiosk service, not inside a window manager.

> ⚠️ Waveshare's own listing for this panel names compatibility with Pi
> 5/4B/3B+/3A+/CM3/3+/4 — that's **3B+**, not the plain 3B. Verify with
> Waveshare (or the product page's fine print) before assuming this
> combination works.

## Building on macOS (dev loop)

No Pi or VM needed for UI/data work — LVGL's SDL2 simulator runs natively.

```
brew install cmake sdl2   # if not already installed
cmake -S . -B build -DPI_DISPLAY_BACKEND=SDL
cmake --build build -j8
./build/pi/adsb_pi
```

Config persists to `~/.config/adsb/config.json` (or
`$XDG_CONFIG_HOME/adsb/config.json`). Delete it to reset to defaults.

## Building on the Pi (real hardware)

Not yet build-tested on real hardware — this is the expected path once a
Pi + the Waveshare panel are in hand:

```
sudo apt install build-essential cmake libsdl2-dev libcurl4-openssl-dev \
    libdrm-dev libinput-dev pkg-config
cmake -S . -B build -DPI_DISPLAY_BACKEND=DRM
cmake --build build -j4
sudo ./build/pi/adsb_pi   # or install as a service, see below -- needs
                          # /dev/dri and /dev/input access either way
```

`display_drm.cpp` assumes the panel enumerates as `/dev/dri/card0`;
`input_libinput.cpp` auto-locates the touch device by capability. Both
are unverified against real hardware — expect to adjust them during
bring-up (task #6 of the port; see `project_pi_port` notes).

## Installing as a kiosk service

```
sudo useradd -r -G video,input,render adsb
sudo mkdir -p /opt/adsb-pi
sudo cp build/pi/adsb_pi /opt/adsb-pi/
sudo cp pi/adsb-pi.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now adsb-pi
```

Logs: `journalctl -u adsb-pi -f`.

## Architecture notes

- Everything Pi/Linux-only lives under `pi/`, outside `src/` — PlatformIO's
  default `src_dir` is `src/` and auto-compiles everything under it, so
  Linux-only headers (libcurl, `<mutex>`, DRM) can't live there without
  breaking the jc1060 build.
- LVGL version is pinned in `pi/CMakeLists.txt` to whatever
  `.pio/libdeps/jc1060/lvgl/library.json` actually resolves for the ESP32
  build (currently 9.5.0) — platformio.ini's `^9.2.2` is a floating range,
  not the real installed version, and shared code (e.g.
  `src/ui/aircraft_icons.h`) uses APIs that differ across LVGL 9.x point
  releases. If a fresh jc1060 build resolves a newer version and something
  here stops compiling, re-check that before assuming it's this port's bug.
- `pi/app_stubs.cpp` holds temporary link-satisfying implementations for
  everything not yet ported (locations/metar/airlines/enrichment/alerts/
  status_bar) — each gets deleted as the real thing lands.
- `src/data/fetcher.cpp` (jc1060's WiFi/C6-co-processor fetch loop) is
  deliberately untouched and not shared — see its own extensive comments
  and `project_p4_heap_constraints`/`project_platform_pin` history for why
  that code is not worth risking a refactor on. `pi/platform_linux/
  datasource_remote.cpp` reimplements the adsb.lol JSON parsing fresh
  instead.
