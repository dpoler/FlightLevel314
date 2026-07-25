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
# Menu options 3 and 5 are intentionally stubbed -- the device firmware
# doesn't support them yet (see the project backlog: the location system is
# still being redesigned, and OTA firmware updates aren't built). The menu
# structure and serial plumbing here are already in place for when they are.

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

function Send-AndRead {
    param($Port, [string]$Line)
    $Port.WriteLine($Line)
    try {
        return $Port.ReadLine().Trim()
    } catch [System.TimeoutException] {
        return ""
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
        Write-Host "3) Set Home location (lat/lon/elevation)  [not yet supported by this firmware]"
        Write-Host "4) Factory reset (erase all settings and saved locations)"
        Write-Host "5) Update firmware  [not yet supported by this firmware]"
        Write-Host "0) Exit"
        $choice = Read-Host "Choose an option"

        switch ($choice) {
            "1" {
                $token = Read-PlainText "Paste your airportdb.io token"
                Show-Response $port "TOKEN=$token"
            }
            "2" {
                $ssid = Read-Host "WiFi SSID"
                Show-Response $port "WIFI_SSID=$ssid"
                $pass = Read-PlainText "WiFi password"
                Show-Response $port "WIFI_PASS=$pass"
                Write-Host "Reboot the device (power cycle) to apply."
            }
            "3" {
                Write-Host "Not yet supported -- the location system is still being redesigned."
                Write-Host 'See the project backlog ("Generalize Home into the saved-locations system").'
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
                Write-Host "Not yet supported -- see the OTA-updates item in the project backlog."
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
