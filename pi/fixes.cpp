// FAA Designated Points + NAVAID overlay for Map (Pi).
// Queries ais-faa ArcGIS FeatureServers for named fixes / navaids inside
// the current map envelope. Worker thread fetches; LVGL thread swaps + draws.

#include "fixes.h"
#include "../src/data/storage.h"
#include "../src/platform/platform.h"
#include "../src/ui/display_prefs.h"
#include "../src/ui/geo.h"

#include <ArduinoJson.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kMaxFixes = 200;
constexpr float kRelocNm = 2.0f; // re-fetch when center moves this far

enum class FixKind : uint8_t { Designated = 0, Navaid = 1 };

struct FixPoint {
    char ident[12];
    float lat;
    float lon;
    FixKind kind;
    uint8_t area_charted; // Designated US_AREA==1 preferred when capping
};

struct Snapshot {
    FixPoint pts[kMaxFixes];
    int count = 0;
    float lat = 0;
    float lon = 0;
    float radius_nm = 0;
};

std::mutex g_mu;
Snapshot g_live;          // drawn
Snapshot g_pending;       // filled by worker
bool g_pending_ready = false;
bool g_busy = false;

float g_req_lat = 0;
float g_req_lon = 0;
float g_req_radius = 0;
bool g_have_live = false;
// Last successful fetch center (set when worker publishes) — used to
// dedupe before LVGL poll_swap copies into g_live.
float g_fetched_lat = 0;
float g_fetched_lon = 0;
float g_fetched_radius = 0;
bool g_have_fetched = false;

static float haversine_nm(float lat1, float lon1, float lat2, float lon2) {
    const float R_nm = 3440.065f;
    const float dlat = (lat2 - lat1) * (float)M_PI / 180.0f;
    const float dlon = (lon2 - lon1) * (float)M_PI / 180.0f;
    const float a = sinf(dlat * 0.5f) * sinf(dlat * 0.5f) +
                    cosf(lat1 * (float)M_PI / 180.0f) *
                        cosf(lat2 * (float)M_PI / 180.0f) *
                        sinf(dlon * 0.5f) * sinf(dlon * 0.5f);
    return 2.0f * R_nm * asinf(fminf(1.0f, sqrtf(a)));
}

static void envelope(float lat, float lon, float radius_nm,
                     double *xmin, double *ymin, double *xmax, double *ymax) {
    // Slight pad so edge fixes still appear when the range ring is tight.
    const float pad = radius_nm * 1.08f;
    const float dlat = pad / 60.0f;
    float clat = cosf(lat * (float)M_PI / 180.0f);
    if (clat < 0.2f) clat = 0.2f;
    const float dlon = pad / (60.0f * clat);
    *xmin = lon - dlon;
    *xmax = lon + dlon;
    *ymin = lat - dlat;
    *ymax = lat + dlat;
}

static bool http_get_json(const char *url, std::string *out) {
    // Slim queries are tens of KB; leave headroom for denser metro areas.
    std::vector<char> buf(512 * 1024);
    size_t len = 0;
    if (!platform_http_get(url, buf.data(), buf.size(), &len) || len == 0)
        return false;
    out->assign(buf.data(), len);
    return true;
}

static void append_designated(Snapshot *snap, const char *json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        platform_log_warn("Fixes: DesignatedPoints JSON parse failed: %s\n", err.c_str());
        return;
    }
    JsonArray feats = doc["features"].as<JsonArray>();
    if (feats.isNull()) return;
    for (JsonObject f : feats) {
        if (snap->count >= kMaxFixes) break;
        const char *ident = f["attributes"]["IDENT"] | "";
        if (!ident[0]) continue;
        double x = f["geometry"]["x"] | 0.0;
        double y = f["geometry"]["y"] | 0.0;
        if (x == 0.0 && y == 0.0) continue;
        FixPoint &p = snap->pts[snap->count];
        strlcpy(p.ident, ident, sizeof(p.ident));
        p.lon = (float)x;
        p.lat = (float)y;
        p.kind = FixKind::Designated;
        p.area_charted = (uint8_t)((int)(f["attributes"]["US_AREA"] | 0) != 0);
        snap->count++;
    }
}

