#pragma once

// Pi Map overlay: FAA AIS Designated Points (named fixes) + NAVAID systems
// near the map center. Free ArcGIS FeatureServer queries — no API key.
// https://ais-faa.opendata.arcgis.com/

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Non-blocking: if toggle off, clears in-memory set. Otherwise kicks a
// worker when center/radius move enough (or cache miss).
void fixes_request(float lat, float lon, float radius_nm);

// Call from LVGL thread (Map timer). Applies a finished worker snapshot.
bool fixes_poll_swap(void);

// Draw labeled markers into the Map canvas layer (above weather, under
// aircraft). No-op until a snapshot is ready.
void fixes_draw(lv_layer_t *layer,
                bool (*to_screen)(float lat, float lon, int *sx, int *sy),
                lv_color_t color, lv_opa_t opa);

int fixes_count(void);
void fixes_clear(void);

#ifdef __cplusplus
}
#endif
