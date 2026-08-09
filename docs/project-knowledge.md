# ADS-B Display / FlightLevel314 — Project Knowledge

> **Agent note:** This file began as the knowledge dump for `dpoler/adsb`
> (ESP32 jc1060 + Pi port). On **2026-08-08** the Pi work was forked into
> **FlightLevel314** and jc1060 development was paused. Keep ESP32 history
> here for optional cherry-picks; new work targets Pi/Linux only unless
> explicitly asked otherwise. Day-to-day agent rules live in `AGENTS.md`.


Generated 2026-08-07 from accumulated cross-session memory; FlightLevel314
fork notes added 2026-08-08; handoff refreshed **2026-08-09**; backlog status
refreshed **2026-08-09** (Dan). Point-in-time snapshot — verify against
current code before treating any specific claim as still true.

Current product: **FlightLevel314** (Pi). Historical ESP32 branch: `dpoler/adsb`.

---

## 0. Agent handoff (2026-08-09) — read this first

### Canonical repo
- **https://github.com/dpoler/FlightLevel314** — Pi / Linux only. Day-to-day work
  belongs here (`master`).
- **https://github.com/dpoler/adsb** — historical dual-target tree; ESP32 /
  jc1060 **paused**. Keep for cherry-picks / archaeology.

### Cloud-agent / GitHub App caveat (important)
A Cursor **cloud** agent started from `dpoler/adsb` gets an installation token
scoped to **adsb only**. Even when the Cursor GitHub App has Read/Write on
FlightLevel314 in the GitHub UI, that adsb-scoped run **cannot** `git push` to
FlightLevel314 (403 `cursor[bot]`). Workarounds: start the cloud agent from
**FlightLevel314**, or have Dan push from his Mac (auth already works there).
The Pi (`dap@adsb`) often has no GitHub credentials — don’t send him through
PAT/SSH setup unless he asks; push from Mac instead.

### Just shipped (2026-08-09)
1. **README rewrite** — product pitch, hardware, SDL/DRM setup, kiosk install,
   config/API keys, Credits (**Neil** per `LICENSE` + `dpoler/adsb` lineage).
   Screenshots in `docs/screenshots/` (panel photos + current Settings). Some
   gallery shots still show older nav labels **ARR / STATS**; current UI is
   **MAP / RADAR / LIST / INFO**. Landed on FlightLevel314 `master` by Dan
   after cherry-pick from `adsb` `cursor/readme-cleanup-7c95` (`69941ef`).
2. **Pi DRM hard freeze fix** — UI painted one frame then froze (touch dead,
   update counter stuck, only `kill -9`). Cause: LVGL v9.5.0
   `lv_linux_drm.c` — dangling non-NULL `req` after failed atomic commit +
   infinite `poll(-1)`. Fix: `pi/patches/apply_lvgl_drm_patch.py` (runs at
   CMake configure), plus blank VC4 hardware cursor in `pi/display_drm.cpp`.
   Branch/commit on adsb: `cursor/drm-flush-hang-7c95` / `0a01252`. Dan
   rebuilt DRM on the Pi; freeze addressed.

### Runtime facts
| Item | Value |
|------|--------|
| Binary | `build/pi/flightlevel314` → `/opt/flightlevel314/flightlevel314` |
| Config | `~/.config/flightlevel314/{config.json,locations.json}` |
| Kiosk | `pi/flightlevel314.service`, `User=flightlevel314`, `HOME=/opt/flightlevel314` |
| SDL build | `cmake -S . -B build -DPI_DISPLAY_BACKEND=SDL` (Mac/dev) |
| DRM build | `cmake -S . -B build -DPI_DISPLAY_BACKEND=DRM` (**Pi only**; needs libdrm) |
| Traffic | adsb.lol; optional AirportDB (`apt_tok`) / AeroDataBox (`adbox_key`) in config.json |

### Open backlog (do **not** start unless Dan asks)
See §7.1. Highest-signal open items:
- Follow Mode (design notes captured 2026-08-09; hold — Dan thinking)
- Device provisioning for API keys / secrets (how they get onto the Pi)
- Optional: replace README gallery shots with fresh LIST/INFO + live traffic
- Tidy Pi boot splash (center / dim or quiet→app)

Deferred (do not start): small airports in static DB; airframes.io ACARS O/D.

Recently closed (2026-08-09): Airport Mode Phase 3 (INFO METAR/ATIS);
Pi-only cleanup (ESP32/jc1060 sources removed); GND default hidden;
edit/rename saved locations; LIST GND refresh; Settings Cancel/Save;
brightness; VIEW two-col.

### Dan / workflow
- Name: Dan. Prefers working directly in code; builds himself unless asked.
- Tired of over-engineered git/auth instructions — match existing machine setup.
- Cut/paste from the agent chat window sometimes mangles commands; keep
  commands on separate lines.

