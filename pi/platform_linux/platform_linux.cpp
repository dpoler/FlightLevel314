#include "../../src/platform/platform.h"
#include <chrono>
#include <mutex>
#include <cstdio>
#include <cstdarg>

// HTTP GET (libcurl) and config storage land separately -- see
// pi/platform_linux/storage_linux.cpp for config storage (task #4 of the
// Pi port; see project_pi_port memory) and task #5 for fetcher.cpp's real
// libcurl-backed platform_http_get().

uint32_t platform_millis() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - start).count();
}

namespace {
platform_log_level_t g_log_min = PLATFORM_LOG_INFO;

const char *level_tag(platform_log_level_t level) {
    switch (level) {
    case PLATFORM_LOG_DEBUG: return "D ";
    case PLATFORM_LOG_INFO:  return "I ";
    case PLATFORM_LOG_WARN:  return "W ";
    case PLATFORM_LOG_ERROR: return "E ";
    }
    return "I ";
}
} // namespace

void platform_log_set_min_level(platform_log_level_t level) {
    g_log_min = level;
}

platform_log_level_t platform_log_get_min_level(void) {
    return g_log_min;
}

void platform_log_at(platform_log_level_t level, const char *fmt, ...) {
    if (level < g_log_min) return;
    // WARN/ERROR → stderr so journald can mark priority; DEBUG/INFO → stdout.
    FILE *out = (level >= PLATFORM_LOG_WARN) ? stderr : stdout;
    fputs(level_tag(level), out);
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);
    fflush(out);
}

void platform_log(const char *fmt, ...) {
    if (PLATFORM_LOG_INFO < g_log_min) return;
    FILE *out = stdout;
    fputs("I ", out);
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);
    fflush(out);
}

namespace {
struct PlatformMutex {
    std::timed_mutex m;
};
}

platform_mutex_t platform_mutex_create() {
    return new PlatformMutex();
}

bool platform_mutex_lock(platform_mutex_t m, uint32_t timeout_ms) {
    auto *pm = static_cast<PlatformMutex *>(m);
    if (timeout_ms == UINT32_MAX) {
        pm->m.lock();
        return true;
    }
    return pm->m.try_lock_for(std::chrono::milliseconds(timeout_ms));
}

void platform_mutex_unlock(platform_mutex_t m) {
    static_cast<PlatformMutex *>(m)->m.unlock();
}
