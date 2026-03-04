#include "fetcher.h"
#include "http_mutex.h"
#include "../config.h"
#include "../ui/alerts.h"
#ifdef USE_ETHERNET
#include <ETH.h>
#else
#include <WiFi.h>
#endif
#include <HTTPClient.h>
#include <ArduinoJson.h>

static AircraftList *_aircraft_list = nullptr;
static uint32_t _last_update = 0;
static TaskHandle_t _fetch_task_handle = nullptr;
static TaskHandle_t _route_task_handle = nullptr;
static FetcherStats _fstats = {};

// Military alert dedup — circular buffer of already-alerted ICAO hexes
#define ALERTED_MAX 64
static char _alerted_hexes[ALERTED_MAX][7];
static int _alerted_count = 0;
static int _alerted_write = 0;

static bool already_alerted(const char *hex) {
    for (int i = 0; i < _alerted_count; i++) {
        if (strcmp(_alerted_hexes[i], hex) == 0) return true;
    }
    return false;
}

static void mark_alerted(const char *hex) {
    strlcpy(_alerted_hexes[_alerted_write], hex, 7);
    _alerted_write = (_alerted_write + 1) % ALERTED_MAX;
    if (_alerted_count < ALERTED_MAX) _alerted_count++;
}

// Check if ICAO hex is in known military ranges
static bool check_military(const char *hex) {
    uint32_t h = strtoul(hex, nullptr, 16);
    // US military (DoD, Army, Navy, USAF, Coast Guard)
    if (h >= 0xADF7C8 && h <= 0xAFFFFF) return true;
    // US Coast Guard
    if (h >= 0xA00001 && h <= 0xA00FFF) return true;
    // UK military (RAF, Royal Navy, Army Air Corps)
    if (h >= 0x43C000 && h <= 0x43CFFF) return true;
    // France military
    if (h >= 0x3B0000 && h <= 0x3BFFFF) return true;
    // Germany military
    if (h >= 0x3F4000 && h <= 0x3F7FFF) return true;
    // Canada military
    if (h >= 0xC0CDF9 && h <= 0xC0FFFF) return true;
    // Australia military
    if (h >= 0x7C8000 && h <= 0x7CBFFF) return true;
    // NATO/international
    if (h >= 0x0A4000 && h <= 0x0A4FFF) return true;
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

// Pre-parsed aircraft entry — extracted from JSON without holding the lock
struct ParsedEntry {
    char hex[7];
    char callsign[9];
    char registration[9];
    char type_code[5];
    char category[3];
    char desc[40];
    char owner_op[32];
    float lat, lon;
    int32_t altitude;
    int16_t speed, heading, vert_rate;
    uint16_t squawk;
    bool on_ground;
    float mach;
    int16_t ias, tas;
    int32_t nav_altitude;
    float roll;
    float nav_qnh;
};

// Apply a pre-parsed entry to an Aircraft in the main list
static void apply_parsed(Aircraft &a, const ParsedEntry &p, bool is_new) {
    strlcpy(a.icao_hex, p.hex, sizeof(a.icao_hex));
    strlcpy(a.callsign, p.callsign, sizeof(a.callsign));
    strlcpy(a.registration, p.registration, sizeof(a.registration));
    strlcpy(a.type_code, p.type_code, sizeof(a.type_code));
    strlcpy(a.category, p.category, sizeof(a.category));
    strlcpy(a.desc, p.desc, sizeof(a.desc));
    strlcpy(a.owner_op, p.owner_op, sizeof(a.owner_op));
    // Don't overwrite enriched route data (origin/dest set by route_enrich_task)
    a.lat = p.lat;
    a.lon = p.lon;
    a.altitude = p.altitude;
    a.speed = p.speed;
    a.heading = p.heading;
    a.vert_rate = p.vert_rate;
    a.squawk = p.squawk;
    a.on_ground = p.on_ground;
    a.mach = p.mach;
    a.ias = p.ias;
    a.tas = p.tas;
    a.nav_altitude = p.nav_altitude;
    a.roll = p.roll;
    a.nav_qnh = p.nav_qnh;
    a.is_military = check_military(a.icao_hex);
    a.is_emergency = check_emergency(a.squawk);
    a.is_watched = false;
    a.last_seen = millis();
    a.stale_since = 0;

    if (is_new) a.trail_count = 0;

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
    JsonArray ac = doc["ac"].as<JsonArray>();

    // Phase 1: Parse JSON into flat array — no lock needed
    static ParsedEntry parsed[MAX_AIRCRAFT];
    int parsed_count = 0;

    for (JsonObject obj : ac) {
        if (parsed_count >= MAX_AIRCRAFT) break;
        float lat = obj["lat"] | 0.0f;
        float lon = obj["lon"] | 0.0f;
        if (lat == 0.0f && lon == 0.0f) continue;

        ParsedEntry &p = parsed[parsed_count];
        strlcpy(p.hex, obj["hex"] | "", sizeof(p.hex));
        strlcpy(p.callsign, obj["flight"] | "", sizeof(p.callsign));
        for (int i = strlen(p.callsign) - 1; i >= 0 && p.callsign[i] == ' '; i--)
            p.callsign[i] = '\0';
        strlcpy(p.registration, obj["r"] | "", sizeof(p.registration));
        strlcpy(p.type_code, obj["t"] | "", sizeof(p.type_code));
        strlcpy(p.category, obj["category"] | "", sizeof(p.category));
        strlcpy(p.desc, obj["desc"] | "", sizeof(p.desc));
        strlcpy(p.owner_op, obj["ownOp"] | "", sizeof(p.owner_op));
        p.lat = lat;
        p.lon = lon;
        p.altitude = obj["alt_baro"].is<int>() ? obj["alt_baro"].as<int>() : 0;
        p.speed = (int16_t)(obj["gs"] | 0.0f);
        p.heading = (int16_t)(obj["track"] | 0.0f);
        p.vert_rate = (int16_t)(obj["baro_rate"] | 0.0f);
        p.squawk = strtoul(obj["squawk"] | "0", nullptr, 10);
        p.on_ground = obj["alt_baro"] == "ground";
        p.mach = obj["mach"] | 0.0f;
        p.ias = (int16_t)(obj["ias"] | 0.0f);
        p.tas = (int16_t)(obj["tas"] | 0.0f);
        p.nav_altitude = obj["nav_altitude_mcp"] | 0;
        p.roll = obj["roll"] | 0.0f;
        p.nav_qnh = obj["nav_qnh"] | 0.0f;
        parsed_count++;
    }

    // Phase 2: Brief lock to merge parsed data into aircraft list
    if (!_aircraft_list->lock()) return;

    uint32_t now = millis();
    bool seen[MAX_AIRCRAFT] = {};

    for (int p = 0; p < parsed_count; p++) {
        int idx = find_aircraft(parsed[p].hex);
        if (idx >= 0) {
            apply_parsed(_aircraft_list->aircraft[idx], parsed[p], false);
            seen[idx] = true;
        } else if (_aircraft_list->count < MAX_AIRCRAFT) {
            int new_idx = _aircraft_list->count;
            _aircraft_list->aircraft[new_idx].clear();
            apply_parsed(_aircraft_list->aircraft[new_idx], parsed[p], true);
            _aircraft_list->count++;
            seen[new_idx] = true;
        }
    }

    // Mark unseen aircraft as stale, remove expired ghosts
    int write = 0;
    for (int i = 0; i < _aircraft_list->count; i++) {
        Aircraft &a = _aircraft_list->aircraft[i];
        if (!seen[i]) {
            if (a.stale_since == 0) a.stale_since = now;
            if (now - a.stale_since > GHOST_TIMEOUT_MS) continue;
        }
        if (write != i) _aircraft_list->aircraft[write] = _aircraft_list->aircraft[i];
        write++;
    }
    _aircraft_list->count = write;

    // Check for alerts
    for (int i = 0; i < _aircraft_list->count; i++) {
        Aircraft &a = _aircraft_list->aircraft[i];
        if (a.stale_since != 0) continue;
        if (a.is_emergency) {
            // Emergency alerts always fire (no dedup)
            char msg[48];
            snprintf(msg, sizeof(msg), "Squawk %04d - %s", a.squawk,
                     a.squawk == 7500 ? "HIJACK" : a.squawk == 7600 ? "COMMS FAIL" : "EMERGENCY");
            alerts_queue(ALERT_EMERGENCY, a.callsign[0] ? a.callsign : a.icao_hex, msg, a.icao_hex);
        } else if (a.is_military && !already_alerted(a.icao_hex)) {
            mark_alerted(a.icao_hex);
            alerts_queue(ALERT_MILITARY, a.callsign[0] ? a.callsign : a.icao_hex, a.type_code, a.icao_hex);
        }
    }

    _aircraft_list->unlock();
    _last_update = millis();
}

static bool network_connected() {
#ifdef USE_ETHERNET
    return ETH.linkUp() && ETH.localIP() != IPAddress(0, 0, 0, 0);
#else
    return WiFi.status() == WL_CONNECTED;
#endif
}

static void fetch_task(void *param) {
    Serial.print("Fetcher: waiting for network");
    while (!network_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
    }
#ifdef USE_ETHERNET
    Serial.printf("\nEthernet connected, IP: %s\n", ETH.localIP().toString().c_str());
    strlcpy(_fstats.ip_addr, ETH.localIP().toString().c_str(), sizeof(_fstats.ip_addr));
#else
    Serial.printf("\nWiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
    strlcpy(_fstats.ip_addr, WiFi.localIP().toString().c_str(), sizeof(_fstats.ip_addr));
#endif

    // Build API URL
    char url[128];
    snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%d",
             HOME_LAT, HOME_LON, ADSB_RADIUS_NM);
    Serial.printf("ADS-B API URL: %s\n", url);

    // Main fetch loop
    while (true) {
        if (network_connected()) {
            if (http_mutex_acquire(pdMS_TO_TICKS(15000))) {
                HTTPClient http;
                http.begin(url);
                http.setTimeout(10000);
                uint32_t t0 = millis();
                int httpCode = http.GET();

                if (httpCode == HTTP_CODE_OK) {
                    String payload = http.getString();
                    _fstats.last_fetch_ms = millis() - t0;
                    _fstats.bytes_received += payload.length();
                    JsonDocument doc;
                    DeserializationError err = deserializeJson(doc, payload);
                    if (!err) {
                        parse_aircraft_json(doc);
                        _fstats.fetch_ok++;
                        Serial.printf("Fetched %d aircraft\n", _aircraft_list->count);
                    } else {
                        _fstats.fetch_fail++;
                        Serial.printf("JSON parse error: %s\n", err.c_str());
                    }
                } else {
                    _fstats.fetch_fail++;
                    Serial.printf("HTTP error: %d\n", httpCode);
                }
                http.end();
                http_mutex_release();
            }
        } else {
#ifdef USE_ETHERNET
            Serial.println("Ethernet link down, waiting...");
#else
            Serial.println("WiFi disconnected, reconnecting...");
            WiFi.reconnect();
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(ADSB_POLL_INTERVAL_MS));
    }
}

// Background task: fetch route (origin/dest) for aircraft with callsigns
static void route_enrich_task(void *param) {
    // Wait for network
    while (!network_connected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(5000)); // let main fetcher populate list first

    while (true) {
        // Find next aircraft that has a callsign but no route data
        char callsign[9] = {};
        char icao_hex[7] = {};
        bool found = false;

        if (_aircraft_list->lock(pdMS_TO_TICKS(100))) {
            for (int i = 0; i < _aircraft_list->count; i++) {
                Aircraft &a = _aircraft_list->aircraft[i];
                if (a.callsign[0] && !a.origin[0] && a.stale_since == 0) {
                    strlcpy(callsign, a.callsign, sizeof(callsign));
                    strlcpy(icao_hex, a.icao_hex, sizeof(icao_hex));
                    found = true;
                    break;
                }
            }
            _aircraft_list->unlock();
        }

        if (!found || !network_connected()) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        // Fetch route from adsbdb
        char origin[5] = {};
        char dest[5] = {};

        if (http_mutex_acquire(pdMS_TO_TICKS(12000))) {
            char url[128];
            snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", callsign);
            HTTPClient http;
            http.begin(url);
            http.setTimeout(8000);
            int code = http.GET();

            if (code == HTTP_CODE_OK) {
                JsonDocument doc;
                if (!deserializeJson(doc, http.getString())) {
                    JsonObject route = doc["response"]["flightroute"];
                    const char *orig_iata = route["origin"]["iata_code"] | "";
                    const char *dest_iata = route["destination"]["iata_code"] | "";
                    strlcpy(origin, orig_iata, sizeof(origin));
                    strlcpy(dest, dest_iata, sizeof(dest));
                    _fstats.enrich_ok++;
                }
            } else {
                _fstats.enrich_fail++;
            }
            http.end();
            http_mutex_release();
        }

        // Write results back (even empty — marks as "tried" so we don't re-fetch)
        if (_aircraft_list->lock(pdMS_TO_TICKS(100))) {
            int idx = find_aircraft(icao_hex);
            if (idx >= 0) {
                Aircraft &a = _aircraft_list->aircraft[idx];
                if (origin[0]) strlcpy(a.origin, origin, sizeof(a.origin));
                else strlcpy(a.origin, "-", sizeof(a.origin)); // mark as tried
                if (dest[0]) strlcpy(a.dest, dest, sizeof(a.dest));
                else strlcpy(a.dest, "-", sizeof(a.dest));
                Serial.printf("Route: %s %s->%s\n", callsign, a.origin, a.dest);
            }
            _aircraft_list->unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(1500)); // rate limit: ~1 req per 1.5s
    }
}

void fetcher_init(AircraftList *list) {
    _aircraft_list = list;
    http_mutex_init();

#ifdef USE_ETHERNET
    ETH.begin();
    Serial.println("Ethernet initialization started");
#else
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.println("WiFi initialization started");
#endif

    xTaskCreatePinnedToCore(fetch_task, "adsb_fetch", 32768, nullptr, 1, &_fetch_task_handle, 1);
    xTaskCreatePinnedToCore(route_enrich_task, "route_enrich", 16384, nullptr, 0, &_route_task_handle, 1);
}

bool fetcher_wifi_connected() {
    return network_connected();
}

uint32_t fetcher_last_update() {
    return _last_update;
}

const FetcherStats* fetcher_get_stats() {
    return &_fstats;
}
