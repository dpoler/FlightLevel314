#pragma once
#include <cstdint>
#include "lvgl.h"

#define TILE_PX 256

// Initialize tile cache (SD card cache + PSRAM LRU + background fetch task)
void tile_cache_init();

// Get LVGL draw buffer for tile, or nullptr if not yet cached.
// Missing tiles are queued for background fetch.
lv_draw_buf_t *tile_cache_get(int z, int x, int y);

// Flush pending fetch requests (call when zoom changes)
void tile_cache_flush_queue();

// OSM tile math
int osm_lon_to_x(float lon, int z);
int osm_lat_to_y(float lat, int z);
float osm_x_to_lon(int x, int z);
float osm_y_to_lat(int y, int z);
int osm_zoom_for_radius(float radius_nm, int screen_h, float center_lat);
