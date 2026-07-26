#!/usr/bin/env bash
# ADS-B Display -- device configuration tool (macOS/Linux)
#
# Talks to the device over USB serial using the plain-text line protocol in
# src/data/serial_config.cpp (see that file for the full command list). Zero
# external dependencies -- uses only `stty` + a shell file descriptor against
# the serial device node, both already present on every Mac/Linux box, so
# this can be run straight from a one-liner:
#
#   curl -sSL https://raw.githubusercontent.com/dpoler/adsb/master/tools/configure_device.sh | bash
#
# Every command sent to the device is a "set to X" (or "clear everything")
# operation, never append/increment -- so this script is safe to run more
# than once, or to run the same menu option more than once in one session.
#
# Menu option 5 is intentionally stubbed -- OTA firmware updates aren't
# built yet (see the project backlog). The menu structure and serial
# plumbing here are already in place for when it is.

set -euo pipefail

BAUD=115200
PING_TIMEOUT=2
CMD_TIMEOUT=5

# ---- serial helpers ---------------------------------------------------

find_candidate_ports() {
    ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || true
}

configure_stty() {
    local port="$1"
    if [[ "$(uname)" == "Darwin" ]]; then
        stty -f "$port" "$BAUD" raw -echo
    else
        stty -F "$port" "$BAUD" raw -echo -echoe -echok
    fi
}

# Sends one line to fd 3 (CRLF -- the device trims a trailing \r either way).
send_line() {
    printf '%s\r\n' "$1" >&3
}

# Reads one line from fd 3 with a timeout, stripping any trailing \r.
# Empty output means "no response" (timed out). The `|| true` matters under
# `set -e`: `read -t` returns non-zero on timeout, and since every caller of
# this function uses it as a plain `resp=$(read_response ...)` assignment
# (not inside an `if`/`&&`), an unguarded timeout would silently kill the
# entire script right there -- no error message, the menu just vanishes.
# ADD_WAYPOINT's synchronous NVS write (slower the more locations are
# already saved -- see the project backlog's cyan-flash-on-NVS-write entry)
# was the one most likely to occasionally miss the window and trip this.
read_response() {
    local timeout="$1" resp=""
    IFS= read -r -t "$timeout" resp <&3 || true
    printf '%s' "${resp%$'\r'}"
}

# Reads and prints one response line, with a clear message if it timed out.
show_response() {
    local resp
    resp=$(read_response "$CMD_TIMEOUT")
    if [[ -z "$resp" ]]; then
        echo "(no response from device -- check it's still connected)"
    else
        echo "$resp"
    fi
}

# Opens `port`, sends PING, checks for "OK PONG", closes. Used only during
# auto-detection to find the right port among possibly several connected
# USB-serial devices -- the main menu session opens its own long-lived fd.
probe_port() {
    local port="$1"
    configure_stty "$port" 2>/dev/null || return 1
    exec 3<>"$port"
    send_line "PING"
    local resp
    resp=$(read_response "$PING_TIMEOUT")
    exec 3<&-; exec 3>&-
    [[ "$resp" == OK* ]]
}

detect_port() {
    local candidates
    candidates=$(find_candidate_ports)
    if [[ -z "$candidates" ]]; then
        echo "No serial devices found. Is the board plugged in via USB?" >&2
        exit 1
    fi
    local p
    while IFS= read -r p; do
        echo "Checking $p..." >&2
        if probe_port "$p"; then
            echo "$p"
            return 0
        fi
    done <<< "$candidates"
    echo "" >&2
    echo "Found serial device(s), but none responded to the device protocol:" >&2
    echo "$candidates" | sed 's/^/  /' >&2
    echo "Is this an ADS-B Display board, and is it fully booted (not mid-flash)?" >&2
    exit 1
}

# Sends REBOOT, closes the current connection, then reconnects on the same
# port -- so the menu loop has a live fd again instead of one talking to a
# device that's mid-boot or briefly gone. Retries the PING-probe for up to
# ~15s (USB CDC re-enumeration timing varies) rather than assuming a single
# fixed delay is always enough.
reboot_and_reconnect() {
    echo "Rebooting device to apply changes..."
    send_line "REBOOT"
    show_response
    exec 3<&- 2>/dev/null; exec 3>&- 2>/dev/null

    local tries=0
    while (( tries < 15 )); do
        sleep 1
        tries=$((tries + 1))
        if [[ -e "$PORT" ]] && probe_port "$PORT"; then
            configure_stty "$PORT"
            exec 3<>"$PORT"
            echo "Reconnected."
            return 0
        fi
    done

    echo "Could not reconnect to $PORT after reboot -- if it re-enumerated" >&2
    echo "under a different name, re-run this script." >&2
    exit 1
}

