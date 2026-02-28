#pragma once
#include <cstdint>

struct AircraftEnrichment {
    char photo_url[256];
    char airline[32];
    char origin_airport[48];      // "JFK - John F. Kennedy Intl"
    char destination_airport[48]; // "LAX - Los Angeles Intl"
    char manufacturer[32];
    char model[32];
    char engine_type[24];
    uint8_t engine_count;
    bool loaded;
    bool loading;
};

// Fetch enrichment data in background. Calls callback when done.
void enrichment_fetch(const char *icao_hex, const char *registration,
                      void (*callback)(AircraftEnrichment *data));

// Get cached enrichment (returns nullptr if not yet fetched)
AircraftEnrichment *enrichment_get_cached(const char *icao_hex);
