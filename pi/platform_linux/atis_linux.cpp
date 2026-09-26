// Linux D-ATIS fetch — datis.clowd.io (US major airports, ~76 fields).
// Waypoints resolve to the nearest airports_db entry within 50nm, then
// query that ICAO. Empty / missing responses → ATIS_UNAVAILABLE.

#include "../../src/data/atis.h"
#include "../../src/data/locations.h"
#include "../../src/platform/platform.h"
#include "../../src/ui/geo.h"
#include "../../src/ui/airports_db_include.h"

#include <ArduinoJson.h>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#define ATIS_REFRESH_MS (15UL * 60UL * 1000UL)
// After a failed fetch for the location on screen. A 15-minute wait left a
// cold switch that hit a network blip showing nothing for 15 minutes.
#define ATIS_RETRY_MS (2UL * 60UL * 1000UL)
#define ATIS_RANGE_NM 50.0f
#define ATIS_LIST_TTL_MS (6UL * 60UL * 60UL * 1000UL)

volatile AtisStatus atis_status = ATIS_IDLE;
char atis_airport[8] = "";
char atis_combined[ATIS_TEXT_LEN] = "";
char atis_arr[ATIS_TEXT_LEN] = "";
char atis_dep[ATIS_TEXT_LEN] = "";
bool atis_split = false;

namespace {

// Guards _busy, _active_icao, _need_fetch and the cache (poll thread + one
// detached fetch thread both touch them).
std::mutex _fetch_mutex;
bool _busy = false;
char _active_icao[8] = "";   // ATIS airport for the location on screen now
char _active_loc_key[24] = "";    // location on screen now (see loc_key_for)
char _published_loc_key[24] = ""; // location the atis_* globals describe

// Location identity, same scheme as metar_linux.cpp's cache key: ICAO for
// airports, rounded lat/lon for waypoints.
void loc_key_for(const Location *loc, char *out, size_t out_sz) {
    if (loc && loc->icao[0]) {
        strlcpy(out, loc->icao, out_sz);
        return;
    }
    snprintf(out, out_sz, "WP%.2f,%.2f",
             loc ? (double)loc->lat : 0.0, loc ? (double)loc->lon : 0.0);
}
bool _need_fetch = false;    // a switch happened while a fetch was running
bool _last_failed = false;   // last fetch for the active location errored

std::mutex _list_mutex;
std::unordered_set<std::string> _datis_icaos;
uint32_t _list_fetched_ms = 0;
uint32_t _list_failed_ms = 0;  // backoff after a failed list download
#define ATIS_LIST_RETRY_MS (10UL * 60UL * 1000UL)

#define ATIS_CACHE_SLOTS 16
// Also used as one fetch's result (built without touching the globals).
struct AtisCacheEntry {
    char icao[8];
    AtisStatus status;
    bool split;
    char combined[ATIS_TEXT_LEN];
    char arr[ATIS_TEXT_LEN];
    char dep[ATIS_TEXT_LEN];
    uint32_t fetched_ms;
};
AtisCacheEntry _atis_cache[ATIS_CACHE_SLOTS] = {};
int _atis_cache_count = 0;

void clear_texts() {
    atis_combined[0] = '\0';
    atis_arr[0] = '\0';
    atis_dep[0] = '\0';
    atis_split = false;
    atis_airport[0] = '\0';
}

// Caller holds _fetch_mutex.
AtisCacheEntry *atis_cache_find(const char *icao) {
    for (int i = 0; i < _atis_cache_count; i++) {
        if (strcmp(_atis_cache[i].icao, icao) == 0) return &_atis_cache[i];
    }
    return nullptr;
}

// Caller holds _fetch_mutex.
void atis_cache_store(const AtisCacheEntry &r) {
    AtisCacheEntry *e = atis_cache_find(r.icao);
    if (!e) {
        e = (_atis_cache_count < ATIS_CACHE_SLOTS)
                ? &_atis_cache[_atis_cache_count++]
                : &_atis_cache[0];
    }
    *e = r;
    e->fetched_ms = platform_millis();
}

void atis_cache_apply(const AtisCacheEntry *e) {
    strlcpy(atis_airport, e->icao, sizeof(atis_airport));
    atis_status = e->status;
    atis_split = e->split;
    strlcpy(atis_combined, e->combined, sizeof(atis_combined));
    strlcpy(atis_arr, e->arr, sizeof(atis_arr));
    strlcpy(atis_dep, e->dep, sizeof(atis_dep));
}

bool refresh_datis_list() {
    std::vector<char> buf(128 * 1024);
    size_t len = 0;
    if (!platform_http_get("https://datis.clowd.io/api/all", buf.data(), buf.size(), &len)) {
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, buf.data(), len) != DeserializationError::Ok) return false;
    if (!doc.is<JsonArray>()) return false;

    std::unordered_set<std::string> next;
    for (JsonObject row : doc.as<JsonArray>()) {
        const char *ap = row["airport"] | "";
        if (ap[0]) next.insert(ap);
    }
    size_t n = next.size();
    {
        std::lock_guard<std::mutex> lock(_list_mutex);
        _datis_icaos.swap(next);
        _list_fetched_ms = platform_millis();
    }
    platform_log_debug("ATIS: datis list %zu airports\n", n);
    return true;
}

