#pragma once
#include <cstdint>
#include <cstring>
#include <cstdlib>

#if defined(ARDUINO)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#else
#include <mutex>
#include <chrono>
#endif

// Aircraft list capacity. ESP32 keeps the historical PSRAM-era cap; Pi has
// room for denser 50–250 nm queries (KJFK alone can exceed 200).
#if defined(ARDUINO)
#define MAX_AIRCRAFT 200
#else
#define MAX_AIRCRAFT 1000
#endif
#define TRAIL_LENGTH 60

struct TrailPoint {
    float lat;
    float lon;
    int32_t alt;
    uint32_t timestamp;
};

struct Aircraft {
    char icao_hex[7];       // e.g. "A0B1C2"
    char callsign[9];       // e.g. "UAL1234"
    char registration[9];   // e.g. "N12345"
    char type_code[5];      // e.g. "B738"
    char category[3];       // ADS-B emitter category e.g. "A3"
    char desc[40];          // type description e.g. "Boeing 737-800"
    char owner_op[32];      // operator e.g. "United Airlines"
    float lat;
    float lon;
    int32_t altitude;       // feet
    int16_t speed;          // knots
    int16_t heading;        // degrees 0-359
    int16_t vert_rate;      // ft/min
    bool vert_rate_valid;   // false when the feed simply didn't report a
                             // vertical rate this cycle -- not the same as a
                             // confirmed 0fpm/level reading
    uint16_t squawk;
    bool on_ground;
    bool is_military;
    bool is_emergency;
    bool is_watched;
    float mach;             // Mach number (0 = not available)
    int16_t ias;            // indicated airspeed kts (0 = n/a)
    int16_t tas;            // true airspeed kts (0 = n/a)
    int32_t nav_altitude;   // autopilot target altitude ft (0 = n/a)
    float roll;             // bank angle degrees (0 = wings level or n/a)
    float nav_qnh;          // altimeter setting hPa (0 = n/a)
    uint32_t last_seen;     // millis() timestamp
    uint32_t stale_since;   // 0 = fresh, else millis() when first went stale
    TrailPoint trail[TRAIL_LENGTH];
    uint8_t trail_count;

    void clear() {
        memset(this, 0, sizeof(Aircraft));
    }
};

#define GHOST_TIMEOUT_MS 30000

// Compute opacity for stale (ghost) aircraft: 255→0 over 30s
// Caller must pass current millis() value
static inline uint8_t compute_aircraft_opacity(uint32_t stale_since, uint32_t now_ms) {
    if (stale_since == 0) return 255;
    uint32_t elapsed = now_ms - stale_since;
    if (elapsed >= GHOST_TIMEOUT_MS) return 0;
    return (uint8_t)(255 - (elapsed * 255 / GHOST_TIMEOUT_MS));
}

// Thread-safe aircraft list. Mutex/allocation are the one place this
// struct forks per-platform (#if defined(ARDUINO)) rather than going
// through src/platform/platform.h -- both are tiny, self-contained
// implementation details entirely inside this header, so a seam header +
// two .cpp files would've been more indirection than the thing it's
// abstracting. lock()/unlock()'s call signature is unchanged either way,
// so every existing call site (data/ui *.cpp) works untouched on both
// targets.
class AircraftList {
public:
    Aircraft *aircraft = nullptr;
    int count = 0;
#if defined(ARDUINO)
    SemaphoreHandle_t mutex;
#else
    std::timed_mutex mutex;
#endif

    void init() {
        count = 0;
#if defined(ARDUINO)
        mutex = xSemaphoreCreateMutex();
        // Allocate in PSRAM — too large for internal DRAM (~226KB at the
        // historical MAX_AIRCRAFT=200 with TRAIL_LENGTH=60).
        aircraft = (Aircraft *)heap_caps_malloc(MAX_AIRCRAFT * sizeof(Aircraft), MALLOC_CAP_SPIRAM);
#else
        // No PSRAM-vs-internal-DRAM split on Linux -- plain heap is plenty.
        aircraft = (Aircraft *)malloc(MAX_AIRCRAFT * sizeof(Aircraft));
#endif
        if (aircraft) memset(aircraft, 0, MAX_AIRCRAFT * sizeof(Aircraft));
    }

#if defined(ARDUINO)
    bool lock(TickType_t timeout = pdMS_TO_TICKS(100)) {
        return xSemaphoreTake(mutex, timeout) == pdTRUE;
    }

    void unlock() {
        xSemaphoreGive(mutex);
    }
#else
    bool lock(uint32_t timeout_ms = 100) {
        return mutex.try_lock_for(std::chrono::milliseconds(timeout_ms));
    }

    void unlock() {
        mutex.unlock();
    }
#endif
};
