// Linux implementation of src/data/enrichment.h -- same two API sources as
// src/data/enrichment.cpp (adsbdb.com aircraft details, then
// planespotters.net photo metadata), plus Pi-only steps: download + decode
// the JPEG thumbnail into RGB565 for the detail card, and (optional)
// AeroDataBox flight status for live origin/destination. Pi has no PSRAM
// cache-coherency erratum, so real photos work.
//
// Threading: one detached std::thread per detail-card tap (Pi threads are
// cheap; ESP32's poll-from-existing-task constraint does not apply). UI
// callbacks are deferred through an LVGL timer, same as the ESP32 side.

#include "../../src/data/enrichment.h"
#include "../../src/data/storage.h"
#include "../../src/platform/platform.h"
#include "lvgl.h"

#include <ArduinoJson.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// stb_image implementation lives in basemap.cpp -- only the declarations here.
#include "../third_party/stb_image.h"

#define MAX_CACHE 20
// Sized to detail_card.cpp's PHOTO_SLOT on 1280-wide layouts (400x220).
// Mild shrink from thumbnail_large (~500x280) with bilinear.
#define PHOTO_MAX_W 400
#define PHOTO_MAX_H 220

namespace {

std::mutex _mutex;
AircraftEnrichment _cache[MAX_CACHE];
char _cache_keys[MAX_CACHE][7];
int _cache_count = 0;
bool _busy = false;

void (*_pending_callback)(AircraftEnrichment *) = nullptr;
volatile AircraftEnrichment *_deferred_entry = nullptr;
volatile bool _deferred_ready = false;

void free_photo(AircraftEnrichment *e) {
    free(e->photo_rgb565);
    e->photo_rgb565 = nullptr;
    e->photo_w = 0;
    e->photo_h = 0;
}

AircraftEnrichment *get_or_create_cache_entry(const char *icao_hex) {
    for (int i = 0; i < _cache_count; i++) {
        if (strcmp(_cache_keys[i], icao_hex) == 0) return &_cache[i];
    }
    int idx = _cache_count < MAX_CACHE ? _cache_count++ : 0;
    free_photo(&_cache[idx]);
    memset(&_cache[idx], 0, sizeof(AircraftEnrichment));
    strlcpy(_cache_keys[idx], icao_hex, 7);
    return &_cache[idx];
}

void notify_callback(AircraftEnrichment *entry) {
    _deferred_entry = entry;
    _deferred_ready = true;
}

bool http_get_json(const char *url, JsonDocument &doc) {
    std::vector<char> buf(48 * 1024);
    size_t len = 0;
    if (!platform_http_get(url, buf.data(), buf.size(), &len)) return false;
    return deserializeJson(doc, buf.data(), len) == DeserializationError::Ok;
}

// RapidAPI headers for AeroDataBox. key_hdr must remain alive for the call.
void adbox_headers(const char *key, char *key_hdr, size_t key_hdr_sz,
                   const char *out[4]) {
    snprintf(key_hdr, key_hdr_sz, "x-rapidapi-key: %s", key);
    out[0] = key_hdr;
    out[1] = "x-rapidapi-host: aerodatabox.p.rapidapi.com";
    out[2] = "Accept: application/json";
    out[3] = nullptr;
}

std::string trim_ws(const char *s) {
    if (!s) return {};
    while (*s && isspace((unsigned char)*s)) ++s;
    std::string out(s);
    while (!out.empty() && isspace((unsigned char)out.back())) out.pop_back();
    return out;
}

bool parse_adbox_route(JsonDocument &doc, char *origin, size_t origin_sz,
                       char *dest, size_t dest_sz) {
    // Response is a JSON array of FlightContract, or occasionally a single object.
    JsonArrayConst arr = doc.as<JsonArrayConst>();
    JsonObjectConst flight;
    if (!arr.isNull() && arr.size() > 0) {
        flight = arr[0].as<JsonObjectConst>();
    } else {
        flight = doc.as<JsonObjectConst>();
    }
    if (flight.isNull()) return false;

    const char *o = flight["departure"]["airport"]["icao"] | "";
    const char *d = flight["arrival"]["airport"]["icao"] | "";
    if (!o[0] && !d[0]) {
        o = flight["departure"]["airport"]["iata"] | "";
        d = flight["arrival"]["airport"]["iata"] | "";
    }
    if (!o[0] && !d[0]) return false;
    strlcpy(origin, o, origin_sz);
    strlcpy(dest, d, dest_sz);
    return true;
}

// Live nearest flight by icao24, then callsign. dateLocalRole=Both.
bool fetch_adbox_route(const char *key, const char *icao_hex, const char *callsign,
                       char *origin, size_t origin_sz, char *dest, size_t dest_sz) {
    origin[0] = '\0';
    dest[0] = '\0';
    if (!key || !key[0]) return false;

    char key_hdr[128];
    const char *hdrs[4];
    adbox_headers(key, key_hdr, sizeof(key_hdr), hdrs);

    auto try_url = [&](const char *url) -> bool {
        std::vector<char> buf(64 * 1024);
        size_t len = 0;
        long status = 0;
        if (!platform_http_get_ex(url, buf.data(), buf.size(), &len, &status, hdrs)) {
            return false;
        }
        // 401/403 = bad key; 204/404 = authenticated but no flight today.
        if (status == 401 || status == 403) {
            platform_log("[Enrich] AeroDataBox auth failed (http=%ld)\n", status);
            return false;
        }
        if (status < 200 || status >= 300 || len == 0) return false;
        JsonDocument doc;
        if (deserializeJson(doc, buf.data(), len) != DeserializationError::Ok) return false;
        return parse_adbox_route(doc, origin, origin_sz, dest, dest_sz);
    };

    char url[256];
    if (icao_hex && icao_hex[0]) {
        snprintf(url, sizeof(url),
                 "https://aerodatabox.p.rapidapi.com/flights/icao24/%s"
                 "?withAircraftImage=false&withLocation=false&dateLocalRole=Both",
                 icao_hex);
        if (try_url(url)) return true;
    }

    std::string cs = trim_ws(callsign);
    if (!cs.empty()) {
        // Callsigns are alphanumeric; strip anything unsafe for the path.
        std::string clean;
        for (char c : cs) {
            if (isalnum((unsigned char)c)) clean.push_back((char)toupper((unsigned char)c));
        }
        if (!clean.empty()) {
            snprintf(url, sizeof(url),
                     "https://aerodatabox.p.rapidapi.com/flights/callsign/%s"
                     "?withAircraftImage=false&withLocation=false&dateLocalRole=Both",
                     clean.c_str());
            if (try_url(url)) return true;
        }
    }
    return false;
}

// Validate RapidAPI key with a cheap airport lookup (not a flight search).
bool validate_adbox_key(const char *key, char *err, size_t err_size) {
    auto fail = [&](const char *msg) {
        if (err && err_size) strlcpy(err, msg, err_size);
        return false;
    };
    if (!key || !key[0]) return fail("no key set");

    char key_hdr[128];
    const char *hdrs[4];
    adbox_headers(key, key_hdr, sizeof(key_hdr), hdrs);

    char buf[4096];
    size_t len = 0;
    long status = 0;
    const char *url = "https://aerodatabox.p.rapidapi.com/airports/icao/KJFK";
    if (!platform_http_get_ex(url, buf, sizeof(buf), &len, &status, hdrs)) {
        return fail("network error");
    }
    if (status == 401 || status == 403) return fail("invalid key");
    if (status < 200 || status >= 300) {
        char msg[48];
        snprintf(msg, sizeof(msg), "http %ld", status);
        return fail(msg);
    }
    return true;
}

std::mutex _verify_mutex;
bool _verify_result_ready = false;
bool _verify_result_ok = false;
char _verify_result_err[48] = {};

bool http_get_bytes(const char *url, std::vector<uint8_t> &out) {
    // thumbnail_large is typically ~30-80KB.
    std::vector<char> buf(512 * 1024);
    size_t len = 0;
    if (!platform_http_get(url, buf.data(), buf.size(), &len) || len == 0) return false;
    out.assign((uint8_t *)buf.data(), (uint8_t *)buf.data() + len);
    return true;
}

uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline uint8_t sample_channel(const unsigned char *rgb, int w, int h,
                                     float x, float y, int c) {
    // Bilinear sample of channel c in an RGB888 buffer.
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > (float)(w - 1)) x = (float)(w - 1);
    if (y > (float)(h - 1)) y = (float)(h - 1);
    int x0 = (int)x;
    int y0 = (int)y;
    int x1 = x0 + 1 < w ? x0 + 1 : x0;
    int y1 = y0 + 1 < h ? y0 + 1 : y0;
    float fx = x - (float)x0;
    float fy = y - (float)y0;
    const unsigned char *p00 = rgb + ((size_t)y0 * (size_t)w + (size_t)x0) * 3 + c;
    const unsigned char *p10 = rgb + ((size_t)y0 * (size_t)w + (size_t)x1) * 3 + c;
    const unsigned char *p01 = rgb + ((size_t)y1 * (size_t)w + (size_t)x0) * 3 + c;
    const unsigned char *p11 = rgb + ((size_t)y1 * (size_t)w + (size_t)x1) * 3 + c;
    float v = (1 - fx) * (1 - fy) * (float)(*p00)
            + fx * (1 - fy) * (float)(*p10)
            + (1 - fx) * fy * (float)(*p01)
            + fx * fy * (float)(*p11);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)(v + 0.5f);
}

