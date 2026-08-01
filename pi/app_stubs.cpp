// Temporary placeholders for subsystems that haven't been ported to Linux
// yet. locations/metar/airlines/enrichment are real network-backed
// features on ESP32 -- see project_pi_port memory for the migration
// order. location_picker.cpp (Home/saved-airport picker chip) isn't
// ported this round either. Everything here gets replaced/deleted as the
// real thing gets ported.

#include "../src/data/locations.h"
#include "../src/data/metar.h"
#include "../src/data/airlines.h"
#include "../src/data/enrichment.h"
#include "../src/ui/location_picker.h"

// Fixed fake coordinates (Seattle-Tacoma Intl) -- fine for now since
// locations.cpp (Home + saved airports, airportdb.io runway fetch) isn't
// ported yet. Real traffic shows up centered here until it is.
int locations_active_index() { return 0; }
bool locations_get_active_coords(float *lat, float *lon, int *elevation_ft) {
    if (lat) *lat = 47.4502f;
    if (lon) *lon = -122.3088f;
    if (elevation_ft) *elevation_ft = 433;
    return true;
}

// No saved locations yet -- Map/Radar's "other saved airports" runway
// overlay (locations_get/locations_count/locations_nearby_get_active) has
// nothing to draw beyond the one active-location marker above.
int locations_count() { return 0; }
const Location *locations_get(int) { return nullptr; }
const Location *locations_nearby_get_active(int *count) {
    if (count) *count = 0;
    return nullptr;
}

volatile MetarStatus metar_status = METAR_IDLE;
char metar_raw[128] = {0};
char metar_station[8] = {0};

// airlines.cpp/enrichment.cpp both do their own adsbdb.com/planespotters.net
// HTTP fetches via Arduino's HTTPClient -- not ported. detail_card.cpp
// falls back to the aircraft's own desc/owner_op fields when these return
// nothing, so the detail card still shows useful info without them.
const AirlineEntry *airline_lookup(const char *) { return nullptr; }
void enrichment_fetch(const char *, const char *, void (*)(AircraftEnrichment *)) {}

// Called by view_menu.cpp (now real) so only one status-bar popover is
// open at a time -- no-op until location_picker.cpp itself exists to
// have something to close.
void location_picker_close() {}
