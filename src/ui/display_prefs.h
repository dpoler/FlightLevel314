#pragma once

// Per-view (Map vs Radar) trail and tag-field visibility, controlled by
// the status bar's VIEW menu (view_menu.cpp). Each accessor resolves
// which of Map/Radar it applies to via views_get_active_index()
// internally -- every caller (map_view.cpp/radar_view.cpp's own draw
// loops, or the VIEW popover while it's open) is only ever asking on
// behalf of whichever view is currently active. Persisted per-view in
// g_config (storage.h) -- toggles/switch taps are a single discrete
// action, so an immediate NVS write on each is safe (same reasoning as
// filter_toggle() in filters.cpp); trails_amount_set() is the exception,
// matching the trail-length slider's drag-vs-commit split (the caller
// persists explicitly on release, not on every drag tick).

bool trails_shown();
void trails_toggle();
int trails_amount();
void trails_amount_set(int val);

// Flight number, falling back to registration then ICAO hex if no
// callsign -- never shows registration alongside an existing callsign.
bool tag_id_shown();
void tag_id_toggle();

// Altitude + speed + climb/descend arrow.
bool tag_data_shown();
void tag_data_toggle();

// Aircraft type / operator.
bool tag_type_shown();
void tag_type_toggle();

// Other saved/static airports + the HOME-elsewhere marker.
bool secondary_locations_shown();
void secondary_locations_toggle();

// Pi Map basemap (VIEW menu, Map only). Opacity is 10-100 percent and is
// stored per style (Dark / Dark no-labels / Sectional / Light each remember
// their own). map_basemap_opa_set() does not persist — caller saves on slider
// release (same split as trails_amount_set).
// Styles: 0=Carto dark, 1=Carto dark no labels, 2=FAA VFR sectional, 3=Carto light.
#define MAP_BASEMAP_STYLE_DARK 0
#define MAP_BASEMAP_STYLE_DARK_NOLABELS 1
#define MAP_BASEMAP_STYLE_SECTIONAL 2
#define MAP_BASEMAP_STYLE_LIGHT 3
#define MAP_BASEMAP_STYLE_COUNT 4

bool map_basemap_shown();
void map_basemap_toggle();
int map_basemap_opa();              // opacity for the active style
void map_basemap_opa_set(int pct);  // sets opacity for the active style
int map_basemap_style();
const char *map_basemap_style_name();
void map_basemap_style_cycle();
