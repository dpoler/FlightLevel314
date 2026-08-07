#pragma once

// Pi-only RainViewer precip radar overlay under Map (on top of basemap).
// Free personal/educational API — no key. See https://www.rainviewer.com/api.html

#include "lvgl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Same geometry args as basemap_request(). Non-blocking; worker fetches the
// latest RainViewer frame + tiles and warps into MapProjection space.
void weather_request(float lat, float lon, float radius_nm, int canvas_w, int canvas_h,
                     int geo_center_y, int bullseye_r_px);

bool weather_poll_swap(void);
void weather_draw(lv_layer_t *layer);
bool weather_ready(void);

// Drop in-memory + on-disk weather mosaics. Returns files removed.
int weather_cache_clear(void);

#ifdef __cplusplus
}
#endif
