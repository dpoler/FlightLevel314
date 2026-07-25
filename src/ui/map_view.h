#pragma once
#include "lvgl.h"
#include "../data/aircraft.h"
#include "geo.h"

void map_view_init(lv_obj_t *parent, AircraftList *list);
void map_view_update();
void map_view_on_show();

// Center the map on a specific lat/lon and redraw
void map_view_center_on(float lat, float lon);

// Set which aircraft has the tracking circle (by ICAO hex, nullptr to clear)
void map_view_track(const char *icao_hex);

// Clear trails on this view only -- see status_bar.cpp's shared CLR chip
void map_view_clear_trails();

// True if a lat/lon currently projects onto Map's visible canvas.
// Deliberately NOT a circular radius_nm check -- Map draws (and lets you
// tap) aircraft beyond the bullseye ring, all the way to the rectangular
// canvas edges, using that extra screen space on purpose (unlike Radar,
// which clips to the circle to look like a radar). main.cpp's status-bar
// aircraft count uses this instead of a radius check when Map is the
// active/fallback view, so the count matches what Map actually draws.
bool map_view_aircraft_visible(float lat, float lon);
