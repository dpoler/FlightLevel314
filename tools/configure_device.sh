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
# On connect, this queries the device's STATUS (added alongside this script
# revision -- requires firmware new enough to understand it, see
# get_status()'s comment for the fallback) and, if it looks never-configured
# (no WiFi credentials saved), runs the guided first-time setup automatically
# instead of showing the menu. An already-configured device goes straight to
# the menu, which also offers to re-run that same guided setup on demand.
#
# Menu option 5 checks for and installs application-firmware updates from
# GitHub Releases (does not touch the ESP32-C6 co-processor's own firmware,
# a deliberately separate problem -- see src/data/ota.cpp).

set -euo pipefail

BAUD=115200
PING_TIMEOUT=2
CMD_TIMEOUT=5

# ---- colors -------------------------------------------------------------
# Disabled automatically when stdout isn't a terminal (piped/redirected --
# e.g. the curl-to-bash one-liner in the header, if someone tees its output)
# so escape codes never end up mixed into a log file.
if [[ -t 1 ]]; then
    C_RESET=$'\033[0m'
    C_BOLD=$'\033[1m'
    C_DIM=$'\033[2m'
    C_GREEN=$'\033[32m'
    C_RED=$'\033[31m'
    C_CYAN=$'\033[36m'
    C_YELLOW=$'\033[33m'
else
    C_RESET='' C_BOLD='' C_DIM='' C_GREEN='' C_RED='' C_CYAN='' C_YELLOW=''
fi

heading() { echo ""; echo "${C_BOLD}${C_CYAN}== $1 ==${C_RESET}"; }

# Colors a device reply by its own OK/ERR prefix rather than needing every
# call site to know which it got -- one place to keep the convention in
# sync with serial_config.cpp's own "every reply starts with OK or ERR" rule.
color_resp() {
    case "$1" in
        OK*)  echo "${C_GREEN}$1${C_RESET}" ;;
        ERR*) echo "${C_RED}$1${C_RESET}" ;;
        *)    echo "$1" ;;
    esac
}

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

# Reads lines from fd 3 until one is an actual protocol reply (starts with
# "OK " or "ERR ") or the overall timeout budget runs out, discarding
# anything else. Necessary because the device's serial line is shared with
# incidental log output -- e.g. storage_save_config() (called by TOKEN=/
# WIFI_SSID=/WIFI_PASS= right before each one's own OK line) unconditionally
# prints "Storage: config saved to NVS" first. A naive "read one line, trust
# it" reader grabs that debug line instead of the real reply, leaving the
# real reply sitting unread in the buffer -- which then desyncs every
# following read for the rest of the session (each one now returns the
# *previous* command's real reply instead of its own), not just this one
# command (reported: WIFI_SSID showed the debug line as if it were the
# response, then the REBOOT reply after it never arrived). Same risk exists
# for any other Serial.println() anywhere in the firmware that might fire
# while a command is being handled -- filtering by prefix instead of
# blindly trusting the first line handles all of those too, not just this
# one instance.
#
# Empty output means "no response" (genuinely timed out, not just skipped a
# log line). The `|| { resp=""; break; }` on the read matters under
# `set -e`: `read -t` returns non-zero on timeout, and an unguarded timeout
# inside this function would otherwise become the *function's* own exit
# status -- and since every caller uses this as a plain
# `resp=$(read_response ...)` assignment (not inside a tested `if`/`&&`),
# that would silently kill the entire script right there under `set -e`,
# no error message, the menu just vanishes.
read_response() {
    local budget="$1" resp="" deadline now remaining
    deadline=$(( $(date +%s) + budget ))
    while :; do
        now=$(date +%s)
        remaining=$(( deadline - now ))
        (( remaining < 1 )) && remaining=1
        if (( now >= deadline )); then
            resp=""
            break
        fi
        IFS= read -r -t "$remaining" resp <&3 || { resp=""; break; }
        resp="${resp%$'\r'}"
        case "$resp" in
            OK*|ERR*) break ;;
            *) resp=""; continue ;; # stray log line -- discard, keep waiting within budget
        esac
    done
    printf '%s' "$resp"
}

