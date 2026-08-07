# AGENTS.md

## Cursor Cloud specific instructions

This repo is **ESP32-P4 firmware** for the JC1060P470C board (PlatformIO /
`pioarduino`), plus an optional **Raspberry Pi / Linux port** under `pi/`
(CMake + LVGL SDL simulator or DRM). Cloud Agents cannot flash hardware;
"running" here means a successful build (firmware.bin and/or `build/pi/adsb_pi`).

**Deeper project knowledge** (history, backlog, CrowPanel/WiFi pin saga, Pi
bring-up, design decisions): see [`docs/project-knowledge.md`](docs/project-knowledge.md).
That file covers the *whole* project (ESP32 + Pi), not only the `pi-port`
branch. Treat it as point-in-time — verify against current code before relying
on specific "done" claims or file:line references.

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
  `55.03.37` / ESP-Hosted `2.11.6` is the only confirmed-stable pair;
  `55.03.39` / `2.12.8` matched still hit `sdio_rx_get_buffer` harder —
  open upstream DMA-fragmentation bug, not fixable here).

### Standing preferences
- **User handles builds.** Do **not** run `pio run` / CMake builds just to
  verify ordinary code changes. Write the code and stop. Note compile concerns
  in text if needed. (Exception: environment-setup / first-boot tooling checks.)
- **No agent self-attribution on GitHub.** Never — includes Claude, Cursor, or
  any other agent. No `Co-authored-by`, agent `Signed-off-by`, "Made with
  Cursor"/credit lines, or other self-attribution in commits, PRs, issues, or
  comments. Past `Co-authored-by` trailers made the agent appear as a GitHub
  contributor; that is unacceptable. Cursor Cloud's `commit-msg.cursor.co-author`
  hook must stay disabled/no-op (`.cursor/install.sh` rewrites it). Verify with
  `git log -1 --format='%B'` before pushing.
- **Prefer explicit commit permission** for ordinary interactive work. Make
  changes, then wait to be told to commit/push. Cloud Agent PR/setup workflows
  that require commits still apply when that is the assigned task.
- **Justify, don't default.** When carrying a tool/library/pattern into new
  work (new board, new platform, simulator, etc.), state why it fits *this*
  context's constraints — not just "it's already used elsewhere."
- **Pi hardware work:** no SSH from the agent to the Pi. Hand Dan exact
  copy-paste command bundles for on-device steps.
- **Layout measurement:** if "where exactly does X render" resists a couple of
  source-only guesses, stop guessing — add a measurement ruler and ask for a
  photo of real hardware (worked for jc1060 and Pi bullseye centering).

### Hard architectural constraints
- **`jc1060` only** on ESP32. CrowPanel / waveshare / S3 / CYD targets were
  removed on purpose; don't resurrect unless Dan asks. CrowPanel blank-display
  bug was never root-caused — history is in `docs/project-knowledge.md`.
- **No new FreeRTOS tasks for network work** on ESP32 (permanent or one-shot).
  Piggyback on an existing task via request/poll/result; keep HTTP serialized
  through `http_mutex`. New task stacks compete with ESP-Hosted SDIO for scarce
  internal DRAM and have crashed with `assert failed: sdio_rx_get_buffer`.
- **Map ≠ Radar visibility.** Map intentionally draws/taps aircraft past the
  bullseye out to the rectangular canvas; Radar intentionally clips to a
  circle. Do not "fix" either to match the other.
- **No origin/destination / route display.** Removed because adsb.lol and
  adsbdb.com share unreliable VRS standing-data route tables. User may revisit
  with a new approach — don't bring the old design back unprompted.
- **LVGL delete-from-handler:** use `lv_obj_delete_async()` when deleting an
  object (or ancestor) from inside its own event handler.
- **Large stack locals off `loopTask`:** anything reachable from LVGL
  draw/timer callbacks runs on Arduino's ~8KB `loopTask` stack — big arrays
  must be `static`/heap, not stack-local.
- **Keep NVS/flash writes small** on the UI path — large synchronous writes
  visibly stall the LCD.
- **Pi vs ESP32 split:** Linux-only code stays under `pi/` (outside `src/`) so
  PlatformIO does not compile it into the jc1060 firmware.
- **`fetcher.cpp` stays ESP32-only.** Do not refactor it into the Pi path; Pi
  uses `pi/platform_linux/datasource_remote.cpp` (accepted JSON-schema drift
  risk).
- **Pi DRM draw budget:** ~4.5–6 ms per `lv_draw_*` call on real hardware
  (`LV_DRAW_SW_DRAW_UNIT_CNT=1`). Budget draw calls tightly on that path.
- **LVGL version pin on Pi:** CMake pins `v9.5.0` to match what PlatformIO
  actually resolves from `^9.2.2` — floating ESP32 deps can desync APIs.
