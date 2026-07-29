# ADS-B Display -- device configuration tool (Windows)
#
# Talks to the device over USB serial using the plain-text line protocol in
# src/data/serial_config.cpp (see that file for the full command list). Zero
# external dependencies -- uses .NET's System.IO.Ports.SerialPort, which
# ships with PowerShell already, so this can be run straight from a
# one-liner:
#
#   irm https://raw.githubusercontent.com/dpoler/adsb/master/tools/configure_device.ps1 | iex
#
# Every command sent to the device is a "set to X" (or "clear everything")
# operation, never append/increment -- so this script is safe to run more
# than once, or to run the same menu option more than once in one session.
#
# On connect, this queries the device's STATUS (added alongside this script
# revision -- requires firmware new enough to understand it, see
# Get-DeviceStatus's comment for the fallback) and, if it looks
# never-configured (no WiFi credentials saved), runs the guided first-time
# setup automatically instead of showing the menu. An already-configured
# device goes straight to the menu, which also offers to re-run that same
# guided setup on demand.
#
# Menu option 5 checks for and installs application-firmware updates from
# GitHub Releases (does not touch the ESP32-C6 co-processor's own firmware,
# a deliberately separate problem -- see src/data/ota.cpp).

$ErrorActionPreference = "Stop"
$Baud = 115200
$PingTimeoutMs = 2000
$CmdTimeoutMs = 5000

# ---- colors ---------------------------------------------------------------
# Write-Host's -ForegroundColor already no-ops sensibly when output isn't a
# real console (redirected to a file, etc.), unlike raw ANSI escapes, so no
# extra "is this a terminal" check is needed here the way the bash version
# needs one.
function Write-Heading {
    param([string]$Text)
    Write-Host ""
    Write-Host "== $Text ==" -ForegroundColor Cyan
}

# Colors a device reply by its own OK/ERR prefix rather than needing every
# call site to know which it got -- one place to keep the convention in
# sync with serial_config.cpp's own "every reply starts with OK or ERR" rule.
function Write-DeviceResponse {
    param([string]$Text)
    if ($Text -like "OK*") {
        Write-Host $Text -ForegroundColor Green
    } elseif ($Text -like "ERR*") {
        Write-Host $Text -ForegroundColor Red
    } else {
        Write-Host $Text
    }
}

function New-DevicePort {
    param([string]$PortName, [int]$TimeoutMs)
    $p = New-Object System.IO.Ports.SerialPort $PortName, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
    $p.ReadTimeout = $TimeoutMs
    $p.WriteTimeout = $TimeoutMs
    $p.NewLine = "`n"
    return $p
}

# Reads lines until one is an actual protocol reply (starts with "OK" or
# "ERR") or the overall read-timeout budget runs out, discarding anything
# else. Necessary because the device's serial line is shared with incidental
# log output -- e.g. storage_save_config() (called by TOKEN=/WIFI_SSID=/
# WIFI_PASS= right before each one's own OK line) unconditionally prints
# "Storage: config saved to NVS" first. A naive "read one line, trust it"
# reader grabs that debug line instead of the real reply, leaving the real
# reply sitting unread in the buffer -- which then desyncs every following
# read for the rest of the session (each one now returns the *previous*
# command's real reply instead of its own), not just this one command
# (reported: WIFI_SSID showed the debug line as if it were the response,
# then the REBOOT reply after it never arrived). Same risk exists for any
# other stray Serial output anywhere in the firmware -- filtering by prefix
# instead of blindly trusting the first line handles all of those too.
function Send-AndRead {
    param($Port, [string]$Line)
    $Port.WriteLine($Line)
    $originalTimeout = $Port.ReadTimeout
    $deadline = (Get-Date).AddMilliseconds($originalTimeout)
    try {
        while ($true) {
            $remainingMs = [int](($deadline - (Get-Date)).TotalMilliseconds)
            if ($remainingMs -le 0) { return "" }
            $Port.ReadTimeout = $remainingMs
            try {
                $resp = $Port.ReadLine().Trim()
            } catch [System.TimeoutException] {
                return ""
            }
            if ($resp -like "OK*" -or $resp -like "ERR*") {
                return $resp
            }
            # else: stray log line -- discard, keep waiting within budget
        }
    } finally {
        $Port.ReadTimeout = $originalTimeout
    }
}

