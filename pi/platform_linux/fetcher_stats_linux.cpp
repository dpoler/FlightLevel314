// map_view.cpp reads fetcher_get_stats()/fetcher_connection_type() for its
// connectivity overlay/stats readout -- everything else in src/data/
// fetcher.h (fetcher_init, fetcher_wifi_connected, fetcher_last_update,
// fetcher_request_immediate_fetch) is the ESP32/WiFi/C6-co-processor
// management API that pi/main.cpp doesn't use (it drives
// RemoteApiDataSource directly instead -- see project_pi_port memory), so
// those stay undefined here deliberately; nothing on Pi calls them.

#include "../../src/data/fetcher.h"
#include <mutex>
#include <cstring>

static std::mutex _mutex;
static FetcherStats _stats = {};

NetType fetcher_connection_type() {
    // The Pi's networking is managed by the OS (NetworkManager/systemd),
    // not this app -- there's no WiFi/Ethernet toggle to track the way
    // jc1060's fetcher.cpp does. Reporting NET_WIFI unconditionally is a
    // reasonable stand-in for map_view.cpp's "WiFi..."/"Ethernet..."
    // overlay text until real network-status detection is worth adding.
    return NET_WIFI;
}

const FetcherStats* fetcher_get_stats() {
    return &_stats;
}

// Called by pi/main.cpp's fetch_loop() after each RemoteApiDataSource
// fetch attempt.
void pi_fetcher_stats_update(bool ok, uint32_t elapsed_ms) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (ok) _stats.fetch_ok++;
    else _stats.fetch_fail++;
    _stats.last_fetch_ms = elapsed_ms;
    strncpy(_stats.ip_addr, "N/A", sizeof(_stats.ip_addr) - 1); // not tracked on Pi yet
}
