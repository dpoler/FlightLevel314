// Linux METAR fetch — aviationweather.gov Data API. Same semantics as
// src/data/metar.cpp, with ICAO-first for saved airports and a 50nm
// nearest-station fallback (waypoints / non-reporting fields).
//
// In-memory per-location cache: switching back to an airport within the
// refresh window restores the last METAR immediately (no blank + refetch).

#include "../../src/data/metar.h"
#include "../../src/data/locations.h"
#include "../../src/platform/platform.h"
#include "../../src/ui/geo.h"

#include <ArduinoJson.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Routine METARs are roughly hourly (US ASOS often ~:51–:58). Poll at ~4×
// that rate so SPECI updates show up without hammering the API.
#define METAR_REFRESH_MS (15UL * 60UL * 1000UL)
#define METAR_CACHE_SLOTS 16

volatile MetarStatus metar_status = METAR_IDLE;
char metar_raw[METAR_RAW_LEN] = "";
char metar_station[8] = "";

namespace {

// Guards _busy, _active_key, _need_fetch and the cache (poll thread + one
// detached fetch thread both touch them).
std::mutex _fetch_mutex;
bool _busy = false;
char _active_key[24] = "";   // cache key of the location on screen now
bool _need_fetch = false;    // a switch happened while a fetch was running

struct MetarCacheEntry {
    char key[24];
    char raw[METAR_RAW_LEN];
    char station[8];
    MetarStatus status;
    uint32_t fetched_ms;
};
MetarCacheEntry _cache[METAR_CACHE_SLOTS] = {};
int _cache_count = 0;

// One fetch's outcome, built without touching the globals.
struct MetarResult {
    MetarStatus status = METAR_ERROR;
    char raw[METAR_RAW_LEN] = "";
    char station[8] = "";
};

void cache_key_for(const Location *loc, char *out, size_t out_sz) {
    if (loc && loc->icao[0]) {
        strlcpy(out, loc->icao, out_sz);
        return;
    }
    // Waypoint: round to ~0.01° so tiny jitter doesn't miss the cache.
    snprintf(out, out_sz, "WP%.2f,%.2f",
             loc ? (double)loc->lat : 0.0, loc ? (double)loc->lon : 0.0);
}

// Caller holds _fetch_mutex.
MetarCacheEntry *cache_find(const char *key) {
    for (int i = 0; i < _cache_count; i++) {
        if (strcmp(_cache[i].key, key) == 0) return &_cache[i];
    }
    return nullptr;
}

// Caller holds _fetch_mutex.
void cache_store(const char *key, const MetarResult &r) {
    MetarCacheEntry *e = cache_find(key);
    if (!e) {
        e = (_cache_count < METAR_CACHE_SLOTS) ? &_cache[_cache_count++] : &_cache[0];
    }
    strlcpy(e->key, key, sizeof(e->key));
    strlcpy(e->raw, r.raw, sizeof(e->raw));
    strlcpy(e->station, r.station, sizeof(e->station));
    e->status = r.status;
    e->fetched_ms = platform_millis();
}

void apply_to_globals(MetarStatus st, const char *raw, const char *station) {
    strlcpy(metar_raw, raw, sizeof(metar_raw));
    strlcpy(metar_station, station, sizeof(metar_station));
    metar_status = st;
}

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

bool fetch_ids(const char *icao, float lat, float lon, MetarResult &out) {
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
        if (doc.is<JsonArray>() && doc.as<JsonArray>().size() > 0) {
            JsonObject s = doc.as<JsonArray>()[0];
            best_raw = s["rawOb"] | "";
            best_id = s["icaoId"] | icao;
            if (!best_raw[0]) return false;
        } else {
            return false;
        }
    }

    strlcpy(out.raw, best_raw, sizeof(out.raw));
    strlcpy(out.station, best_id && best_id[0] ? best_id : icao, sizeof(out.station));
    out.status = METAR_OK;
    platform_log_debug("METAR: %s (ids)\n", out.station);
    return true;
}

