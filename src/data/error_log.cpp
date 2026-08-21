#include "error_log.h"
#include "../platform/platform.h"
#include <stdarg.h>
#include <string.h>
#include <cstdio>

static ErrorEntry _ring[ERROR_LOG_MAX];
static int _write_idx = 0;
static int _count = 0;         // entries in ring (max ERROR_LOG_MAX)
static uint32_t _total = 0;    // lifetime count
static platform_mutex_t _mutex = nullptr;

void error_log_init() {
    _mutex = platform_mutex_create();
    memset(_ring, 0, sizeof(_ring));
}

void error_log_add(const char *fmt, ...) {
    if (!_mutex) return;
    if (!platform_mutex_lock(_mutex, 50)) return;

    ErrorEntry &e = _ring[_write_idx];
    va_list args;
    va_start(args, fmt);
    vsnprintf(e.msg, ERROR_LOG_MSG_LEN, fmt, args);
    va_end(args);
    e.timestamp = millis();

    // Also journal — Settings ring is easy to miss when SSHing.
    platform_log_warn("ErrorLog: %s\n", e.msg);

    _write_idx = (_write_idx + 1) % ERROR_LOG_MAX;
    if (_count < ERROR_LOG_MAX) _count++;
    _total++;

    platform_mutex_unlock(_mutex);
}

ErrorSnapshot error_log_snapshot() {
    ErrorSnapshot snap = {};
    if (!_mutex) return snap;
    if (!platform_mutex_lock(_mutex, 50)) return snap;

    snap.count = _count;
    // Copy in chronological order (oldest first)
    int start = (_count < ERROR_LOG_MAX) ? 0 : _write_idx;
    for (int i = 0; i < _count; i++) {
        snap.entries[i] = _ring[(start + i) % ERROR_LOG_MAX];
    }

    platform_mutex_unlock(_mutex);
    return snap;
}

void error_log_clear() {
    if (!_mutex) return;
    if (!platform_mutex_lock(_mutex, 50)) return;
    _count = 0;
    _write_idx = 0;
    memset(_ring, 0, sizeof(_ring));
    platform_mutex_unlock(_mutex);
}

uint32_t error_log_total_count() {
    return _total;  // atomic read on ESP32
}
