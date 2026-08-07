# AGENTS.md

## Cursor Cloud specific instructions

This repo is **ESP32-P4 firmware** for the JC1060P470C board (PlatformIO /
`pioarduino`), plus an optional **Raspberry Pi / Linux port** under `pi/`
(CMake + LVGL SDL simulator or DRM). Cloud Agents cannot flash hardware;
"running" here means a successful build (firmware.bin and/or `build/pi/adsb_pi`).

### Toolchain / where things live
- Cloud env: `.cursor/environment.json` + `.cursor/Dockerfile` + `.cursor/install.sh`.
- `pio` lives in `~/.platformio/penv/bin` (on `PATH` via `~/.bashrc`). If
  missing in a non-interactive shell:
  `export PATH="$HOME/.platformio/penv/bin:$HOME/.local/bin:$PATH"`.
- First ESP32 build downloads platform `55.03.37`, the RISC-V toolchain, and
  libs into `~/.platformio` (cached in environment Builds).
- PlatformIO's internal venv needs `python3-venv` (provided by the Dockerfile).
- Pi SDL simulator needs `cmake`, `libsdl2-dev`, `libcurl4-openssl-dev`
  (Dockerfile). Present only on branches that include `pi/` (e.g. `pi-port`).

### Build
- ESP32 (always): `pio run -e jc1060` — only supported board target.
- Pi SDL simulator (when `pi/` exists):
  ```
  cmake -S . -B build -DPI_DISPLAY_BACKEND=SDL
  cmake --build build -j$(nproc)
  ./build/pi/adsb_pi
  ```
  Headless Cloud VMs can compile the SDL binary; interactive display may need
  Computer Use / a display. Real Pi DRM hardware (`-DPI_DISPLAY_BACKEND=DRM`)
  is out of scope for this cloud environment.
- Flashing (`-t upload`) and `pio device monitor` require the physical board.

### Lint / tests
- No C++ unit tests and no lint config. CI (`.github/workflows/release.yml`)
  only builds firmware on tag pushes. Treat a clean `pio run -e jc1060` (and,
  on pi-port, a clean CMake SDL build) as the verification signal.

### Optional Python codegen tools (`tools/`)
Generate **gitignored** headers under `src/ui/` (`__has_include`, so builds
work without them):
- `python3 tools/generate_airports_db.py` → `src/ui/airports_db.h`
- `python3 tools/generate_static_map.py --lat LAT --lon LON` →
  `src/ui/static_map_data.h` (needs Pillow + requests; always pass `--lat`/`--lon`)

### Do not
- Do not bump `platform =` in `platformio.ini` (ESP-Hosted/C6 pairing;
  `55.03.37` / ESP-Hosted `2.11.6` is the stable pair — see comment there).

### Standing preferences
- **User handles builds.** Do **not** run `pio run` / CMake builds just to
  verify ordinary code changes. Write the code and stop. Note compile concerns
  in text if needed. (Exception: environment-setup / first-boot tooling checks.)
- **No agent self-attribution on GitHub.** Never add `Co-authored-by`,
  `Signed-off-by` for the agent, "Made with Cursor"/agent credit lines, or
  other self-attribution in commits, PRs, issues, or comments. Cursor Cloud's
  `commit-msg.cursor.co-author` hook must stay disabled/no-op for this repo
  (`.cursor/install.sh` rewrites it). Verify with `git log -1 --format='%B'`
  before pushing.
- **Justify, don't default.** When carrying a tool/library/pattern into new
  work, state why it fits *this* context's constraints.
- **Prefer explicit commit permission** for ordinary interactive work. Cloud
  Agent PR/setup workflows that require commits still apply when assigned.

### Hard architectural constraints
- **`jc1060` only.** Don't resurrect CrowPanel / waveshare / S3 / CYD targets
  unless Dan asks.
- **No new FreeRTOS tasks for network work** on ESP32. Piggyback on an
  existing task; keep HTTP serialized through `http_mutex`.
- **Map ≠ Radar visibility.** Map draws past the bullseye to the rectangle;
  Radar clips to a circle. Do not "fix" either to match the other.
- **No origin/destination / route display.** Removed on purpose.
- **LVGL delete-from-handler:** use `lv_obj_delete_async()` when deleting an
  object (or ancestor) from inside its own event handler.
- **Large stack locals off `loopTask`:** big arrays in LVGL draw/timer paths
  must be `static`/heap, not stack-local (~8KB `loopTask` stack).
- **Pi vs ESP32 split:** Linux-only code stays under `pi/` (outside `src/`) so
  PlatformIO does not compile it into the jc1060 firmware.
