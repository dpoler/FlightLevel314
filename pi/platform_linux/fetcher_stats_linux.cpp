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
#include <cstdio>
#include <cstring>
#include <string>
#include <dirent.h>
#include <sys/stat.h>

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

void fetcher_wake() {
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

// Link state from sysfs (the OS manages networking; this only observes it).
// Physical interfaces only -- they have a /device link; loopback, docker,
// veth, VPN tunnels don't. Ethernet wins if both are up. Used to be a
// constant NET_WIFI, so the status-bar icon was green even with no network.
NetType fetcher_connection_type() {
    DIR *d = opendir("/sys/class/net");
    if (!d) return NET_WIFI; // no sysfs (SDL build on macOS): unknown, assume up
    bool wifi = false, eth = false;
    while (dirent *ent = readdir(d)) {
        const char *name = ent->d_name;
        if (name[0] == '.' || strcmp(name, "lo") == 0) continue;
        const std::string base = std::string("/sys/class/net/") + name;
        struct stat st {};
        if (stat((base + "/device").c_str(), &st) != 0) continue; // virtual
        char state[16] = "";
        if (FILE *f = fopen((base + "/operstate").c_str(), "r")) {
            if (!fgets(state, sizeof(state), f)) state[0] = '\0';
            fclose(f);
        }
        if (strncmp(state, "up", 2) != 0) continue;
        if (stat((base + "/wireless").c_str(), &st) == 0 ||
            stat((base + "/phy80211").c_str(), &st) == 0)
            wifi = true;
        else
            eth = true;
    }
    closedir(d);
    return eth ? NET_ETHERNET : (wifi ? NET_WIFI : NET_NONE);
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
