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
# Menu option 5 checks for and installs application-firmware updates from
# GitHub Releases (does not touch the ESP32-C6 co-processor's own firmware,
# a deliberately separate problem -- see src/data/ota.cpp).

$ErrorActionPreference = "Stop"
$Baud = 115200
$PingTimeoutMs = 2000
$CmdTimeoutMs = 5000

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
        Write-Host "(no response from device -- check it's still connected)"
    } else {
        Write-Host $resp
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
# to $script:port so the caller's variable is updated in place.
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
            Write-Host "Reconnected."
            return
        }
    }

    Write-Host "Could not reconnect to $PortName after reboot -- if it re-enumerated under a different name, re-run this script." -ForegroundColor Red
    exit 1
}

# ---- setup steps, shared by the standalone menu items and the wizard ----

function Set-DeviceToken {
    $token = Read-PlainText "Paste your airportdb.io token"
    Show-Response $script:port "TOKEN=$token"
    Write-Host "Used automatically the next time you add an airport by ICAO on the device."
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
        Write-Host "(no response from device -- check it's still connected)"
        return
    }
    Write-Host $resp
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
        Write-Host "(no response from device -- check it's still connected)"
        return
    }
    Write-Host $resp

    $tries = 0
    while ($tries -lt 10) {
        Start-Sleep -Seconds 1
        $resp = Send-AndRead $script:port "OTA_STATUS"
        $tries++
        if ($resp -notlike "*checking*") { break }
    }

    if ([string]::IsNullOrEmpty($resp)) {
        Write-Host "(no response checking status -- try again)"
        return
    }
    Write-Host $resp

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
        Write-Host "(no response from device -- check it's still connected)"
        return
    }
    Write-Host $resp

    Write-Host "Downloading -- this can take a minute or two. The device reboots itself automatically once done."
    $tries = 0
    while ($tries -lt 90) {
        Start-Sleep -Seconds 2
        $resp = Send-AndRead $script:port "OTA_STATUS"
        $tries++
        if ($resp -like "*downloading*") {
            Write-Host $resp
            continue
        }
        break
    }

    if ($resp -like "*error*") {
        Write-Host "Update failed -- device should still be running its previous firmware."
    } elseif ([string]::IsNullOrEmpty($resp)) {
        Write-Host "Device stopped responding -- if the download had finished, this is expected (it reboots itself into the new firmware). Re-run this script to confirm it came back up. If it doesn't, reflash over USB."
    }
}

# ---- menu -----------------------------------------------------------------

Write-Host "ADS-B Display -- device configuration"
Write-Host "Looking for the device..."
$portName = Find-DevicePort
Write-Host "Found device on $portName"

$port = New-DevicePort $portName $CmdTimeoutMs
$port.Open()

try {
    :menu while ($true) {
        Write-Host ""
        Write-Host "1) Set airportdb.io API token"
        Write-Host "2) Set WiFi credentials"
        Write-Host "3) Add a saved location (name/lat/lon/elevation)"
        Write-Host "4) Factory reset (erase all settings and saved locations)"
        Write-Host "5) Check for / install a firmware update"
        Write-Host "6) First-time setup wizard (WiFi + token + one location)"
        Write-Host "0) Exit"
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
                Write-Host ""
                Write-Host "== First-time setup wizard =="
                Write-Host "Walks through WiFi, your airportdb.io token, and one saved"
                Write-Host "location, then reboots the device once at the end."
                Write-Host ""
                Write-Host "-- WiFi --"
                Set-DeviceWifi
                Write-Host ""
                Write-Host "-- airportdb.io token --"
                Set-DeviceToken
                Write-Host ""
                Write-Host "-- First saved location --"
                Add-DeviceLocation
                Write-Host ""
                Invoke-RebootAndReconnect $portName
                Write-Host "Setup complete."
            }
            "0" {
                break menu
            }
            default {
                Write-Host "Not a valid option."
            }
        }
    }
} finally {
    $port.Close()
}

Write-Host "Done."
