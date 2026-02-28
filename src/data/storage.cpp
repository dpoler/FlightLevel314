#include "storage.h"
#include "../config.h"
#include <SD_MMC.h>
#include <ArduinoJson.h>

#define CONFIG_PATH "/config.json"

UserConfig storage_load_config() {
    UserConfig cfg;
    strlcpy(cfg.wifi_ssid, WIFI_SSID, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass, WIFI_PASS, sizeof(cfg.wifi_pass));
    cfg.home_lat = HOME_LAT;
    cfg.home_lon = HOME_LON;
    cfg.radius_nm = ADSB_RADIUS_NM;
    cfg.use_metric = false;
    cfg.watchlist_count = 0;

    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("SD card mount failed, using defaults");
        return cfg;
    }

    File f = SD_MMC.open(CONFIG_PATH, FILE_READ);
    if (!f) return cfg;

    JsonDocument doc;
    if (deserializeJson(doc, f)) {
        f.close();
        return cfg;
    }
    f.close();

    strlcpy(cfg.wifi_ssid, doc["wifi_ssid"] | WIFI_SSID, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass, doc["wifi_pass"] | WIFI_PASS, sizeof(cfg.wifi_pass));
    cfg.home_lat = doc["home_lat"] | HOME_LAT;
    cfg.home_lon = doc["home_lon"] | HOME_LON;
    cfg.radius_nm = doc["radius_nm"] | ADSB_RADIUS_NM;
    cfg.use_metric = doc["use_metric"] | false;

    JsonArray wl = doc["watchlist"].as<JsonArray>();
    cfg.watchlist_count = 0;
    for (JsonVariant v : wl) {
        if (cfg.watchlist_count >= 10) break;
        strlcpy(cfg.watchlist[cfg.watchlist_count++], v.as<const char *>(), 7);
    }

    return cfg;
}

void storage_save_config(const UserConfig &cfg) {
    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("SD card mount failed, cannot save config");
        return;
    }

    File f = SD_MMC.open(CONFIG_PATH, FILE_WRITE);
    if (!f) return;

    JsonDocument doc;
    doc["wifi_ssid"] = cfg.wifi_ssid;
    doc["wifi_pass"] = cfg.wifi_pass;
    doc["home_lat"] = cfg.home_lat;
    doc["home_lon"] = cfg.home_lon;
    doc["radius_nm"] = cfg.radius_nm;
    doc["use_metric"] = cfg.use_metric;

    JsonArray wl = doc["watchlist"].to<JsonArray>();
    for (int i = 0; i < cfg.watchlist_count; i++) {
        wl.add(cfg.watchlist[i]);
    }

    serializeJsonPretty(doc, f);
    f.close();
}
