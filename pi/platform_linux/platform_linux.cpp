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

void platform_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout); // stdout is fully-buffered when not a TTY (journald, redirected files) -- flush so log lines show up promptly
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
