// Pi Map basemap: CartoDB dark_all tiles, disk-cached RGB565, async fetch.
// Lives under pi/ so PlatformIO never compiles it into the jc1060 firmware.
//
// Threading model (important — two past crash sources):
// 1) Never call LVGL alloc/decode from the worker. LVGL's heap is not safe
//    off the LVGL thread; the bundled lodepng is patched to use it. Decode
//    PNGs with stb_image (plain malloc) on the worker instead.
// 2) Never mutate the drawn buffer while LVGL SW draw units may still be
//    reading it (LV_DRAW_SW_DRAW_UNIT_CNT>1 queues work). Worker publishes
//    into an inbox; the LVGL thread installs it via basemap_poll_swap()
//    between frames.

#include "basemap.h"

#include "../src/platform/platform.h"

#include <curl/curl.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <cstdlib>

#define STBI_ONLY_PNG
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

namespace {

constexpr int TILE_PX = 256;
constexpr const char *TILE_URL_FMT =
    "https://basemaps.cartocdn.com/dark_all/%d/%d/%d.png";
constexpr lv_opa_t BASEMAP_DRAW_OPA = LV_OPA_50; // keep aircraft readable on top

struct BasemapSlot {
    std::vector<uint8_t> rgb565;
    lv_image_dsc_t dsc{};
    float lat = 0, lon = 0, radius_nm = 0;
    int w = 0, h = 0;
    int geo_cy = 0;
    int bullseye_r = 0;
    bool valid = false;

    void bind_dsc() {
        memset(&dsc, 0, sizeof(dsc));
        dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        dsc.header.w = w;
        dsc.header.h = h;
        dsc.header.stride = w * 2;
        dsc.data_size = (uint32_t)rgb565.size();
        dsc.data = rgb565.data();
    }
};

std::mutex g_mu;
BasemapSlot g_front;          // LVGL thread only (after install)
BasemapSlot g_inbox;          // worker → LVGL publish slot
bool g_inbox_ready = false;
bool g_worker_busy = false;
float g_req_lat = 0, g_req_lon = 0, g_req_radius = 0;
int g_req_w = 0, g_req_h = 0, g_req_cy = 0, g_req_br = 0;
uint32_t g_req_gen = 0;

struct CurlBuf {
    std::vector<uint8_t> data;
};

size_t curl_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *b = static_cast<CurlBuf *>(userdata);
    size_t n = size * nmemb;
    b->data.insert(b->data.end(), ptr, ptr + n);
    return n;
}

bool http_get(const std::string &url, std::vector<uint8_t> &out) {
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    CurlBuf buf;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ADS-B-Display-Basemap/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || (code != 200 && code != 404)) return false;
    out.swap(buf.data);
    return true;
}

int osm_zoom_for_radius(float radius_nm, int usable_h, float center_lat) {
    float our_ppd = (float)usable_h / (radius_nm * 2.0f) * 60.0f;
    float cos_lat = cosf(center_lat * (float)M_PI / 180.0f);
    int best_z = 4;
    float best_diff = 1e9f;
    for (int z = 4; z < 16; z++) {
        float osm_ppd = 256.0f * (float)(1 << z) / 360.0f * cos_lat;
        float diff = fabsf(logf(osm_ppd / our_ppd));
        if (diff < best_diff) {
            best_diff = diff;
            best_z = z;
        }
    }
    return best_z;
}

void pixel_of_coord(float lat, float lon, int z, double &px, double &py) {
    int n = 1 << z;
    px = (lon + 180.0) / 360.0 * n * TILE_PX;
    double lat_rad = lat * M_PI / 180.0;
    py = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n * TILE_PX;
}

uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

std::string cache_dir() {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    std::string base = xdg && xdg[0] ? xdg : "";
    if (base.empty()) {
        const char *home = getenv("HOME");
        base = home && home[0] ? std::string(home) + "/.config" : "/tmp";
    }
    return base + "/adsb/basemap";
}

void ensure_dir(const std::string &path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur.push_back(path[i]);
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur.size() > 1 && cur.back() == '/') {
                std::string d = cur.substr(0, cur.size() - 1);
                mkdir(d.c_str(), 0755);
            } else if (i + 1 == path.size()) {
                mkdir(cur.c_str(), 0755);
            }
        }
    }
    mkdir(path.c_str(), 0755);
}

