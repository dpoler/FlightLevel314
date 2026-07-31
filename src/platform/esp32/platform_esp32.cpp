#include "../platform.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Only the mutex primitives are implemented here so far -- http_mutex.cpp
// is the one caller today (see src/data/http_mutex.cpp). millis()/log/
// HTTP/config-storage primitives get filled in here as their own
// migrations land (fetcher.cpp/storage.cpp, task #5 of the Pi port -- see
// project_pi_port memory); nothing on the ESP32 side calls them yet, so
// leaving them undefined here is deliberate, not an oversight.

platform_mutex_t platform_mutex_create() {
    return (platform_mutex_t)xSemaphoreCreateMutex();
}

bool platform_mutex_lock(platform_mutex_t m, uint32_t timeout_ms) {
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake((SemaphoreHandle_t)m, ticks) == pdTRUE;
}

void platform_mutex_unlock(platform_mutex_t m) {
    xSemaphoreGive((SemaphoreHandle_t)m);
}
