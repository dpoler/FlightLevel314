#include "locations.h"
#include "storage.h"
#include "http_mutex.h"
#include "fetcher.h"
#include "../ui/range.h"
#include "../ui/geo.h"
#include "lvgl.h"
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <cctype>

// Static airport glyph DB (icao/lat/lon/large-flag only, no runway geometry
// -- see tools/generate_airports_db.py) is gitignored like static_map_data.h;
// same __has_include pattern map_view.cpp/radar_view.cpp use, needed here
// too since the nearby-large-airport scan (locations_nearby_set_enabled())
// reads it directly rather than going through a view file.
#if __has_include("../ui/airports_db.h")
#include "../ui/airports_db.h"
#define HAS_AIRPORTS_DB 1
#else
#define HAS_AIRPORTS_DB 0
#endif

// NetworkClientSecure's default TLS handshake timeout is 120s and is NOT
// bounded by HTTPClient::setTimeout() (that only covers the read phase after
// a connection succeeds) -- a slow/hung handshake here would otherwise hold
// http_mutex for up to two full minutes, starving every other network
// consumer in the app. Must construct the WiFiClientSecure ourselves and
// call setHandshakeTimeout() on it before HTTPClient::begin(), since the
// single-string begin(url) overload creates its own client with the 120s
// default baked in.
#define TLS_HANDSHAKE_TIMEOUT_S 8

// PSRAM allocator for the airportdb.io JSON response — keeps this off internal
// DRAM, which this board (ESP32-P4 + C6 co-processor) already runs thin on.
struct PsramAllocator : ArduinoJson::Allocator {
    void* allocate(size_t size) override {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }
    void deallocate(void* p) override {
        heap_caps_free(p);
    }
    void* reallocate(void* p, size_t size) override {
        return heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM);
    }
};
static PsramAllocator _psram_alloc;

static Preferences _prefs;
static Location _locations[MAX_LOCATIONS];
static int _count = 0;
static int _active_index = -1; // -1 = Home

// Nearby-large-airport cache -- only the *active* location's data is ever
// resident in DRAM (loaded lazily, see locations_nearby_get_active()).
// Everything else lives in NVS under a per-owner key until it's needed.
static Location _nearby[NEARBY_MAX];
static int _nearby_loaded_count = 0;
static int _nearby_loaded_for = -2; // sentinel: -2 = nothing loaded yet, -1 = Home, >=0 = saved idx

// Fetch queue for the currently-in-progress nearby-airport cache pass (one
// owner at a time, same "no dedicated task, ride location_poll_task's
// existing stack" reasoning as the add-by-ICAO queue below).
//
// The owner is tracked by ICAO (or "is home"), NOT by _locations[] index --
// a fetch pass can take several seconds to drain, during which
// locations_reorder()/locations_remove() can freely change what a given
// index points at (both are explicitly documented as shifting the array).
// Resolving by ICAO at commit time (nearby_resolve_owner_idx()) avoids
// silently writing a completed fetch into the wrong airport's cache slot.
static char _nearby_queue_icao[NEARBY_MAX][LOC_ICAO_LEN];
static int _nearby_queue_len = 0;
static int _nearby_queue_pos = 0;
static bool _nearby_queue_active = false;
static bool _nearby_queue_owner_is_home = false;
static char _nearby_queue_owner_icao[LOC_ICAO_LEN] = {};
static Location _nearby_fetch_buf[NEARBY_MAX]; // accumulates results until the queue drains, then one NVS write
static int _nearby_fetch_buf_count = 0;

// If a toggle-on happens while a different owner's fetch pass is already in
// flight, it's queued here rather than clobbering the in-progress one (which
// would silently strand the first owner's toggle as "enabled" with nothing
// ever fetched for it, since a scan is only ever triggered on the off->on
// transition). Small fixed cap -- toggling more than a few locations on in
// one sitting before the first pass finishes is not a realistic case worth
// unbounded queuing for.
#define NEARBY_PENDING_OWNERS_MAX 4
static char _nearby_pending_icao[NEARBY_PENDING_OWNERS_MAX][LOC_ICAO_LEN];
static bool _nearby_pending_is_home[NEARBY_PENDING_OWNERS_MAX];
static int _nearby_pending_count = 0;

static void location_sync_timer_cb(lv_timer_t *t);