# Reads and prints one response line, with a clear message if it timed out.
show_response() {
    local resp
    resp=$(read_response "$CMD_TIMEOUT")
    if [[ -z "$resp" ]]; then
        echo "${C_RED}(no response from device -- check it's still connected)${C_RESET}"
    else
        color_resp "$resp"
    fi
}

# Opens `port`, sends PING, checks for "OK PONG", closes. Used only during
# auto-detection to find the right port among possibly several connected
# USB-serial devices -- the main menu session opens its own long-lived fd.
#
# Retries PING for up to ~12s rather than one 2s attempt: opening a USB-
# serial port wired the same way esptool expects (DTR/RTS -> EN/IO0, which
# this board's WCH bridge is) typically resets the board right as the OS
# opens the device, same as it does before flashing. A single 2s PING_TIMEOUT
# window landed entirely inside that reboot -- LCD/touch/LVGL init alone
# takes longer than that before loop()'s serial_config_poll() is even
# running -- so probe_port() reported "no response" even though the board
# was fine and would have answered a few seconds later. reboot_and_reconnect()
# already handles this same situation (device reboots, needs time to come
# back) with a multi-second retry loop; this just applies the same idea here.
probe_port() {
    local port="$1"
    configure_stty "$port" 2>/dev/null || return 1
    exec 3<>"$port"
    local resp="" tries=0
    while (( tries < 6 )); do
        send_line "PING"
        resp=$(read_response "$PING_TIMEOUT")
        [[ "$resp" == OK* ]] && break
        tries=$((tries + 1))
    done
    exec 3<&-; exec 3>&-
    [[ "$resp" == OK* ]]
}

detect_port() {
    local candidates
    candidates=$(find_candidate_ports)
    if [[ -z "$candidates" ]]; then
        echo "${C_RED}No serial devices found. Is the board plugged in via USB?${C_RESET}" >&2
        exit 1
    fi
    local p
    while IFS= read -r p; do
        echo "${C_DIM}Checking $p...${C_RESET}" >&2
        if probe_port "$p"; then
            echo "$p"
            return 0
        fi
    done <<< "$candidates"
    echo "" >&2
    echo "${C_RED}Found serial device(s), but none responded to the device protocol:${C_RESET}" >&2
    echo "$candidates" | sed 's/^/  /' >&2
    echo "${C_RED}Is this an ADS-B Display board, and is it fully booted (not mid-flash)?${C_RESET}" >&2
    exit 1
}

# Sends REBOOT, closes the current connection, then reconnects on the same
# port -- so the menu loop has a live fd again instead of one talking to a
# device that's mid-boot or briefly gone. Retries the PING-probe for up to
# ~15s (USB CDC re-enumeration timing varies) rather than assuming a single
# fixed delay is always enough. Reconnecting over USB only confirms the
# *device* came back, not that WiFi did -- both current callers only ever
# reboot to apply new WiFi credentials, so this also polls WIFI_STATUS
# afterward and reports plainly whether they actually connected (a typo'd
# SSID/password otherwise looks identical to success here -- USB comes back
# fine either way).
reboot_and_reconnect() {
    echo "Rebooting device to apply changes..."
    send_line "REBOOT"
    show_response
    { exec 3<&- ; exec 3>&- ; } 2>/dev/null

    local tries=0
    while (( tries < 15 )); do
        sleep 1
        tries=$((tries + 1))
        if [[ -e "$PORT" ]] && probe_port "$PORT"; then
            configure_stty "$PORT"
            exec 3<>"$PORT"
            echo "${C_GREEN}Reconnected.${C_RESET}"
            check_wifi_status
            return 0
        fi
    done

    echo "${C_RED}Could not reconnect to $PORT after reboot -- if it re-enumerated" >&2
    echo "under a different name, re-run this script.${C_RESET}" >&2
    exit 1
}

