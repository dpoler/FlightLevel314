#pragma once
#include <cstdint>
#include <cstddef>

struct AircraftEnrichment {
    char photo_url[256];
    char photo_photographer[48];
    char manufacturer[32];
    char model[48];
    char registered_country[24];
    char engine_type[24];
    uint8_t engine_count;
    uint16_t year_built;
    // Live flight origin/destination ICAO (AeroDataBox when enabled).
    // Empty when unavailable / service off.
    char origin_icao[8];
    char dest_icao[8];
    bool route_checked; // true once AeroDataBox was attempted (or skipped as off)
    // Callsign used for the last route lookup (alnum upper). Empty if none.
    char route_callsign[16];
    // platform_millis() when route_checked was set (for short O/D TTL).
    uint32_t route_checked_ms;
    bool loaded;
    bool loading;

    // Decoded thumbnail for detail-card display (RGB565, little-endian).
    // Filled after the planespotters JPEG download.
    // Owned by the enrichment cache entry; freed when the slot is reused.
    uint8_t *photo_rgb565;
    uint16_t photo_w;
    uint16_t photo_h;
};

// Initialize enrichment system (installs LVGL timer for deferred callbacks)
void enrichment_init();

// Fetch enrichment data in background. Calls callback progressively as data arrives.
// Callback is always called from LVGL context (safe to update UI).
// callsign/category/type_code/is_military feed AeroDataBox eligibility
// (skip small GA / HELI / MIL to save API quota).
void enrichment_fetch(const char *icao_hex, const char *registration,
                      const char *callsign,
                      const char *category, const char *type_code, bool is_military,
                      void (*callback)(AircraftEnrichment *data));

// True when AeroDataBox O/D is worth a query: commercial / large (A3–A6)
// fixed-wing. Skips military, helicopters, and light/small GA (A0–A2, B/C).
bool enrichment_route_eligible(const char *callsign, const char *category,
                               const char *type_code, bool is_military);

// On Pi this is a no-op (fetch runs on its own thread). Kept for call-site
// compatibility with the historical poll loop.
void enrichment_poll();

// Get cached enrichment (returns nullptr if not yet fetched)
AircraftEnrichment *enrichment_get_cached(const char *icao_hex);

// Drop all cached enrichment entries (e.g. after toggling AeroDataBox on).
void enrichment_clear_cache();

// Async RapidAPI key check against AeroDataBox (airport lookup). Same
// request/result shape as locations_request_verify_token().
void aerodatabox_request_verify();
bool aerodatabox_verify_result(bool *ok, char *err, size_t err_size);

// Local monthly usage (persisted in UserConfig). Marketplace remaining
// units are not available via the API key — only this local counter and
// HTTP 429 / soft-limit auto-disable.
void aerodatabox_usage_snapshot(int *yyyymm, int *count, int *soft_limit, bool *rate_limited);
// Clear the sticky rate-limit flag (caller should also re-enable if desired).
void aerodatabox_clear_rate_limit();
