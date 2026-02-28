#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"

// Show detail card for an aircraft (bottom sheet)
void detail_card_show(const Aircraft *ac);

// Hide the detail card
void detail_card_hide();

// Initialize the detail card (call once in setup)
void detail_card_init(lv_obj_t *parent);

// Returns true if card is currently visible
bool detail_card_is_visible();
