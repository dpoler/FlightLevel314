#pragma once
#include <cstdint>

// Global HTTP request serialization -- one shared mutex so concurrent
// fetch/enrichment calls don't hit the network stack at once. Routed
// through src/platform/platform.h's mutex primitive so this same file
// works on both jc1060 (FreeRTOS semaphore, src/platform/esp32/) and Pi
// (std::mutex, pi/platform_linux/). UINT32_MAX means "wait indefinitely",
// same as the old default of portMAX_DELAY.

void http_mutex_init();
bool http_mutex_acquire(uint32_t timeout_ms = UINT32_MAX);
void http_mutex_release();
