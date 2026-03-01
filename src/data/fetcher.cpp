#include "fetcher.h"
#include "../config.h"
#include "../ui/alerts.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static AircraftList *_aircraft_list = nullptr;
static uint32_t _last_update = 0;
static TaskHandle_t _fetch_task_handle = nullptr;

// Check if ICAO hex is in known military ranges
static bool check_military(const char *hex) {
    uint32_t h = strtoul(hex, nullptr, 16);
    // US military
    if (h >= 0xAE0000 && h <= 0xAFFFFF) return true;
    if (h >= 0xADF7C8 && h <= 0xAFFFFF) return true;
    // UK military
    if (h >= 0x43C000 && h <= 0x43CFFF) return true;
    return false;
}

static bool check_emergency(uint16_t squawk) {
    return squawk == 7500 || squawk == 7600 || squawk == 7700;
}

// Find existing aircraft by ICAO hex, returns index or -1
static int find_aircraft(const char *hex) {
    for (int i = 0; i < _aircraft_list->count; i++) {
        if (strcmp(_aircraft_list->aircraft[i].icao_hex, hex) == 0)
            return i;
    }
    return -1;
}

// Update an aircraft entry from JSON, preserving trail data
static void update_aircraft_from_json(Aircraft &a, JsonObject &obj, bool is_new) {
    const char *hex = obj["hex"] | "";
    strncpy(a.icao_hex, hex, sizeof(a.icao_hex) - 1);
    a.icao_hex[sizeof(a.icao_hex) - 1] = '\0';
    strncpy(a.callsign, obj["flight"] | "", sizeof(a.callsign) - 1);
    a.callsign[sizeof(a.callsign) - 1] = '\0';
    for (int i = strlen(a.callsign) - 1; i >= 0 && a.callsign[i] == ' '; i--)
        a.callsign[i] = '\0';

    strncpy(a.registration, obj["r"] | "", sizeof(a.registration) - 1);
    a.registration[sizeof(a.registration) - 1] = '\0';
    strncpy(a.type_code, obj["t"] | "", sizeof(a.type_code) - 1);
    a.type_code[sizeof(a.type_code) - 1] = '\0';

    a.lat = obj["lat"] | 0.0f;
    a.lon = obj["lon"] | 0.0f;
    a.altitude = obj["alt_baro"].is<int>() ? obj["alt_baro"].as<int>() : 0;
    a.speed = (int16_t)(obj["gs"] | 0.0f);
    a.heading = (int16_t)(obj["track"] | 0.0f);
    a.vert_rate = (int16_t)(obj["baro_rate"] | 0.0f);
    a.squawk = strtoul(obj["squawk"] | "0", nullptr, 10);
    a.on_ground = obj["alt_baro"] == "ground";

    a.is_military = check_military(a.icao_hex);
    a.is_emergency = check_emergency(a.squawk);
    a.is_watched = false;
    a.last_seen = millis();
    a.stale_since = 0; // fresh

    if (is_new) a.trail_count = 0;

    // Append to trail
    if (a.lat != 0.0f || a.lon != 0.0f) {
        if (a.trail_count < TRAIL_LENGTH) {
            a.trail[a.trail_count] = {a.lat, a.lon, a.altitude, a.last_seen};
            a.trail_count++;
        } else {
            memmove(&a.trail[0], &a.trail[1], (TRAIL_LENGTH - 1) * sizeof(TrailPoint));
            a.trail[TRAIL_LENGTH - 1] = {a.lat, a.lon, a.altitude, a.last_seen};
        }
    }
}

