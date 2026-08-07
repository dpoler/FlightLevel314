// Temporary placeholders for subsystems that haven't been ported to Linux
// yet. metar/enrichment are real network-backed features on ESP32 -- see
// project_pi_port memory for the migration order. airlines is ported
// (platform_linux/airlines_linux.cpp). Everything here gets
// replaced/deleted as the real thing gets ported.

#include "../src/data/metar.h"
#include "../src/data/enrichment.h"

volatile MetarStatus metar_status = METAR_IDLE;
char metar_raw[128] = {0};
char metar_station[8] = {0};

// enrichment.cpp does its own adsbdb.com/planespotters.net HTTP fetches
// via Arduino's HTTPClient -- not ported. detail_card.cpp falls back to
// the aircraft's own desc/owner_op fields when enrichment returns nothing,
// so the detail card still shows useful info without it.
void enrichment_fetch(const char *, const char *, void (*)(AircraftEnrichment *)) {}