std::string cache_path(float lat, float lon, float radius_nm, int w, int h, int cy, int br) {
    char name[192];
    snprintf(name, sizeof(name), "%s/%.4f_%.4f_r%.0f_%dx%d_cy%d_br%d.rgb565",
             cache_dir().c_str(), lat, lon, radius_nm, w, h, cy, br);
    return name;
}

bool load_cache(const std::string &path, BasemapSlot &slot) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz != slot.rgb565.size()) {
        fclose(f);
        return false;
    }
    size_t n = fread(slot.rgb565.data(), 1, slot.rgb565.size(), f);
    fclose(f);
    return n == slot.rgb565.size();
}

void save_cache(const std::string &path, const BasemapSlot &slot) {
    ensure_dir(cache_dir());
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    fwrite(slot.rgb565.data(), 1, slot.rgb565.size(), f);
    fclose(f);
}

bool decode_png_tile(const std::vector<uint8_t> &png, std::vector<uint8_t> &rgba,
                     unsigned &tw, unsigned &th) {
    int w = 0, h = 0, comp = 0;
    unsigned char *out = stbi_load_from_memory(png.data(), (int)png.size(),
                                               &w, &h, &comp, 4);
    if (!out) return false;
    tw = (unsigned)w;
    th = (unsigned)h;
    if (tw != (unsigned)TILE_PX || th != (unsigned)TILE_PX) {
        stbi_image_free(out);
        return false;
    }
    rgba.assign(out, out + (size_t)tw * th * 4);
    stbi_image_free(out);
    return true;
}

void blit_tile_rgb565(BasemapSlot &slot, const std::vector<uint8_t> &rgba,
                      int dst_x0, int dst_y0) {
    for (int ty = 0; ty < TILE_PX; ty++) {
        int dy = dst_y0 + ty;
        if (dy < 0 || dy >= slot.h) continue;
        for (int tx = 0; tx < TILE_PX; tx++) {
            int dx = dst_x0 + tx;
            if (dx < 0 || dx >= slot.w) continue;
            const uint8_t *p = &rgba[((size_t)ty * TILE_PX + tx) * 4];
            uint16_t pix = rgb888_to_rgb565(p[0], p[1], p[2]);
            size_t off = ((size_t)dy * slot.w + dx) * 2;
            slot.rgb565[off] = (uint8_t)(pix & 0xFF);
            slot.rgb565[off + 1] = (uint8_t)(pix >> 8);
        }
    }
}

bool build_basemap(BasemapSlot &slot, uint32_t gen) {
    const int usable_h = slot.bullseye_r * 2;
    if (usable_h <= 0 || slot.w <= 0 || slot.h <= 0) return false;

    int z = osm_zoom_for_radius(slot.radius_nm, usable_h, slot.lat);
    double cx, cy;
    pixel_of_coord(slot.lat, slot.lon, z, cx, cy);

    double vp_left = cx - slot.w / 2.0;
    double vp_top = cy - slot.geo_cy;
    double vp_right = vp_left + slot.w;
    double vp_bottom = vp_top + slot.h;

    int tx0 = (int)floor(vp_left / TILE_PX);
    int tx1 = (int)floor((vp_right - 1) / TILE_PX);
    int ty0 = (int)floor(vp_top / TILE_PX);
    int ty1 = (int)floor((vp_bottom - 1) / TILE_PX);
    int n = 1 << z;

    uint16_t bg = rgb888_to_rgb565(0x0a, 0x0a, 0x1a);
    for (size_t i = 0; i < slot.rgb565.size(); i += 2) {
        slot.rgb565[i] = (uint8_t)(bg & 0xFF);
        slot.rgb565[i + 1] = (uint8_t)(bg >> 8);
    }

    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            {
                std::lock_guard<std::mutex> lock(g_mu);
                if (gen != g_req_gen) return false; // superseded
            }
            int wtx = tx % n;
            if (wtx < 0) wtx += n;
            if (ty < 0 || ty >= n) continue;

            char url[256];
            snprintf(url, sizeof(url), TILE_URL_FMT, z, wtx, ty);
            std::vector<uint8_t> png;
            if (!http_get(url, png)) continue;

            std::vector<uint8_t> rgba;
            unsigned tw = 0, th = 0;
            if (png.empty()) {
                rgba.assign((size_t)TILE_PX * TILE_PX * 4, 0);
                for (size_t i = 0; i < rgba.size(); i += 4) {
                    rgba[i] = 0x0a; rgba[i + 1] = 0x0a; rgba[i + 2] = 0x1a; rgba[i + 3] = 255;
                }
                tw = th = TILE_PX;
            } else if (!decode_png_tile(png, rgba, tw, th)) {
                continue;
            }

            int dst_x = (int)((tx * TILE_PX) - vp_left);
            int dst_y = (int)((ty * TILE_PX) - vp_top);
            blit_tile_rgb565(slot, rgba, dst_x, dst_y);
        }
    }

    slot.bind_dsc();
    slot.valid = true;
    return true;
}

