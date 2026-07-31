// Temporary placeholders for subsystems that haven't been ported to Linux
// yet (storage/locations/metar are real network/NVS-backed features on
// ESP32 -- see project_pi_port memory for the migration order). Also
// link-satisfying no-op stubs for the other three views' *_init() calls
// that src/ui/views.cpp references -- this milestone stands up Stats
// directly, without the full tileview manager, so those are never actually
// called, just linked. Everything here gets replaced/deleted as the real
// thing gets ported (storage.cpp + locations.cpp in a later phase; the
// other views in task #7).

#include "../src/data/storage.h"
#include "../src/data/locations.h"
#include "../src/data/metar.h"
#include "../src/ui/map_view.h"
#include "../src/ui/radar_view.h"
#include "../src/ui/arrivals_view.h"
#include "../src/ui/detail_card.h"
#include "../src/ui/alerts.h"
#include "../src/ui/status_bar.h"

UserConfig g_config;

UserConfig storage_load_config() {
    UserConfig cfg{};
    cfg.radius_nm = 20;
    cfg.radius_presets[0] = 5;
    cfg.radius_presets[1] = 10;
    cfg.radius_presets[2] = 20;
    cfg.radius_presets[3] = 50;
    return cfg;
}

void storage_save_config(const UserConfig &cfg) { g_config = cfg; }
void storage_factory_reset() { g_config = UserConfig{}; }

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
