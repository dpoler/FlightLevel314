#include "lvgl.h"
#include "display.h"
#include "../src/platform/platform.h"
#include "../src/data/aircraft.h"
#include "../src/data/storage.h"
#include "../src/data/datasource.h"
#include "../src/ui/views.h"
#include "../src/ui/detail_card.h"
#include "../src/ui/range.h"
#include <chrono>
#include <thread>

// Milestone 5 of the Pi port (see project_pi_port memory / pi-port
// branch's plan): the real 4-view tileview (Map/Radar/Arrivals/Stats,
// swipeable via LVGL's native tileview scrolling) fed by real adsb.lol
// traffic, replacing milestone 4's Stats-only screen. No status bar yet
// (gear icon, nav dots, location/range chips) -- that's status_bar.cpp,
// not ported this round, see pi/app_stubs.cpp's comment -- so there's a
// blank ~48px strip at the top where it will eventually go. Active
// location is still the fixed KSEA stub in pi/app_stubs.cpp
// (locations.cpp isn't ported yet).

AircraftList aircraft_list;

// Read by map_view.cpp/radar_view.cpp to defer heavy rendering during a
// touch drag (views.h). Always false here for now -- SDL/libinput aren't
// wired to update it the way the ESP32 touch driver does in its own
// main.cpp, and it's a perf optimization rather than a correctness
// requirement, one the Pi's far larger compute budget needs far less than
// the ESP32-P4 does anyway.
volatile bool touch_active = false;

extern void pi_fetcher_stats_update(bool ok, uint32_t elapsed_ms);

static void fetch_loop() {
    RemoteApiDataSource src;
    while (true) {
        uint32_t t0 = platform_millis();
        bool ok = src.fetch(&aircraft_list);
        uint32_t elapsed = platform_millis() - t0;
        pi_fetcher_stats_update(ok, elapsed);
        platform_log("Fetch (%s): %s, %d aircraft tracked (%ums)\n",
                      src.name(), ok ? "OK" : "FAILED", aircraft_list.count, elapsed);
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
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    range_set_levels(g_config.radius_presets, 4);
    range_set_index(g_config.last_range_idx);

    views_init(screen, &aircraft_list);
    detail_card_init(screen, &aircraft_list);
    views_resume_last_view();

    while (true) {
        uint32_t sleep_ms = lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms ? sleep_ms : 5));
    }
}