static void parse_aircraft_json(JsonDocument &doc) {
    if (!_aircraft_list->lock()) return;

    uint32_t now = millis();

    // Track which existing aircraft were seen this cycle
    bool seen[MAX_AIRCRAFT] = {};

    JsonArray ac = doc["ac"].as<JsonArray>();

    for (JsonObject obj : ac) {
        const char *hex = obj["hex"] | "";
        float lat = obj["lat"] | 0.0f;
        float lon = obj["lon"] | 0.0f;
        if (lat == 0.0f && lon == 0.0f) continue;

        int idx = find_aircraft(hex);
        if (idx >= 0) {
            // Update existing aircraft in-place
            update_aircraft_from_json(_aircraft_list->aircraft[idx], obj, false);
            seen[idx] = true;
        } else if (_aircraft_list->count < MAX_AIRCRAFT) {
            // Append new aircraft
            int new_idx = _aircraft_list->count;
            _aircraft_list->aircraft[new_idx].clear();
            update_aircraft_from_json(_aircraft_list->aircraft[new_idx], obj, true);
            _aircraft_list->count++;
            seen[new_idx] = true;
        }
    }

    // Mark unseen aircraft as stale, remove expired ghosts
    int write = 0;
    for (int i = 0; i < _aircraft_list->count; i++) {
        Aircraft &a = _aircraft_list->aircraft[i];
        if (!seen[i]) {
            // Not in this cycle's response
            if (a.stale_since == 0) a.stale_since = now;
            if (now - a.stale_since > GHOST_TIMEOUT_MS) continue; // expired, skip
        }
        if (write != i) _aircraft_list->aircraft[write] = _aircraft_list->aircraft[i];
        write++;
    }
    _aircraft_list->count = write;

    // Check for alerts
    for (int i = 0; i < _aircraft_list->count; i++) {
        Aircraft &a = _aircraft_list->aircraft[i];
        if (a.stale_since != 0) continue; // don't alert on stale aircraft
        if (a.is_emergency) {
            char msg[48];
            snprintf(msg, sizeof(msg), "Squawk %04d - %s", a.squawk,
                     a.squawk == 7500 ? "HIJACK" : a.squawk == 7600 ? "COMMS FAIL" : "EMERGENCY");
            alerts_queue(ALERT_EMERGENCY, a.callsign[0] ? a.callsign : a.icao_hex, msg);
        } else if (a.is_military && a.trail_count <= 1) {
            alerts_queue(ALERT_MILITARY, a.callsign[0] ? a.callsign : a.icao_hex, a.type_code);
        }
    }

    _aircraft_list->unlock();
    _last_update = millis();
}

static void fetch_task(void *param) {
    Serial.print("Fetcher: waiting for WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
    }
    Serial.printf("\nWiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());

    // Build API URL
    char url[128];
    snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%d",
             HOME_LAT, HOME_LON, ADSB_RADIUS_NM);
    Serial.printf("ADS-B API URL: %s\n", url);

    // Main fetch loop
    while (true) {
        if (WiFi.status() == WL_CONNECTED) {
            HTTPClient http;
            http.begin(url);
            http.setTimeout(10000);
            int httpCode = http.GET();

            if (httpCode == HTTP_CODE_OK) {
                String payload = http.getString();
                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, payload);
                if (!err) {
                    parse_aircraft_json(doc);
                    Serial.printf("Fetched %d aircraft\n", _aircraft_list->count);
                } else {
                    Serial.printf("JSON parse error: %s\n", err.c_str());
                }
            } else {
                Serial.printf("HTTP error: %d\n", httpCode);
            }
            http.end();
        } else {
            Serial.println("WiFi disconnected, reconnecting...");
            WiFi.reconnect();
        }
        vTaskDelay(pdMS_TO_TICKS(ADSB_POLL_INTERVAL_MS));
    }
}

void fetcher_init(AircraftList *list) {
    _aircraft_list = list;

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.println("WiFi initialization started");

    xTaskCreatePinnedToCore(fetch_task, "adsb_fetch", 32768, nullptr, 1, &_fetch_task_handle, 1);
}

bool fetcher_wifi_connected() {
    return WiFi.status() == WL_CONNECTED;
}

uint32_t fetcher_last_update() {
    return _last_update;
}
