#include "enrichment.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cstring>

#define MAX_CACHE 20

static AircraftEnrichment _cache[MAX_CACHE];
static char _cache_keys[MAX_CACHE][7];
static int _cache_count = 0;

static void (*_pending_callback)(AircraftEnrichment *) = nullptr;
static AircraftEnrichment *_pending_result = nullptr;

AircraftEnrichment *enrichment_get_cached(const char *icao_hex) {
    for (int i = 0; i < _cache_count; i++) {
        if (strcmp(_cache_keys[i], icao_hex) == 0 && _cache[i].loaded) {
            return &_cache[i];
        }
    }
    return nullptr;
}

static AircraftEnrichment *get_or_create_cache_entry(const char *icao_hex) {
    // Check existing
    for (int i = 0; i < _cache_count; i++) {
        if (strcmp(_cache_keys[i], icao_hex) == 0) return &_cache[i];
    }
    // Create new (evict oldest if full)
    int idx = _cache_count < MAX_CACHE ? _cache_count++ : 0;
    memset(&_cache[idx], 0, sizeof(AircraftEnrichment));
    strlcpy(_cache_keys[idx], icao_hex, 7);
    return &_cache[idx];
}

static void fetch_task(void *param) {
    char *icao_hex = (char *)param;
    AircraftEnrichment *entry = get_or_create_cache_entry(icao_hex);
    entry->loading = true;

    // Fetch route info from adsb.lol
    {
        char url[128];
        snprintf(url, sizeof(url), "https://api.adsb.lol/v2/hex/%s", icao_hex);
        HTTPClient http;
        http.begin(url);
        http.setTimeout(10000);
        int code = http.GET();
        if (code == HTTP_CODE_OK) {
            JsonDocument doc;
            if (!deserializeJson(doc, http.getString())) {
                JsonArray ac = doc["ac"].as<JsonArray>();
                if (ac.size() > 0) {
                    JsonObject obj = ac[0];
                    strlcpy(entry->airline, obj["ownOp"] | "", sizeof(entry->airline));
                    // Route: "KJFK-KLAX" format
                    const char *route = obj["route"] | "";
                    if (strlen(route) > 0) {
                        char route_buf[96];
                        strlcpy(route_buf, route, sizeof(route_buf));
                        char *dash = strchr(route_buf, '-');
                        if (dash) {
                            *dash = '\0';
                            strlcpy(entry->origin_airport, route_buf, sizeof(entry->origin_airport));
                            strlcpy(entry->destination_airport, dash + 1, sizeof(entry->destination_airport));
                        }
                    }
                }
            }
        }
        http.end();
    }

    // Fetch photo from planespotters.net
    {
        char url[128];
        snprintf(url, sizeof(url),
                 "https://api.planespotters.net/pub/photos/hex/%s", icao_hex);
        HTTPClient http;
        http.begin(url);
        http.setTimeout(10000);
        int code = http.GET();
        if (code == HTTP_CODE_OK) {
            JsonDocument doc;
            if (!deserializeJson(doc, http.getString())) {
                JsonArray photos = doc["photos"].as<JsonArray>();
                if (photos.size() > 0) {
                    const char *thumb = photos[0]["thumbnail_large"]["src"] | "";
                    strlcpy(entry->photo_url, thumb, sizeof(entry->photo_url));
                }
            }
        }
        http.end();
    }

    entry->loaded = true;
    entry->loading = false;

    if (_pending_callback) {
        _pending_callback(entry);
        _pending_callback = nullptr;
    }

    free(icao_hex);
    vTaskDelete(nullptr);
}

void enrichment_fetch(const char *icao_hex, const char *registration,
                      void (*callback)(AircraftEnrichment *data)) {
    // Check cache first
    AircraftEnrichment *cached = enrichment_get_cached(icao_hex);
    if (cached) {
        callback(cached);
        return;
    }

    _pending_callback = callback;
    char *hex_copy = strdup(icao_hex);
    xTaskCreatePinnedToCore(fetch_task, "enrich", 8192, hex_copy, 0, nullptr, 1);
}
