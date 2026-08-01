// Temporary placeholders for subsystems that haven't been ported to Linux
// yet. metar/airlines/enrichment are real network-backed features on
// ESP32 -- see project_pi_port memory for the migration order. Everything
// here gets replaced/deleted as the real thing gets ported.

#include "../src/data/metar.h"
#include "../src/data/airlines.h"
#include "../src/data/enrichment.h"

volatile MetarStatus metar_status = METAR_IDLE;
char metar_raw[128] = {0};
char metar_station[8] = {0};

// airlines.cpp/enrichment.cpp both do their own adsbdb.com/planespotters.net
// HTTP fetches via Arduino's HTTPClient -- not ported. detail_card.cpp
// falls back to the aircraft's own desc/owner_op fields when these return
// nothing, so the detail card still shows useful info without them.
const AirlineEntry *airline_lookup(const char *) { return nullptr; }
void enrichment_fetch(const char *, const char *, void (*)(AircraftEnrichment *)) {}
