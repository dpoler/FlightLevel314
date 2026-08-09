#pragma once

// US major-airport D-ATIS via datis.clowd.io (no key). Coverage is ~76
// fields with type=combined or separate arr/dep. Europe and most other
// regions have no comparable free API — callers should surface
// ATIS_UNAVAILABLE with a clear "US major airports only" message.

enum AtisStatus {
    ATIS_IDLE,         // no active location
    ATIS_FETCHING,
    ATIS_OK,           // at least one of combined / arr / dep is populated
    ATIS_UNAVAILABLE,  // airport known but no D-ATIS (or outside US majors)
    ATIS_ERROR,         // network/parse failure — keep last good text if any
};

#define ATIS_TEXT_LEN 2048

extern volatile AtisStatus atis_status;
extern char atis_airport[8];     // ICAO the text is for (may differ for waypoints)
extern char atis_combined[ATIS_TEXT_LEN];
extern char atis_arr[ATIS_TEXT_LEN];
extern char atis_dep[ATIS_TEXT_LEN];
extern bool atis_split;          // true when arr and/or dep present (not combined-only)

// Rate-limited; safe to call often from a background loop. Fetches on
// active-location change or every ~15 minutes (same cadence as METAR).
void atis_poll();
