#pragma once

// Global shared range control
// Zoom levels: 150, 100, 50, 20, 5, 1 nautical miles
#define RANGE_NUM_LEVELS 6

float range_get_nm();
int range_get_index();
void range_cycle();         // advance to next level, wrapping
const char* range_label();  // e.g. "50nm"
