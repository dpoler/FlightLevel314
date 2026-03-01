#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"

void stats_view_init(lv_obj_t *parent, AircraftList *list);
void stats_view_update();
