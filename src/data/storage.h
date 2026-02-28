#pragma once

struct UserConfig {
    char wifi_ssid[33];
    char wifi_pass[65];
    float home_lat;
    float home_lon;
    int radius_nm;
    bool use_metric;
    char watchlist[10][7]; // up to 10 ICAO hex codes
    int watchlist_count;
};

// Load config from SD card. Returns defaults if not found.
UserConfig storage_load_config();

// Save config to SD card
void storage_save_config(const UserConfig &cfg);
