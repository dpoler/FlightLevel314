// Temporary placeholders for subsystems that haven't been ported to Linux
// yet (locations/metar are real network-backed features on ESP32 -- see
// project_pi_port memory for the migration order; storage is real now,
// see pi/platform_linux/storage_linux.cpp). Also link-satisfying no-op
// stubs for the other three views' *_init() calls that src/ui/views.cpp
// references -- this milestone stands up Stats directly, without the full
// tileview manager, so those are never actually called, just linked.
// Everything here gets replaced/deleted as the real thing gets ported
// (locations.cpp in a later phase; the other views in task #7).

#include "../src/data/locations.h"
#include "../src/data/metar.h"
#include "../src/ui/map_view.h"
#include "../src/ui/radar_view.h"
#include "../src/ui/arrivals_view.h"
#include "../src/ui/detail_card.h"
#include "../src/ui/alerts.h"
#include "../src/ui/status_bar.h"

// Fixed fake coordinates (Seattle-Tacoma Intl) -- fine for this milestone
// since nothing here actually renders a map/runways against them, only
// Stats' CLOSEST-aircraft distance calc reads these.
int locations_active_index() { return 0; }
bool locations_get_active_coords(float *lat, float *lon, int *elevation_ft) {
    if (lat) *lat = 47.4502f;
    if (lon) *lon = -122.3088f;
    if (elevation_ft) *elevation_ft = 433;
    return true;
}

volatile MetarStatus metar_status = METAR_IDLE;
char metar_raw[128] = {0};
char metar_station[8] = {0};

void map_view_init(lv_obj_t *, AircraftList *) {}
void map_view_on_show() {}
void radar_view_init(lv_obj_t *, AircraftList *) {}
void arrivals_view_init(lv_obj_t *, AircraftList *) {}
void arrivals_view_on_show() {}
void detail_card_hide() {}
void alerts_dismiss() {}
void status_bar_set_active_dot(int) {}
