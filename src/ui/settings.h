#pragma once
#include "lvgl.h"

// Initialize settings overlay (call once)
void settings_init(lv_obj_t *parent);

// Show/hide the settings overlay
void settings_show();
void settings_hide();

// Returns true if settings overlay is visible
bool settings_is_visible();