// "Add by ICAO" request/response — processed by locations_add_poll(), called
// from location_poll_task's existing loop rather than a dedicated task.
static SemaphoreHandle_t _add_mutex = nullptr;
static bool _add_pending = false;
static char _add_pending_icao[LOC_ICAO_LEN] = {};
static bool _add_result_ready = false;
static bool _add_result_ok = false;
static char _add_result_err[48] = {};

// On-disk format: for each saved location, a fixed-size header (icao, lat,
// lon, elevation_ft, runway_count) followed by exactly runway_count
// LocRunway entries -- NOT a fixed MAX_RUNWAYS reservation. A location's
// runways[] array in memory is sized for the worst case (KORD-sized
// airports), but writing that full reservation to NVS for every saved
// airport regardless of how many runways it actually has is exactly the
// kind of large blocking flash write that visibly stalls this board's LCD
// panel (see project_p4_heap_constraints memory -- the cyan-flash bug).
// Packing tightly keeps the write proportional to real data.
// Also reused as-is for each entry in a nearby-airport cache blob (see
// nearby_commit()/locations_nearby_get_active()) -- same per-airport fields,
// just written under a different key and not counted against MAX_LOCATIONS.
// Growing this header (nearby_enabled/nearby_count added) changes the
// on-disk format -- same accepted precedent as the MAX_RUNWAYS bump: saved
// airports reset to empty once on the first boot after upgrading.
struct LocationHeader {
    char icao[LOC_ICAO_LEN];
    float lat, lon;
    int elevation_ft;
    int runway_count;
    int nearby_enabled; // stored as int, not bool, to keep the struct's
                          // layout unambiguous across a raw memcpy into NVS
    int nearby_count;
};

static void save_all() {
    _prefs.begin("adsb_locs", false);
    _prefs.putInt("count", _count);
    if (_count > 0) {
        size_t buf_size = 0;
        for (int i = 0; i < _count; i++)
            buf_size += sizeof(LocationHeader) + (size_t)_locations[i].runway_count * sizeof(LocRunway);

        uint8_t *buf = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        if (buf) {
            size_t pos = 0;
            for (int i = 0; i < _count; i++) {
                const Location &loc = _locations[i];
                LocationHeader hdr;
                strlcpy(hdr.icao, loc.icao, sizeof(hdr.icao));
                hdr.lat = loc.lat;
                hdr.lon = loc.lon;
                hdr.elevation_ft = loc.elevation_ft;
                hdr.runway_count = loc.runway_count;
                hdr.nearby_enabled = loc.nearby_enabled ? 1 : 0;
                hdr.nearby_count = loc.nearby_count;
                memcpy(buf + pos, &hdr, sizeof(hdr));
                pos += sizeof(hdr);
                size_t rwy_bytes = (size_t)loc.runway_count * sizeof(LocRunway);
                memcpy(buf + pos, loc.runways, rwy_bytes);
                pos += rwy_bytes;
            }
            _prefs.putBytes("locs", buf, buf_size);
            heap_caps_free(buf);
        }
    } else {
        _prefs.remove("locs");
    }
    _prefs.end();
}

