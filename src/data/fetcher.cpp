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

static void parse_aircraft_json(JsonDocument &doc) {
    if (!_aircraft_list->lock()) return;

    JsonArray ac = doc["ac"].as<JsonArray>();
    int new_count = 0;

    for (JsonObject obj : ac) {
        if (new_count >= MAX_AIRCRAFT) break;

        Aircraft &a = _aircraft_list->aircraft[new_count];

        // Check if this aircraft was already tracked (for trail continuity)
        const char *hex = obj["hex"] | "";
        int existing_idx = -1;
        for (int i = 0; i < _aircraft_list->count; i++) {
            if (strcmp(_aircraft_list->aircraft[i].icao_hex, hex) == 0) {
                existing_idx = i;
                break;
            }
        }

        if (existing_idx >= 0) {
            // Preserve trail from existing entry
            Aircraft &old = _aircraft_list->aircraft[existing_idx];
            memcpy(a.trail, old.trail, sizeof(a.trail));
            a.trail_count = old.trail_count;
        } else {
            a.trail_count = 0;
        }

        // Parse fields
        strlcpy(a.icao_hex, hex, sizeof(a.icao_hex));
        strlcpy(a.callsign, obj["flight"] | "", sizeof(a.callsign));
        // Trim trailing spaces from callsign
        for (int i = strlen(a.callsign) - 1; i >= 0 && a.callsign[i] == ' '; i--)
            a.callsign[i] = '\0';

        strlcpy(a.registration, obj["r"] | "", sizeof(a.registration));
        strlcpy(a.type_code, obj["t"] | "", sizeof(a.type_code));

        a.lat = obj["lat"] | 0.0f;
        a.lon = obj["lon"] | 0.0f;
        a.altitude = obj["alt_baro"].is<int>() ? obj["alt_baro"].as<int>() : 0;
        a.speed = obj["gs"] | 0;
        a.heading = obj["track"] | 0;
        a.vert_rate = obj["baro_rate"] | 0;
        a.squawk = strtoul(obj["squawk"] | "0", nullptr, 10);
        a.on_ground = obj["alt_baro"] == "ground";

        // Only include aircraft with valid position
        if (a.lat == 0.0f && a.lon == 0.0f) continue;

        // Flags
        a.is_military = check_military(a.icao_hex);
        a.is_emergency = check_emergency(a.squawk);
        a.is_watched = false;
        a.last_seen = millis();

        // Append to trail
        if (a.trail_count < TRAIL_LENGTH) {
            a.trail[a.trail_count] = {a.lat, a.lon, a.altitude, a.last_seen};
            a.trail_count++;
        } else {
            // Shift trail left, append new point
            memmove(&a.trail[0], &a.trail[1], (TRAIL_LENGTH - 1) * sizeof(TrailPoint));
            a.trail[TRAIL_LENGTH - 1] = {a.lat, a.lon, a.altitude, a.last_seen};
        }

        new_count++;
    }

    // Check for alerts on newly-parsed aircraft
    for (int i = 0; i < new_count; i++) {
        Aircraft &a = _aircraft_list->aircraft[i];
        if (a.is_emergency) {
            char msg[48];
            snprintf(msg, sizeof(msg), "Squawk %04d - %s", a.squawk,
                     a.squawk == 7500 ? "HIJACK" : a.squawk == 7600 ? "COMMS FAIL" : "EMERGENCY");
            alerts_queue(ALERT_EMERGENCY, a.callsign[0] ? a.callsign : a.icao_hex, msg);
        } else if (a.is_military && a.trail_count <= 1) {
            alerts_queue(ALERT_MILITARY, a.callsign[0] ? a.callsign : a.icao_hex, a.type_code);
        }
    }

    _aircraft_list->count = new_count;
    _aircraft_list->unlock();
    _last_update = millis();
}

static void fetch_task(void *param) {
    // Connect WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
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
    xTaskCreatePinnedToCore(fetch_task, "adsb_fetch", 16384, nullptr, 1, &_fetch_task_handle, 1);
}

bool fetcher_wifi_connected() {
    return WiFi.status() == WL_CONNECTED;
}

uint32_t fetcher_last_update() {
    return _last_update;
}
