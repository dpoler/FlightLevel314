#!/usr/bin/env bash
# Idempotent Cloud Agent install for dpoler/adsb.
# Warms PlatformIO (ESP32) and, when present, the Pi SDL simulator caches.
set -euo pipefail

export PATH="${HOME}/.platformio/penv/bin:${HOME}/.local/bin:${PATH}"

# Standing preference: no agent self-attribution on GitHub (no Co-authored-by).
for hook in "${HOME}"/.cursor/agent-hooks/*/commit-msg.cursor.co-author; do
  if [[ -f "${hook}" ]]; then
    printf '%s\n' '#!/bin/bash' '# Disabled for dpoler/adsb — no agent self-attribution.' 'exit 0' > "${hook}"
    chmod +x "${hook}"
  fi
done

# PlatformIO Core via official installer (isolated venv under ~/.platformio/penv).
# Avoids Ubuntu 24.04 PEP 668 "externally-managed-environment" errors from bare pip.
if ! command -v pio >/dev/null 2>&1; then
  curl -fsSL -o /tmp/get-platformio.py \
    https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py
  python3 /tmp/get-platformio.py
fi

# Optional codegen tools (Pillow/requests) into PlatformIO's venv.
"${HOME}/.platformio/penv/bin/pip" install --upgrade pip
"${HOME}/.platformio/penv/bin/pip" install Pillow requests

# Ensure non-interactive shells find `pio`.
MARKER='# adsb-cloud-pio-path'
if ! grep -q "${MARKER}" "${HOME}/.bashrc" 2>/dev/null; then
  {
    echo "${MARKER}"
    echo 'export PATH="$HOME/.platformio/penv/bin:$HOME/.local/bin:$PATH"'
  } >> "${HOME}/.bashrc"
fi

# Warm ESP32 platform / toolchain / libs into ~/.platformio (cached in Builds).
# Exception to "user handles builds": environment-setup may run pio to prove tooling.
pio run -e jc1060

# Optional but required for VIEW → Other Airports glyphs on Map/Radar.
if [[ -f tools/generate_airports_db.py ]]; then
  python3 tools/generate_airports_db.py
fi

# Pi / Linux SDL simulator (only on branches that include the pi/ tree, e.g. pi-port).
if [[ -f CMakeLists.txt && -d pi ]]; then
  cmake -S . -B build -DPI_DISPLAY_BACKEND=SDL
  cmake --build build -j"$(nproc)"
fi

echo "adsb cloud install complete"
