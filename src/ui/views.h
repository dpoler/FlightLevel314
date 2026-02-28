#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"

// View indices
#define VIEW_MAP 0
#define VIEW_RADAR 1
#define VIEW_ARRIVALS 2

// Initialize the tileview with all view containers
void views_init(lv_obj_t *parent, AircraftList *list);

// Get the container object for a specific view (for adding child widgets)
lv_obj_t *views_get_tile(int view_index);
