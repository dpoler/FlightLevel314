#include "lvgl.h"
#include "display.h"
#include "../src/platform/platform.h"
#include "../src/data/aircraft.h"
#include "../src/ui/stats.h"
#include "../src/ui/stats_view.h"
#include <chrono>
#include <thread>
#include <cstring>

// Milestone 3 of the Pi port (see project_pi_port memory / pi-port
// branch's plan): render one real, shared view -- Stats -- against fake
// aircraft data, proving src/data + src/ui code genuinely compiles and
// runs on Linux, not just the placeholder scene from milestone 2. Bypasses
// src/ui/views.cpp's tileview manager for now (that needs all four views
// ported -- see pi/app_stubs.cpp's comment) and calls stats_view_init()
// directly on a plain full-screen container instead.

AircraftList aircraft_list;

static void add_fake_aircraft(const char *icao, const char *callsign, const char *type,
                               float lat, float lon, int32_t alt, int16_t speed,
                               int16_t heading, bool military = false) {
    if (aircraft_list.count >= MAX_AIRCRAFT) return;
    Aircraft &ac = aircraft_list.aircraft[aircraft_list.count++];
    ac.clear();
    strncpy(ac.icao_hex, icao, sizeof(ac.icao_hex) - 1);
    strncpy(ac.callsign, callsign, sizeof(ac.callsign) - 1);
    strncpy(ac.type_code, type, sizeof(ac.type_code) - 1);
    ac.category[0] = 'A';
    ac.category[1] = military ? '7' : '3';
    ac.lat = lat;
    ac.lon = lon;
    ac.altitude = alt;
    ac.speed = speed;
    ac.heading = heading;
    ac.vert_rate_valid = true;
    ac.is_military = military;
    ac.last_seen = platform_millis();
    ac.stale_since = 0;
}

static void populate_fake_aircraft() {
    if (!aircraft_list.lock()) return;
    // Scattered around KSEA (pi/app_stubs.cpp's fake active-location coords).
    add_fake_aircraft("A0B1C2", "UAL123",  "B738", 47.55f, -122.30f, 8000,  250, 90);
    add_fake_aircraft("A0B1C3", "DAL456",  "A320", 47.40f, -122.20f, 15000, 320, 180);
    add_fake_aircraft("A0B1C4", "N12345",  "C172", 47.48f, -122.35f, 2500,  110, 270);
    add_fake_aircraft("A0B1C5", "SWA789",  "B737", 47.60f, -122.40f, 35000, 480, 45);
    add_fake_aircraft("A0B1C6", "ASA1000", "E75L", 47.35f, -122.25f, 500,   140, 0);
    add_fake_aircraft("AE1234", "REACH12", "C17",  47.30f, -122.45f, 22000, 350, 200, /*military=*/true);
    aircraft_list.unlock();
}

static uint32_t pi_tick_cb() {
    return platform_millis();
}

int main() {
    aircraft_list.init();
    populate_fake_aircraft();

    lv_init();
    lv_tick_set_cb(pi_tick_cb);

    lv_display_t *disp = pi_display_init();
    pi_input_init(disp);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    stats_init();
    stats_view_init(screen, &aircraft_list);

    while (true) {
        uint32_t sleep_ms = lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms ? sleep_ms : 5));
    }
}
