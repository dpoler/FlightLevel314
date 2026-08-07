#pragma once

// Pi-only live/cached dark OSM basemap under Map view.
// ESP32 keeps optional compile-in static_map_data.h; this path is Linux-only.

#include "lvgl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call when active location or range changes (or on init). Kicks a background
// fetch if the cached basemap doesn't already match. Non-blocking.
void basemap_request(float lat, float lon, float radius_nm, int canvas_w, int canvas_h,
                     int geo_center_y, int bullseye_r_px);

// LVGL-thread only. Installs a newly-built basemap (if any) into the buffer
// that basemap_draw reads. Call between frames (e.g. map timer), never from
// a worker thread. Returns true if a new basemap was installed.
bool basemap_poll_swap(void);

// Draw the current basemap (if ready) under map content. Safe from the
// LVGL draw callback. No-op until the first successful fetch completes.
void basemap_draw(lv_layer_t *layer);

// True once at least one basemap buffer is ready to blit.
bool basemap_ready(void);

#ifdef __cplusplus
}
#endif
