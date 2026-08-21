// Fetches a remote ADS-B aggregator and merges results into an AircraftList.
// Provider is selected by UserConfig::traffic_provider:
//   0 — api.adsb.lol/v2/point/<lat>/<lon>/<radius>
//   1 — opendata.adsb.fi/api/v3/lat/<lat>/lon/<lon>/dist/<radius> (cap 250 nm)
// Both return an ADSBx-v2-shaped `ac` array. Deliberately a fresh
// implementation, not a shared extraction from src/data/fetcher.cpp's
// parse_aircraft_json() -- that file's hardware-recovery logic (WiFi/C6
// co-processor handling) is extensively hard-won (see
// project_p4_heap_constraints / project_platform_pin memories) and not worth
// risking a shared-code refactor on for this port. Known duplication of the
// JSON schema knowledge between this file and fetcher.cpp -- worth revisiting
// once the Pi side is hardware-validated, see project_pi_port memory.
//
// Alert-queueing (military/emergency toasts) below mirrors fetcher.cpp's
// do_alerts block, including its military-alert dedup ring buffer -- now
// that alerts.cpp is ported for real (task #10), nothing else calls
// alerts_queue() on Pi, so this is the one place it needs to happen.

#include "../../src/data/datasource.h"
#include "../../src/data/storage.h"
#include "../../src/data/locations.h"
#include "../../src/platform/platform.h"
#include "../../src/ui/alerts.h"
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

bool is_test_signal(const char *hex, const char *callsign) {
    if (strncmp(callsign, "QPK", 3) == 0) return true;
    if (strncmp(callsign, "TEST", 4) == 0) return true;
    if (strncmp(callsign, "TSTR", 4) == 0) return true;
    if (strcmp(hex, "000000") == 0) return true;
    return false;
}

bool check_military(const char *hex) {
    uint32_t h = strtoul(hex, nullptr, 16);
    if (h >= 0xADF7C8 && h <= 0xAFFFFF) return true; // US DoD/Army/Navy/USAF
    if (h >= 0x43C000 && h <= 0x43CFFF) return true; // UK RAF/RN/AAC
    if (h >= 0x3B0000 && h <= 0x3BFFFF) return true; // France
    if (h >= 0x3F4000 && h <= 0x3F7FFF) return true; // Germany
    if (h >= 0xC0CDF9 && h <= 0xC0FFFF) return true; // Canada
    if (h >= 0x7C8000 && h <= 0x7CBFFF) return true; // Australia
    if (h >= 0x0A4000 && h <= 0x0A4FFF) return true; // NATO/international
    return false;
}

bool check_emergency(uint16_t squawk) {
    return squawk == 7500 || squawk == 7600 || squawk == 7700;
}

int find_aircraft(AircraftList *list, const char *hex) {
    for (int i = 0; i < list->count; i++)
        if (strcmp(list->aircraft[i].icao_hex, hex) == 0) return i;
    return -1;
}

// Military alert dedup -- circular buffer of already-alerted ICAO hexes,
// same as fetcher.cpp's, so the same military aircraft sitting in view
// across multiple fetch cycles doesn't re-toast every ~20s. Emergency
// alerts deliberately have no dedup, matching fetcher.cpp.
#define ALERTED_MAX 64
char _alerted_hexes[ALERTED_MAX][7];
int _alerted_count = 0;
int _alerted_write = 0;

bool already_alerted(const char *hex) {
    for (int i = 0; i < _alerted_count; i++)
        if (strcmp(_alerted_hexes[i], hex) == 0) return true;
    return false;
}

void mark_alerted(const char *hex) {
    strlcpy(_alerted_hexes[_alerted_write], hex, 7);
    _alerted_write = (_alerted_write + 1) % ALERTED_MAX;
    if (_alerted_count < ALERTED_MAX) _alerted_count++;
}

