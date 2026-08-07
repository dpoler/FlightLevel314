#pragma once
#include <cstdint>

struct AircraftEnrichment {
    char photo_url[256];
    char photo_photographer[48];
    char manufacturer[32];
    char model[48];
    char registered_country[24];
    char engine_type[24];
    uint8_t engine_count;
    uint16_t year_built;
    bool loaded;
    bool loading;

    // Decoded thumbnail for detail-card display (RGB565, little-endian).
    // Filled on Pi after the planespotters JPEG download; always null on
    // ESP32 (PSRAM image path is broken there — see README Known Issues).
    // Owned by the enrichment cache entry; freed when the slot is reused.
    uint8_t *photo_rgb565;
    uint16_t photo_w;
    uint16_t photo_h;
};

// Initialize enrichment system (installs LVGL timer for deferred callbacks)
void enrichment_init();

// Fetch enrichment data in background. Calls callback progressively as data arrives.
// Callback is always called from LVGL context (safe to update UI).
void enrichment_fetch(const char *icao_hex, const char *registration,
                      void (*callback)(AircraftEnrichment *data));

// Drives the in-progress fetch, one stage per call -- call from an existing
// task's loop (location_poll_task in fetcher.cpp), never spawn a dedicated
// task for this (see project_p4_heap_constraints memory).
// On Pi this is a no-op (fetch runs on its own thread).
void enrichment_poll();

// Get cached enrichment (returns nullptr if not yet fetched)
AircraftEnrichment *enrichment_get_cached(const char *icao_hex);