### Transfer branches on adsb (history only once FL314 has the commits)
- `cursor/readme-cleanup-7c95` — README (PR #8)
- `cursor/drm-flush-hang-7c95` — DRM freeze (PR #9)
- `cursor/flightlevel314-7c95` — original Pi extraction

---

## 1. What this project is

**FlightLevel314** — a touchscreen ADS-B aircraft display for Raspberry Pi
(Waveshare 10.1" DSI, 1280×800). Live traffic from adsb.lol on Map / Radar /
Arrivals / Stats, with saved locations, filters, trails, alerts,
basemap/weather, and optional AeroDataBox O/D + AirportDB enrichment.

Originally also an ESP32-P4 (jc1060) target in `dpoler/adsb`. That board is
**paused** in this fork; features can be brought back to the ESP32 tree later
by comparing against this project.


## 2. Board targets (ESP32 side)

**Only one ESP32 board target exists today: `jc1060`** (JC1060P470C, ESP32-P4 +
ESP32-C6 WiFi co-processor over SDIO/ESP-Hosted, 1024x600 7" panel — "1060" is a
resolution-derived name, not a diagonal size). This is Dan's daily-use device.

Removed board targets, all deliberately, all at explicit user request:
- **`esp32s3_35`** (3.5" ESP32-S3, TFT_eSPI, had its own `src/ui_s3/` UI copy) —
  removed 2026-07-21, "I don't use it."
- **`cyd`** (bare standalone TFT_eSPI sketch, predates the LVGL `ui/`/`ui_s3`
  architecture) — removed 2026-07-24.
- **`waveshare`** (ESP32-**S3** board, Waveshare ESP32-S3-Touch-LCD-7B — not P4,
  correcting an earlier wrong note) — removed 2026-07-24.
- **`crowpanel`** (Elecrow CrowPanel Advanced 10.1" ESP32-P4) — added 2026-07-26
  after a full real-hardware bring-up (see §3 below), paused 2026-07-27 after an
  unsolved recurring blank-display bug survived three fix attempts, then fully
  **removed** 2026-07-28 at explicit request ("remove everything that isn't
  jc1060"). The bring-up work and the unsolved bug are kept as historical record
  in memory, not deleted, in case a similar board is revisited.

`platformio.ini` currently has exactly one env: `jc1060`.

**Convention note**: an earlier "ui/ vs ui_s3/ scoping" convention (shared
data-layer fixes go to both UI targets, new UI-only features only to `ui/`) is
now moot since there's no second ESP32 UI target. Don't assume any future board
should reuse the old `ui_s3/` approach — ask what it actually needs first.

---

## 3. CrowPanel bring-up (historical — board since removed)

Real-hardware bring-up done 2026-07-26 on branch `crowpanel-port`. Kept as
detailed history since the debugging trail (SDIO pin mapping, a real upstream
GT911 touch driver bug, DMA/cache-coherency investigation) is broadly relevant to
other P4-based work.

- **macOS USB driver**: WCH CH34x chip installs as a DriverKit system extension,
  not a legacy kext — enable via System Settings → General → Login Items &
  Extensions → Driver Extensions (no "blocked software" banner appears).
- **Upload speed**: esptool's default 1.5Mbps failed; settled on 230400 baud
  based on a matching GitHub issue from someone else on the same chip family
  (460800 "doesn't ack reliably").
- **SDIO/WiFi pins**: `sdkconfig.defaults.crowpanel` does NOT reach the build
  under `framework=arduino` (arduino-esp32 links a precompiled P4 lib with
  ESP-Hosted Kconfig baked in) — needed a runtime `hostedSetPins()` override in
  `fetcher_init()` instead. Elecrow's own official example gave wrong pin
  values (1-bit bus, D0=14/D1=15); the actually-correct values (4-bit bus,
  D0=17/D1=16/D2=15/D3=14, CLK=18, CMD=19, reset=32) came from a GitHub issue on
  the same repo where someone else reverse-engineered it under MicroPython.
  Lesson: vendor docs and vendor GitHub issues disagreed twice in the same repo
  — trust real hardware reports over official examples.
- **Touch — real upstream driver bug**: Espressif's own vendored
  `esp_lcd_touch_gt911.c` had a real bug (`espressif/esp-bsp#501`) —
  `get_xy()` unconditionally zeroed the touch-point count every call, causing
  false "released" reports mid-touch. Ported the upstream fix in two parts (the
  first part alone made it *worse* — touch latched permanently pressed).
  Software debounce in `gt911_touch.cpp` kept as belt-and-suspenders (3 tuning
  passes: V1 press-only didn't fix release-side chatter; V2 added release-side
  debounce but broke light taps; V3 = press-accept-immediately + 80ms
  post-release cooldown).
- **Two USB ports**: UART0 (WCH bridge, flashing/ROM banner) vs "USB2.0" (P4's
  native USB-OTG peripheral, what `Serial` actually uses under
  `ARDUINO_USB_MODE=0`). Two attempts to collapse to one cable both hung the
  board — root cause: ESP-IDF's console already owns UART0 from boot
  (`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`), and `HardwareSerial.begin()` doesn't
  special-case an already-console-owned UART. **Decided: two cables is the
  permanent answer for this board.**
- **jc1060 build break found (not caused) by this bring-up**: PlatformIO
  compiles every `.cpp` under `src/` for every env by default — needed explicit
  `build_src_filter` exclusions per board for board-specific `hal/` files.
- **Blank-display bug — never root-caused, board removed instead**: recurring
  full-blank screen, backlight still on (ruled out backlight-only failure).
  Three independent, source-verified fixes all failed: `esp_cache_msync()`
  (redundant — driver already handles it), `num_fbs=2` alone (would cause
  tearing under LVGL partial-render mode, caught before flashing),
  `num_fbs=2`+`RENDER_MODE_FULL` (structurally correct, still didn't fix it).
  New evidence pointed at a possible shared power/electrical fault (a
  simultaneous I2C touch-bus failure burst was found in a separate UART0 log).
  Paused 2026-07-27 per user decision, then the whole board was **removed**
  2026-07-28.

---

## 4. Platform version pin history (jc1060, ESP32-P4/C6)

`platformio.ini` pins `pioarduino`/`platform-espressif32` to **`55.03.37`**
(Arduino-ESP32 3.3.7) for `jc1060`. This is a hard-won, confirmed-stable value —
**do not bump without new evidence**.

History:
- Started at `53.03.13` in the initial scaffold, bumped to `55.03.37` early on.
- Both `jc1060` and `waveshare` (now-removed) were bumped together to `55.03.39`
  on 2026-07-18 for a WiFi crash fix scoped to `waveshare` — but this silently
  raised jc1060's expected ESP-Hosted protocol version (2.11.6 → 2.12.8),
  reopening a host/C6 co-processor version mismatch that had just been fixed via
  `dpoler/c6_updater`.
- Reverted 2026-07-21 (`jc1060` back to `55.03.37`), confirmed via new boot-time
  version logging: `host v2.11.6, C6 co-processor v2.11.6`.
- **Re-attempted and reverted again same-day, 2026-07-24**: user recalled the
  newer platform being "much faster" for WiFi bring-up and wanted to try
  matching both sides at the newer version instead of pinning backward forever.
  Retargeted `c6_updater` to 2.12.8, bumped `jc1060` to `55.03.39`. Result: WiFi
  connected fast (~1s) with no mismatch — but then crashed hard with
  `assert failed: sdio_rx_get_buffer` within ~10s of every boot. **Conclusion:
  matching host/C6 versions does NOT fix the underlying open upstream
  DMA-fragmentation bug (espressif/esp-hosted-mcu #144/#167) — it only removes
  mismatch as a separate contributing factor. 2.12.8 triggers the remaining bug
  far more severely than 2.11.6 on this board.** Fully reverted same day.
  **Hard rule: do not retarget to 2.12.8 (or assume "newer = better") without
  new evidence the upstream DMA-fragmentation bug is fixed.**
- Same risk resurfaced on CrowPanel (factory C6 firmware was "V2.12.3") —
  deliberately pinned to the same 2.11.6 host to avoid the crash, accepting a
  guaranteed version mismatch instead (never got to test which was worse before
  the board was removed for the unrelated blank-display bug).

---

## 5. ESP32-P4 heap / DMA constraints (jc1060)

The board runs thin on internal DRAM (PSRAM is abundant, 32MB, but not what task
stacks/TLS buffers/SDIO driver allocations use).

- **Never spawn new FreeRTOS tasks for network work** — even a short-lived
  one-shot task (8KB stack) for a single HTTPS request has crashed the SDIO
  driver with `assert failed: sdio_rx_get_buffer sdio_drv.c:953 (*buf)`.
  Piggyback new fetch logic onto an *existing* task's loop instead (the
  established request/poll/result pattern — see `locations_request_add()`/
  `locations_add_poll()`/`locations_add_result()`).
- **Root cause identified as upstream, not our task-spawn pattern**: an
  Espressif engineer confirmed the real mechanism is DMA-capable memory
  fragmentation (not just low free heap) — `espressif/esp-hosted-mcu` issues
  #144/#167, still open, hit by many unrelated projects/boards. Not fixable
  from this project's application code or build setup.
- **C6 co-processor firmware was stale (2.3.2), updated to 2.11.6** via
  `dpoler/c6_updater` (the user's own tool, uses Arduino-ESP32's official
  `ESP_HostedOTA` mechanism over the existing SDIO link). Crash persisted even
  after the update, confirming it's the still-open upstream bug, not a stale-C6
  problem, at 2.11.6.
- **Fixed, separate bug: NetworkClientSecure's 120s handshake timeout wasn't
  bounded by `HTTPClient::setTimeout()`** — caused ~150s silent stalls (mutex
  held the whole time, starving every other network consumer). Fixed at all 6
  active HTTPS call sites by constructing `WiFiClientSecure` explicitly and
  calling `.setHandshakeTimeout(8)` before `HTTPClient::begin(client, url)`.
- **Packet-mode mitigation (upstream docs) is out of reach**: requires
  `idf.py menuconfig` on both P4 host and C6 co-processor sides; this project
  has neither a checked-in sdkconfig nor builds/flashes the C6's own firmware.
  Not attempted.
- **LVGL object deletion from inside its own/a descendant's event handler is
  undefined behavior** — fixed by using `lv_obj_delete_async()` instead of
  `lv_obj_delete()` (LVGL's own recommended pattern, used by `lv_msgbox`
  internally). Applies to any future LVGL popover/modal code.
- **Large blocking NVS writes visibly stall the LCD panel** — a ~3.2KB write
  caused a visible cyan flash; an ~400 byte write (Settings) did not. Fix:
  write only what changed/is used, not fixed-size worst-case buffers.
- **Large stack-local variables in the LVGL render path crash `loopTask`**
  (only ~8KB stack, separate/smaller than background task stacks) — a ~3.75KB
  struct declared as a plain stack-local inside a draw callback caused a
  deterministic stack-canary panic. Fix: make it `static` instead.

---

## 6. Raspberry Pi port (branch `pi-port`)

**Goal**: fork this ESP32-P4 ADS-B display to also run on a Raspberry Pi 3B+
with a Waveshare 10.1" DSI capacitive touch panel (1280x800, resolves an earlier
open "is it 3B or 3B+" question — confirmed 3B+ once real hardware arrived),
to unlock functionality the ESP32 fundamentally can't do (real aircraft photos,
richer maps/charts, more compute). Architecture plan doc:
`/Users/dap/.claude/plans/misty-exploring-cook.md` (may be pruned over time).

### Key architecture decisions
- **Data source**: stays on remote adsb.lol for now, same as ESP32. An
  `AircraftDataSource` interface (`src/data/datasource.h`) is stubbed so a
  future local RTL-SDR + dump1090/readsb feed is a new class
  (`LocalSdrDataSource`, not yet implemented), not a fetcher rewrite.
- **Code sharing**: shared core + a platform-abstraction layer
  (`src/platform/platform.h`: mutex, monotonic time, HTTP GET, config storage,
  log). Monorepo, not a separate repo. Most of `src/ui/` compiles for both
  jc1060 (PlatformIO/Arduino) and Pi (CMake/g++) with minimal per-file changes.
- **Layout constraint**: everything Pi/Linux-only lives in a top-level `pi/`
  directory, outside `src/` (PlatformIO auto-compiles everything under `src/`,
  so Linux-only headers there would break the jc1060 build).
- **OS**: Raspberry Pi OS (came pre-installed with desktop; desktop was
  **disabled**, not reinstalled to Lite — `systemctl set-default
  multi-user.target` + disable lightdm, reversible). LVGL directly over DRM/KMS,
  running as a systemd kiosk service (`pi/adsb-pi.service`).
- **Mac dev loop**: LVGL's SDL2 simulator (fast UI iteration) plus an aarch64 Pi
  OS VM via UTM/QEMU for system-integration testing (no real GPU passthrough,
  so not for pixel-exact display testing).
- **LVGL was justified, not defaulted to**: user explicitly required a real
  justification for carrying LVGL forward rather than assuming it because it's
  already used — confirmed it's the right fit given the Pi 3B's weak GPU and
  this app's canvas-heavy custom rendering (radar sweep, phosphor fade,
  split-flap board), and it's what keeps the UI layer shareable with ESP32.
- **`src/data/fetcher.cpp` (jc1060's WiFi/C6 fetch loop) deliberately left
  untouched** — its hardware-recovery logic is hard-won (see §4/§5). Pi instead
  has a fresh reimplementation, `pi/platform_linux/datasource_remote.cpp`
  (same adsb.lol JSON schema, independent code — a known drift risk if the
  schema changes, accepted).
- **No SSH access from Claude** to the Pi — all hardware bring-up work happens
  by handing the user exact copy-paste command bundles; standing operating mode
  for all `pi/` hardware work.

### Status as of 2026-08-06 (most recent)
Real Pi 3B+ hardware arrived 2026-08-06. **Task #6 (real hardware bring-up)
closed out** — DRM display, real touch, real network fetch, and surviving an
unattended reboot are all verified on actual hardware (not just SDL simulator).

Two real build bugs found and fixed getting the DRM build running:
1. `pi/CMakeLists.txt` set nonexistent LVGL v9.5.0 CMake options
   (`LV_CONF_BUILD_DISABLE_EXAMPLES/_DEMOS`) — silently a no-op, harmless on
   SDL. Real option names are `CONFIG_LV_BUILD_EXAMPLES`/`_DEMOS`.
2. `${DRM_INCLUDE_DIRS}`/`${LIBINPUT_INCLUDE_DIRS}` were only added to the
   `adsb_pi` target, not the `lvgl` target itself — but `LV_USE_LINUX_DRM`
   makes `lvgl.h` itself pull in `<drm.h>` for every LVGL source file. Debian
   ships `drm.h` under `/usr/include/libdrm/`. Fixed by adding those include
   dirs with `PUBLIC` scope to the `lvgl` target too.
3. Separate bug: several shared files call `snprintf` without `#include
   <cstdio>`, relying on transitive inclusion — true on macOS/ESP32 Arduino,
   not true on Debian/GCC 14. Fixed the four call sites actually in the Pi
   build's `SHARED_SOURCES` list; left the same latent bug alone in
   ESP32-only/Pi-stubbed files (out of scope).

`display_drm.cpp`/`input_libinput.cpp` needed **zero changes** — both
previously-unverified hardware assumptions (`/dev/dri/card0`, capability-based
Goodix touch auto-detection) were correct on the first real run.

**Kiosk service installed and confirmed surviving an unattended reboot** — fix
was pinning `Environment=HOME=/opt/adsb-pi` in the systemd unit and
seeding/chowning `/opt/adsb-pi/.config/adsb` from the already-tested config
before first launch.

**Real perf constraint found on Pi's DRM rendering path**: each single
`lv_draw_line`/`lv_draw_rect` call costs roughly **4.5–6ms**, flat, regardless
of pixels touched (confirmed via real per-phase profiling, not guessing) —
likely because `pi/lv_conf.h`'s `LV_DRAW_SW_DRAW_UNIT_CNT=1` means
single-threaded software rasterization, not using the Pi's other cores (not
root-caused further). **Practical rule for future Pi rendering work: budget
draw calls tightly** — at ~5ms/call, 20-30 extra draw calls can blow a 100ms
(10fps) frame budget. `draw_rings()`'s ~10-12 calls already cost ~27ms baseline.

**Display-sizing pass closed out (commits 03cc328, bbaaa32, db58560)**: List's
`MAX_ROWS` (was a jc1060-tuned literal 15) now derives from `BOARD_H`; confirmed
21 rows on Pi matching the formula. Map/Radar bullseye center/radius
(`MAP_BULLSEYE_CY/R`, `RADAR_CY/R`) are per-screen `#if LCD_V_RES == 800`
measured constants (via a real ruler+photo measurement pass, same method as the
original jc1060 bullseye-centering saga), jc1060's values kept byte-for-byte
unchanged in `#else`. User confirmed "good for now."

### What's ported and working on Pi (as of 2026-08-06)
Platform seam, config storage, real adsb.lol fetch, Map/Radar/Arrivals/Stats
views, detail_card, filters, display_prefs, range, status_bar (nav tabs/gear/
range chip), view_menu (VIEW chip popover — trails/tags/secondary-locations/
alert toggles), alerts.cpp (military/emergency toasts — plumbing verified,
toast animation itself not yet visually exercised), a real saved-locations
system (fresh Linux `locations_linux.cpp` implementation + ported
`location_picker.cpp` UI), systemd kiosk service, README.

### Still stubbed (`pi/app_stubs.cpp`)
`metar.cpp`, `airlines.cpp`, `enrichment.cpp` — all three network-backed
(adsbdb.com/planespotters.net), independently portable, none block each other.
`enrichment.cpp` is flagged as a genuine Pi-exclusive opportunity: jc1060 can't
render fetched photos at all due to a PSRAM cache-coherency erratum (README
known-issue), but the Pi has no such constraint — real aircraft photos in the
detail card. See backlog §7 for full scope.

### Notable bugs found and fixed during the port (worth remembering)
- **Pi DRM hard freeze (UI painted once, then touch/timer dead, kill -9)**:
  LVGL v9.5.0 `lv_linux_drm.c` can hang the UI thread in `drm_flush_wait()`
  forever — `poll(..., -1)` with no timeout, and a failed
  `drmModeAtomicCommit` frees `drm_dev->req` but leaves the dangling
  non-NULL pointer so the next wait never exits. Matches: first frame
  visible, update counter stuck, touch dead. Fixed via idempotent configure
  patch `pi/patches/apply_lvgl_drm_patch.py` (NULL `req` on failure, 500ms
  poll timeout, NONBLOCK→blocking retry). Also blank the VC4 hardware
  cursor plane in `display_drm.cpp` (stuck pointer at upper-left). Re-blank
  also runs for a few `REFR_READY` frames after LVGL's first
  `ALLOW_MODESET` flush, which can resurrect the cursor plane. Input init
  must not `lv_free` the path from `lv_libinput_find_dev` (static cache)
  and must not fall back to `/dev/input/event0`.
  Reported 2026-08-09 after FlightLevel314 redeploy; README changes were
  unrelated. **Rebuild DRM binary after pull** (`cmake` reconfigure runs
  the patch).
- **`platform.h` must `#include <Arduino.h>` itself under `#if defined(ARDUINO)`**
  — a file switched from directly including Arduino.h to including platform.h
  can silently lose `millis()` on the ESP32 build if platform.h doesn't
  re-export it. Cost one failed jc1060 build to catch (user builds jc1060
  themselves per [[feedback_builds]] — this class of bug only surfaces on
  their side).
- **Tileview scroll-range bug**: `lv_tileview_add_tile()` positions tiles with
  `lv_pct()`, only resolved into real pixels during a layout pass — the
  scrollable-width calc got stuck against whatever was resolved at that moment.
  Fixed with an explicit `lv_obj_update_layout()` call right after
  `views_init()` in `pi/main.cpp` (jc1060 never hit this because it creates
  enough other widgets between init and first frame to force the recompute
  incidentally).
- **SDL mouse driver incompatible with LVGL's scroll-momentum math**
  (`LV_INDEV_MODE_EVENT` + many events in the same observable millisecond →
  velocity reads as near-infinite → tileview flings to the far edge — LVGL
  issue #6832). Fixed by writing a custom POLL-mode SDL input driver
  (`pi/input_sdl.cpp`) instead of using LVGL's bundled `lv_sdl_mouse_create()`.
  Pi-simulator-only, doesn't affect real hardware (`input_libinput.cpp` is a
  separate driver).
- **The actual decisive root cause of the "swipe jumps to extremes" bug**: a
  separate, **shared-code** manual swipe detector in `views.cpp`
  (`views_attach_swipe()`) used `% NUM_VIEWS` which wraps (Map→Stats), but the
  tileview's actual layout is a straight line, not a loop — wrapping jumped
  straight across with no animation. User chose **clamp at the ends** over
  keeping wraparound. This was shared code, not Pi-specific, so needed a jc1060
  build check. **Process lesson**: several earlier plausible-but-wrong theories
  (tile position, input driver) were each real and partially confirmed but not
  the actual cause — once local instrumentation cleanly isolates a symptom,
  check *all* code paths that could react to that exact trigger, not just the
  one already under suspicion.
- **Behavior change, intentional**: Pi's first boot now starts with **zero**
  saved locations (matches ESP32's real first-boot state), not the old
  hardcoded-KSEA stub. No aircraft show until a location is added via the
  picker.

### Remaining known gaps (not blocking, tracked in backlog)
- Boot sequence isn't tidy — console text/login prompt likely visible before
  the kiosk grabs the display (bright/off-center Pi logo; see §7.1).
- Getting API keys / tokens onto the device — no good story yet (see §7.1).

---

## 7. Full backlog (as of 2026-08-09)

This preserves detail for open / deferred items and a short trail for recently
closed ones. Dan refreshed status **2026-08-09** (done / deferred / removed).
**Do not start open work unless explicitly asked.**

### 7.1 Open

- ~~**Pi-only cleanup — remove / archive leftover jc1060 / ESP32 surface area
  in this repo**~~ **done 2026-08-09** — deleted unlinked ESP32 sources
  (`src/hal/`, `src/main.cpp`, PlatformIO-era data .cpp, serial_config,
  screensaver, tile_cache, configure_device scripts, root `lv_conf.h`);
  flattened Pi-only headers (`aircraft.h`, `locations.h`, `platform.h`, …).
  Historical ESP32 narrative remains in this doc for archaeology /
  cherry-picks to `dpoler/adsb`.

- ~~**Detail card photo credit appears before the photo**~~ **done 2026-08-09**
  — Pi shows credit only with visible pixels.

- ~~**Basemap / sectional outside US; UK tile AABB**~~ **done 2026-08-09** —
  drop zoom when AABB > 300 tiles; VIEW label "VFR Sectional (US)". Follow-up
  same day: skip sectional fetch outside coverage; chart-paper placeholder +
  corner/status message (EGLL no longer greys out after a fake progress bar).

- ~~**Pi online app updates**~~ **done 2026-08-09** — GitHub Releases check /
  status-bar notice / Settings install + systemd restart (`ota_linux.cpp`).

- ~~**Settings taller / three columns (DEVICE+OTA made it scroll)**~~
  **done 2026-08-09** — Pi Settings `1100×700`, three columns, no body scroll.

- ~~**Basemap loads while still cycling styles; VIEW menu too long**~~
  **done 2026-08-09** — basemap style is a dropdown (one commit per pick);
  Pi Map VIEW is two columns (~604×470: left Trails/Tags/Locations/Alerts,
  right Basemap/Weather). Radar stays single-column.

- ~~**LIST shows ground aircraft when GND is filtered off** until you toggle
  the filter off and back on~~ **done 2026-08-09** — `gnd_click_cb` now
  calls `update_board` immediately (matched FILT taps); tileview activate
  also forces `arrivals_view_on_show`.

- ~~**Rework LIST screen for Pi real estate**~~ **done 2026-08-09** — on
  1280×800: wider column layout left of the filter stack, taller rows/title,
  `montserrat_28` title. jc1060 layout unchanged.

- **Optional: fresher README gallery LIST/INFO shots** — still open.

- ~~**Ground traffic (GND) should default to hidden, not shown**~~
  **done 2026-08-09** — `view_hide_ground[i]` defaults true (fresh
  factory-reset / new config only; existing installs keep saved values).

- ~~**Need a way to view/edit/rename saved locations, not just add/remove**~~
  **done 2026-08-09** — picker **ⓘ** opens a read-only details panel (name /
  ICAO / lat / lon / elev). Edits = delete + re-add. Icon order: Eye | Info |
  Grip | X. (`locations_update` remains available on both backends if needed.)

- **Follow Mode — track a single flight as it travels** (design notes
  2026-08-09; **hold — Dan thinking; do not implement yet**):
  - **Entry (current lean):** location menu **"+ Add Flight"** (not yet
    built). Earlier idea was detail-card / map tap; still open.
  - **While following:** Map keeps the flight centered (projection recenter
    on the aircraft each tick). Status-bar chip; clear exit (chip tap and/or
    location change).
  - **Basemap / continuous scroll:** today's basemap is a full-frame baked
    mosaic for one `(lat, lon, range, style)` — not a slippy layer.
    `MapProjection.offset_x/y` exist but are unused. Recenter → new mosaic
    (HTTP + warp or disk cache). True per-frame basemap scroll needs a real
    tile/pan layer (major). Practical v1: center overlays every tick; debounce
    basemap rebuild (every N nm / M s); accept brief geography lag. Rebuilding
    every small move would thrash cache and blank/misalign the map.
  - **ADS-B fetch:** still fixed-radius around the *active location*, not map
    center. Following a plane out of range loses it unless the query center
    moves with the aircraft (or follow ends at the edge). Main open design
    question.
  - **Existing "track":** `map_view_track` only draws a red ring; does not
    move center or fetch.

- ~~**Airport Mode, Phase 3 (INFO METAR/ATIS)**~~ **done 2026-08-09** —
  Pi INFO four-quadrant / 1/3–2/3 layout; METAR via aviationweather.gov;
  D-ATIS via datis.clowd.io (US majors). Europe deferred (no solid free API).

- **Quality of life / display settings**: color themes, font size — not
  started. ~~Brightness backend is real/complete but has no working UI~~
  **Pi brightness slider done 2026-08-09** (Settings DEVICE → sysfs
  `/sys/class/backlight`; needs udev write perms — see README). ESP32 UI
  still only lived in the deactivated screensaver settings.

- **Screensaver / sleep mode (brightness control included)**: built once
  (commit cf531b2: independent dim/blank idle timers, brightness slider, a
  drifting/jumping aircraft-count screensaver), then deliberately deactivated
  (`#if 0` in `screensaver.cpp`, both board targets) after user questioned the
  motivating burn-in rationale (this is an LCD panel, not OLED — burn-in
  doesn't apply the same way; the closest LCD analog, "image persistence,"
  fades on its own). Open question if revisited: pure inactivity-based
  dim/blank doesn't fit a rarely-touched wall-mounted "picture frame" use case
  — might want a time-of-day schedule instead/in addition, which would need
  NTP/RTC wall-clock time from scratch (nothing in this codebase currently has
  real time-of-day, only `millis()`-based elapsed time). Not decided — deferred
  for a later conversation.

- ~~**Redesign `configure_device.sh`/`.ps1`'s UX**~~ **removed 2026-08-09** —
  scripts deleted with Pi-only cleanup (USB-serial board tool). Use Settings
  / editing `~/.config/flightlevel314/config.json` on Pi instead.

- **Tidy up the Pi's boot sequence**: see §6. Current pain: off-center
  bright white Pi logo on the Waveshare 1280×800 DSI before the kiosk
  grabs DRM. On-device OS config, not app code.
  **Lean (2026-08-09, Dan):** quiet fullscreen image is fine — e.g. small
  RPi logo on black, centered for 1280×800. Open to a stock quieter theme.
  Practical options when we do it:
  - **Preferred simple path:** early fullscreen splash via
    `rpi-splash-screen-support` / `configure-splash` (TGA; black margins from
    top-left pixel; max 1920×1080) + `disable_splash=1` (no rainbow) +
    `logo.nologo` / quiet cmdline so kernel raspberries don't sit top-left.
  - **Stock quieter Plymouth (if Plymouth stays on):** install
    `plymouth-themes` and try **`spinner`** (dark bg + small spinner — least
    loud). Avoid stock **`pix`** ("Welcome to…" big white raspberry — what's
    likely offending now). `fade-in` / `spinfinity` still logo-heavy.
  - **Clone `pix`:** swap `splash.png` for a dim 1280×800 black+logo PNG
    (don't edit stock `pix` in place — updates overwrite; clone theme).
  - Still need to mind the getty/login flash until the kiosk owns DRM
    (console→tty3 / delay / earlier service) — separate from the logo itself.

- **Device provisioning — get API keys / tokens onto the Pi**: remember to
  sort out the story. Today: hand-edit
  `~/.config/flightlevel314/config.json` (or the kiosk path under
  `/opt/flightlevel314/.config/…`) for `apt_tok`, `adbox_key`, etc. Settings
  toggles features but does **not** accept typing secrets on the touchscreen
  (deliberate). Old ESP32 path was USB-serial `configure_device.sh` +
  `serial_config` — removed in Pi-only cleanup. Need a Pi-appropriate
  approach (SSH/scp recipe, first-boot wizard over SSH, companion script,
  USB stick drop, etc.). Scope: AirportDB, AeroDataBox, and any future
  keys — not just airportdb.

- **Pi-exclusive: real maps/sector charts using the extra resource budget**:
  raster/vector basemap tiles, FAA VFR sectionals. `tile_cache.cpp` exists in
  `src/ui/` but is explicitly disabled on ESP32 ("tiles broken on ESP32-P4") —
  worth checking if it's closer to reusable on Pi than starting fresh.
  Licensing/sourcing/storage-budget for sectionals not investigated. Not
  scoped. (Partial: Pi Map now has live/cached basemap styles via
  `pi/basemap.cpp` — Carto dark / dark_nolabels / Voyager cream light /
  voyager_nolabels / OpenTopoMap / FAA VFR sectional — with per-style
  disk-cache TTLs and a Settings "Clear map cache" button; see PR #4.)

- **Basemap vs runway/aircraft alignment (Pi)**: ~~Mercator tiles blitted
  1:1 vs equirectangular `MapProjection`~~ — addressed 2026-08-07: basemap
  build now warps tiles into the MapProjection frame (`eq1` cache key in
  `pi/basemap.cpp`). Residual mismatch can still come from OSM/FAA chart
  artwork vs airportdb runway endpoint definitions (different datasets).

- **Tap-to-open ATIS overlay on the INFO/Stats screen**: follow-on to the
  already-shipped METAR readout. Scoped to US airports first. Known complexity:
  some airports (KDEN named specifically) publish separate arrival/departure
  ATIS, not one combined broadcast — overlay needs to show both distinctly.
  No ATIS data source identified/vetted yet.

- **Logging cleanup — broader scope**: the concrete gap that motivated this
  (enrichment.cpp silent failures) is fixed, but ~31 `Serial.print*` call sites
  across the codebase still have no log-level system, no consistent
  prefix/format, mixed ad hoc debug prints and durable status logs. Needs
  clarification on the actual goal before a full pass.

- **README.md needs a massive update**: badly stale (still describes the old
  single-location architecture, lists removed route/origin-destination
  fields, lists removed FAST/SLOW/ODD filters). Must include a documented
  known-issues section on the SDIO crash (confirmed open upstream bug, not
  fixable here) and the full WiFi/platform-version-pin saga (host/C6 matching
  requirement, the confirmed-stable 55.03.37/2.11.6 pairing vs. confirmed-bad
  55.03.39/2.12.8, `dpoler/c6_updater`, the fast-fail/flat-delay WiFi fixes,
  and that this is a two-repo story). Note: a proper FlightLevel314 README
  already landed 2026-08-09 — this entry is the remaining ESP32/history/
  known-issues depth, not a from-scratch rewrite.

### 7.1b Deferred (Dan, 2026-08-09 — do not start)

- **Include small airports in the static on-device airport DB**: today's
  `tools/generate_airports_db.py` keeps only OurAirports `large_airport` +
  `medium_airport` (~5k entries, ~0.4 MB const with `name[64]`). Adding
  `small_airport` (ident ≤4, same filter) is ~+25.6k rows → ~30.6k total and
  ~2.5 MB aligned const (~+2.1 MB). Fine on Pi; painful if the same table
  stays shared with ESP32. Sized 2026-08-08; **deferred**.

- **Departure/destination via airframes.io ACARS**: Static callsign→route
  tables (adsbdb, adsb.lol) are unreliable/stale/non-directional. Better
  source: airframes.io OOOI events, but free tier needs an ACARS feeder
  (acarsdec/dumpvdl2) on the same SDR hardware. Commercial-jets-only.
  **Deferred** until/unless an ACARS feeder exists. (AeroDataBox / other O/D
  path on Pi is separate — see §8.)

### 7.1c Closed 2026-08-09 (Dan confirmation)

- ~~**Location switch: empty Map for a couple of refreshes (EGLL→KDEN)**~~
  **done** — list clear + recenter on Pi location switch (PR #3 era and
  follow-ups).
- ~~**planespotters.net photo fetch on Pi**~~ **done** — Pi detail-card
  image path (jc1060 remains text-only / PSRAM-limited).
- ~~**Ethernet-aware setup messaging / `ETHERNET=`**~~ **done**.
- ~~**Generalize Home into saved locations**~~ **done** — Home/saved-airport
  split removed; unified locations list (see §11).
- ~~**Settings "Device" column; move VERSION off Stats**~~ **done**.
- ~~**Map legend backdrop vs basemap**~~ **done**.
- ~~**Radar sweep arm smoother / fade trail**~~ **done**.
- ~~**Origin/destination display — revisit with new approach**~~ **done** —
  see §8 (no longer an open "revisit" item).

Removed from backlog entirely (2026-08-09): alert beeper (no hardware);
FILTER-menu-next-to-VIEW experiment (built then reverted); "flight
following / tracking" as a separate item (duplicate of Follow Mode);
configurable poll/refresh interval (deprioritized, dropped).

### 7.2 Notable "done" items worth knowing about (bugs, root causes, decisions)

*(Kept because the debugging trail/rationale is broadly useful — e.g. don't
re-introduce these exact bugs, or don't second-guess these exact decisions
without new evidence.)*

- **Map vs Radar visibility rules are intentionally different** (see §9 below)
  — do not "fix" one to match the other.
- **WiFi attempt-1-wastes-30s / fast-fail / C6-reset-readiness saga**: multiple
  rounds — root-caused that `wifi_connect_with_timeout()` never inspected
  `WiFi.status()` until its own timeout fired; added fast-fail on
  `WL_CONNECT_FAILED`/`WL_NO_SSID_AVAIL`. This then **exposed a second bug**:
  `reset_wifi_c6()`'s "readiness wait" polled for a status transition
  (255→other) that could never actually be observed (real status was 254 from
  a fresh boot) — it had always been a flat ~1.2s delay dressed up as adaptive.
  Fixed to an honest flat 3s delay. A regression was found and fixed after
  that: back-to-back fast-fail retries at 0-1ms intervals crashed the SDIO
  transport — added a 1s inter-retry delay.
- **Bullseye/legend centering saga (jc1060)**: four wrong guesses in a row
  before the team added a labeled 50px debug ruler overlay + forced the
  tileview scrollbar always-visible, then had the user photograph real
  hardware and read exact numbers off it. **Lesson: don't keep guessing from
  source alone for "where exactly does X render" questions — add a measurement
  ruler and ask for a photo immediately.** (This exact method was reused
  successfully for the Pi's bullseye sizing — see §6.)
- **"Waiting for aircraft" overlay stuck forever with 0 err shown**: root cause
  was a completely separate, unmonitored fetch path (`location_fetch_poll()`
  for saved/non-Home locations) with no stats tracking and no error logging.
  Fixed with a second stats counter + error logging. A real repro then
  surfaced: a saved location with a sign-flipped longitude landed in Inner
  Mongolia (zero real coverage there) — confirmed not a bug, a data-entry
  mistake, but exposed a genuine UX gap: the overlay still can't distinguish
  "genuinely empty feed" from "still connecting" or "broken." Not implemented:
  dismiss-after-N-empty-fetches or an explicit "connected, no traffic" state.
- **VIEW menu (trails, tag fields, secondary-locations)**: replaced separate
  TRAIL/TAG status-bar chips with one popover. Went through several follow-up
  fixes: cross-view setting leak when switching views with the popover open
  (fixed by closing the popover on any view change), a `lv_switch` resize bug
  that made "Show trails" unresponsive (root cause: an explicit
  `lv_obj_set_size()` after creation desynced the switch's hit-test region from
  its visual layout — two earlier "fixes" that replaced the switch with custom
  pill widgets were reverted once this was found), and per-view (Map vs Radar
  vs Arrivals) settings for trails/tags/secondary-locations/filters/GND, all
  stored as small `[N]`-indexed arrays.
- **Per-location "show nearby large airports' runways" toggle**: narrowed from
  "all airports in radius" to large-only after a sizing discussion (KJFK at
  50nm has ~20 airports). Only the active location's nearby-cache is kept
  resident in DRAM (lazy-loaded, avoiding a ~15x DRAM multiply).
- **VERT/GND mutual exclusion, GND illumination convention flip,
  HIGH/LOW filters, multi-select filter AND/OR semantics** — all done, with the
  general rule: category filters (COM/GA/HELI/MIL/EMG) are OR'd together
  (alternative classifications), state filters (VERT/HIGH/LOW) are AND'd
  against the category selection (narrowing conditions), and GND is a separate
  unconditional exclude, not part of the bitmask at all.
- **adsb.lol 429 rate-limiting backoff**: added `Retry-After`-aware and
  exponential (capped 5min) backoff on both pollers, after a user-reported
  ~13% error rate.
- **KORD showed decommissioned runways / missing active ones**: root-caused as
  airportdb.io's own upstream data being stale (confirmed against the live
  OurAirports CSV mirror), not a parsing bug in this project — user reported it
  directly to airportdb.io.
- **Cyan-flash-on-delete, and the general NVS-write-size lesson**: see §5.
- **Location picker LVGL delete-from-event-handler bug**: see §5's
  `lv_obj_delete_async()` note — the same underlying issue recurred/was fixed
  across multiple UI files (location picker, trail menu).
- **OTA updates**: full application-firmware OTA via GitHub Releases, built
  v0.1.0→v0.1.4 in one evening (see §10 below for full detail).
- **Resume last-used view/radius/location/filters after reboot**: persisted
  from discrete human-paced actions only (never from the since-removed
  auto-cycle timer, to avoid frequent blocking NVS writes). Location persisted
  by ICAO string (not array index, which isn't stable across removes). Needed
  two follow-up fixes: a boot crash when resuming into Arrivals with a
  non-Home location active (a mutex wasn't initialized yet at the point the
  resume call ran), and Map's WiFi-connecting overlay being invisible when
  boot resumed into a non-Map view (it was parented to the Map tile
  specifically).
- **Auto-cycle-views feature removed entirely** (2026-07-28) — if ever wanted
  back, needs to be rebuilt from scratch; nothing was left half-wired.
- **Settings panel progressively trimmed to a single column** — WiFi/Ethernet/
  Range/Metric only; Auto-Cycle, Home lat/lon, GND, Trails, and the
  airportdb.io token field were all removed from Settings over several passes
  (moved to VIEW menu, promoted to filter-column buttons, or removed entirely
  in the token's case — "we're not going to type it in here").
- **Small airports deliberately excluded from the static DB** — an accepted,
  permanent size/perf tradeoff (~42,700 more airports, ~683KB, ~8x scan cost),
  not a bug to revisit casually.
- **Better airportdb.io token entry mechanism**: replaced ad hoc manual serial
  typing with a structured `OK `/`ERR `-prefixed line protocol
  (`serial_config.cpp`) plus cross-platform CLI scripts
  (`configure_device.sh`/`.ps1`) — grew from "just the token" into a general
  low-friction config channel (WiFi credentials, factory reset, a first-time
  setup wizard). Two real protocol bugs found and fixed during testing: a
  `set -e`-triggered silent script death on read-timeout, and a serial-line
  desync where an unrelated debug print line got read as the real command
  reply, permanently offsetting every subsequent read by one command.

---

## 8. Route/origin-destination data

**History:** VRS/adsbdb-sourced route tables were removed from the app
(`Aircraft.origin`/`.dest`, Arrivals ROUTE column, etc.) because callsign-keyed
crowd data was unreliable (same SDM staleness as adsb.lol).

**2026-08-09 (Dan):** origin/destination is **done** again via the current
approach (AeroDataBox / enrichment on Pi — not the old VRS tables). airframes.io
ACARS remains a separate **deferred** idea (§7.1b) if a feeder ever exists.

---

## 9. Map vs Radar visibility design (deliberate, not a bug)

Map draws and lets you tap aircraft beyond the bullseye range ring, all the way
to the rectangular canvas edges — intentional, explicitly confirmed by the user
after an earlier "fix" wrongly corrected it away. This is what differentiates
Map (uses the full screen, looks like a map) from Radar (clips strictly to the
circular bullseye ring, to look like a radar). `MapProjection::to_screen()`
only checks the rectangular canvas bound; `radar_view.cpp`'s
`to_radar_screen()` explicitly enforces a circular `dist_nm > radius_nm` cutoff.
**Do not add a radius cutoff to Map's draw/tap-hit-test, and do not loosen
Radar's circular clip.** Any "is this visible" question should ask the specific
active view, not assume one universal rule (see `map_view_aircraft_visible()`
as the established pattern for this).

---

## 10. OTA updates (application firmware, via GitHub Releases)

Built and iterated v0.1.0→v0.1.4 in one evening (2026-07-26), real-hardware-
tested end-to-end on both boards (back when CrowPanel still existed).
Deliberately does **not** touch the ESP32-C6 co-processor's own firmware (a
separate, harder problem — see §4/§5).

- `partitions.csv` redesigned for two OTA app slots (3MB each).
- `src/version.h` — dev builds always report `"v0.0.0-dev"` so a local build
  never accidentally reports "up to date."
- `src/data/ota.{h,cpp}` — state machine, piggybacked on the existing
  `location_poll_task` loop rather than a dedicated task (same DRAM-safety
  reasoning as §5).
- `.github/workflows/release.yml` — tag push builds and attaches firmware
  binaries to a GitHub Release.
- **v0.1.3** fixed a Settings layout overlap bug (a `LV_ALIGN_BOTTOM_MID`
  padding-math mistake) and addressed reported screen flashing during flash
  write by freezing the whole UI (`lv_timer_enable(false)`) during download.
- **v0.1.4**: user wanted progress feedback during the freeze — added a live
  0-100% bar, a **deliberate, bounded exception** to the flash-free freeze
  (needs its own explicit `lv_refr_now()` per tick). Message text updated to
  say flashing during this phase is expected/harmless. Explicitly not chasing
  a fully flash-free update — visibility judged more valuable once the *bulk*
  of the flashing (other views' redraw traffic) was already eliminated.
- Open: optional fresher README gallery shots. Settings Device / VERSION and
  Pi online updates are **done** (Dan / 2026-08-09). The `.ps1` Windows OTA
  function was never syntax-checked (no pwsh in the dev environment).

---

## 11. Location-picker architecture

Moved from "APRT is a 5th swipeable tile" to a location-picker model: a unified
list of airports/waypoints (`src/data/locations.h/.cpp`), selectable via a
picker button. All views (Map/Radar/Arrivals/Stats) read from whichever
location is currently active. Runway diagrams draw inline in Map view instead
of a separate screen; the old `aprt_view.cpp`/`VIEW_APRT` was deleted.

`locations_add_from_icao()` fetches `airportdb.io`'s API and parses OurAirports
column names — all numeric fields arrive as JSON *strings*, not numbers, so
every numeric read uses `.as<float>()`/`.as<int>()`, not `| default`. Per-runway
`closed` field (also string-typed) must be checked — some airports (KORD) have
decommissioned runways that still carry valid coordinates.

**2026-08-09 (Dan):** generalizing Home into the saved-locations system is
**done** — do not treat Home as an architecturally distinct special case.

---

## 12. User preferences / how to work in this project (feedback memory)

- **Never add `Co-Authored-By: Claude...` to commits.** No exceptions. Past
  violations caused "claude" to appear as a GitHub contributor, which the user
  found unacceptable. Also: do not commit or push without explicit permission
  — make changes, then wait to be told to commit. Treat "no Co-Authored-By" as
  a hard checklist item on every single commit, not a one-time preference.
- **User handles firmware builds themselves.** Do not run `pio run` to verify
  changes — write the code and stop; note compile-error concerns in text if
  relevant, but don't build.
- **Justify, don't default**: when proposing to carry an existing tool/library/
  pattern forward into new work (e.g. porting to a new platform), state a real
  justification tied to the *new* context's actual constraints — don't just
  reuse it because it's already there. (This came from the LVGL-on-Pi decision
  — see §6.)

---

## 13. Cross-cutting lessons worth remembering

- **When "where exactly does X render" resists a few rounds of guessing, stop
  guessing from source and add a measurement ruler + ask for a real photo.**
  Used successfully twice (jc1060 bullseye centering, Pi bullseye centering).
- **Vendor docs and even a vendor's own GitHub issues can disagree with each
  other — trust real hardware/community reports over official examples**
  (CrowPanel SDIO pin values).
- **Any new FreeRTOS task on this ESP32-P4 board is a crash risk for network
  work, regardless of task lifetime** — piggyback on an existing task's loop.
- **`lv_obj_delete()` from inside an event handler on that object or a
  descendant is undefined behavior on this LVGL version** — use
  `lv_obj_delete_async()`.
- **Blocking NVS/flash writes over roughly a few hundred bytes, done
  synchronously on the UI/render thread, visibly stall this board's LCD panel**
  — keep such writes small or move them off the render path.
- **Once local instrumentation cleanly isolates a symptom's exact trigger,
  check *all* code paths that could react to that trigger — not just the one
  already under suspicion.** (The Pi tileview-wraparound bug was found this
  way after two other plausible-but-wrong theories were chased first.)
- **A shared cross-platform LVGL API can silently differ by resolved version**
  (`lv_draw_triangle_dsc_t`'s field names changed between LVGL 9.2.2 and
  9.5.0) — when only one platform build fails, check the *actual resolved*
  version on the working side; a `^9.2.2`-style semver range in
  `platformio.ini` does not tell you what's really installed.