static void append_navaids(Snapshot *snap, const char *json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        platform_log_warn("Fixes: NAVAID JSON parse failed: %s\n", err.c_str());
        return;
    }
    JsonArray feats = doc["features"].as<JsonArray>();
    if (feats.isNull()) return;
    for (JsonObject f : feats) {
        if (snap->count >= kMaxFixes) break;
        const char *ident = f["attributes"]["IDENT"] | "";
        if (!ident[0]) continue;
        double x = f["geometry"]["x"] | 0.0;
        double y = f["geometry"]["y"] | 0.0;
        if (x == 0.0 && y == 0.0) continue;
        // Skip duplicate IDENT already present as a designated point.
        bool dup = false;
        for (int i = 0; i < snap->count; i++) {
            if (strcmp(snap->pts[i].ident, ident) == 0) { dup = true; break; }
        }
        if (dup) continue;
        FixPoint &p = snap->pts[snap->count];
        strlcpy(p.ident, ident, sizeof(p.ident));
        p.lon = (float)x;
        p.lat = (float)y;
        p.kind = FixKind::Navaid;
        p.area_charted = 1;
        snap->count++;
    }
}

static void sort_prefer_area_and_distance(Snapshot *snap) {
    // Prefer charted area fixes, then closer to center — simple insertion
    // for N<=200.
    auto score = [&](const FixPoint &p) {
        float d = haversine_nm(snap->lat, snap->lon, p.lat, p.lon);
        return (p.area_charted ? 0.0f : 1000.0f) + d;
    };
    for (int i = 1; i < snap->count; i++) {
        FixPoint key = snap->pts[i];
        float ks = score(key);
        int j = i - 1;
        while (j >= 0 && score(snap->pts[j]) > ks) {
            snap->pts[j + 1] = snap->pts[j];
            j--;
        }
        snap->pts[j + 1] = key;
    }
}

static void worker_fetch(float lat, float lon, float radius_nm) {
    double xmin, ymin, xmax, ymax;
    envelope(lat, lon, radius_nm, &xmin, &ymin, &xmax, &ymax);

    char url[640];
    snprintf(url, sizeof(url),
             "https://services6.arcgis.com/ssFJjBXIUyZDrSYZ/arcgis/rest/services/"
             "DesignatedPoints/FeatureServer/0/query"
             "?where=1%%3D1"
             "&geometry=%.5f%%2C%.5f%%2C%.5f%%2C%.5f"
             "&geometryType=esriGeometryEnvelope&inSR=4326"
             "&spatialRel=esriSpatialRelIntersects"
             "&outFields=IDENT%%2CTYPE_CODE%%2CUS_AREA"
             "&returnGeometry=true&outSR=4326"
             "&resultRecordCount=%d&f=json",
             xmin, ymin, xmax, ymax, kMaxFixes);

    Snapshot snap{};
    snap.lat = lat;
    snap.lon = lon;
    snap.radius_nm = radius_nm;

    std::string body;
    if (http_get_json(url, &body)) {
        append_designated(&snap, body.c_str());
    } else {
        platform_log_warn("Fixes: DesignatedPoints fetch failed\n");
    }

    snprintf(url, sizeof(url),
             "https://services6.arcgis.com/ssFJjBXIUyZDrSYZ/arcgis/rest/services/"
             "NAVAIDSystem/FeatureServer/0/query"
             "?where=1%%3D1"
             "&geometry=%.5f%%2C%.5f%%2C%.5f%%2C%.5f"
             "&geometryType=esriGeometryEnvelope&inSR=4326"
             "&spatialRel=esriSpatialRelIntersects"
             "&outFields=IDENT%%2CTYPE_CODE"
             "&returnGeometry=true&outSR=4326"
             "&resultRecordCount=100&f=json",
             xmin, ymin, xmax, ymax);

    body.clear();
    if (http_get_json(url, &body)) {
        append_navaids(&snap, body.c_str());
    } else {
        platform_log_warn("Fixes: NAVAID fetch failed\n");
    }

    sort_prefer_area_and_distance(&snap);

    // Drop anything outside the true radius (envelope is a rectangle).
    int w = 0;
    for (int i = 0; i < snap.count; i++) {
        if (haversine_nm(lat, lon, snap.pts[i].lat, snap.pts[i].lon) <= radius_nm * 1.05f)
            snap.pts[w++] = snap.pts[i];
    }
    snap.count = w;

    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_pending = snap;
        g_pending_ready = true;
        g_fetched_lat = lat;
        g_fetched_lon = lon;
        g_fetched_radius = radius_nm;
        g_have_fetched = true;
        g_busy = false;
    }
    platform_log_info("Fixes: loaded %d near %.2f,%.2f r=%.0fnm\n",
                      snap.count, lat, lon, radius_nm);
}

} // namespace

