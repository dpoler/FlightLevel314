#pragma once

// ICAO airline code -> full name lookup, derived from a flight's callsign
// prefix (e.g. "UAL1234" → "UAL" → "United Airlines").
//
// Data: OpenTravelData `optd_airline_best_known_so_far.csv` (actively
// maintained, global ICAO 3-letter coverage). Fetched at boot over HTTPS.
// Pi-sized table — not the old hand-curated AirlinesCSV (max 250).
#define AIRLINES_MAX 1200

struct AirlineEntry {
    char code[4];      // ICAO airline code, e.g. "UAL"
    char name[40];     // e.g. "United Airlines"
    char callsign[16]; // telephony callsign if known (often empty from OPTD)
};

// Fetch and parse the airline table. Blocking network call — call once from
// a background thread at boot, not the UI thread. Returns true on success.
bool airlines_load();

// Look up the airline for a full flight callsign (e.g. "UAL1234") by
// extracting its 2-3 letter ICAO prefix. Returns nullptr if not found or
// airlines_load() hasn't completed yet.
const AirlineEntry *airline_lookup(const char *callsign);