bool icao_in_datis_list(const char *icao) {
    std::lock_guard<std::mutex> lock(_list_mutex);
    return _datis_icaos.count(icao) > 0;
}

// Refreshes the D-ATIS airport list when stale. After a failed download,
// waits ATIS_LIST_RETRY_MS before trying again -- this runs on every poll
// (1 Hz) for waypoint locations, and used to hit the API every second while
// it was down or the network was out.
bool ensure_datis_list() {
    uint32_t now = platform_millis();
    {
        std::lock_guard<std::mutex> lock(_list_mutex);
        if (!_datis_icaos.empty() && (now - _list_fetched_ms) < ATIS_LIST_TTL_MS)
            return true;
        if (_list_failed_ms && (now - _list_failed_ms) < ATIS_LIST_RETRY_MS)
            return !_datis_icaos.empty();
    }
    bool ok = refresh_datis_list();
    std::lock_guard<std::mutex> lock(_list_mutex);
    _list_failed_ms = ok ? 0 : (now ? now : 1);
    return ok || !_datis_icaos.empty();
}

#if HAS_AIRPORTS_DB
bool nearest_airport_icao(float lat, float lon, char *out, size_t out_sz, bool prefer_datis) {
    out[0] = '\0';
    float best = 1e9f;
    const char *best_icao = nullptr;
    float best_any = 1e9f;
    const char *best_any_icao = nullptr;

    for (int i = 0; i < AIRPORTS_DB_COUNT; i++) {
        const StaticAirport &ap = airports_db[i];
        float d = MapProjection::distance_nm(lat, lon, ap.lat, ap.lon);
        if (d > ATIS_RANGE_NM) continue;
        if (d < best_any) {
            best_any = d;
            best_any_icao = ap.icao;
        }
        if (prefer_datis && icao_in_datis_list(ap.icao) && d < best) {
            best = d;
            best_icao = ap.icao;
        }
    }
    const char *pick = best_icao ? best_icao : best_any_icao;
    if (!pick) return false;
    strlcpy(out, pick, out_sz);
    return true;
}
#else
bool nearest_airport_icao(float, float, char *out, size_t, bool) {
    out[0] = '\0';
    return false;
}
#endif

bool resolve_icao(char *out, size_t out_sz) {
    int idx = locations_active_index();
    if (idx < 0) return false;
    const Location *loc = locations_get(idx);
    if (!loc) return false;

    if (loc->icao[0]) {
        strlcpy(out, loc->icao, out_sz);
        return true;
    }

    ensure_datis_list();
    return nearest_airport_icao(loc->lat, loc->lon, out, out_sz, true);
}

void apply_rows(JsonArrayConst arr, AtisCacheEntry &r) {
    bool have_combined = false;
    bool have_arr = false;
    bool have_dep = false;
    for (JsonObjectConst row : arr) {
        const char *type = row["type"] | "";
        const char *text = row["datis"] | "";
        if (!text[0]) continue;
        if (strcmp(type, "arr") == 0) {
            strlcpy(r.arr, text, sizeof(r.arr));
            have_arr = true;
        } else if (strcmp(type, "dep") == 0) {
            strlcpy(r.dep, text, sizeof(r.dep));
            have_dep = true;
        } else {
            // combined or unknown
            strlcpy(r.combined, text, sizeof(r.combined));
            have_combined = true;
        }
    }

    if (have_arr || have_dep) {
        r.split = true;
        r.status = ATIS_OK;
    } else if (have_combined) {
        r.split = false;
        r.status = ATIS_OK;
    } else {
        r.status = ATIS_UNAVAILABLE;
    }
}

void do_fetch(const char *icao, AtisCacheEntry &r) {
    r = AtisCacheEntry{};
    strlcpy(r.icao, icao, sizeof(r.icao));
    r.status = ATIS_ERROR;

    char url[96];
    snprintf(url, sizeof(url), "https://datis.clowd.io/api/%s", icao);

    std::vector<char> buf(64 * 1024);
    size_t len = 0;
    long http_status = 0;
    // Use _ex: EGLL (and other non-US majors) return HTTP 404 with
    // {"error":"No results found"} — platform_http_get() treats that as
    // failure and we used to leave ATIS_ERROR (UI showed nothing).
    if (!platform_http_get_ex(url, buf.data(), buf.size(), &len, &http_status, nullptr)) {
        platform_log_warn("ATIS: network failed for %s\n", icao);
        return;
    }

    auto mark_unavailable = [&]() {
        r.status = ATIS_UNAVAILABLE;
        platform_log_info("ATIS: unavailable for %s (http %ld)\n", icao, http_status);
    };

    if (http_status == 404 || len == 0) {
        mark_unavailable();
        return;
    }
    if (http_status < 200 || http_status >= 300) {
        platform_log_warn("ATIS: HTTP %ld for %s\n", http_status, icao);
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, buf.data(), len) != DeserializationError::Ok) {
        platform_log_warn("ATIS: JSON parse error for %s\n", icao);
        return;
    }
    // {"error":"..."} object, empty array, or no rows.
    if (doc.is<JsonObject>() && doc["error"].is<const char *>()) {
        mark_unavailable();
        return;
    }
    if (!doc.is<JsonArray>() || doc.as<JsonArray>().size() == 0) {
        mark_unavailable();
        return;
    }

    apply_rows(doc.as<JsonArray>(), r);
    if (r.status == ATIS_OK) {
        platform_log_debug("ATIS: %s (%s)\n", icao, r.split ? "arr/dep" : "combined");
    }
}