# Polls WIFI_STATUS for up to ~10s -- association after a fresh boot isn't
# instant, so a single immediate check can catch it still connecting and
# wrongly report failure. Prints a clear result either way; callers that
# just need to know whether it's safe to attempt something network-dependent
# (e.g. do_set_token()'s live verification) can check this function's exit
# status instead of re-parsing its output.
check_wifi_status() {
    local resp="" tries=0
    while (( tries < 10 )); do
        send_line "WIFI_STATUS"
        resp=$(read_response "$CMD_TIMEOUT")
        [[ "$resp" == "OK connected" ]] && break
        sleep 1
        tries=$((tries + 1))
    done
    if [[ "$resp" == "OK connected" ]]; then
        echo "${C_GREEN}WiFi connected.${C_RESET}"
        return 0
    else
        echo "${C_RED}WiFi did not connect -- double-check the SSID/password (menu option 2, or re-run first-time setup).${C_RESET}"
        return 1
    fi
}

# Requests a live token check against airportdb.io and polls for the result
# -- a real HTTPS round trip (locations_verify_token_poll(), locations.cpp),
# so this can take several seconds, same as OTA_CHECK. Only call this once
# WiFi is confirmed connected (see do_set_token() below) -- with no network
# it'll just report "no response" indefinitely, which reads as a token
# problem when it isn't one.
verify_token() {
    send_line "TOKEN_VERIFY"
    local resp
    resp=$(read_response "$CMD_TIMEOUT")
    if [[ -z "$resp" ]]; then
        echo "${C_RED}(no response from device -- check it's still connected)${C_RESET}"
        return
    fi
    local tries=0
    while (( tries < 15 )); do
        sleep 1
        send_line "TOKEN_VERIFY_STATUS"
        resp=$(read_response "$CMD_TIMEOUT")
        tries=$((tries + 1))
        [[ "$resp" == "OK pending" ]] || break
    done
    case "$resp" in
        "OK valid")
            echo "${C_GREEN}Token verified -- it works.${C_RESET}"
            ;;
        OK\ invalid*)
            echo "${C_RED}Token did NOT verify: ${resp#OK invalid }${C_RESET}"
            echo "${C_YELLOW}Double-check what you pasted (menu option 1) -- airportdb.io rejected it.${C_RESET}"
            ;;
        *)
            echo "${C_YELLOW}Could not verify right now -- it'll be checked again the next time it's used.${C_RESET}"
            ;;
    esac
}

# Queries device configuration state (added to serial_config.cpp alongside
# this script revision). Returns "wifi=0/1 token=0/1 locations=N" on
# success, or an empty string if the device didn't answer (offline) or
# doesn't understand STATUS yet (older firmware, predating this command) --
# either way, callers treat that as "can't tell, don't guess" and fall back
# to showing the normal menu rather than assuming unconfigured.
get_status() {
    send_line "STATUS"
    local resp
    resp=$(read_response "$CMD_TIMEOUT")
    if [[ "$resp" == OK\ wifi=* ]]; then
        printf '%s' "${resp#OK }"
    else
        printf ''
    fi
}

# ---- setup steps, shared by the standalone menu items and the wizard ----

