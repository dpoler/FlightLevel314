#pragma once
#include <cmath>
#include "lvgl.h"

// Convert nautical miles to approximate degrees of latitude
#define NM_TO_DEG_LAT (1.0f / 60.0f)

struct MapProjection {
    float center_lat;
    float center_lon;
    float radius_nm;    // visible radius in nautical miles
    int screen_w;
    int screen_h;
    int offset_x;       // for panning
    int offset_y;

    // Convert lat/lon to screen x,y. Returns false if off-screen.
    bool to_screen(float lat, float lon, int &sx, int &sy) const {
        float dx_nm = (lon - center_lon) * 60.0f * cosf(center_lat * M_PI / 180.0f);
        float dy_nm = (lat - center_lat) * 60.0f;

        float scale = (float)screen_h / (radius_nm * 2.0f);
        sx = (int)(screen_w / 2 + dx_nm * scale) + offset_x;
        sy = (int)(screen_h / 2 - dy_nm * scale) + offset_y;

        return (sx >= -20 && sx < screen_w + 20 && sy >= -20 && sy < screen_h + 20);
    }

    // Distance in nautical miles between two points (Haversine)
    static float distance_nm(float lat1, float lon1, float lat2, float lon2) {
        float dlat = (lat2 - lat1) * M_PI / 180.0f;
        float dlon = (lon2 - lon1) * M_PI / 180.0f;
        float a = sinf(dlat / 2) * sinf(dlat / 2) +
                  cosf(lat1 * M_PI / 180.0f) * cosf(lat2 * M_PI / 180.0f) *
                  sinf(dlon / 2) * sinf(dlon / 2);
        float c = 2.0f * atan2f(sqrtf(a), sqrtf(1 - a));
        return c * 3440.065f; // Earth radius in NM
    }
};

// Altitude to color for trails (green=low, yellow=mid, red=high)
static inline lv_color_t altitude_color(int32_t alt_ft) {
    if (alt_ft <= 0) return lv_color_hex(0x666666);       // ground
    if (alt_ft < 5000) return lv_color_hex(0x00cc44);     // green
    if (alt_ft < 15000) return lv_color_hex(0x88cc00);    // yellow-green
    if (alt_ft < 25000) return lv_color_hex(0xcccc00);    // yellow
    if (alt_ft < 35000) return lv_color_hex(0xcc8800);    // orange
    return lv_color_hex(0xcc2200);                         // red (high altitude)
}

// Aircraft category colors — distinctive per type
#define COLOR_COMMERCIAL  lv_color_hex(0x00bbff)  // cyan-blue
#define COLOR_MILITARY    lv_color_hex(0xffaa00)  // amber/gold
#define COLOR_GA_PRIVATE  lv_color_hex(0x44dd44)  // bright green
#define COLOR_HELI_CAT    lv_color_hex(0xdd44ff)  // magenta/purple
#define COLOR_EMERGENCY   lv_color_hex(0xff3333)  // red