// Fetch, cache under its own ICAO, and publish only if that airport is still
// the one on screen (see metar_linux.cpp's run_fetch for the old race).
void run_fetch(std::string icao) {
    AtisCacheEntry r;
    do_fetch(icao.c_str(), r);

    std::lock_guard<std::mutex> lock(_fetch_mutex);
    const bool ok = (r.status == ATIS_OK || r.status == ATIS_UNAVAILABLE);
    if (ok) atis_cache_store(r);
    if (strcmp(_active_icao, icao.c_str()) == 0) {
        _last_failed = !ok;
        if (ok) atis_cache_apply(&r);
        else atis_status = ATIS_ERROR;
        strlcpy(_published_loc_key, _active_loc_key, sizeof(_published_loc_key));
    }
    _busy = false;
}

} // namespace

void atis_poll() {
    static uint32_t last_fetch_ms = 0;
    static int last_loc_idx = -2;
    static char last_icao[8] = "";
    static char last_loc_key[24] = "";

    int idx = locations_active_index();
    if (idx == -1) {
        if (last_loc_idx != -1) {
            std::lock_guard<std::mutex> lock(_fetch_mutex);
            _active_icao[0] = '\0';
            _active_loc_key[0] = '\0';
            _published_loc_key[0] = '\0';
            atis_status = ATIS_IDLE;
            clear_texts();
            last_loc_idx = -1;
            last_icao[0] = '\0';
            last_loc_key[0] = '\0';
        }
        return;
    }

    char loc_key[24] = "";
    loc_key_for(locations_get(idx), loc_key, sizeof(loc_key));

    char icao[8] = {};
    if (!resolve_icao(icao, sizeof(icao))) {
        if (last_loc_idx != idx || last_icao[0] || strcmp(loc_key, last_loc_key) != 0) {
            std::lock_guard<std::mutex> lock(_fetch_mutex);
            _active_icao[0] = '\0';
            strlcpy(_active_loc_key, loc_key, sizeof(_active_loc_key));
            strlcpy(_published_loc_key, loc_key, sizeof(_published_loc_key));
            clear_texts();
            atis_status = ATIS_UNAVAILABLE;
            last_loc_idx = idx;
            last_icao[0] = '\0';
            strlcpy(last_loc_key, loc_key, sizeof(last_loc_key));
        }
        return;
    }

    uint32_t now = platform_millis();
    // Location key too: a removed location's neighbor can take the same index
    // and even the same nearest ATIS airport, but it's still a switch.
    bool loc_changed = (idx != last_loc_idx) || (strcmp(icao, last_icao) != 0)
                       || (strcmp(loc_key, last_loc_key) != 0);

    std::lock_guard<std::mutex> lock(_fetch_mutex);
    if (loc_changed) {
        _last_failed = false;
        last_loc_idx = idx;
        strlcpy(last_loc_key, loc_key, sizeof(last_loc_key));
        strlcpy(last_icao, icao, sizeof(last_icao));
        strlcpy(_active_icao, icao, sizeof(_active_icao));
        strlcpy(_active_loc_key, loc_key, sizeof(_active_loc_key));
        // Published immediately below either way: cached text or FETCHING.
        strlcpy(_published_loc_key, loc_key, sizeof(_published_loc_key));
        AtisCacheEntry *hit = atis_cache_find(icao);
        if (hit && (now - hit->fetched_ms) < ATIS_REFRESH_MS) {
            atis_cache_apply(hit);
            last_fetch_ms = hit->fetched_ms;
            _need_fetch = false;
            platform_log_debug("ATIS: cache hit %s\n", icao);
            return;
        }
        clear_texts();
        strlcpy(atis_airport, icao, sizeof(atis_airport));
        atis_status = ATIS_FETCHING;
        _need_fetch = true;
    } else if (!_need_fetch &&
               now - last_fetch_ms < (_last_failed ? ATIS_RETRY_MS : ATIS_REFRESH_MS)) {
        return;
    }

    if (_busy) return; // _need_fetch keeps the request until this one finishes
    _busy = true;
    _need_fetch = false;
    last_fetch_ms = now;
    std::thread([icao = std::string(icao)]() { run_fetch(icao); }).detach();
}

bool atis_for_active_location() {
    char key[24] = "";
    int idx = locations_active_index();
    if (idx >= 0) loc_key_for(locations_get(idx), key, sizeof(key));
    std::lock_guard<std::mutex> lock(_fetch_mutex);
    return strcmp(key, _published_loc_key) == 0;
}