function Show-Response {
    param($Port, [string]$Line)
    $resp = Send-AndRead $Port $Line
    if ([string]::IsNullOrEmpty($resp)) {
        Write-Host "(no response from device -- check it's still connected)" -ForegroundColor Red
    } else {
        Write-DeviceResponse $resp
    }
}

function Test-DevicePort {
    param([string]$PortName)
    try {
        $p = New-DevicePort $PortName $PingTimeoutMs
        $p.Open()
        $resp = Send-AndRead $p "PING"
        $p.Close()
        return $resp -like "OK*"
    } catch {
        return $false
    }
}

function Find-DevicePort {
    $candidates = [System.IO.Ports.SerialPort]::GetPortNames()
    if (-not $candidates -or $candidates.Count -eq 0) {
        Write-Host "No serial devices found. Is the board plugged in via USB?" -ForegroundColor Red
        exit 1
    }
    foreach ($name in $candidates) {
        Write-Host "Checking $name..."
        if (Test-DevicePort $name) {
            return $name
        }
    }
    Write-Host ""
    Write-Host "Found serial port(s), but none responded to the device protocol: $($candidates -join ', ')" -ForegroundColor Red
    Write-Host "Is this an ADS-B Display board, and is it fully booted (not mid-flash)?" -ForegroundColor Red
    exit 1
}