void worker_main(uint32_t gen) {
    BasemapSlot local;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        local.lat = g_req_lat;
        local.lon = g_req_lon;
        local.radius_nm = g_req_radius;
        local.w = g_req_w;
        local.h = g_req_h;
        local.geo_cy = g_req_cy;
        local.bullseye_r = g_req_br;
    }
    local.rgb565.assign((size_t)local.w * local.h * 2, 0);

    std::string path = cache_path(local.lat, local.lon, local.radius_nm,
                                  local.w, local.h, local.geo_cy, local.bullseye_r);
    bool ok = false;
    if (load_cache(path, local)) {
        local.bind_dsc();
        local.valid = true;
        ok = true;
        platform_log("Basemap: cache hit %s\n", path.c_str());
    } else {
        platform_log("Basemap: fetching (%.4f,%.4f) r=%.0fnm %dx%d\n",
                     local.lat, local.lon, local.radius_nm, local.w, local.h);
        ok = build_basemap(local, gen);
        if (ok) save_cache(path, local);
    }

    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (ok && gen == g_req_gen) {
            // Publish to inbox only — LVGL thread installs into g_front.
            g_inbox = std::move(local);
            g_inbox.bind_dsc();
            g_inbox_ready = true;
        }
        g_worker_busy = false;
    }
}

} // namespace

void basemap_request(float lat, float lon, float radius_nm, int canvas_w, int canvas_h,
                     int geo_center_y, int bullseye_r_px) {
    if (canvas_w <= 0 || canvas_h <= 0 || bullseye_r_px <= 0) return;

    std::lock_guard<std::mutex> lock(g_mu);
    // Skip if front already matches and nothing is in flight / pending.
    if (g_front.valid && !g_worker_busy && !g_inbox_ready &&
        fabsf(g_front.lat - lat) < 1e-4f && fabsf(g_front.lon - lon) < 1e-4f &&
        fabsf(g_front.radius_nm - radius_nm) < 0.5f &&
        g_front.w == canvas_w && g_front.h == canvas_h &&
        g_front.geo_cy == geo_center_y && g_front.bullseye_r == bullseye_r_px) {
        return;
    }
    g_req_lat = lat;
    g_req_lon = lon;
    g_req_radius = radius_nm;
    g_req_w = canvas_w;
    g_req_h = canvas_h;
    g_req_cy = geo_center_y;
    g_req_br = bullseye_r_px;
    g_req_gen++;
    if (g_worker_busy) return;
    g_worker_busy = true;
    uint32_t gen = g_req_gen;
    std::thread([gen]() {
        worker_main(gen);
        uint32_t latest = 0;
        bool need = false;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            latest = g_req_gen;
            if (latest != gen && !g_worker_busy) {
                g_worker_busy = true;
                need = true;
            }
        }
        if (need) {
            std::thread(worker_main, latest).detach();
        }
    }).detach();
}

bool basemap_poll_swap(void) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_inbox_ready) return false;
    g_front = std::move(g_inbox);
    g_front.bind_dsc();
    g_inbox_ready = false;
    return g_front.valid;
}

void basemap_draw(lv_layer_t *layer) {
    // LVGL thread only. g_front is only mutated on this thread via poll_swap,
    // so draw units can keep reading dsc.data after this returns.
    if (!g_front.valid || g_front.rgb565.empty()) return;

    g_front.bind_dsc();

    lv_draw_image_dsc_t img;
    lv_draw_image_dsc_init(&img);
    img.src = &g_front.dsc;
    img.opa = BASEMAP_DRAW_OPA;
    lv_area_t a = {0, 0, (lv_coord_t)(g_front.w - 1), (lv_coord_t)(g_front.h - 1)};
    lv_draw_image(layer, &img, &a);
}

bool basemap_ready(void) {
    return g_front.valid;
}