# Application-firmware update via GitHub Releases (data/ota.cpp) -- does
# NOT touch the ESP32-C6 co-processor's own firmware, see that file's
# comment for why. Checks, shows the result, and only downloads/installs
# with explicit confirmation once an update is confirmed available.
do_ota_update() {
    send_line "OTA_CHECK"
    local resp
    resp=$(read_response "$CMD_TIMEOUT")
    if [[ -z "$resp" ]]; then
        echo "${C_RED}(no response from device -- check it's still connected)${C_RESET}"
        return
    fi
    color_resp "$resp"

    local tries=0
    while (( tries < 10 )); do
        sleep 1
        send_line "OTA_STATUS"
        resp=$(read_response "$CMD_TIMEOUT")
        tries=$((tries + 1))
        [[ "$resp" == *"checking"* ]] || break
    done

    if [[ -z "$resp" ]]; then
        echo "${C_RED}(no response checking status -- try again)${C_RESET}"
        return
    fi
    color_resp "$resp"

    if [[ "$resp" != *"update available"* ]]; then
        return
    fi

    read -r -p "${C_CYAN}Download and install this update now? [y/N]: ${C_RESET}" confirm
    if [[ "$confirm" != "y" && "$confirm" != "Y" ]]; then
        echo "Cancelled."
        return
    fi

    send_line "OTA_UPDATE"
    resp=$(read_response "$CMD_TIMEOUT")
    if [[ -z "$resp" ]]; then
        echo "${C_RED}(no response from device -- check it's still connected)${C_RESET}"
        return
    fi
    color_resp "$resp"

    echo "Downloading -- this can take a minute or two. The device reboots itself automatically once done."
    tries=0
    while (( tries < 90 )); do
        sleep 2
        send_line "OTA_STATUS"
        resp=$(read_response "$CMD_TIMEOUT")
        tries=$((tries + 1))
        if [[ "$resp" == *"downloading"* ]]; then
            echo "${C_DIM}$resp${C_RESET}"
            continue
        fi
        break
    done

    if [[ "$resp" == *"error"* ]]; then
        echo "${C_RED}Update failed -- device should still be running its previous firmware.${C_RESET}"
    elif [[ -z "$resp" ]]; then
        echo "${C_YELLOW}Device stopped responding -- if the download had finished, this is expected (it reboots itself into the new firmware). Re-run this script to confirm it came back up. If it doesn't, reflash over USB.${C_RESET}"
    fi
}

# Empty input skips rather than sending TOKEN= with an empty value -- makes
# this safe to back out of both standalone (menu option 1: pressing Enter
# won't accidentally clear an already-set token) and inside the wizard
# below (where it's explicitly optional -- "offer", not require). Live-
# verifies afterward when possible -- a plain "OK Saved" here previously
# looked identical whether the token was right or a typo, and the actual
# 401/403/400 rejection only ever surfaced later, on-device, while trying
# to add a real airport (reported: an invalid token still read as
# "Configured" everywhere until that point).
do_set_token() {
    read -r -s -p "Paste your airportdb.io token: " token
    echo ""
    if [[ -z "$token" ]]; then
        echo "${C_DIM}Skipped -- you can set this anytime from the menu.${C_RESET}"
        return
    fi
    send_line "TOKEN=$token"
    show_response
    echo "Used automatically the next time you add an airport by ICAO on the device."

    # Quick, no-retry check -- unlike reboot_and_reconnect()'s use of this
    # same WIFI_STATUS command, there's no reason to expect a connection to
    # still be settling right here, so a single read is enough to decide
    # whether it's worth attempting live verification at all.
    send_line "WIFI_STATUS"
    local wifi_resp
    wifi_resp=$(read_response "$CMD_TIMEOUT")
    if [[ "$wifi_resp" == "OK connected" ]]; then
        echo "Verifying against airportdb.io..."
        verify_token
    else
        echo "${C_DIM}Not connected to WiFi right now -- skipping live verification. It'll be caught the first time you actually add an airport if it's wrong.${C_RESET}"
    fi
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
        echo "${C_RED}(no response from device -- check it's still connected)${C_RESET}"
        return
    fi
    color_resp "$resp"
    if [[ "$resp" == OK* ]]; then
        echo "Saved to the device's location list. This does NOT change what's"
        echo "currently on screen and the new location is not auto-selected --"
        echo "on the device, tap the location button (top of the screen) and"
        echo "choose \"$loc_name\" from the list to actually view it. No reboot"
        echo "needed for this to take effect."
    fi
}

