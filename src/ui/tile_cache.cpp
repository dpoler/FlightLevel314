#include "tile_cache.h"
#include "../config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <cmath>
#include <cstring>

// lodepng is compiled by LVGL when LV_USE_LODEPNG is enabled
extern "C" {
    unsigned lodepng_decode32(unsigned char **out, unsigned *w, unsigned *h,
                              const unsigned char *in, size_t insize);
}

// --- OSM tile math ---

int osm_lon_to_x(float lon, int z) {
    return (int)floorf((lon + 180.0f) / 360.0f * (float)(1 << z));
}

int osm_lat_to_y(float lat, int z) {
    float lat_rad = lat * M_PI / 180.0f;
    return (int)floorf((1.0f - logf(tanf(lat_rad) + 1.0f / cosf(lat_rad)) / M_PI) / 2.0f * (float)(1 << z));
}

float osm_x_to_lon(int x, int z) {
    return (float)x / (float)(1 << z) * 360.0f - 180.0f;
}

float osm_y_to_lat(int y, int z) {
    float n = M_PI - 2.0f * M_PI * (float)y / (float)(1 << z);
    return 180.0f / M_PI * atanf(0.5f * (expf(n) - expf(-n)));
}

int osm_zoom_for_radius(float radius_nm, int screen_h, float center_lat) {
    // Our map's pixels per degree
    float our_ppd = (float)screen_h / (radius_nm * 2.0f) * 60.0f;
    float cos_lat = cosf(center_lat * M_PI / 180.0f);

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

// --- PSRAM tile cache ---

#define MAX_CACHED_TILES 16

struct CachedTile {
    int z, x, y;
    uint16_t *pixels;    // TILE_PX * TILE_PX RGB565 in PSRAM
    uint32_t last_used;
    bool valid;
    bool empty;          // fetched but no content (404, ocean, etc.)
};

static CachedTile _tiles[MAX_CACHED_TILES];
static SemaphoreHandle_t _cache_mutex;
static QueueHandle_t _fetch_queue;
static bool _sd_available = false;

struct FetchRequest {
    int z, x, y;
};

static CachedTile *find_tile(int z, int x, int y) {
    for (int i = 0; i < MAX_CACHED_TILES; i++) {
        if (_tiles[i].valid && _tiles[i].z == z && _tiles[i].x == x && _tiles[i].y == y) {
            _tiles[i].last_used = millis();
            return &_tiles[i];
        }
    }
    return nullptr;
}

static CachedTile *evict_lru() {
    int oldest_idx = 0;
    uint32_t oldest_time = UINT32_MAX;
    for (int i = 0; i < MAX_CACHED_TILES; i++) {
        if (!_tiles[i].valid) return &_tiles[i];
        if (_tiles[i].last_used < oldest_time) {
            oldest_time = _tiles[i].last_used;
            oldest_idx = i;
        }
    }
    return &_tiles[oldest_idx];
}

static void cache_insert(int z, int x, int y, uint16_t *pixels, bool empty) {
    if (xSemaphoreTake(_cache_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (pixels) free(pixels);
        return;
    }

    CachedTile *slot = find_tile(z, x, y);
    if (slot) {
        // Already cached (race condition)
        xSemaphoreGive(_cache_mutex);
        if (pixels) free(pixels);
        return;
    }

    slot = evict_lru();
    if (slot->pixels) {
        free(slot->pixels);
        slot->pixels = nullptr;
    }
    slot->z = z;
    slot->x = x;
    slot->y = y;
    slot->pixels = pixels;
    slot->last_used = millis();
    slot->valid = true;
    slot->empty = empty;
    xSemaphoreGive(_cache_mutex);
}

// --- PNG decode ---

static bool decode_png_rgb565(const uint8_t *png, size_t len, uint16_t *out) {
    unsigned char *rgba = nullptr;
    unsigned w, h;
    unsigned err = lodepng_decode32(&rgba, &w, &h, png, len);
    if (err || w != TILE_PX || h != TILE_PX) {
        if (rgba) free(rgba);
        return false;
    }

    // RGBA8888 → RGB565
    for (int i = 0; i < TILE_PX * TILE_PX; i++) {
        uint8_t r = rgba[i * 4];
        uint8_t g = rgba[i * 4 + 1];
        uint8_t b = rgba[i * 4 + 2];
        out[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }
    free(rgba);
    return true;
}

// --- SD card cache ---

static void sd_cache_path(int z, int x, int y, char *buf, size_t len) {
    snprintf(buf, len, "/tiles/%d/%d/%d.png", z, x, y);
}

static bool sd_load_tile(int z, int x, int y, uint16_t *out) {
    if (!_sd_available) return false;

    char path[64];
    sd_cache_path(z, x, y, path, sizeof(path));

    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;

    size_t size = f.size();
    if (size == 0 || size > 200000) { f.close(); return false; }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!buf) { f.close(); return false; }

    f.read(buf, size);
    f.close();

    bool ok = decode_png_rgb565(buf, size, out);
    free(buf);
    return ok;
}

static void sd_save_tile(int z, int x, int y, const uint8_t *png, size_t len) {
    if (!_sd_available) return;

    char dir[48];
    snprintf(dir, sizeof(dir), "/tiles/%d", z);
    SD_MMC.mkdir(dir);
    snprintf(dir, sizeof(dir), "/tiles/%d/%d", z, x);
    SD_MMC.mkdir(dir);

    char path[64];
    sd_cache_path(z, x, y, path, sizeof(path));

    File f = SD_MMC.open(path, FILE_WRITE);
    if (f) {
        f.write(png, len);
        f.close();
    }
}

// --- Background fetch task ---

static void fetch_task(void *param) {
    FetchRequest req;
    char url[128];

    while (true) {
        if (xQueueReceive(_fetch_queue, &req, portMAX_DELAY) != pdTRUE) continue;

        // Skip if already cached
        if (xSemaphoreTake(_cache_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            bool cached = find_tile(req.z, req.x, req.y) != nullptr;
            xSemaphoreGive(_cache_mutex);
            if (cached) continue;
        }

        // Allocate pixel buffer in PSRAM
        uint16_t *pixels = (uint16_t *)heap_caps_malloc(
            TILE_PX * TILE_PX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!pixels) continue;

        // Try SD card first
        if (sd_load_tile(req.z, req.x, req.y, pixels)) {
            cache_insert(req.z, req.x, req.y, pixels, false);
            continue;
        }

        // Fetch from network
        if (WiFi.status() != WL_CONNECTED) {
            free(pixels);
            continue;
        }

        // CartoDB dark tiles — good contrast with our dark UI
        snprintf(url, sizeof(url),
            "https://basemaps.cartocdn.com/dark_all/%d/%d/%d.png",
            req.z, req.x, req.y);

        HTTPClient http;
        http.begin(url);
        http.addHeader("User-Agent", "ADS-B-Display/1.0");
        http.setTimeout(10000);
        int code = http.GET();

        if (code == HTTP_CODE_OK) {
            int content_len = http.getSize();
            if (content_len > 0 && content_len < 200000) {
                uint8_t *png = (uint8_t *)heap_caps_malloc(content_len, MALLOC_CAP_SPIRAM);
                if (png) {
                    WiFiClient *stream = http.getStreamPtr();
                    int total = 0;
                    uint32_t timeout = millis() + 10000;
                    while (total < content_len && millis() < timeout) {
                        size_t avail = stream->available();
                        if (avail > 0) {
                            int n = stream->readBytes(png + total,
                                min((int)avail, content_len - total));
                            total += n;
                        } else {
                            vTaskDelay(pdMS_TO_TICKS(5));
                        }
                    }

                    if (total == content_len) {
                        sd_save_tile(req.z, req.x, req.y, png, content_len);

                        if (decode_png_rgb565(png, content_len, pixels)) {
                            cache_insert(req.z, req.x, req.y, pixels, false);
                            pixels = nullptr; // ownership transferred
                        }
                    }
                    free(png);
                }
            }
        } else if (code == 404) {
            // Mark as empty so we don't re-fetch
            cache_insert(req.z, req.x, req.y, nullptr, true);
        }

        http.end();
        if (pixels) free(pixels);

        // Rate limit: 200ms between fetches
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// --- Public API ---

void tile_cache_init() {
    _cache_mutex = xSemaphoreCreateMutex();
    _fetch_queue = xQueueCreate(24, sizeof(FetchRequest));
    memset(_tiles, 0, sizeof(_tiles));

    _sd_available = SD_MMC.begin("/sdcard", true);
    if (_sd_available) {
        SD_MMC.mkdir("/tiles");
        Serial.println("Tile cache: SD card ready");
    } else {
        Serial.println("Tile cache: no SD card, network-only caching");
    }

    xTaskCreatePinnedToCore(fetch_task, "tile_fetch", 16384, nullptr, 0, nullptr, 1);
}

uint16_t *tile_cache_get(int z, int x, int y) {
    if (xSemaphoreTake(_cache_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return nullptr;

    CachedTile *t = find_tile(z, x, y);
    uint16_t *result = nullptr;
    bool found = false;
    if (t) {
        found = true;
        if (!t->empty) result = t->pixels;
    }
    xSemaphoreGive(_cache_mutex);

    if (!found) {
        FetchRequest req = {z, x, y};
        xQueueSend(_fetch_queue, &req, 0);
    }

    return result;
}

void tile_cache_flush_queue() {
    xQueueReset(_fetch_queue);
}
