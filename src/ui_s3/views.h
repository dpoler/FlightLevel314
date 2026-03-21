#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"

#define VIEW_MAP 0
#define VIEW_RADAR 1
#define VIEW_ARRIVALS 2
#define VIEW_STATS 3

void views_init(lv_obj_t *parent, AircraftList *list);
lv_obj_t *views_get_tile(int view_index);
int views_get_active_index();
lv_obj_t *views_get_tileview();
void views_pause_cycle();

extern volatile bool touch_active;
