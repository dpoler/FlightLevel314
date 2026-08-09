// map_view.cpp and (now) status_bar.cpp read fetcher_get_stats()/
// fetcher_connection_type()/fetcher_wifi_connected()/fetcher_last_update()
// -- fetcher_init() on Pi only stores the AircraftList pointer so
// fetcher_request_immediate_fetch() can clear it on location switch
// (matching ESP32 fetcher.cpp). Pi drives RemoteApiDataSource from
// main.cpp instead of the ESP32 WiFi/C6 fetch task.
//
// fetcher_request_immediate_fetch() wakes the fetch loop and clears the
// aircraft list. Without the clear, a full list (MAX_AIRCRAFT, often after
// a busy KJFK fetch) cannot accept aircraft for the new location until
// ghosts age out -- Map recenters immediately so old traffic is off-screen
// and the bar reads 0/N until the next successful merge. locations_set_active()
// calls this on every location switch.

#include "../../src/data/fetcher.h"
#include "../../src/data/aircraft.h"
#include "../../src/platform/platform.h"
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstring>

static std::mutex _mutex;
static FetcherStats _stats = {};
static bool _last_fetch_ok = false;
static uint32_t _last_success_ms = 0;

static std::condition_variable _fetch_cv;
static bool _fetch_wake = false;
static AircraftList *_aircraft_list = nullptr;

void fetcher_init(AircraftList *list) {
    _aircraft_list = list;
}

void fetcher_request_immediate_fetch() {
    // Location switch is a hard cut, not a 30s ghost fade -- same rationale
    // as ESP32 fetcher.cpp. Clear before waking the fetch loop so the next
    // merge starts from an empty list and can accept the new location's
    // aircraft even when the previous site had filled MAX_AIRCRAFT.
    if (_aircraft_list && _aircraft_list->lock(50)) {
        _aircraft_list->count = 0;
        _aircraft_list->unlock();
    }
    std::lock_guard<std::mutex> lock(_mutex);
    _fetch_wake = true;
    _fetch_cv.notify_all();
}

// Called by pi/main.cpp's fetch_loop() in place of a flat
// std::this_thread::sleep_for(seconds(20)) -- returns early if
// fetcher_request_immediate_fetch() is called mid-wait.
void pi_wait_for_next_fetch(int seconds) {
    std::unique_lock<std::mutex> lock(_mutex);
    _fetch_cv.wait_for(lock, std::chrono::seconds(seconds), [] { return _fetch_wake; });
    _fetch_wake = false;
}

NetType fetcher_connection_type() {
    // The Pi's networking is managed by the OS (NetworkManager/systemd),
    // not this app -- there's no WiFi/Ethernet toggle to track the way
    // jc1060's fetcher.cpp does. Reporting NET_WIFI unconditionally is a
    // reasonable stand-in for map_view.cpp's "WiFi..."/"Ethernet..."
    // overlay text until real network-status detection is worth adding.
    return NET_WIFI;
}

bool fetcher_wifi_connected() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _last_fetch_ok;
}

uint32_t fetcher_last_update() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _last_success_ms;
}

const FetcherStats* fetcher_get_stats() {
    return &_stats;
}

// Called by pi/main.cpp's fetch_loop() after each RemoteApiDataSource
// fetch attempt.
void pi_fetcher_stats_update(bool ok, uint32_t elapsed_ms) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (ok) {
        _stats.fetch_ok++;
        _last_success_ms = platform_millis();
    } else {
        _stats.fetch_fail++;
    }
    _last_fetch_ok = ok;
    _stats.last_fetch_ms = elapsed_ms;
    strncpy(_stats.ip_addr, "N/A", sizeof(_stats.ip_addr) - 1); // not tracked on Pi yet
}
