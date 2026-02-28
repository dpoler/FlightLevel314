#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"
#include "geo.h"

void map_view_init(lv_obj_t *parent, AircraftList *list);
void map_view_update();
