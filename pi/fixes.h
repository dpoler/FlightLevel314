#pragma once

// Pi Map overlay: FAA named fixes near the map center.
// - AIS DesignatedPoints (enroute reporting / RNAV fixes on charts)
// - AIS NAVAIDSystem (VORs etc.), skipping idents that duplicate a nearby
//   airport (LAX vs KLAX, SLI vs KSLI)
// - CIFP terminal waypoints (PC) for the active airport — approach/SID/STAR
//   fixes such as GRASP at KDEN (not present in DesignatedPoints)

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// airport_icao: active location ICAO when it is an airport (e.g. "KDEN"),
// or "" / nullptr for waypoints — CIFP terminal WPs only load when set.
void fixes_request(float lat, float lon, float radius_nm, const char *airport_icao);

bool fixes_poll_swap(void);

void fixes_draw(lv_layer_t *layer,
                bool (*to_screen)(float lat, float lon, int *sx, int *sy),
                lv_color_t color, lv_opa_t opa);

int fixes_count(void);
void fixes_clear(void);

#ifdef __cplusplus
}
#endif
