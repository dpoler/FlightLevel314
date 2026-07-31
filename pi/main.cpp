#include "lvgl.h"
#include "display.h"
#include "../src/platform/platform.h"
#include "../src/data/aircraft.h"
#include "../src/data/storage.h"
#include "../src/data/datasource.h"
#include "../src/ui/stats.h"
#include "../src/ui/stats_view.h"
#include <chrono>
#include <thread>

// Milestone 4 of the Pi port (see project_pi_port memory / pi-port
// branch's plan): real adsb.lol traffic via RemoteApiDataSource
// (pi/platform_linux/datasource_remote.cpp), replacing milestone 3's fake
// aircraft. Still bypasses src/ui/views.cpp's tileview manager (see
// pi/app_stubs.cpp's comment) -- Stats is stood up directly on a
// full-screen container. Active location is still the fixed KSEA stub in
// pi/app_stubs.cpp (locations.cpp itself isn't ported yet), so real
// traffic shows up centered there.

AircraftList aircraft_list;

static void fetch_loop() {
    RemoteApiDataSource src;
    while (true) {
        bool ok = src.fetch(&aircraft_list);
        platform_log("Fetch (%s): %s, %d aircraft tracked\n",
                      src.name(), ok ? "OK" : "FAILED", aircraft_list.count);
        std::this_thread::sleep_for(std::chrono::seconds(20));
    }
}

static uint32_t pi_tick_cb() {
    return platform_millis();
}

int main() {
    aircraft_list.init();

    // Real round-trip through pi/platform_linux/storage_linux.cpp --
    // proves the JSON-file config storage actually works, not just links.
    g_config = storage_load_config();
    storage_save_config(g_config);

    std::thread fetch_thread(fetch_loop);
    fetch_thread.detach();

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
