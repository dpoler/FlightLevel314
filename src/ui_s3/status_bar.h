#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"

lv_obj_t *status_bar_create(lv_obj_t *parent);
void status_bar_update(bool wifi_connected, int aircraft_count, uint32_t last_update_ms);
void status_bar_set_active_dot(int view_index);
void status_bar_set_gear_callback(lv_event_cb_t cb);
void status_bar_set_auto_indicator(bool visible);
