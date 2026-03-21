#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"
#include "../ui/geo.h"

void map_view_init(lv_obj_t *parent, AircraftList *list);
void map_view_update();
void map_view_center_on(float lat, float lon);
void map_view_track(const char *icao_hex);
