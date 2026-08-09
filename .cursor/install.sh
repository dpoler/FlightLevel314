#!/usr/bin/env bash
# Idempotent Cloud Agent install for dpoler/FlightLevel314 (Pi / Linux SDL).
# PlatformIO / ESP32 / jc1060 are intentionally not installed — this fork is Pi-only.
set -euo pipefail

VENV_DIR="${HOME}/.venvs/flightlevel314"
export PATH="${VENV_DIR}/bin:${HOME}/.local/bin:${PATH}"

# Standing preference: no agent self-attribution on GitHub (no Co-authored-by).
for hook in "${HOME}"/.cursor/agent-hooks/*/commit-msg.cursor.co-author; do
  if [[ -f "${hook}" ]]; then
    printf '%s\n' '#!/bin/bash' '# Disabled for dpoler/FlightLevel314 — no agent self-attribution.' 'exit 0' > "${hook}"
    chmod +x "${hook}"
  fi
done

# Optional codegen tools (Pillow/requests) in an isolated venv.
# Avoids Ubuntu 24.04 PEP 668 "externally-managed-environment" errors from bare pip.
if [[ ! -x "${VENV_DIR}/bin/python" ]]; then
  python3 -m venv "${VENV_DIR}"
fi
"${VENV_DIR}/bin/pip" install --upgrade pip
"${VENV_DIR}/bin/pip" install Pillow requests

# Ensure non-interactive shells find the codegen venv.
MARKER='# flightlevel314-cloud-venv-path'
if ! grep -q "${MARKER}" "${HOME}/.bashrc" 2>/dev/null; then
  {
    echo "${MARKER}"
    echo "export PATH=\"\$HOME/.venvs/flightlevel314/bin:\$HOME/.local/bin:\$PATH\""
  } >> "${HOME}/.bashrc"
fi

# Optional but required for VIEW → Other Airports glyphs on Map/Radar.
if [[ -f tools/generate_airports_db.py ]]; then
  python3 tools/generate_airports_db.py
fi

# Pi / Linux SDL simulator — exception to "user handles builds": warm the cache
# so Cloud Agents and environment Builds start with a verified binary.
if [[ -f CMakeLists.txt && -d pi ]]; then
  cmake -S . -B build -DPI_DISPLAY_BACKEND=SDL
  cmake --build build -j"$(nproc)"
fi

echo "FlightLevel314 cloud install complete"
