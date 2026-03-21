#include "input_nav.h"
#include "views.h"
#include "detail_card.h"
#include "alerts.h"
#include "map_view.h"
#include "../hal/encoder_input.h"
#include <cstring>

static AircraftList *_list = nullptr;
static NavState _state = NAV_VIEW_BROWSING;
static int _selected_idx = -1;  // Index into visible aircraft list

void input_nav_init(AircraftList *list) {
    _list = list;
    _state = NAV_VIEW_BROWSING;
    _selected_idx = -1;
}

// Get the number of aircraft currently available
static int get_aircraft_count() {
    if (!_list) return 0;
    return _list->count;
}

void input_nav_tick() {
    if (!_list) return;

    int delta = encoder_get_delta();
    bool select = encoder_select_pressed();
    bool back = encoder_back_pressed();

    // If neither input, skip
    if (delta == 0 && !select && !back) return;

    switch (_state) {
        case NAV_VIEW_BROWSING: {
            if (back || (delta != 0)) {
                // Rotate or back = cycle views
                int d = (delta != 0) ? delta : 1;
                int current = views_get_active_index();
                int next = (current + d) % 4;
                if (next < 0) next += 4;
                lv_tileview_set_tile_by_index(views_get_tileview(), next, 0, LV_ANIM_OFF);
                views_pause_cycle();
            }
            if (select) {
                // Enter aircraft selection mode
                int count = get_aircraft_count();
                if (count > 0) {
                    _selected_idx = 0;
                    _state = NAV_AIRCRAFT_SELECTED;
                    views_pause_cycle();

                    // Center map on selected aircraft
                    if (_list->lock(pdMS_TO_TICKS(10))) {
                        if (_selected_idx < _list->count) {
                            Aircraft &ac = _list->aircraft[_selected_idx];
                            map_view_center_on(ac.lat, ac.lon);
                            map_view_track(ac.icao_hex);
                        }
                        _list->unlock();
                    }
                }
            }
            break;
        }

        case NAV_AIRCRAFT_SELECTED: {
            if (back) {
                // Back to browsing
                _selected_idx = -1;
                _state = NAV_VIEW_BROWSING;
                map_view_track(nullptr);  // Stop tracking
            } else if (delta != 0) {
                // Cycle through aircraft
                int count = get_aircraft_count();
                if (count > 0) {
                    _selected_idx = (_selected_idx + delta) % count;
                    if (_selected_idx < 0) _selected_idx += count;

                    // Center on newly selected aircraft
                    if (_list->lock(pdMS_TO_TICKS(10))) {
                        if (_selected_idx < _list->count) {
                            Aircraft &ac = _list->aircraft[_selected_idx];
                            map_view_center_on(ac.lat, ac.lon);
                            map_view_track(ac.icao_hex);
                        }
                        _list->unlock();
                    }
                }
            } else if (select) {
                // Open detail card for selected aircraft
                if (_list->lock(pdMS_TO_TICKS(10))) {
                    if (_selected_idx >= 0 && _selected_idx < _list->count) {
                        Aircraft ac_copy = _list->aircraft[_selected_idx];
                        _list->unlock();
                        detail_card_show(&ac_copy);
                        _state = NAV_DETAIL_CARD_OPEN;
                    } else {
                        _list->unlock();
                    }
                }
            }
            break;
        }

        case NAV_DETAIL_CARD_OPEN: {
            if (back) {
                // Dismiss detail card, back to aircraft selection
                detail_card_hide();
                _state = NAV_AIRCRAFT_SELECTED;
            } else if (delta != 0) {
                // Scroll detail card
                detail_card_scroll(delta);
            } else if (select) {
                // Dismiss on select too
                detail_card_hide();
                _state = NAV_AIRCRAFT_SELECTED;
            }
            break;
        }
    }
}

int input_nav_get_selected_index() {
    return _selected_idx;
}

bool input_nav_is_selecting() {
    return _state == NAV_AIRCRAFT_SELECTED || _state == NAV_DETAIL_CARD_OPEN;
}

NavState input_nav_get_state() {
    return _state;
}
