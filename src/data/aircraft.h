#pragma once
#include <cstdint>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
    float lat;
    float lon;
    int32_t altitude;       // feet
    int16_t speed;          // knots
    int16_t heading;        // degrees 0-359
    int16_t vert_rate;      // ft/min
    uint16_t squawk;
    bool on_ground;
    bool is_military;
    bool is_emergency;
    bool is_watched;
    uint32_t last_seen;     // millis() timestamp
    TrailPoint trail[60];
    uint8_t trail_count;

    void clear() {
        memset(this, 0, sizeof(Aircraft));
    }
};

// Thread-safe aircraft list
class AircraftList {
public:
    Aircraft aircraft[200];
    int count = 0;
    SemaphoreHandle_t mutex;

    void init() {
        mutex = xSemaphoreCreateMutex();
        count = 0;
    }

    bool lock(TickType_t timeout = pdMS_TO_TICKS(100)) {
        return xSemaphoreTake(mutex, timeout) == pdTRUE;
    }

    void unlock() {
        xSemaphoreGive(mutex);
    }
};
