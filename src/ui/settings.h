#pragma once
#include "lvgl.h"
#include "../data/storage.h"

// Callback type — called when settings are saved
typedef void (*settings_changed_cb_t)(const UserConfig *cfg);

// Initialize settings overlay (call once)
void settings_init(lv_obj_t *parent);

// Show/hide the settings overlay
void settings_show();
void settings_hide();

// Returns true if settings overlay is visible
bool settings_is_visible();

// Register a callback for when settings change
void settings_set_change_callback(settings_changed_cb_t cb);
