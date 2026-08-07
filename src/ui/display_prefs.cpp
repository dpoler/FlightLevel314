#include "display_prefs.h"
#include "views.h"
#include "../data/storage.h"

// Arrivals/Stats have no VIEW chip and never call these -- default to
// Map's slot rather than depend on undefined behavior if one somehow
// were called outside a Map/Radar context.
static int active_view_idx() {
    return (views_get_active_index() == VIEW_RADAR) ? 1 : 0;
}

bool trails_shown() { return g_config.view_trails_enabled[active_view_idx()]; }
void trails_toggle() {
    int i = active_view_idx();
    g_config.view_trails_enabled[i] = !g_config.view_trails_enabled[i];
    storage_save_config(g_config);
}

int trails_amount() { return g_config.view_trail_max_points[active_view_idx()]; }
void trails_amount_set(int val) {
    g_config.view_trail_max_points[active_view_idx()] = val;
}

bool tag_id_shown() { return g_config.view_show_tag_id[active_view_idx()]; }
void tag_id_toggle() {
    int i = active_view_idx();
    g_config.view_show_tag_id[i] = !g_config.view_show_tag_id[i];
    storage_save_config(g_config);
}

bool tag_data_shown() { return g_config.view_show_tag_data[active_view_idx()]; }
void tag_data_toggle() {
    int i = active_view_idx();
    g_config.view_show_tag_data[i] = !g_config.view_show_tag_data[i];
    storage_save_config(g_config);
}

bool tag_type_shown() { return g_config.view_show_tag_type[active_view_idx()]; }
void tag_type_toggle() {
    int i = active_view_idx();
    g_config.view_show_tag_type[i] = !g_config.view_show_tag_type[i];
    storage_save_config(g_config);
}

bool secondary_locations_shown() { return g_config.view_show_secondary_locations[active_view_idx()]; }
void secondary_locations_toggle() {
    int i = active_view_idx();
    g_config.view_show_secondary_locations[i] = !g_config.view_show_secondary_locations[i];
    storage_save_config(g_config);
}

bool map_basemap_shown() { return g_config.map_basemap_enabled; }
void map_basemap_toggle() {
    g_config.map_basemap_enabled = !g_config.map_basemap_enabled;
    storage_save_config(g_config);
}

int map_basemap_opa() { return g_config.map_basemap_opa; }
void map_basemap_opa_set(int pct) {
    if (pct < 10) pct = 10;
    if (pct > 100) pct = 100;
    g_config.map_basemap_opa = pct;
}

int map_basemap_style() { return g_config.map_basemap_style; }

const char *map_basemap_style_name() {
    switch (g_config.map_basemap_style) {
    case MAP_BASEMAP_STYLE_DARK_NOLABELS: return "Dark (no labels)";
    case MAP_BASEMAP_STYLE_SECTIONAL:     return "VFR Sectional";
    case MAP_BASEMAP_STYLE_DARK:
    default:                              return "Dark";
    }
}

void map_basemap_style_cycle() {
    g_config.map_basemap_style =
        (g_config.map_basemap_style + 1) % MAP_BASEMAP_STYLE_COUNT;
    storage_save_config(g_config);
}