void fetch_bbox(float lat, float lon, MetarResult &out) {
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
        out.status = METAR_ERROR;
        platform_log_warn("METAR: network failed\n");
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, buf.data(), len) != DeserializationError::Ok) {
        out.status = METAR_ERROR;
        platform_log_warn("METAR: JSON parse error\n");
        return;
    }

    const char *best_raw = nullptr;
    const char *best_id = nullptr;
    float best_dist = 1e9f;
    if (pick_best_from_array(doc, lat, lon, &best_raw, &best_id, &best_dist)) {
        strlcpy(out.raw, best_raw, sizeof(out.raw));
        strlcpy(out.station, best_id, sizeof(out.station));
        out.status = METAR_OK;
        platform_log_debug("METAR: %s (%.1fnm)\n", out.station, (double)best_dist);
        return;
    }

    out.status = METAR_NO_STATION;
    platform_log_info("METAR: no station within %.0fnm\n", (double)METAR_RANGE_NM);
}

// Fetches for one location (by cache key), caches the result under that key,
// and only publishes it to the globals if that location is still on screen --
// a slow fetch for the previous airport used to land after a switch and stay
// up, mislabeled, until the next 15-minute refresh.
void run_fetch(float lat, float lon, std::string icao, std::string key) {
    MetarResult r;
    if (icao.empty() || !fetch_ids(icao.c_str(), lat, lon, r)) fetch_bbox(lat, lon, r);

    std::lock_guard<std::mutex> lock(_fetch_mutex);
    const bool ok = (r.status == METAR_OK || r.status == METAR_NO_STATION);
    if (ok) cache_store(key.c_str(), r);
    if (strcmp(_active_key, key.c_str()) == 0) {
        // On error keep showing the last good text for this location, if any.
        if (ok || !metar_raw[0]) apply_to_globals(r.status, r.raw, r.station);
        else metar_status = METAR_ERROR;
    }
    _busy = false;
}

} // namespace

void metar_poll() {
    static uint32_t last_fetch_ms = 0;
    static int last_loc_idx = -2;
    static char last_key[24] = "";

    int idx = locations_active_index();
    if (idx == -1) {
        if (last_loc_idx != -1) {
            std::lock_guard<std::mutex> lock(_fetch_mutex);
            _active_key[0] = '\0';
            apply_to_globals(METAR_IDLE, "", "");
            last_loc_idx = -1;
            last_key[0] = '\0';
        }
        return;
    }

    float lat, lon;
    if (!locations_get_active_coords(&lat, &lon, nullptr)) return;
    const Location *loc = locations_get(idx);
    char key[24];
    cache_key_for(loc, key, sizeof(key));
    std::string icao = (loc && loc->icao[0]) ? loc->icao : "";

    uint32_t now = platform_millis();
    // Key (not just index) so an edited waypoint position or a reorder that
    // lands another location at this index also counts as a switch.
    bool loc_changed = (idx != last_loc_idx) || strcmp(key, last_key) != 0;

    std::lock_guard<std::mutex> lock(_fetch_mutex);
    if (loc_changed) {
        last_loc_idx = idx;
        strlcpy(last_key, key, sizeof(last_key));
        strlcpy(_active_key, key, sizeof(_active_key));
        MetarCacheEntry *hit = cache_find(key);
        if (hit && (now - hit->fetched_ms) < METAR_REFRESH_MS) {
            apply_to_globals(hit->status, hit->raw, hit->station);
            last_fetch_ms = hit->fetched_ms;
            _need_fetch = false;
            platform_log_debug("METAR: cache hit %s\n", key);
            return; // still fresh -- no network
        }
        // Cold switch: don't leave the previous airport's text up.
        apply_to_globals(METAR_FETCHING, "", "");
        _need_fetch = true;
    } else if (!_need_fetch && now - last_fetch_ms < METAR_REFRESH_MS) {
        return;
    }

    if (_busy) return; // _need_fetch keeps the request until this one finishes
    _busy = true;
    _need_fetch = false;
    last_fetch_ms = now;
    std::string key_copy(key);
    std::thread([lat, lon, icao, key_copy]() {
        run_fetch(lat, lon, icao, key_copy);
    }).detach();
}
