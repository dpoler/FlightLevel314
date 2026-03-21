#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"

void radar_view_init(lv_obj_t *parent, AircraftList *list);
void radar_view_update();
