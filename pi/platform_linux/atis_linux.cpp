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
#define ATIS_RANGE_NM 50.0f
#define ATIS_LIST_TTL_MS (6UL * 60UL * 60UL * 1000UL)

volatile AtisStatus atis_status = ATIS_IDLE;
char atis_airport[8] = "";
char atis_combined[ATIS_TEXT_LEN] = "";
char atis_arr[ATIS_TEXT_LEN] = "";
char atis_dep[ATIS_TEXT_LEN] = "";
bool atis_split = false;

namespace {

std::mutex _fetch_mutex;
bool _busy = false;

std::mutex _list_mutex;
std::unordered_set<std::string> _datis_icaos;
uint32_t _list_fetched_ms = 0;

void clear_texts() {
    atis_combined[0] = '\0';
    atis_arr[0] = '\0';
    atis_dep[0] = '\0';
    atis_split = false;
    atis_airport[0] = '\0';
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
    {
        std::lock_guard<std::mutex> lock(_list_mutex);
        _datis_icaos.swap(next);
        _list_fetched_ms = platform_millis();
    }
    platform_log("ATIS: datis list %zu airports\n", _datis_icaos.size());
    return true;
}

bool icao_in_datis_list(const char *icao) {
    std::lock_guard<std::mutex> lock(_list_mutex);
    return _datis_icaos.count(icao) > 0;
}

bool ensure_datis_list() {
    uint32_t now = platform_millis();
    {
        std::lock_guard<std::mutex> lock(_list_mutex);
        if (!_datis_icaos.empty() && (now - _list_fetched_ms) < ATIS_LIST_TTL_MS)
            return true;
    }
    return refresh_datis_list();
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

void apply_rows(JsonArrayConst arr, const char *icao) {
    clear_texts();
    strlcpy(atis_airport, icao, sizeof(atis_airport));

    bool have_combined = false;
    bool have_arr = false;
    bool have_dep = false;
    for (JsonObjectConst row : arr) {
        const char *type = row["type"] | "";
        const char *text = row["datis"] | "";
        if (!text[0]) continue;
        if (strcmp(type, "arr") == 0) {
            strlcpy(atis_arr, text, sizeof(atis_arr));
            have_arr = true;
        } else if (strcmp(type, "dep") == 0) {
            strlcpy(atis_dep, text, sizeof(atis_dep));
            have_dep = true;
        } else {
            // combined or unknown
            strlcpy(atis_combined, text, sizeof(atis_combined));
            have_combined = true;
        }
    }

    if (have_arr || have_dep) {
        atis_split = true;
        atis_status = ATIS_OK;
    } else if (have_combined) {
        atis_split = false;
        atis_status = ATIS_OK;
    } else {
        atis_status = ATIS_UNAVAILABLE;
    }
}

void do_fetch(const char *icao) {
    atis_status = ATIS_FETCHING;

    char url[96];
    snprintf(url, sizeof(url), "https://datis.clowd.io/api/%s", icao);

    std::vector<char> buf(64 * 1024);
    size_t len = 0;
    if (!platform_http_get(url, buf.data(), buf.size(), &len)) {
        atis_status = ATIS_ERROR;
        platform_log("ATIS: network failed for %s\n", icao);
        return;
    }

    // Empty body / "[]" → not in the US majors set (or no current text).
    if (len == 0 || (len <= 2 && buf[0] == '[')) {
        clear_texts();
        strlcpy(atis_airport, icao, sizeof(atis_airport));
        atis_status = ATIS_UNAVAILABLE;
        platform_log("ATIS: unavailable for %s\n", icao);
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, buf.data(), len) != DeserializationError::Ok) {
        atis_status = ATIS_ERROR;
        platform_log("ATIS: JSON parse error for %s\n", icao);
        return;
    }
    if (!doc.is<JsonArray>() || doc.as<JsonArray>().size() == 0) {
        clear_texts();
        strlcpy(atis_airport, icao, sizeof(atis_airport));
        atis_status = ATIS_UNAVAILABLE;
        return;
    }

    apply_rows(doc.as<JsonArray>(), icao);
    if (atis_status == ATIS_OK) {
        platform_log("ATIS: %s (%s)\n", atis_airport, atis_split ? "arr/dep" : "combined");
    }
}

void run_fetch(std::string icao) {
    do_fetch(icao.c_str());
    std::lock_guard<std::mutex> lock(_fetch_mutex);
    _busy = false;
}

} // namespace

void atis_poll() {
    static uint32_t last_fetch_ms = 0;
    static int last_loc_idx = -2;
    static char last_icao[8] = "";

    int idx = locations_active_index();
    if (idx == -1) {
        if (last_loc_idx != -1) {
            atis_status = ATIS_IDLE;
            clear_texts();
            last_loc_idx = -1;
            last_icao[0] = '\0';
        }
        return;
    }

    char icao[8] = {};
    if (!resolve_icao(icao, sizeof(icao))) {
        // Waypoint with nothing within 50nm
        if (last_loc_idx != idx || last_icao[0]) {
            clear_texts();
            atis_status = ATIS_UNAVAILABLE;
            last_loc_idx = idx;
            last_icao[0] = '\0';
        }
        return;
    }

    uint32_t now = platform_millis();
    bool loc_changed = (idx != last_loc_idx) || (strcmp(icao, last_icao) != 0);
    bool due = (now - last_fetch_ms >= ATIS_REFRESH_MS);
    if (!loc_changed && !due) return;

    {
        std::lock_guard<std::mutex> lock(_fetch_mutex);
        if (_busy) return;
        _busy = true;
    }

    if (loc_changed) clear_texts();
    last_loc_idx = idx;
    strlcpy(last_icao, icao, sizeof(last_icao));
    last_fetch_ms = now;

    std::thread([icao = std::string(icao)]() { run_fetch(icao); }).detach();
}
