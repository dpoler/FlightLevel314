#pragma once
#include "aircraft.h"

// Initialize WiFi and start the background fetch task
void fetcher_init(AircraftList *list);

// Returns true if WiFi is connected
bool fetcher_wifi_connected();

// Returns the timestamp of the last successful fetch
uint32_t fetcher_last_update();
