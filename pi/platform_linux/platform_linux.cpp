#include "../../src/platform/platform.h"
#include <chrono>
#include <mutex>
#include <cstdio>
#include <cstdarg>
#include <cerrno>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// HTTP GET (libcurl) and config storage land separately -- see
// pi/platform_linux/storage_linux.cpp for config storage (task #4 of the
// Pi port; see project_pi_port memory) and task #5 for fetcher.cpp's real
// libcurl-backed platform_http_get().

bool platform_mkdirs(const char *path) {
    if (!path || !path[0]) return false;
    std::string cur;
    const std::string p(path);
    for (size_t i = 0; i < p.size(); i++) {
        cur.push_back(p[i]);
        if ((p[i] == '/' && cur.size() > 1) || i + 1 == p.size()) {
            if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    struct stat st {};
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool platform_write_file_atomic(const char *path, const void *data, size_t len) {
    if (!path || !path[0]) return false;
    // Per-process temp name: tools/set_api_keys.py (often run as root)
    // writes the same config file, and a shared "<path>.tmp" let the two
    // clobber each other -- or a leftover root-owned one blocked every later
    // app save. Unlink first in case a crashed earlier run with this PID
    // left one behind.
    const std::string tmp = std::string(path) + ".tmp." + std::to_string(getpid());
    unlink(tmp.c_str());
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const char *p = static_cast<const char *>(data);
    size_t left = len;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        p += w;
        left -= (size_t)w;
    }
    const bool synced = fsync(fd) == 0;
    if (close(fd) != 0 || !synced) { // always close -- || used to skip it
        unlink(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

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
