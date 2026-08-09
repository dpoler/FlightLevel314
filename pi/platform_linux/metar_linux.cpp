// Linux METAR fetch — aviationweather.gov Data API. Same semantics as
// src/data/metar.cpp, with ICAO-first for saved airports and a 50nm
// nearest-station fallback (waypoints / non-reporting fields).

#include "../../src/data/metar.h"
#include "../../src/data/locations.h"
#include "../../src/platform/platform.h"
#include "../../src/ui/geo.h"

#include <ArduinoJson.h>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// Routine METARs are roughly hourly (US ASOS often ~:51–:58). Poll at ~4×
// that rate so SPECI updates show up without hammering the API.
#define METAR_REFRESH_MS (15UL * 60UL * 1000UL)

volatile MetarStatus metar_status = METAR_IDLE;
char metar_raw[METAR_RAW_LEN] = "";
char metar_station[8] = "";

namespace {

std::mutex _fetch_mutex;
bool _busy = false;

bool pick_best_from_array(JsonDocument &doc, float lat, float lon,
                          const char **best_raw, const char **best_id, float *best_dist) {
    *best_raw = nullptr;
    *best_id = nullptr;
    *best_dist = 1e9f;
    if (!doc.is<JsonArray>()) return false;
    for (JsonObject station : doc.as<JsonArray>()) {
        const char *raw = station["rawOb"] | "";
        if (!raw[0]) continue;
        float slat = station["lat"] | 0.0f;
        float slon = station["lon"] | 0.0f;
        float d = MapProjection::distance_nm(lat, lon, slat, slon);
        if (d <= METAR_RANGE_NM && d < *best_dist) {
            *best_dist = d;
            *best_raw = raw;
            *best_id = station["icaoId"] | "";
        }
    }
    return *best_raw != nullptr;
}

bool fetch_ids(const char *icao, float lat, float lon) {
    char url[160];
    snprintf(url, sizeof(url),
             "https://aviationweather.gov/api/data/metar?ids=%s&format=json", icao);

    std::vector<char> buf(64 * 1024);
    size_t len = 0;
    if (!platform_http_get(url, buf.data(), buf.size(), &len)) return false;

    JsonDocument doc;
    if (deserializeJson(doc, buf.data(), len) != DeserializationError::Ok) return false;

    const char *best_raw = nullptr;
    const char *best_id = nullptr;
    float best_dist = 1e9f;
    if (!pick_best_from_array(doc, lat, lon, &best_raw, &best_id, &best_dist)) {
        // Exact ICAO hit may omit lat/lon — still accept a single rawOb.
        if (doc.is<JsonArray>() && doc.as<JsonArray>().size() > 0) {
            JsonObject s = doc.as<JsonArray>()[0];
            best_raw = s["rawOb"] | "";
            best_id = s["icaoId"] | icao;
            if (!best_raw[0]) return false;
        } else {
            return false;
        }
    }

    strlcpy(metar_raw, best_raw, sizeof(metar_raw));
    strlcpy(metar_station, best_id && best_id[0] ? best_id : icao, sizeof(metar_station));
    metar_status = METAR_OK;
    platform_log("METAR: %s (ids)\n", metar_station);
    return true;
}

bool fetch_bbox(float lat, float lon) {
    float dlat = METAR_RANGE_NM / 60.0f;
    float dlon = METAR_RANGE_NM / (60.0f * cosf(lat * (float)M_PI / 180.0f));
    if (dlon < 0.01f) dlon = dlat;

    char url[192];
    snprintf(url, sizeof(url),
             "https://aviationweather.gov/api/data/metar?bbox=%.4f,%.4f,%.4f,%.4f&format=json",
             lat - dlat, lon - dlon, lat + dlat, lon + dlon);

    std::vector<char> buf(256 * 1024);
    size_t len = 0;
    if (!platform_http_get(url, buf.data(), buf.size(), &len)) {
        metar_status = METAR_ERROR;
        platform_log("METAR: network failed\n");
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, buf.data(), len) != DeserializationError::Ok) {
        metar_status = METAR_ERROR;
        platform_log("METAR: JSON parse error\n");
        return false;
    }

    const char *best_raw = nullptr;
    const char *best_id = nullptr;
    float best_dist = 1e9f;
    if (pick_best_from_array(doc, lat, lon, &best_raw, &best_id, &best_dist)) {
        strlcpy(metar_raw, best_raw, sizeof(metar_raw));
        strlcpy(metar_station, best_id, sizeof(metar_station));
        metar_status = METAR_OK;
        platform_log("METAR: %s (%.1fnm)\n", metar_station, (double)best_dist);
        return true;
    }

    metar_raw[0] = '\0';
    metar_station[0] = '\0';
    metar_status = METAR_NO_STATION;
    platform_log("METAR: no station within %.0fnm\n", (double)METAR_RANGE_NM);
    return false;
}

void do_fetch(float lat, float lon, const char *icao) {
    metar_status = METAR_FETCHING;
    if (icao && icao[0]) {
        if (fetch_ids(icao, lat, lon)) return;
        // Airport ICAO with no ASOS — fall through to nearest in range.
    }
    fetch_bbox(lat, lon);
}

void run_fetch(float lat, float lon, std::string icao, int loc_idx) {
    do_fetch(lat, lon, icao.c_str());
    std::lock_guard<std::mutex> lock(_fetch_mutex);
    _busy = false;
    (void)loc_idx;
}

} // namespace

void metar_poll() {
    static uint32_t last_fetch_ms = 0;
    static int last_loc_idx = -2;

    int idx = locations_active_index();
    if (idx == -1) {
        if (last_loc_idx != -1) {
            metar_status = METAR_IDLE;
            metar_raw[0] = '\0';
            metar_station[0] = '\0';
            last_loc_idx = -1;
        }
        return;
    }

    uint32_t now = platform_millis();
    bool loc_changed = (idx != last_loc_idx);
    bool due = (now - last_fetch_ms >= METAR_REFRESH_MS);
    if (!loc_changed && !due) return;

    float lat, lon;
    if (!locations_get_active_coords(&lat, &lon, nullptr)) return;

    const Location *loc = locations_get(idx);
    std::string icao = (loc && loc->icao[0]) ? loc->icao : "";

    {
        std::lock_guard<std::mutex> lock(_fetch_mutex);
        if (_busy) return;
        _busy = true;
    }

    if (loc_changed) {
        metar_raw[0] = '\0';
        metar_station[0] = '\0';
    }
    last_loc_idx = idx;
    last_fetch_ms = now;

    std::thread([lat, lon, icao, idx]() { run_fetch(lat, lon, icao, idx); }).detach();
}
