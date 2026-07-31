#include "lvgl.h"
#include "display.h"
#include "../src/platform/platform.h"
#include "../src/data/aircraft.h"
#include "../src/data/storage.h"
#include "../src/data/datasource.h"
#include "../src/ui/views.h"
#include "../src/ui/detail_card.h"
#include "../src/ui/range.h"
#include "../src/ui/settings.h"
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

    // Force LVGL to resolve the tileview's 4 tiles' lv_pct()-based
    // positions into real pixel coordinates right now. Without this, the
    // 4th tile (Stats) was found sitting at (0,0) -- on top of Map --
    // until *something* eventually triggered a layout pass; the
    // tileview's own scrollable-content width got computed (and
    // effectively cached) against that wrong position first, capping
    // real scrolling at 3 tiles' worth (confirmed via lv_obj_get_scroll_right()
    // reading 2560 instead of 3840 before this call, 3840 after). The
    // ESP32 main.cpp never hits this: it creates a lot more (status bar,
    // location picker, alerts, settings, OTA overlay) between views_init()
    // and its first real frame, incidentally forcing enough layout work
    // to resolve this as a side effect. This main.cpp doesn't do enough
    // of that on its own, so it needs an explicit nudge.
    lv_obj_update_layout(views_get_tileview());

    // Minimal stand-in for status_bar.cpp's gear icon (not ported this
    // round -- see this file's top comment): a small always-on-top button
    // so Settings is actually reachable without a real status bar yet.
    settings_init(screen);
    lv_obj_t *gear_btn = lv_button_create(screen);
    lv_obj_set_size(gear_btn, 40, 40);
    lv_obj_align(gear_btn, LV_ALIGN_TOP_RIGHT, -6, 4);
    lv_obj_set_style_bg_color(gear_btn, lv_color_hex(0x1a1a3a), 0);
    lv_obj_set_style_bg_opa(gear_btn, LV_OPA_70, 0);
    lv_obj_set_style_radius(gear_btn, 20, 0);
    lv_obj_t *gear_lbl = lv_label_create(gear_btn);
    lv_label_set_text(gear_lbl, LV_SYMBOL_SETTINGS);
    lv_obj_center(gear_lbl);
    lv_obj_add_event_cb(gear_btn, [](lv_event_t *) { settings_show(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_move_foreground(gear_btn);

    // A fixed ~1ms cadence, NOT lv_timer_handler()'s own returned "next
    // timer due" hint -- that value can be large (hundreds of ms) when
    // nothing's animating, and sleeping for it starves SDL's event pump
    // (lv_sdl_window.c's internal sdl_event_handler() timer, which drains
    // the OS mouse-motion queue via SDL_PollEvent() -- only serviced when
    // lv_timer_handler() actually runs). Real-world symptom this caused:
    // a slow drag across the whole session would queue many motion
    // events during one long sleep, then get drained in one burst with
    // near-zero elapsed time between them once the loop finally woke --
    // LVGL's scroll/gesture code reads that as a huge instantaneous
    // velocity and flings straight to the tileview's edge, so dragging
    // could only ever land on the first or last tile, never one in
    // between. ESP32's main.cpp loop() never hits this: it already
    // ignores lv_timer_handler()'s return value and just runs a fixed
    // 1ms vTaskDelay every iteration, unconditionally -- matched here.
    while (true) {
        lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
