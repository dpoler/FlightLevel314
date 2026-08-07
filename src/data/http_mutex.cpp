#include "http_mutex.h"
#include "../platform/platform.h"

static platform_mutex_t _mutex = nullptr;

void http_mutex_init() {
    _mutex = platform_mutex_create();
}

bool http_mutex_acquire(uint32_t timeout_ms) {
    return platform_mutex_lock(_mutex, timeout_ms);
}

void http_mutex_release() {
    platform_mutex_unlock(_mutex);
}
