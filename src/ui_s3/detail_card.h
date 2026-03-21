#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"

void detail_card_show(const Aircraft *ac);
void detail_card_hide();
void detail_card_init(lv_obj_t *parent);
bool detail_card_is_visible();
