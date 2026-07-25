#pragma once
#include "aircraft.h"

// Connection type
enum NetType { NET_NONE, NET_ETHERNET, NET_WIFI };

// Initialize network and start the background fetch task
void fetcher_init(AircraftList *list);

// Returns true if any network is connected
bool fetcher_wifi_connected();

// Returns which network is currently active
NetType fetcher_connection_type();

// Returns the timestamp of the last successful fetch
uint32_t fetcher_last_update();

// Wakes the fetch loop immediately instead of leaving it to its normal
// ~20s cadence -- call whenever the active location changes (locations.cpp's
// locations_set_active() does) so switching locations doesn't leave the view
// showing stale (or no) data until the next scheduled tick. Safe to call
// before fetcher_init() (a no-op until the semaphore it signals exists).
void fetcher_request_immediate_fetch();

// Network stats
struct FetcherStats {
    uint32_t fetch_ok;
    uint32_t fetch_fail;
    uint32_t bytes_received;
    uint32_t last_fetch_ms;     // duration of last successful fetch
    char ip_addr[16];
};
const FetcherStats* fetcher_get_stats();