void fixes_clear(void) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_live = Snapshot{};
    g_pending = Snapshot{};
    g_pending_ready = false;
    g_have_live = false;
    g_have_fetched = false;
}

void fixes_request(float lat, float lon, float radius_nm) {
    if (!map_fixes_shown()) {
        fixes_clear();
        return;
    }
    if (radius_nm < 1.0f) radius_nm = 1.0f;
    if (radius_nm > 200.0f) radius_nm = 200.0f;

    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_busy) return;
        if (g_have_fetched) {
            float moved = haversine_nm(g_fetched_lat, g_fetched_lon, lat, lon);
            float rdelta = fabsf(g_fetched_radius - radius_nm);
            if (moved < kRelocNm && rdelta < 1.0f) return;
        }
        g_busy = true;
        g_req_lat = lat;
        g_req_lon = lon;
        g_req_radius = radius_nm;
    }

    platform_log_info("Fixes: fetching near %.2f,%.2f r=%.0fnm\n", lat, lon, radius_nm);
    std::thread([lat, lon, radius_nm]() {
        worker_fetch(lat, lon, radius_nm);
    }).detach();
}

bool fixes_poll_swap(void) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_pending_ready) return false;
    g_live = g_pending;
    g_pending_ready = false;
    g_have_live = true;
    return true;
}

int fixes_count(void) {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_live.count;
}

void fixes_draw(lv_layer_t *layer,
                bool (*to_screen)(float lat, float lon, int *sx, int *sy),
                lv_color_t color, lv_opa_t opa) {
    if (!layer || !to_screen || !map_fixes_shown()) return;

    Snapshot local;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_have_live || g_live.count <= 0) return;
        local = g_live;
    }

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = color;
    line.width = 1;
    line.opa = opa;

    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.color = color;
    lbl.font = &lv_font_montserrat_10;
    lbl.opa = opa;
    lbl.text_local = 1;

    constexpr int kMark = 3;
    for (int i = 0; i < local.count; i++) {
        const FixPoint &p = local.pts[i];
        int sx = 0, sy = 0;
        if (!to_screen(p.lat, p.lon, &sx, &sy)) continue;

        if (p.kind == FixKind::Navaid) {
            // Small diamond for navaids.
            line.p1 = {(lv_value_precise_t)sx, (lv_value_precise_t)(sy - kMark - 1)};
            line.p2 = {(lv_value_precise_t)(sx + kMark + 1), (lv_value_precise_t)sy};
            lv_draw_line(layer, &line);
            line.p1 = {(lv_value_precise_t)(sx + kMark + 1), (lv_value_precise_t)sy};
            line.p2 = {(lv_value_precise_t)sx, (lv_value_precise_t)(sy + kMark + 1)};
            lv_draw_line(layer, &line);
            line.p1 = {(lv_value_precise_t)sx, (lv_value_precise_t)(sy + kMark + 1)};
            line.p2 = {(lv_value_precise_t)(sx - kMark - 1), (lv_value_precise_t)sy};
            lv_draw_line(layer, &line);
            line.p1 = {(lv_value_precise_t)(sx - kMark - 1), (lv_value_precise_t)sy};
            line.p2 = {(lv_value_precise_t)sx, (lv_value_precise_t)(sy - kMark - 1)};
            lv_draw_line(layer, &line);
        } else {
            // Small + for designated points / RNAV fixes.
            line.p1 = {(lv_value_precise_t)(sx - kMark), (lv_value_precise_t)sy};
            line.p2 = {(lv_value_precise_t)(sx + kMark), (lv_value_precise_t)sy};
            lv_draw_line(layer, &line);
            line.p1 = {(lv_value_precise_t)sx, (lv_value_precise_t)(sy - kMark)};
            line.p2 = {(lv_value_precise_t)sx, (lv_value_precise_t)(sy + kMark)};
            lv_draw_line(layer, &line);
        }

        lbl.text = p.ident;
        lv_area_t area = {(lv_coord_t)(sx + kMark + 2), (lv_coord_t)(sy - 6),
                          (lv_coord_t)(sx + kMark + 52), (lv_coord_t)(sy + 6)};
        lv_draw_label(layer, &lbl, &area);
    }
}
