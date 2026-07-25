#pragma once

// Poll Serial (USB CDC) for one-line config commands, so long values (like an
// airportdb.io API token or a WiFi password) can be pasted/scripted from a
// computer instead of typed on the on-screen keyboard. Call once per loop()
// iteration — non-blocking. Backs tools/configure_device.sh (macOS/Linux)
// and tools/configure_device.ps1 (Windows) — see those for the actual
// user-facing menu; this is just the wire protocol they speak.
//
// Every command replies with exactly one line, prefixed "OK " (succeeded,
// rest of the line is a human-readable detail) or "ERR " (failed/unknown).
// Every command is idempotent — a "set to X" or "clear everything"
// operation, never append/increment — so sending the same line twice always
// converges to the same end state.
//
// Supported commands (one per line, terminated by \n):
//   PING                      -- connectivity/protocol check, replies "OK PONG"
//   TOKEN=<value>             -- airportdb.io API token
//   WIFI_SSID=<value>         -- WiFi network name (takes effect after reboot)
//   WIFI_PASS=<value>         -- WiFi password (takes effect after reboot)
//   ADD_WAYPOINT=<name>|<lat>|<lon>|<elevation_ft>
//                             -- adds a saved waypoint location (pipe-delimited,
//                                not comma, so name can contain a comma)
//   FACTORY_RESET=CONFIRM     -- erases all settings + saved locations, reboots.
//                                Exact confirm string required on purpose --
//                                this is destructive and this protocol is
//                                meant to be easy to script, so it shouldn't
//                                also be easy to trigger by accident.
void serial_config_poll();
