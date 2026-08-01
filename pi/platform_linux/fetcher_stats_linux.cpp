// map_view.cpp and (now) status_bar.cpp read fetcher_get_stats()/
// fetcher_connection_type()/fetcher_wifi_connected()/fetcher_last_update()
// -- fetcher_init() is the ESP32/WiFi/C6-co-processor management API that
// pi/main.cpp doesn't use (it drives RemoteApiDataSource directly instead --
// see project_pi_port memory), so that one stays undefined here
// deliberately; nothing on Pi calls it.
//
// fetcher_request_immediate_fetch() DOES need a real implementation now
// that locations_linux.cpp's locations_set_active() calls it (switching
// location should refresh data immediately, not wait up to ~20s for the
// next scheduled tick, same as ESP32). Implemented as a condition variable
// pi_wait_for_next_fetch() (called from main.cpp's fetch_loop() instead of a
// flat sleep_for) waits on, so a wake-up call cuts the wait short instead of
// needing a signal/interrupt mechanism.

#include "../../src/data/fetcher.h"
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

void fetcher_request_immediate_fetch() {
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
