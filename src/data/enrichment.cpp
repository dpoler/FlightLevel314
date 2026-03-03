#include "enrichment.h"
#include "http_mutex.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cstring>

#define MAX_CACHE 20

static AircraftEnrichment _cache[MAX_CACHE];
static char _cache_keys[MAX_CACHE][7];
static int _cache_count = 0;

static void (*_pending_callback)(AircraftEnrichment *) = nullptr;
static AircraftEnrichment *_pending_result = nullptr;
static volatile bool _task_running = false;

struct EnrichParams {
    char icao_hex[7];
    char callsign[9];
};

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
    _task_running = true;
    EnrichParams *params = (EnrichParams *)param;
    AircraftEnrichment *entry = get_or_create_cache_entry(params->icao_hex);
    entry->loading = true;

    // Stage 1: Full airport names from adsbdb callsign API
    if (params->callsign[0] && http_mutex_acquire(pdMS_TO_TICKS(8000))) {
        char url[128];
        snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", params->callsign);
        HTTPClient http;
        http.begin(url);
        http.setTimeout(5000);
        int code = http.GET();
        if (code == HTTP_CODE_OK) {
            JsonDocument doc;
            if (!deserializeJson(doc, http.getString())) {
                JsonObject route = doc["response"]["flightroute"];
                const char *orig_name = route["origin"]["name"] | "";
                const char *dest_name = route["destination"]["name"] | "";
                strlcpy(entry->origin_airport, orig_name, sizeof(entry->origin_airport));
                strlcpy(entry->destination_airport, dest_name, sizeof(entry->destination_airport));
                const char *airline = route["airline"]["name"] | "";
                if (airline[0]) strlcpy(entry->airline, airline, sizeof(entry->airline));
            }
        }
        http.end();
        http_mutex_release();
        if (_pending_callback) _pending_callback(entry);
    }

    // Stage 2: Aircraft details from adsbdb
    if (http_mutex_acquire(pdMS_TO_TICKS(8000))) {
        char url[128];
        snprintf(url, sizeof(url),
                 "https://api.adsbdb.com/v0/aircraft/%s", params->icao_hex);
        HTTPClient http;
        http.begin(url);
        http.setTimeout(5000);
        int code = http.GET();
        if (code == HTTP_CODE_OK) {
            JsonDocument doc;
            if (!deserializeJson(doc, http.getString())) {
                JsonObject ac = doc["response"]["aircraft"];
                strlcpy(entry->manufacturer, ac["manufacturer"] | "", sizeof(entry->manufacturer));
                strlcpy(entry->model, ac["type"] | "", sizeof(entry->model));
                strlcpy(entry->owner, ac["registered_owner"] | "", sizeof(entry->owner));
                strlcpy(entry->registered_country, ac["registered_owner_country_name"] | "",
                        sizeof(entry->registered_country));
                entry->engine_count = ac["engine_count"] | 0;
                strlcpy(entry->engine_type, ac["engine_type"] | "", sizeof(entry->engine_type));
                entry->year_built = ac["year_built"] | 0;
            }
        }
        http.end();
        http_mutex_release();
        if (_pending_callback) _pending_callback(entry);
    }

    // Stage 3: Photo from planespotters.net (brief delay for rate limit)
    vTaskDelay(pdMS_TO_TICKS(200));
    if (http_mutex_acquire(pdMS_TO_TICKS(8000))) {
        char url[128];
        snprintf(url, sizeof(url),
                 "https://api.planespotters.net/pub/photos/hex/%s", params->icao_hex);
        HTTPClient http;
        http.begin(url);
        http.setTimeout(5000);
        int code = http.GET();
        if (code == HTTP_CODE_OK) {
            JsonDocument doc;
            if (!deserializeJson(doc, http.getString())) {
                JsonArray photos = doc["photos"].as<JsonArray>();
                if (photos.size() > 0) {
                    const char *thumb = photos[0]["thumbnail_large"]["src"] | "";
                    strlcpy(entry->photo_url, thumb, sizeof(entry->photo_url));
                    const char *photog = photos[0]["photographer"] | "";
                    strlcpy(entry->photo_photographer, photog, sizeof(entry->photo_photographer));
                }
            }
        }
        http.end();
        http_mutex_release();
    }

    entry->loaded = true;
    entry->loading = false;

    if (_pending_callback) {
        _pending_callback(entry);
        _pending_callback = nullptr;
    }

    free(params);
    _task_running = false;
    vTaskDelete(nullptr);
}

void enrichment_fetch(const char *icao_hex, const char *registration,
                      const char *callsign,
                      void (*callback)(AircraftEnrichment *data)) {
    // Check cache first
    AircraftEnrichment *cached = enrichment_get_cached(icao_hex);
    if (cached) {
        callback(cached);
        return;
    }

    // Only one enrichment task at a time — concurrent HTTP exhausts WiFi TX buffers
    if (_task_running) {
        Serial.println("enrich: skipped (task already running)");
        return;
    }

    _pending_callback = callback;
    EnrichParams *params = (EnrichParams *)malloc(sizeof(EnrichParams));
    strlcpy(params->icao_hex, icao_hex, sizeof(params->icao_hex));
    strlcpy(params->callsign, callsign ? callsign : "", sizeof(params->callsign));
    xTaskCreatePinnedToCore(fetch_task, "enrich", 10240, params, 0, nullptr, 1);
}
