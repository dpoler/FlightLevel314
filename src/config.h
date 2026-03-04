#pragma once

// Network: uncomment ONE of these
#define USE_ETHERNET      // Built-in 100Mbps Ethernet (IP101 PHY)
// #define USE_WIFI       // ESP32-C6 hosted WiFi

// WiFi credentials (only used if USE_WIFI is defined)
#define WIFI_SSID "ATTGpyJKmS"
#define WIFI_PASS "cw7nqpsr?sun"

// Home location (change to your coordinates)
#define HOME_LAT 30.6905
#define HOME_LON -88.1632

// ADS-B settings
#define ADSB_RADIUS_NM 100
#define ADSB_POLL_INTERVAL_MS 5000
#define MAX_AIRCRAFT 200
#define TRAIL_LENGTH 60
