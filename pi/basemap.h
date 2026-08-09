#pragma once

// Pi-only live/cached basemap under Map view (Carto dark / FAA VFR sectional).
// ESP32 keeps optional compile-in static_map_data.h; this path is Linux-only.

#include "lvgl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call when active location or range changes (or on init). Kicks a background
// fetch if the cached basemap doesn't already match. Non-blocking.
// canvas_h should be LCD_V_RES: Map draws in absolute screen Y (same space
// as geo_center_y), so a shorter buffer leaves a solid strip at the bottom.
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

// FAA VFR sectional coverage (CONUS / AK / HI / PR rough bounds).
bool basemap_sectional_covered(float lat, float lon);

// Non-null when the active style cannot be shown for the current request
// (e.g. sectional outside the US). LVGL-thread safe; string is static.
const char *basemap_unavailable_message(void);

// True while a network/build is in progress for the current request
// (not shown for instant disk-cache hits). *out_pct is 0..100 best-effort
// progress; pass nullptr if unused. LVGL-thread safe.
bool basemap_updating(int *out_pct);

// Delete all on-disk basemap mosaics and drop the in-memory front/inbox
// buffers. LVGL-thread only. Next basemap_request() will refetch.
// Returns the number of cache files removed.
int basemap_cache_clear(void);

#ifdef __cplusplus
}
#endif