void apply_json_entry(Aircraft &a, JsonObject obj, bool is_new) {
    strlcpy(a.icao_hex, obj["hex"] | "", sizeof(a.icao_hex));
    strlcpy(a.callsign, obj["flight"] | "", sizeof(a.callsign));
    for (int i = (int)strlen(a.callsign) - 1; i >= 0 && a.callsign[i] == ' '; i--)
        a.callsign[i] = '\0';
    strlcpy(a.registration, obj["r"] | "", sizeof(a.registration));
    strlcpy(a.type_code, obj["t"] | "", sizeof(a.type_code));
    strlcpy(a.category, obj["category"] | "", sizeof(a.category));
    strlcpy(a.desc, obj["desc"] | "", sizeof(a.desc));
    strlcpy(a.owner_op, obj["ownOp"] | "", sizeof(a.owner_op));
    a.lat = obj["lat"] | 0.0f;
    a.lon = obj["lon"] | 0.0f;
    a.altitude = obj["alt_baro"].is<int>() ? obj["alt_baro"].as<int>() : 0;
    a.speed = (int16_t)(obj["gs"] | 0.0f);
    a.heading = (int16_t)(obj["track"] | 0.0f);
    a.vert_rate_valid = !obj["baro_rate"].isNull();
    a.vert_rate = (int16_t)(obj["baro_rate"] | 0.0f);
    a.squawk = (uint16_t)strtoul(obj["squawk"] | "0", nullptr, 10);
    a.on_ground = obj["alt_baro"] == "ground";
    a.mach = obj["mach"] | 0.0f;
    a.ias = (int16_t)(obj["ias"] | 0.0f);
    a.tas = (int16_t)(obj["tas"] | 0.0f);
    a.nav_altitude = obj["nav_altitude_mcp"] | 0;
    a.roll = obj["roll"] | 0.0f;
    a.nav_qnh = obj["nav_qnh"] | 0.0f;
    a.is_military = check_military(a.icao_hex);
    a.is_emergency = check_emergency(a.squawk);
    a.is_watched = false;
    a.last_seen = platform_millis();
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

} // namespace

const char *RemoteApiDataSource::name() const {
    return g_config.traffic_provider == 1 ? "adsb.fi" : "adsb.lol";
}

bool RemoteApiDataSource::fetch(AircraftList *list) {
    float lat, lon;
    if (!locations_get_active_coords(&lat, &lon, nullptr)) return false;

    int radius = g_config.radius_nm > 0 ? g_config.radius_nm : 50;
    char url[192];
    // adsb.fi public opendata: v3 lat/lon/dist returns the same `ac` envelope
    // as adsb.lol's /v2/point. Dist is capped at 250 nm by their API.
    if (g_config.traffic_provider == 1) {
        if (radius > 250) radius = 250;
        snprintf(url, sizeof(url),
                 "https://opendata.adsb.fi/api/v3/lat/%.4f/lon/%.4f/dist/%d",
                 lat, lon, radius);
    } else {
        snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%d",
                 lat, lon, radius);
    }

    static std::vector<char> buf(1024 * 1024); // aggregator responses can run large at dense/wide radii -- Pi has RAM to spare
    size_t len = 0;
    if (!platform_http_get(url, buf.data(), buf.size(), &len)) return false;

    JsonDocument doc;
    if (deserializeJson(doc, buf.data(), len) != DeserializationError::Ok) {
        platform_log_warn("RemoteApiDataSource: JSON parse failed (%zuB)\n", len);
        return false;
    }

    JsonArray ac = doc["ac"].as<JsonArray>();
    if (!list->lock(1000)) return false;

    uint32_t now = platform_millis();
    std::vector<bool> seen(MAX_AIRCRAFT, false);

    for (JsonObject obj : ac) {
        float olat = obj["lat"] | 0.0f;
        float olon = obj["lon"] | 0.0f;
        if (olat == 0.0f && olon == 0.0f) continue;
        char hex[7];
        strlcpy(hex, obj["hex"] | "", sizeof(hex));
        char callsign[9];
        strlcpy(callsign, obj["flight"] | "", sizeof(callsign));
        if (is_test_signal(hex, callsign)) continue;

        int idx = find_aircraft(list, hex);
        bool is_new = (idx < 0);
        if (idx < 0) {
            if (list->count >= MAX_AIRCRAFT) continue;
            idx = list->count++;
            list->aircraft[idx].clear();
        }
        apply_json_entry(list->aircraft[idx], obj, is_new);
        seen[idx] = true;
    }

    int write = 0;
    for (int i = 0; i < list->count; i++) {
        Aircraft &a = list->aircraft[i];
        if (!seen[i]) {
            if (a.stale_since == 0) a.stale_since = now;
            if (now - a.stale_since > GHOST_TIMEOUT_MS) continue;
        }
        if (write != i) list->aircraft[write] = list->aircraft[i];
        write++;
    }
    list->count = write;

    for (int i = 0; i < list->count; i++) {
        Aircraft &a = list->aircraft[i];
        if (a.stale_since != 0) continue;
        if (a.is_emergency && g_config.alert_emergency) {
            char msg[48];
            snprintf(msg, sizeof(msg), "Squawk %04d - %s", a.squawk,
                     a.squawk == 7500 ? "HIJACK" : a.squawk == 7600 ? "COMMS FAIL" : "EMERGENCY");
            alerts_queue(ALERT_EMERGENCY, a.callsign[0] ? a.callsign : a.icao_hex, msg, a.icao_hex);
        } else if (a.is_military && g_config.alert_military && !already_alerted(a.icao_hex)) {
            mark_alerted(a.icao_hex);
            alerts_queue(ALERT_MILITARY, a.callsign[0] ? a.callsign : a.icao_hex, a.type_code, a.icao_hex);
        }
    }

    list->unlock();
    return true;
}
