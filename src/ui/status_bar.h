#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"

// Create the status bar at the top of the screen (30px tall)
lv_obj_t *status_bar_create(lv_obj_t *parent);

// Update status bar with current data
void status_bar_update(bool wifi_connected, int aircraft_count, uint32_t last_update_ms);

// Update the active view dot indicator
void status_bar_set_active_dot(int view_index);