// Decode JPEG/PNG to RGB565. Keeps near-native size; bilinear when scaling.
bool decode_photo_rgb565(const uint8_t *bytes, size_t len,
                         uint8_t **out_rgb, uint16_t *out_w, uint16_t *out_h) {
    int w = 0, h = 0, n = 0;
    unsigned char *rgb = stbi_load_from_memory(bytes, (int)len, &w, &h, &n, 3);
    if (!rgb || w <= 0 || h <= 0) {
        if (rgb) stbi_image_free(rgb);
        return false;
    }

    int dw = w, dh = h;
    if (dw > PHOTO_MAX_W || dh > PHOTO_MAX_H) {
        float sx = (float)PHOTO_MAX_W / (float)dw;
        float sy = (float)PHOTO_MAX_H / (float)dh;
        float s = sx < sy ? sx : sy;
        dw = (int)(dw * s + 0.5f);
        dh = (int)(dh * s + 0.5f);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }

    size_t bytes_out = (size_t)dw * (size_t)dh * 2;
    uint8_t *rgb565 = (uint8_t *)malloc(bytes_out);
    if (!rgb565) {
        stbi_image_free(rgb);
        return false;
    }

    // Ordered 2x2 dither softens RGB565 banding (reads as grain on sky/fuselage).
    static const float dither[2][2] = {
        { 0.0f / 4.0f, 2.0f / 4.0f },
        { 3.0f / 4.0f, 1.0f / 4.0f },
    };

    const bool scale = (dw != w) || (dh != h);
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            uint8_t r, g, b;
            if (!scale) {
                const unsigned char *p = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3;
                r = p[0]; g = p[1]; b = p[2];
            } else {
                float sx = ((float)x + 0.5f) * (float)w / (float)dw - 0.5f;
                float sy = ((float)y + 0.5f) * (float)h / (float)dh - 0.5f;
                r = sample_channel(rgb, w, h, sx, sy, 0);
                g = sample_channel(rgb, w, h, sx, sy, 1);
                b = sample_channel(rgb, w, h, sx, sy, 2);
            }
            float d = dither[y & 1][x & 1];
            int ri = (int)r + (int)(d * 7.0f);
            int gi = (int)g + (int)(d * 3.0f);
            int bi = (int)b + (int)(d * 7.0f);
            if (ri > 255) ri = 255;
            if (gi > 255) gi = 255;
            if (bi > 255) bi = 255;
            uint16_t pix = rgb888_to_rgb565((uint8_t)ri, (uint8_t)gi, (uint8_t)bi);
            size_t off = ((size_t)y * (size_t)dw + (size_t)x) * 2;
            rgb565[off] = (uint8_t)(pix & 0xFF);
            rgb565[off + 1] = (uint8_t)(pix >> 8);
        }
    }
    stbi_image_free(rgb);
    *out_rgb = rgb565;
    *out_w = (uint16_t)dw;
    *out_h = (uint16_t)dh;
    return true;
}