void locations_init() {
    _prefs.begin("adsb_locs", true);
    _count = _prefs.getInt("count", 0);
    if (_count < 0) _count = 0;
    if (_count > MAX_LOCATIONS) _count = MAX_LOCATIONS;
    memset(_locations, 0, sizeof(_locations));

    size_t blob_len = _prefs.getBytesLength("locs");
    if (_count > 0 && blob_len > 0) {
        uint8_t *buf = (uint8_t *)heap_caps_malloc(blob_len, MALLOC_CAP_SPIRAM);
        int parsed = 0;
        if (buf) {
            size_t got = _prefs.getBytes("locs", buf, blob_len);
            size_t pos = 0;
            for (; parsed < _count; parsed++) {
                if (pos + sizeof(LocationHeader) > got) break; // truncated -- bail, don't trust the rest
                LocationHeader hdr;
                memcpy(&hdr, buf + pos, sizeof(hdr));
                pos += sizeof(hdr);
                if (hdr.runway_count < 0 || hdr.runway_count > MAX_RUNWAYS) break; // corrupt
                size_t rwy_bytes = (size_t)hdr.runway_count * sizeof(LocRunway);
                if (pos + rwy_bytes > got) break; // truncated

                Location &loc = _locations[parsed];
                strlcpy(loc.icao, hdr.icao, sizeof(loc.icao));
                loc.lat = hdr.lat;
                loc.lon = hdr.lon;
                loc.elevation_ft = hdr.elevation_ft;
                loc.runway_count = hdr.runway_count;
                loc.nearby_enabled = hdr.nearby_enabled != 0;
                loc.nearby_count = hdr.nearby_count;
                memcpy(loc.runways, buf + pos, rwy_bytes);
                pos += rwy_bytes;
            }
            heap_caps_free(buf);
        }
        if (parsed != _count) {
            // Inconsistent/corrupt/old-format blob -- don't trust partial data.
            _count = 0;
            memset(_locations, 0, sizeof(_locations));
        }
    } else if (_count > 0) {
        // count > 0 but no blob at all -- inconsistent, reset.
        _count = 0;
    }

    _prefs.end();

    // Resume-on-boot: restore whichever location was active last, matched by
    // ICAO rather than raw index since a saved airport's position in
    // _locations[] can shift across reboots (removals compact the array).
    // Falls back to Home (-1) if it wasn't found (removed, or never set).
    _active_index = -1;
    if (g_config.last_location_icao[0]) {
        for (int i = 0; i < _count; i++) {
            if (strcmp(_locations[i].icao, g_config.last_location_icao) == 0) {
                _active_index = i;
                break;
            }
        }
    }

    _add_mutex = xSemaphoreCreateMutex();

    lv_timer_create(location_sync_timer_cb, 1000, nullptr);
}

int locations_count() {
    return _count;
}

const Location* locations_get(int idx) {
    if (idx < 0 || idx >= _count) return nullptr;
    return &_locations[idx];
}

void locations_remove(int idx) {
    if (idx < 0 || idx >= _count) return;
    bool was_active = (_active_index == idx);
    for (int i = idx; i < _count - 1; i++) {
        _locations[i] = _locations[i + 1];
    }
    _count--;
    memset(&_locations[_count], 0, sizeof(Location));
    if (_active_index == idx) _active_index = -1;
    else if (_active_index > idx) _active_index--;
    if (was_active && g_config.last_location_icao[0]) {
        // Don't leave a stale ICAO in NVS pointing at a now-removed airport
        // -- resuming would fall back to Home anyway (not found in
        // locations_init()'s lookup), but clear it here for consistency.
        g_config.last_location_icao[0] = '\0';
        storage_save_config(g_config);
    }
    save_all();
}

void locations_reorder(int from, int to) {
    if (from < 0 || from >= _count || to < 0 || to >= _count || from == to) return;

    Location moved = _locations[from];
    if (from < to) {
        for (int i = from; i < to; i++) _locations[i] = _locations[i + 1];
    } else {
        for (int i = from; i > to; i--) _locations[i] = _locations[i - 1];
    }
    _locations[to] = moved;

    // The active selection (if it was one of the affected slots) needs to
    // keep pointing at the same airport it did before the shift, not the
    // same array index -- same reasoning as locations_remove()'s own index
    // bookkeeping below.
    if (_active_index == from) {
        _active_index = to;
    } else if (from < to && _active_index > from && _active_index <= to) {
        _active_index--;
    } else if (from > to && _active_index >= to && _active_index < from) {
        _active_index++;
    }

    save_all();
}

int locations_active_index() {
    return _active_index;
}

void locations_set_active(int idx) {
    if (idx < -1 || idx >= _count) return;
    _active_index = idx;

    // Persist for resume-on-boot -- this is only ever called from the
    // location picker's own row-tap handler, a discrete human action, so an
    // immediate NVS write is safe.
    const char *icao = (idx == -1) ? "" : _locations[idx].icao;
    if (strcmp(g_config.last_location_icao, icao) != 0) {
        strlcpy(g_config.last_location_icao, icao, sizeof(g_config.last_location_icao));
        storage_save_config(g_config);
    }
}

bool locations_get_active_coords(float *lat, float *lon, int *elevation_ft) {
    if (_active_index == -1) {
        if (lat) *lat = g_config.home_lat;
        if (lon) *lon = g_config.home_lon;
        if (elevation_ft) *elevation_ft = g_config.home_elevation_ft;
        return true;
    }
    const Location *loc = locations_get(_active_index);
    if (!loc) return false;
    if (lat) *lat = loc->lat;
    if (lon) *lon = loc->lon;
    if (elevation_ft) *elevation_ft = loc->elevation_ft;
    return true;
}