function Read-PlainText {
    param([string]$Prompt)
    $secure = Read-Host $Prompt -AsSecureString
    $bstr = [System.Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try {
        return [System.Runtime.InteropServices.Marshal]::PtrToStringAuto($bstr)
    } finally {
        [System.Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
    }
}

# Sends REBOOT, closes the current connection, then reconnects on the same
# port name -- so the menu loop has a live port object again instead of one
# talking to a device that's mid-boot or briefly gone. Retries for up to
# ~15s rather than assuming a single fixed delay is always enough. Writes
# to $script:port so the caller's variable is updated in place. Reconnecting
# over USB only confirms the *device* came back, not that WiFi did -- both
# current callers only ever reboot to apply new WiFi credentials, so this
# also polls WIFI_STATUS afterward and reports plainly whether they
# actually connected (a typo'd SSID/password otherwise looks identical to
# success here -- USB comes back fine either way).
function Invoke-RebootAndReconnect {
    param([string]$PortName)
    Write-Host "Rebooting device to apply changes..."
    Show-Response $script:port "REBOOT"
    $script:port.Close()

    $tries = 0
    while ($tries -lt 15) {
        Start-Sleep -Seconds 1
        $tries++
        if (Test-DevicePort $PortName) {
            $newPort = New-DevicePort $PortName $CmdTimeoutMs
            $newPort.Open()
            $script:port = $newPort
            Write-Host "Reconnected." -ForegroundColor Green
            Test-WifiConnected | Out-Null
            return
        }
    }

    Write-Host "Could not reconnect to $PortName after reboot -- if it re-enumerated under a different name, re-run this script." -ForegroundColor Red
    exit 1
}

# Polls WIFI_STATUS for up to ~10s -- association after a fresh boot isn't
# instant, so a single immediate check can catch it still connecting and
# wrongly report failure. Prints a clear result either way and returns
# whether it connected, for callers (e.g. Set-DeviceToken's live
# verification) that need to decide whether it's worth attempting something
# network-dependent right now.
function Test-WifiConnected {
    $resp = ""
    $tries = 0
    while ($tries -lt 10) {
        $resp = Send-AndRead $script:port "WIFI_STATUS"
        if ($resp -eq "OK connected") { break }
        Start-Sleep -Seconds 1
        $tries++
    }
    if ($resp -eq "OK connected") {
        Write-Host "WiFi connected." -ForegroundColor Green
        return $true
    } else {
        Write-Host "WiFi did not connect -- double-check the SSID/password (menu option 2, or re-run first-time setup)." -ForegroundColor Red
        return $false
    }
}

# Requests a live token check against airportdb.io and polls for the result
# -- a real HTTPS round trip (locations_verify_token_poll(), locations.cpp),
# so this can take several seconds, same as Update-DeviceFirmware's check.
# Only call this once WiFi is confirmed connected -- with no network it'll
# just report "no response" indefinitely, which reads as a token problem
# when it isn't one.
function Test-DeviceToken {
    $resp = Send-AndRead $script:port "TOKEN_VERIFY"
    if ([string]::IsNullOrEmpty($resp)) {
        Write-Host "(no response from device -- check it's still connected)" -ForegroundColor Red
        return
    }
    $tries = 0
    while ($tries -lt 15) {
        Start-Sleep -Seconds 1
        $resp = Send-AndRead $script:port "TOKEN_VERIFY_STATUS"
        $tries++
        if ($resp -ne "OK pending") { break }
    }
    if ($resp -eq "OK valid") {
        Write-Host "Token verified -- it works." -ForegroundColor Green
    } elseif ($resp -like "OK invalid*") {
        Write-Host "Token did NOT verify: $($resp.Substring(11))" -ForegroundColor Red
        Write-Host "Double-check what you pasted (menu option 1) -- airportdb.io rejected it." -ForegroundColor Yellow
    } else {
        Write-Host "Could not verify right now -- it'll be checked again the next time it's used." -ForegroundColor Yellow
    }
}

# Queries device configuration state (added to serial_config.cpp alongside
# this script revision). Returns a hashtable @{Wifi=0/1; Token=0/1;
# Locations=N} on success, or $null if the device didn't answer (offline)
# or doesn't understand STATUS yet (older firmware, predating this command)
# -- either way, callers treat that as "can't tell, don't guess" and fall
# back to showing the normal menu rather than assuming unconfigured.
function Get-DeviceStatus {
    $resp = Send-AndRead $script:port "STATUS"
    if ($resp -notlike "OK wifi=*") {
        return $null
    }
    $result = @{ Wifi = 0; Token = 0; Locations = 0 }
    foreach ($pair in $resp.Substring(3).Split(" ")) {
        $kv = $pair.Split("=")
        if ($kv.Length -ne 2) { continue }
        switch ($kv[0]) {
            "wifi"      { $result.Wifi = [int]$kv[1] }
            "token"     { $result.Token = [int]$kv[1] }
            "locations" { $result.Locations = [int]$kv[1] }
        }
    }
    return $result
}

# ---- setup steps, shared by the standalone menu items and the wizard ----

# Empty input skips rather than sending TOKEN= with an empty value -- makes
# this safe to back out of both standalone (menu option 1: pressing Enter
# won't accidentally clear an already-set token) and inside the wizard
# below (where it's explicitly optional -- "offer", not require). Live-
# verifies afterward when possible -- a plain "OK Saved" here previously
# looked identical whether the token was right or a typo, and the actual
# rejection only ever surfaced later, on-device, while trying to add a real
# airport (reported: an invalid token still read as "Configured" everywhere
# until that point).
function Set-DeviceToken {
    $token = Read-PlainText "Paste your airportdb.io token"
    if ([string]::IsNullOrEmpty($token)) {
        Write-Host "Skipped -- you can set this anytime from the menu." -ForegroundColor DarkGray
        return
    }
    Show-Response $script:port "TOKEN=$token"
    Write-Host "Used automatically the next time you add an airport by ICAO on the device."

    # Quick, no-retry check -- unlike Invoke-RebootAndReconnect's use of
    # Test-WifiConnected, there's no reason to expect a connection to still
    # be settling right here, so a single read is enough to decide whether
    # it's worth attempting live verification at all.
    $wifiResp = Send-AndRead $script:port "WIFI_STATUS"
    if ($wifiResp -eq "OK connected") {
        Write-Host "Verifying against airportdb.io..."
        Test-DeviceToken
    } else {
        Write-Host "Not connected to WiFi right now -- skipping live verification. It'll be caught the first time you actually add an airport if it's wrong." -ForegroundColor DarkGray
    }
}

function Set-DeviceWifi {
    $ssid = Read-Host "WiFi SSID"
    Show-Response $script:port "WIFI_SSID=$ssid"
    $pass = Read-PlainText "WiFi password"
    Show-Response $script:port "WIFI_PASS=$pass"
    Write-Host "The device only reads WiFi credentials at boot, so a restart is needed"
    Write-Host "before it connects with these -- this script will do that for you."
}

# Distinct from Show-Response -- needs the actual response text (not just
# printed) to decide whether to show the follow-up explanation, since that
# only makes sense if the add actually succeeded (an ERR here is usually
# "location list full" or "name already used", not a connectivity problem).
function Add-DeviceLocation {
    $locName = Read-Host "Location name (short, no '|')"
    $locLat = Read-Host "Latitude"
    $locLon = Read-Host "Longitude"
    $locElev = Read-Host "Elevation (ft)"
    $resp = Send-AndRead $script:port "ADD_WAYPOINT=$locName|$locLat|$locLon|$locElev"
    if ([string]::IsNullOrEmpty($resp)) {
        Write-Host "(no response from device -- check it's still connected)" -ForegroundColor Red
        return
    }
    Write-DeviceResponse $resp
    if ($resp -like "OK*") {
        Write-Host "Saved to the device's location list. This does NOT change what's"
        Write-Host "currently on screen and the new location is not auto-selected --"
        Write-Host "on the device, tap the location button (top of the screen) and"
        Write-Host "choose `"$locName`" from the list to actually view it. No reboot"
        Write-Host "needed for this to take effect."
    }
}

# Application-firmware update via GitHub Releases (data/ota.cpp) -- does NOT
# touch the ESP32-C6 co-processor's own firmware, see that file's comment
# for why. Checks, shows the result, and only downloads/installs with
# explicit confirmation once an update is confirmed available.
function Update-DeviceFirmware {
    $resp = Send-AndRead $script:port "OTA_CHECK"
    if ([string]::IsNullOrEmpty($resp)) {
        Write-Host "(no response from device -- check it's still connected)" -ForegroundColor Red
        return
    }
    Write-DeviceResponse $resp

    $tries = 0
    while ($tries -lt 10) {
        Start-Sleep -Seconds 1
        $resp = Send-AndRead $script:port "OTA_STATUS"
        $tries++
        if ($resp -notlike "*checking*") { break }
    }

    if ([string]::IsNullOrEmpty($resp)) {
        Write-Host "(no response checking status -- try again)" -ForegroundColor Red
        return
    }
    Write-DeviceResponse $resp

    if ($resp -notlike "*update available*") {
        return
    }

    $confirm = Read-Host "Download and install this update now? [y/N]"
    if ($confirm -ne "y" -and $confirm -ne "Y") {
        Write-Host "Cancelled."
        return
    }

    $resp = Send-AndRead $script:port "OTA_UPDATE"
    if ([string]::IsNullOrEmpty($resp)) {
        Write-Host "(no response from device -- check it's still connected)" -ForegroundColor Red
        return
    }
    Write-DeviceResponse $resp

    Write-Host "Downloading -- this can take a minute or two. The device reboots itself automatically once done."
    $tries = 0
    while ($tries -lt 90) {
        Start-Sleep -Seconds 2
        $resp = Send-AndRead $script:port "OTA_STATUS"
        $tries++
        if ($resp -like "*downloading*") {
            Write-Host $resp -ForegroundColor DarkGray
            continue
        }
        break
    }

    if ($resp -like "*error*") {
        Write-Host "Update failed -- device should still be running its previous firmware." -ForegroundColor Red
    } elseif ([string]::IsNullOrEmpty($resp)) {
        Write-Host "Device stopped responding -- if the download had finished, this is expected (it reboots itself into the new firmware). Re-run this script to confirm it came back up. If it doesn't, reflash over USB." -ForegroundColor Yellow
    }
}

# Guided first-time setup -- runs automatically on an unconfigured device
# (see the Get-DeviceStatus check in the main script body below) or on
# demand from the menu ("re-run the initial setup"). WiFi first, then an
# immediate reboot to actually apply it and confirm it connected --
# deliberately *before* the token step, not after (like the original
# version of this wizard had it), so airportdb.io token entry can
# live-verify against a real connection instead of just trusting whatever
# was typed. Location is offered/optional after that; the update check
# comes last since it also needs real connectivity, which by then it
# already has.
function Invoke-FirstTimeSetup {
    Write-Heading "First-time setup"
    Write-Host "Walks through WiFi, your airportdb.io token, and one saved location,"
    Write-Host "then checks for a firmware update."

    Write-Heading "WiFi"
    Set-DeviceWifi
    Invoke-RebootAndReconnect $portName

    Write-Heading "airportdb.io token"
    Write-Host "This is a free token that lets the device fetch runway geometry when"
    Write-Host "you save an airport by ICAO code (not needed for Home or plain"
    Write-Host "lat/lon waypoints). Get one at:"
    Write-Host ""
    Write-Host "  https://airportdb.io" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "Sign up, then paste the token below -- or just press Enter to skip"
    Write-Host "this for now and set it later from the menu."
    Set-DeviceToken

    Write-Heading "First saved location"
    $addedLocation = $false
    $locConfirm = Read-Host "Save a location now? [Y/n]"
    if ([string]::IsNullOrEmpty($locConfirm) -or $locConfirm -eq "y" -or $locConfirm -eq "Y") {
        Add-DeviceLocation
        $addedLocation = $true
    } else {
        Write-Host "Skipped -- you can add locations anytime from the menu, or on the device itself." -ForegroundColor DarkGray
    }

    Write-Heading "Checking for updates"
    Update-DeviceFirmware

    Write-Host ""
    Write-Host "ok, good to go!" -ForegroundColor Green
    if ($addedLocation) {
        Write-Host "One more step:" -ForegroundColor Yellow -NoNewline
        Write-Host " on the device, tap the location button"
        Write-Host "(top of the screen) and select the location you just added -- it"
        Write-Host "won't show any traffic until you do, since nothing is selected yet."
    }
}

# ---- entry point ------------------------------------------------------------

Write-Host "ADS-B Display -- device configuration"
Write-Host "Looking for the device..."
$portName = Find-DevicePort
Write-Host "Found device on $portName"

$port = New-DevicePort $portName $CmdTimeoutMs
$port.Open()

try {
    $status = Get-DeviceStatus
    if ($status -ne $null -and $status.Wifi -eq 0) {
        Invoke-FirstTimeSetup
    } else {
        # $status.Wifi -eq 1 (already configured) or $status -eq $null
        # (offline reply / older firmware predating STATUS -- can't tell
        # either way, fall through to the same menu rather than guessing).
        :menu while ($true) {
            Write-Host ""
            Write-Host "1) Set airportdb.io API token" -ForegroundColor Cyan
            Write-Host "2) Set WiFi credentials" -ForegroundColor Cyan
            Write-Host "3) Add a saved location (name/lat/lon/elevation)" -ForegroundColor Cyan
            Write-Host "4) Factory reset (erase all settings and saved locations)" -ForegroundColor Cyan
            Write-Host "5) Check for / install a firmware update" -ForegroundColor Cyan
            Write-Host "6) Re-run first-time setup (WiFi + token + one location)" -ForegroundColor Cyan
            Write-Host "0) Exit" -ForegroundColor Cyan
            $choice = Read-Host "Choose an option"

            switch ($choice) {
                "1" {
                    Set-DeviceToken
                }
                "2" {
                    Set-DeviceWifi
                    Invoke-RebootAndReconnect $portName
                }
                "3" {
                    Add-DeviceLocation
                }
                "4" {
                    $confirm = Read-Host "This will ERASE ALL settings and saved locations. Type YES to confirm"
                    if ($confirm -eq "YES") {
                        Show-Response $port "FACTORY_RESET=CONFIRM"
                        Write-Host "Device is rebooting. Re-run this script if you want to configure it again."
                        break menu
                    } else {
                        Write-Host "Cancelled."
                    }
                }
                "5" {
                    Update-DeviceFirmware
                }
                "6" {
                    Invoke-FirstTimeSetup
                }
                "0" {
                    break menu
                }
                default {
                    Write-Host "Not a valid option." -ForegroundColor Red
                }
            }
        }
    }
} finally {
    $port.Close()
}

Write-Host "Done."