void run_enrichment(std::string icao, std::string registration, std::string callsign) {
    AircraftEnrichment *entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        entry = get_or_create_cache_entry(icao.c_str());
        entry->loading = true;
    }

    // --- Stage 1: adsbdb aircraft details ---
    {
        char url[128];
        snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/aircraft/%s", icao.c_str());
        JsonDocument doc;
        if (http_get_json(url, doc)) {
            JsonObjectConst ac = doc["response"]["aircraft"];
            std::lock_guard<std::mutex> lock(_mutex);
            strlcpy(entry->manufacturer, ac["manufacturer"] | "", sizeof(entry->manufacturer));
            strlcpy(entry->model, ac["type"] | "", sizeof(entry->model));
            strlcpy(entry->registered_country, ac["registered_owner_country_name"] | "",
                    sizeof(entry->registered_country));
            entry->engine_count = ac["engine_count"] | 0;
            strlcpy(entry->engine_type, ac["engine_type"] | "", sizeof(entry->engine_type));
            entry->year_built = ac["year_built"] | 0;
        } else {
            platform_log("[Enrich] stage1 (adsbdb) failed for %s\n", icao.c_str());
        }
        notify_callback(entry);
    }

    // --- Stage 2: planespotters photo metadata ---
    {
        char url[160];
        snprintf(url, sizeof(url), "https://api.planespotters.net/pub/photos/hex/%s", icao.c_str());
        JsonDocument doc;
        bool got = http_get_json(url, doc);
        JsonArrayConst photos = got ? doc["photos"].as<JsonArrayConst>() : JsonArrayConst{};
        if ((!got || photos.size() == 0) && !registration.empty()) {
            // Fallback: some airframes are keyed by registration only.
            snprintf(url, sizeof(url), "https://api.planespotters.net/pub/photos/reg/%s",
                     registration.c_str());
            got = http_get_json(url, doc);
            photos = got ? doc["photos"].as<JsonArrayConst>() : JsonArrayConst{};
        }
        if (got && photos.size() > 0) {
            std::lock_guard<std::mutex> lock(_mutex);
            strlcpy(entry->photo_url, photos[0]["thumbnail_large"]["src"] | "",
                    sizeof(entry->photo_url));
            if (!entry->photo_url[0]) {
                strlcpy(entry->photo_url, photos[0]["thumbnail"]["src"] | "",
                        sizeof(entry->photo_url));
            }
            strlcpy(entry->photo_photographer, photos[0]["photographer"] | "",
                    sizeof(entry->photo_photographer));
        } else {
            platform_log("[Enrich] stage2 (planespotters) no photos for %s\n", icao.c_str());
        }
        notify_callback(entry);
    }

    // --- Stage 3: download + decode thumbnail (Pi-only opportunity) ---
    char photo_url[256] = {};
    {
        std::lock_guard<std::mutex> lock(_mutex);
        strlcpy(photo_url, entry->photo_url, sizeof(photo_url));
    }
    if (photo_url[0]) {
        std::vector<uint8_t> jpeg;
        if (http_get_bytes(photo_url, jpeg)) {
            uint8_t *rgb = nullptr;
            uint16_t pw = 0, ph = 0;
            if (decode_photo_rgb565(jpeg.data(), jpeg.size(), &rgb, &pw, &ph)) {
                std::lock_guard<std::mutex> lock(_mutex);
                free_photo(entry);
                entry->photo_rgb565 = rgb;
                entry->photo_w = pw;
                entry->photo_h = ph;
                platform_log("[Enrich] photo %ux%u for %s\n", pw, ph, icao.c_str());
            } else {
                platform_log("[Enrich] photo decode failed for %s\n", icao.c_str());
            }
        } else {
            platform_log("[Enrich] photo download failed for %s\n", icao.c_str());
        }
        notify_callback(entry);
    }

    // --- Stage 4: AeroDataBox live origin/destination (optional) ---
    char adbox_key[sizeof(g_config.aerodatabox_key)] = {};
    bool adbox_on = false;
    {
        // Snapshot under no lock -- g_config is only written from UI/settings.
        adbox_on = g_config.aerodatabox_enabled && g_config.aerodatabox_key[0];
        if (adbox_on) strlcpy(adbox_key, g_config.aerodatabox_key, sizeof(adbox_key));
    }
    if (adbox_on) {
        char origin[8] = {}, dest[8] = {};
        if (fetch_adbox_route(adbox_key, icao.c_str(), callsign.c_str(),
                              origin, sizeof(origin), dest, sizeof(dest))) {
            std::lock_guard<std::mutex> lock(_mutex);
            strlcpy(entry->origin_icao, origin, sizeof(entry->origin_icao));
            strlcpy(entry->dest_icao, dest, sizeof(entry->dest_icao));
            platform_log("[Enrich] route %s → %s for %s\n", origin, dest, icao.c_str());
        } else {
            platform_log("[Enrich] AeroDataBox no route for %s\n", icao.c_str());
        }
        notify_callback(entry);
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        entry->loaded = true;
        entry->loading = false;
        _busy = false;
    }
    notify_callback(entry);
}

} // namespace

