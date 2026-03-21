#pragma once

#include "../data/aircraft.h"

enum NavState {
    NAV_VIEW_BROWSING,      // Rotate = switch views, Select = enter aircraft selection
    NAV_AIRCRAFT_SELECTED,  // Rotate = cycle aircraft, Select = open detail, Back = browse
    NAV_DETAIL_CARD_OPEN,   // Rotate = scroll card, Back = dismiss card
};

// Initialize navigation controller (call after views_init and encoder_input_init)
void input_nav_init(AircraftList *list);

// Poll encoder/buttons and update UI (call from LVGL timer, ~20ms)
void input_nav_tick();

// Query selection state (used by view draw callbacks for highlighting)
int  input_nav_get_selected_index();   // -1 if none
bool input_nav_is_selecting();         // true when in AIRCRAFT_SELECTED state
NavState input_nav_get_state();
