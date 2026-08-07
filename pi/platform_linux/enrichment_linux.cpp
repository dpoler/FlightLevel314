// Linux implementation of src/data/enrichment.h -- same two API sources as
// src/data/enrichment.cpp (adsbdb.com aircraft details, then
// planespotters.net photo metadata), plus a third step the ESP32 can't
// do: download + decode the JPEG thumbnail into RGB565 for the detail
// card. Pi has no PSRAM cache-coherency erratum, so real photos work.
//
// Threading: one detached std::thread per detail-card tap (Pi threads are
// cheap; ESP32's poll-from-existing-task constraint does not apply). UI
// callbacks are deferred through an LVGL timer, same as the ESP32 side.

#include "../../src/data/enrichment.h"
#include "../../src/platform/platform.h"
#include "lvgl.h"

#include <ArduinoJson.h>
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
#define PHOTO_MAX_W 260
#define PHOTO_MAX_H 150

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

bool http_get_bytes(const char *url, std::vector<uint8_t> &out) {
    // Thumbnails are ~30KB; leave headroom for larger "thumbnail_large".
    std::vector<char> buf(256 * 1024);
    size_t len = 0;
    if (!platform_http_get(url, buf.data(), buf.size(), &len) || len == 0) return false;
    out.assign((uint8_t *)buf.data(), (uint8_t *)buf.data() + len);
    return true;
}

uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Decode JPEG/PNG bytes to a downscaled RGB565 buffer owned by the caller.
bool decode_photo_rgb565(const uint8_t *bytes, size_t len,
                         uint8_t **out_rgb, uint16_t *out_w, uint16_t *out_h) {
    int w = 0, h = 0, n = 0;
    unsigned char *rgba = stbi_load_from_memory(bytes, (int)len, &w, &h, &n, 3);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) stbi_image_free(rgba);
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
        stbi_image_free(rgba);
        return false;
    }

    for (int y = 0; y < dh; y++) {
        int sy = (y * h) / dh;
        for (int x = 0; x < dw; x++) {
            int sx = (x * w) / dw;
            const unsigned char *p = rgba + ((size_t)sy * (size_t)w + (size_t)sx) * 3;
            uint16_t pix = rgb888_to_rgb565(p[0], p[1], p[2]);
            size_t off = ((size_t)y * (size_t)dw + (size_t)x) * 2;
            rgb565[off] = (uint8_t)(pix & 0xFF);
            rgb565[off + 1] = (uint8_t)(pix >> 8);
        }
    }
    stbi_image_free(rgba);
    *out_rgb = rgb565;
    *out_w = (uint16_t)dw;
    *out_h = (uint16_t)dh;
    return true;
}

void run_enrichment(std::string icao, std::string registration) {
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
    std::thread([icao, reg]() { run_enrichment(icao, reg); }).detach();
}

void enrichment_init() {
    lv_timer_create([](lv_timer_t *t) {
        if (_deferred_ready && _pending_callback && _deferred_entry) {
            _deferred_ready = false;
            _pending_callback((AircraftEnrichment *)_deferred_entry);
        }
    }, 100, nullptr);
}