AircraftEnrichment *enrichment_get_cached(const char *icao_hex) {
    std::lock_guard<std::mutex> lock(_mutex);
    for (int i = 0; i < _cache_count; i++) {
        if (strcmp(_cache_keys[i], icao_hex) == 0 && _cache[i].loaded) {
            return &_cache[i];
        }
    }
    return nullptr;
}

void enrichment_poll() {
    // No-op -- Pi drives stages on a dedicated thread from enrichment_fetch().
}

void enrichment_fetch(const char *icao_hex, const char *registration,
                      const char *callsign,
                      void (*callback)(AircraftEnrichment *data)) {
    if (!icao_hex || !icao_hex[0]) return;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (int i = 0; i < _cache_count; i++) {
            if (strcmp(_cache_keys[i], icao_hex) == 0 && _cache[i].loaded) {
                _pending_callback = callback;
                notify_callback(&_cache[i]);
                return;
            }
        }
        if (_busy) {
            platform_log("enrich: skipped (fetch already in progress)\n");
            return;
        }
        _busy = true;
        _pending_callback = callback;
        _deferred_ready = false;
    }

    std::string icao(icao_hex);
    std::string reg(registration ? registration : "");
    std::string cs(callsign ? callsign : "");
    std::thread([icao, reg, cs]() { run_enrichment(icao, reg, cs); }).detach();
}