# ---- setup steps, shared by the standalone menu items and the wizard ----

do_set_token() {
    read -r -s -p "Paste your airportdb.io token: " token
    echo ""
    send_line "TOKEN=$token"
    show_response
    echo "Used automatically the next time you add an airport by ICAO on the device."
}

do_set_wifi() {
    read -r -p "WiFi SSID: " ssid
    send_line "WIFI_SSID=$ssid"
    show_response
    read -r -s -p "WiFi password: " pass
    echo ""
    send_line "WIFI_PASS=$pass"
    show_response
    echo "The device only reads WiFi credentials at boot, so a restart is needed"
    echo "before it connects with these -- this script will do that for you."
}

# Distinct from show_response -- needs the actual response text (not just
# printed) to decide whether to show the follow-up explanation, since that
# only makes sense if the add actually succeeded (an ERR here is usually
# "location list full" or "name already used", not a connectivity problem).
do_add_location() {
    read -r -p "Location name (short, no '|'): " loc_name
    read -r -p "Latitude: " loc_lat
    read -r -p "Longitude: " loc_lon
    read -r -p "Elevation (ft): " loc_elev
    send_line "ADD_WAYPOINT=${loc_name}|${loc_lat}|${loc_lon}|${loc_elev}"
    local resp
    resp=$(read_response "$CMD_TIMEOUT")
    if [[ -z "$resp" ]]; then
        echo "(no response from device -- check it's still connected)"
        return
    fi
    echo "$resp"
    if [[ "$resp" == OK* ]]; then
        echo "Saved to the device's location list. This does NOT change what's"
        echo "currently on screen and the new location is not auto-selected --"
        echo "on the device, tap the location button (top of the screen) and"
        echo "choose \"$loc_name\" from the list to actually view it. No reboot"
        echo "needed for this to take effect."
    fi
}

# ---- menu ---------------------------------------------------------------

echo "ADS-B Display -- device configuration"
echo "Looking for the device..."
PORT=$(detect_port)
echo "Found device on $PORT"
configure_stty "$PORT"
exec 3<>"$PORT"
trap 'exec 3<&- 2>/dev/null; exec 3>&- 2>/dev/null' EXIT

while true; do
    echo ""
    echo "1) Set airportdb.io API token"
    echo "2) Set WiFi credentials"
    echo "3) Add a saved location (name/lat/lon/elevation)"
    echo "4) Factory reset (erase all settings and saved locations)"
    echo "5) Update firmware  [not yet supported by this firmware]"
    echo "6) First-time setup wizard (WiFi + token + one location)"
    echo "0) Exit"
    read -r -p "Choose an option: " choice

    case "$choice" in
        1)
            do_set_token
            ;;
        2)
            do_set_wifi
            reboot_and_reconnect
            ;;
        3)
            do_add_location
            ;;
        4)
            read -r -p "This will ERASE ALL settings and saved locations. Type YES to confirm: " confirm
            if [[ "$confirm" == "YES" ]]; then
                send_line "FACTORY_RESET=CONFIRM"
                show_response
                echo "Device is rebooting. Re-run this script if you want to configure it again."
                break
            else
                echo "Cancelled."
            fi
            ;;
        5)
            echo "Not yet supported -- see the OTA-updates item in the project backlog."
            ;;
        6)
            echo ""
            echo "== First-time setup wizard =="
            echo "Walks through WiFi, your airportdb.io token, and one saved"
            echo "location, then reboots the device once at the end."
            echo ""
            echo "-- WiFi --"
            do_set_wifi
            echo ""
            echo "-- airportdb.io token --"
            do_set_token
            echo ""
            echo "-- First saved location --"
            do_add_location
            echo ""
            reboot_and_reconnect
            echo "Setup complete."
            ;;
        0)
            break
            ;;
        *)
            echo "Not a valid option."
            ;;
    esac
done

echo "Done."