# Guided first-time setup -- runs automatically on an unconfigured device
# (see the STATUS check in the main script body below) or on demand from
# the menu ("re-run the initial setup"). WiFi first, then an immediate
# reboot to actually apply it and confirm it connected -- deliberately
# *before* the token step, not after (like the original version of this
# wizard had it), so airportdb.io token entry can live-verify against a
# real connection instead of just trusting whatever was typed. Location is
# offered/optional after that; the update check comes last since it also
# needs real connectivity, which by then it already has.
run_first_time_setup() {
    heading "First-time setup"
    echo "Walks through WiFi, your airportdb.io token, and one saved location,"
    echo "then checks for a firmware update."

    heading "WiFi"
    do_set_wifi
    reboot_and_reconnect

    heading "airportdb.io token"
    echo "This is a free token that lets the device fetch runway geometry when"
    echo "you save an airport by ICAO code (not needed for Home or plain"
    echo "lat/lon waypoints). Get one at:"
    echo ""
    echo "  ${C_CYAN}https://airportdb.io${C_RESET}"
    echo ""
    echo "Sign up, then paste the token below -- or just press Enter to skip"
    echo "this for now and set it later from the menu."
    do_set_token

    heading "First saved location"
    local added_location=0
    read -r -p "Save a location now? [Y/n]: " loc_confirm
    if [[ -z "$loc_confirm" || "$loc_confirm" == "y" || "$loc_confirm" == "Y" ]]; then
        do_add_location
        added_location=1
    else
        echo "${C_DIM}Skipped -- you can add locations anytime from the menu, or on the device itself.${C_RESET}"
    fi

    heading "Checking for updates"
    do_ota_update

    echo ""
    echo "${C_BOLD}${C_GREEN}ok, good to go!${C_RESET}"
    if [[ "$added_location" -eq 1 ]]; then
        echo "${C_YELLOW}One more step:${C_RESET} on the device, tap the location button"
        echo "(top of the screen) and select the location you just added -- it"
        echo "won't show any traffic until you do, since nothing is selected yet."
    fi
}

# ---- entry point ----------------------------------------------------------

echo "ADS-B Display -- device configuration"
echo "Looking for the device..."
PORT=$(detect_port)
echo "Found device on $PORT"
configure_stty "$PORT"
exec 3<>"$PORT"
# EXIT traps are inherited by every subshell, including the ones $(...)
# command substitution spawns -- and this script uses $(...) constantly
# (every read_response call). Without the $BASH_SUBSHELL guard, this was
# firing (and closing this shell's copy of fd 3) on every single one of
# those, not just at real script exit -- dozens of spurious partial-closes
# on the serial port over the course of a single run. Harmless on a regular
# file (each process has its own fd table either way), but a serial device
# on macOS can have real side effects from any close(), not just the last
# reference's -- a plausible source of cumulative flakiness that's hard to
# pin down by reading code alone. BASH_SUBSHELL is 0 only in the top-level
# shell, so this now only actually closes anything on a real exit.
trap '[ "$BASH_SUBSHELL" -eq 0 ] && { { exec 3<&- ; exec 3>&- ; } 2>/dev/null; }' EXIT

status=$(get_status)
if [[ -n "$status" && "$status" == wifi=0* ]]; then
    run_first_time_setup
    exit 0
fi
# status is either "wifi=1 ..." (already configured -- show the normal menu)
# or empty (offline reply / older firmware that predates STATUS -- can't
# tell either way, so fall through to the same menu rather than guessing).

while true; do
    echo ""
    echo "${C_BOLD}${C_CYAN}1) Set airportdb.io API token${C_RESET}"
    echo "${C_BOLD}${C_CYAN}2) Set WiFi credentials${C_RESET}"
    echo "${C_BOLD}${C_CYAN}3) Add a saved location (name/lat/lon/elevation)${C_RESET}"
    echo "${C_BOLD}${C_CYAN}4) Factory reset (erase all settings and saved locations)${C_RESET}"
    echo "${C_BOLD}${C_CYAN}5) Check for / install a firmware update${C_RESET}"
    echo "${C_BOLD}${C_CYAN}6) Re-run first-time setup (WiFi + token + one location)${C_RESET}"
    echo "${C_BOLD}${C_CYAN}0) Exit${C_RESET}"
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
            read -r -p "${C_RED}This will ERASE ALL settings and saved locations. Type YES to confirm: ${C_RESET}" confirm
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
            do_ota_update
            ;;
        6)
            run_first_time_setup
            ;;
        0)
            break
            ;;
        *)
            echo "${C_RED}Not a valid option.${C_RESET}"
            ;;
    esac
done

echo "Done."
