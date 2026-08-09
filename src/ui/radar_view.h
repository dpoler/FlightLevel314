#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"

void radar_view_init(lv_obj_t *parent, AircraftList *list);
void radar_view_update();

// Center the radar on a specific lat/lon (location picker / shared recenter).
void radar_view_center_on(float lat, float lon);

// Clear trails on this view only -- see status_bar.cpp's shared CLR chip
void radar_view_clear_trails();