void enrichment_clear_cache() {
    std::lock_guard<std::mutex> lock(_mutex);
    for (int i = 0; i < _cache_count; i++) {
        free_photo(&_cache[i]);
        memset(&_cache[i], 0, sizeof(AircraftEnrichment));
        _cache_keys[i][0] = '\0';
    }
    _cache_count = 0;
}

void aerodatabox_request_verify() {
    {
        std::lock_guard<std::mutex> lock(_verify_mutex);
        _verify_result_ready = false;
    }
    std::thread([]() {
        char err[48] = {};
        bool ok = validate_adbox_key(g_config.aerodatabox_key, err, sizeof(err));
        std::lock_guard<std::mutex> lock(_verify_mutex);
        _verify_result_ok = ok;
        strlcpy(_verify_result_err, err, sizeof(_verify_result_err));
        _verify_result_ready = true;
    }).detach();
}

bool aerodatabox_verify_result(bool *ok, char *err, size_t err_size) {
    std::lock_guard<std::mutex> lock(_verify_mutex);
    if (!_verify_result_ready) return false;
    if (ok) *ok = _verify_result_ok;
    if (err && err_size) strlcpy(err, _verify_result_err, err_size);
    _verify_result_ready = false;
    return true;
}

void enrichment_init() {
    lv_timer_create([](lv_timer_t *t) {
        if (_deferred_ready && _pending_callback && _deferred_entry) {
            _deferred_ready = false;
            _pending_callback((AircraftEnrichment *)_deferred_entry);
        }
    }, 100, nullptr);
}
