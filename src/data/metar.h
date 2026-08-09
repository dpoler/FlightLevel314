#pragma once

// METAR lookup via aviationweather.gov's public Data API (no key).
// For a saved airport: query that ICAO first, then fall back to nearest
// reporting station within METAR_RANGE_NM. For a waypoint (no ICAO):
// nearest station within range only. Driven by metar_poll().

enum MetarStatus {
    METAR_IDLE,       // no active location, or nothing fetched yet
    METAR_FETCHING,
    METAR_OK,
    METAR_NO_STATION, // fetch succeeded, nothing reporting within range
    METAR_ERROR,      // network/HTTP/parse failure -- metar_raw keeps its last good value
};

#define METAR_RAW_LEN 256
#define METAR_RANGE_NM 50.0f

extern volatile MetarStatus metar_status;
extern char metar_raw[METAR_RAW_LEN]; // winning station's raw METAR text, verbatim
extern char metar_station[8]; // which ICAO metar_raw actually came from

// Internally rate-limited (~15 min, ~4x the routine hourly METAR cycle) plus
// immediate re-fetch on active-location change. Safe to call every tick.
void metar_poll();
