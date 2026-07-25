#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"

// Show detail card for an aircraft (bottom sheet)
void detail_card_show(const Aircraft *ac);

// Hide the detail card
void detail_card_hide();

// Initialize the detail card (call once in setup). `list` is the live
// aircraft list -- the card's own update timer looks the shown aircraft up
// in it by icao_hex every tick, so DIST/BEARING/ALT/SPD/etc. stay live
// instead of freezing at whatever they were the moment the card was tapped
// open.
void detail_card_init(lv_obj_t *parent, AircraftList *list);

// Returns true if card is currently visible
bool detail_card_is_visible();

// Scroll the detail card content (for encoder navigation)
void detail_card_scroll(int delta);
