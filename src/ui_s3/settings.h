#pragma once
#include "lvgl.h"
#include "../data/storage.h"

typedef void (*settings_changed_cb_t)(const UserConfig *cfg);

void settings_init(lv_obj_t *parent);
void settings_show();
void settings_hide();
bool settings_is_visible();
void settings_set_change_callback(settings_changed_cb_t cb);