AircraftList* locations_active_list(AircraftList *home_list) {
    return (_active_index == -1) ? home_list : fetcher_location_list();
}

// Keeps the on-demand secondary fetch targeted at whichever non-home location
// is active, using the currently selected range as the query radius. Runs
// regardless of which of the 4 views is on screen — any of them may be
// showing a saved airport's data.
static void location_sync_timer_cb(lv_timer_t *t) {
    if (_active_index == -1) {
        fetcher_set_location_target(0, 0, 0); // Home is covered by the main fetch loop
        return;
    }
    const Location *loc = locations_get(_active_index);
    if (!loc) return;
    fetcher_set_location_target(loc->lat, loc->lon, (int)range_get_nm());
}

// NOTE: airportdb.io wraps OurAirports data. Field names below follow
// OurAirports' well-known airports.csv/runways.csv column names
// (latitude_deg/longitude_deg/elevation_ft, le_ident/le_latitude_deg/...).
// Not verified against a live response in this environment (no network
// access while writing this) — if fields come back zeroed/missing on real
// hardware, check the actual response shape and adjust the keys below.
//
// Shared by locations_add_from_icao() (adds a user-saved location) and the
// nearby-large-airport cache fetch (locations_nearby_poll()) -- same
// network/parsing logic, only what the caller does with the result differs.
// Does NOT check g_config.airportdb_token, MAX_LOCATIONS, or de-dupe against
// existing saves -- those are caller-specific and stay in
// locations_add_from_icao(); the nearby-cache path has none of them (it
// doesn't count against MAX_LOCATIONS and duplicates across different
// owners' nearby lists are fine).
static bool fetch_airport_data(const char *icao_upper, Location &out, char *err, size_t err_size) {
    auto fail = [&](const char *msg) {
        if (err && err_size) strlcpy(err, msg, err_size);
        return false;
    };

    if (!http_mutex_acquire(pdMS_TO_TICKS(15000))) return fail("network busy, try again");

    char url[160];
    snprintf(url, sizeof(url), "https://airportdb.io/api/v1/airport/%s?apiToken=%s",
             icao_upper, g_config.airportdb_token);

    WiFiClientSecure client;
    client.setInsecure(); // matches http.begin(url)'s own no-CA-cert behavior
    client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(10000);
    int code = http.GET();

    bool ok = false;
    if (code == HTTP_CODE_OK) {
        int len = http.getSize();
        size_t buf_size = (len > 0) ? (size_t)len + 1 : 64 * 1024;
        char *buf = (char *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        if (buf) {
            size_t total = 0;
            size_t target = (len > 0) ? (size_t)len : buf_size - 1;
            WiFiClient *stream = http.getStreamPtr();
            uint32_t deadline = millis() + 10000;
            while (total < target && millis() < deadline) {
                int avail = stream->available();
                if (avail > 0) {
                    int to_read = min((size_t)avail, target - total);
                    total += stream->readBytes(buf + total, to_read);
                } else if (!stream->connected()) {
                    break;
                } else {
                    vTaskDelay(1);
                }
            }
            buf[total] = '\0';

            if (total > 0) {
                JsonDocument doc(&_psram_alloc);
                if (!deserializeJson(doc, buf, total)) {
                    // airportdb.io returns every coordinate as a JSON *string*
                    // (e.g. "39.8409"), not a number — inherited from its CSV
                    // pipeline. Use .as<float>() (does string->number
                    // conversion) rather than the `| default` operator (which
                    // only converts when the stored type already matches).
                    float lat = doc["latitude_deg"].as<float>();
                    float lon = doc["longitude_deg"].as<float>();
                    if (lat != 0.0f || lon != 0.0f) {
                        out = Location{};
                        strlcpy(out.icao, icao_upper, sizeof(out.icao));
                        out.lat = lat;
                        out.lon = lon;
                        out.elevation_ft = doc["elevation_ft"].as<int>();

                        JsonArray rwys = doc["runways"].as<JsonArray>();
                        for (JsonObject r : rwys) {
                            if (out.runway_count >= MAX_RUNWAYS) break;
                            // Skip decommissioned runways -- OurAirports (which
                            // airportdb.io wraps) tracks this per-runway (e.g.
                            // KORD's old diagonals 14L/32R and 18/36 are marked
                            // closed=1 despite still having valid coordinates)
                            // and without this check we'd draw them as if
                            // active, and -- since MAX_RUNWAYS is a fixed cap --
                            // potentially crowd out a real active runway that
                            // arrives later in the array. Confirmed via a live
                            // airportdb.io response (2026-07) that this field
                            // and its string "0"/"1" encoding are handled
                            // correctly; airportdb.io's own data can still lag
                            // OurAirports for very recent runway changes (see
                            // project_location_architecture memory) -- that's
                            // a data-source staleness limitation, not a parsing
                            // bug, and isn't fixable here.
                            if (r["closed"].as<int>() == 1) continue;
                            float le_lat = r["le_latitude_deg"].as<float>();
                            float le_lon = r["le_longitude_deg"].as<float>();
                            float he_lat = r["he_latitude_deg"].as<float>();
                            float he_lon = r["he_longitude_deg"].as<float>();
                            // Skip runways OurAirports has no threshold coordinates for —
                            // draw only what we can actually place on the map.
                            if ((le_lat == 0.0f && le_lon == 0.0f) ||
                                (he_lat == 0.0f && he_lon == 0.0f)) continue;

                            LocRunway &rw = out.runways[out.runway_count++];
                            rw.le_lat = le_lat;
                            rw.le_lon = le_lon;
                            rw.he_lat = he_lat;
                            rw.he_lon = he_lon;
                            strlcpy(rw.le_id, r["le_ident"] | "", sizeof(rw.le_id));
                            strlcpy(rw.he_id, r["he_ident"] | "", sizeof(rw.he_id));
                        }

                        ok = true;
                    } else {
                        fail("airport not found");
                    }
                } else {
                    fail("bad response from airportdb.io");
                }
            } else {
                fail("empty response");
            }
            heap_caps_free(buf);
        } else {
            fail("out of memory");
        }
    } else if (code == 404) {
        fail("airport not found");
    } else if (code == 401 || code == 403) {
        fail("invalid airportdb.io token");
    } else {
        char msg[32];
        snprintf(msg, sizeof(msg), "HTTP error %d", code);
        fail(msg);
    }

    http.end();
    http_mutex_release();
    return ok;
}

bool locations_add_from_icao(const char *icao, char *err, size_t err_size) {
    auto fail = [&](const char *msg) {
        if (err && err_size) strlcpy(err, msg, err_size);
        return false;
    };

    if (!icao || !icao[0]) return fail("no ICAO given");
    if (_count >= MAX_LOCATIONS) return fail("location list full");
    if (!g_config.airportdb_token[0]) return fail("no airportdb.io token set");

    char icao_upper[LOC_ICAO_LEN] = {};
    strlcpy(icao_upper, icao, sizeof(icao_upper));
    for (char *p = icao_upper; *p; p++) *p = toupper((unsigned char)*p);

    // Check for an existing entry first — avoid duplicate network calls.
    for (int i = 0; i < _count; i++) {
        if (strcmp(_locations[i].icao, icao_upper) == 0) return fail("already saved");
    }

    Location loc;
    if (!fetch_airport_data(icao_upper, loc, err, err_size)) return false;

    _locations[_count++] = loc;
    save_all();
    return true;
}

void locations_request_add(const char *icao) {
    xSemaphoreTake(_add_mutex, portMAX_DELAY);
    strlcpy(_add_pending_icao, icao, sizeof(_add_pending_icao));
    _add_pending = true;
    _add_result_ready = false;
    xSemaphoreGive(_add_mutex);
}

void locations_add_poll() {
    char icao[LOC_ICAO_LEN];
    bool has_request = false;

    xSemaphoreTake(_add_mutex, portMAX_DELAY);
    if (_add_pending) {
        strlcpy(icao, _add_pending_icao, sizeof(icao));
        _add_pending = false;
        has_request = true;
    }
    xSemaphoreGive(_add_mutex);

    if (!has_request) return;

    char err[48] = {};
    bool ok = locations_add_from_icao(icao, err, sizeof(err));

    xSemaphoreTake(_add_mutex, portMAX_DELAY);
    _add_result_ok = ok;
    strlcpy(_add_result_err, err, sizeof(_add_result_err));
    _add_result_ready = true;
    xSemaphoreGive(_add_mutex);
}

bool locations_add_result(bool *ok, char *err, size_t err_size) {
    bool ready;
    xSemaphoreTake(_add_mutex, portMAX_DELAY);
    ready = _add_result_ready;
    if (ready) {
        if (ok) *ok = _add_result_ok;
        if (err && err_size) strlcpy(err, _add_result_err, err_size);
        _add_result_ready = false;
    }
    xSemaphoreGive(_add_mutex);
    return ready;
}

// ---- Nearby-large-airport cache ----------------------------------------
//
// No new synchronization here beyond what _locations[]/_count already have
// (none) -- locations_nearby_poll() runs on location_poll_task same as
// locations_add_poll(), and the picker/draw-side reads happen from the
// UI/render thread with the same accepted eventual-consistency as every
// other read of _locations[] in this file.

static bool nearby_owner_coords(int idx, float *lat, float *lon, const char **icao_or_null) {
    if (idx == -1) {
        *lat = g_config.home_lat;
        *lon = g_config.home_lon;
        if (icao_or_null) *icao_or_null = nullptr;
        return true;
    }
    const Location *loc = locations_get(idx);
    if (!loc) return false;
    *lat = loc->lat;
    *lon = loc->lon;
    if (icao_or_null) *icao_or_null = loc->icao;
    return true;
}

// "nb_home" for Home, "nb_<ICAO>" for a saved location -- both comfortably
// under Preferences' 15-char key limit (LOC_ICAO_LEN caps ICAO at 7 chars).
static void nearby_nvs_key(int idx, char *out, size_t out_size) {
    if (idx == -1) {
        strlcpy(out, "nb_home", out_size);
        return;
    }
    const Location *loc = locations_get(idx);
    snprintf(out, out_size, "nb_%s", loc ? loc->icao : "");
}

bool locations_nearby_enabled(int idx) {
    if (idx == -1) return g_config.home_nearby_enabled;
    const Location *loc = locations_get(idx);
    return loc ? loc->nearby_enabled : false;
}

int locations_nearby_count(int idx) {
    if (idx == -1) return g_config.home_nearby_count;
    const Location *loc = locations_get(idx);
    return loc ? loc->nearby_count : 0;
}

// Writes the fully-drained fetch batch for `owner_idx` to NVS in one shot
// (not incrementally per-fetch -- avoids repeated read-modify-write churn on
// a blob that's growing across a burst of several sequential fetches), and
// updates that owner's persisted nearby_count header field. If the owner is
// the currently active location, also refreshes the resident draw-time
// cache (_nearby[]) immediately rather than waiting for the next active-
// index change to trigger a reload.
static void nearby_commit(int owner_idx, const Location *entries, int n) {
    char key[16];
    nearby_nvs_key(owner_idx, key, sizeof(key));

    _prefs.begin("adsb_locs", false);
    if (n > 0) {
        size_t buf_size = sizeof(int32_t);
        for (int i = 0; i < n; i++)
            buf_size += sizeof(LocationHeader) + (size_t)entries[i].runway_count * sizeof(LocRunway);

        uint8_t *buf = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
        if (buf) {
            size_t pos = 0;
            int32_t cnt = n;
            memcpy(buf, &cnt, sizeof(cnt));
            pos += sizeof(cnt);
            for (int i = 0; i < n; i++) {
                const Location &e = entries[i];
                LocationHeader hdr;
                strlcpy(hdr.icao, e.icao, sizeof(hdr.icao));
                hdr.lat = e.lat;
                hdr.lon = e.lon;
                hdr.elevation_ft = e.elevation_ft;
                hdr.runway_count = e.runway_count;
                hdr.nearby_enabled = 0; // unused for nearby-cache entries themselves
                hdr.nearby_count = 0;
                memcpy(buf + pos, &hdr, sizeof(hdr));
                pos += sizeof(hdr);
                size_t rwy_bytes = (size_t)e.runway_count * sizeof(LocRunway);
                memcpy(buf + pos, e.runways, rwy_bytes);
                pos += rwy_bytes;
            }
            _prefs.putBytes(key, buf, buf_size);
            heap_caps_free(buf);
        }
    } else {
        _prefs.remove(key);
    }
    _prefs.end();

    if (owner_idx == -1) {
        g_config.home_nearby_count = n;
        storage_save_config(g_config);
    } else if (owner_idx >= 0 && owner_idx < _count) {
        _locations[owner_idx].nearby_count = n;
        save_all();
    }

    if (owner_idx == _active_index) {
        int copy_n = (n < NEARBY_MAX) ? n : NEARBY_MAX;
        memcpy(_nearby, entries, copy_n * sizeof(Location));
        _nearby_loaded_count = copy_n;
        _nearby_loaded_for = owner_idx;
    }
}

// Resolves the in-flight queue's owner (tracked by ICAO/is-home, see above)
// back to a current _locations[] index at commit time. Returns -1 for Home
// (always resolvable), or -2 if a saved owner was removed while its fetch
// pass was still in flight (nothing to commit it into anymore).
static int nearby_resolve_owner_idx() {
    if (_nearby_queue_owner_is_home) return -1;
    for (int i = 0; i < _count; i++)
        if (strcmp(_locations[i].icao, _nearby_queue_owner_icao) == 0) return i;
    return -2;
}

// Builds and starts the static-DB scan queue for one owner (idx: -1 = Home).
// Assumes no other pass is currently active -- callers check
// _nearby_queue_active first (locations_nearby_set_enabled queues instead of
// calling this directly when one is already running).
static void nearby_start_scan(int idx) {
#if HAS_AIRPORTS_DB
    float lat, lon;
    const char *own_icao = nullptr;
    if (!nearby_owner_coords(idx, &lat, &lon, &own_icao)) return;

    // Widest configured radius preset -- same "fetch once, cover every zoom
    // level" reasoning as the primary location save itself, so this never
    // needs to re-fetch as the user zooms in/out afterward.
    float radius = (float)g_config.radius_presets[3];

    _nearby_queue_len = 0;
    for (int i = 0; i < AIRPORTS_DB_COUNT && _nearby_queue_len < NEARBY_MAX; i++) {
        const StaticAirport &ap = airports_db[i];
        if (!ap.large) continue; // large airports only -- see locations.h
        if (own_icao && strcmp(ap.icao, own_icao) == 0) continue; // skip the owner itself
        if (MapProjection::distance_nm(lat, lon, ap.lat, ap.lon) > radius) continue;
        strlcpy(_nearby_queue_icao[_nearby_queue_len], ap.icao, LOC_ICAO_LEN);
        _nearby_queue_len++;
    }
    _nearby_queue_pos = 0;
    _nearby_queue_owner_is_home = (idx == -1);
    _nearby_queue_owner_icao[0] = '\0';
    if (own_icao) strlcpy(_nearby_queue_owner_icao, own_icao, sizeof(_nearby_queue_owner_icao));
    _nearby_fetch_buf_count = 0;
    _nearby_queue_active = true;

    if (_nearby_queue_len == 0) {
        // Nothing nearby -- persist the (empty) result immediately so this
        // isn't re-scanned every time the toggle is flipped.
        _nearby_queue_active = false;
        nearby_commit(idx, nullptr, 0);
    }
#endif
}

static void nearby_queue_pending(int idx) {
    if (_nearby_pending_count >= NEARBY_PENDING_OWNERS_MAX) return; // best-effort cap, silently dropped
    int slot = _nearby_pending_count++;
    _nearby_pending_is_home[slot] = (idx == -1);
    _nearby_pending_icao[slot][0] = '\0';
    if (idx != -1) strlcpy(_nearby_pending_icao[slot], _locations[idx].icao, LOC_ICAO_LEN);
}

void locations_nearby_set_enabled(int idx, bool on) {
    if (idx == -1) {
        if (g_config.home_nearby_enabled == on) return;
        g_config.home_nearby_enabled = on;
        storage_save_config(g_config);
    } else {
        if (idx < 0 || idx >= _count) return;
        if (_locations[idx].nearby_enabled == on) return;
        _locations[idx].nearby_enabled = on;
        save_all();
    }

    // Turning off just stops drawing it -- cached data stays on disk so
    // turning back on later doesn't need to re-fetch.
    if (!on) return;
    if (locations_nearby_count(idx) > 0) return; // already cached, nothing to do

    if (_nearby_queue_active) {
        nearby_queue_pending(idx); // another owner's pass is already running -- wait our turn
    } else {
        nearby_start_scan(idx);
    }
}

void locations_nearby_poll() {
    if (!_nearby_queue_active || _nearby_queue_pos >= _nearby_queue_len) return;

    const char *icao = _nearby_queue_icao[_nearby_queue_pos++];
    if (_nearby_fetch_buf_count < NEARBY_MAX) {
        Location entry;
        char err[48];
        // Best-effort: one bad/rate-limited fetch shouldn't abort caching
        // the rest of the queue -- this is a background visual enhancement,
        // not a user-facing add flow with its own error surface to report to
        // (unlike locations_add_from_icao(), whose failure the picker shows
        // directly).
        if (fetch_airport_data(icao, entry, err, sizeof(err))) {
            _nearby_fetch_buf[_nearby_fetch_buf_count++] = entry;
        }
    }

    if (_nearby_queue_pos >= _nearby_queue_len) {
        _nearby_queue_active = false;
        int resolved = nearby_resolve_owner_idx();
        // resolved == -2: the owner was removed mid-fetch -- drop the
        // results, there's nothing left to commit them into.
        if (resolved != -2) nearby_commit(resolved, _nearby_fetch_buf, _nearby_fetch_buf_count);

        if (_nearby_pending_count > 0) {
            bool next_is_home = _nearby_pending_is_home[0];
            char next_icao[LOC_ICAO_LEN];
            strlcpy(next_icao, _nearby_pending_icao[0], sizeof(next_icao));
            for (int i = 1; i < _nearby_pending_count; i++) {
                _nearby_pending_is_home[i - 1] = _nearby_pending_is_home[i];
                strlcpy(_nearby_pending_icao[i - 1], _nearby_pending_icao[i], sizeof(_nearby_pending_icao[i - 1]));
            }
            _nearby_pending_count--;

            int next_idx = -2;
            if (next_is_home) {
                next_idx = -1;
            } else {
                for (int i = 0; i < _count; i++)
                    if (strcmp(_locations[i].icao, next_icao) == 0) { next_idx = i; break; }
            }
            if (next_idx != -2) nearby_start_scan(next_idx); // else: that owner was removed while pending -- just drop it
        }
    }
}

const Location* locations_nearby_get_active(int *count) {
    if (_nearby_loaded_for != _active_index) {
        _nearby_loaded_count = 0;
        if (locations_nearby_enabled(_active_index)) {
            char key[16];
            nearby_nvs_key(_active_index, key, sizeof(key));
            _prefs.begin("adsb_locs", true);
            size_t blob_len = _prefs.getBytesLength(key);
            if (blob_len >= sizeof(int32_t)) {
                uint8_t *buf = (uint8_t *)heap_caps_malloc(blob_len, MALLOC_CAP_SPIRAM);
                if (buf) {
                    size_t got = _prefs.getBytes(key, buf, blob_len);
                    size_t pos = 0;
                    int32_t cnt = 0;
                    if (pos + sizeof(cnt) <= got) {
                        memcpy(&cnt, buf, sizeof(cnt));
                        pos += sizeof(cnt);
                    }
                    int parsed = 0;
                    for (; parsed < cnt && parsed < NEARBY_MAX; parsed++) {
                        if (pos + sizeof(LocationHeader) > got) break; // truncated -- bail
                        LocationHeader hdr;
                        memcpy(&hdr, buf + pos, sizeof(hdr));
                        pos += sizeof(hdr);
                        if (hdr.runway_count < 0 || hdr.runway_count > MAX_RUNWAYS) break; // corrupt
                        size_t rwy_bytes = (size_t)hdr.runway_count * sizeof(LocRunway);
                        if (pos + rwy_bytes > got) break; // truncated

                        Location &e = _nearby[parsed];
                        strlcpy(e.icao, hdr.icao, sizeof(e.icao));
                        e.lat = hdr.lat;
                        e.lon = hdr.lon;
                        e.elevation_ft = hdr.elevation_ft;
                        e.runway_count = hdr.runway_count;
                        e.nearby_enabled = false;
                        e.nearby_count = 0;
                        memcpy(e.runways, buf + pos, rwy_bytes);
                        pos += rwy_bytes;
                    }
                    _nearby_loaded_count = parsed;
                    heap_caps_free(buf);
                }
            }
            _prefs.end();
        }
        _nearby_loaded_for = _active_index;
    }
    if (count) *count = _nearby_loaded_count;
    return _nearby;
}

void locations_factory_reset() {
    _prefs.begin("adsb_locs", false);
    _prefs.clear(); // takes every nb_<ICAO>/nb_home nearby-cache blob with it too, same namespace
    _prefs.end();
    Serial.println("Locations: adsb_locs namespace erased (factory reset)");
}
