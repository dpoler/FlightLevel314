#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

// Platform seam for FlightLevel314 (Pi/Linux). Implemented in
// pi/platform_linux/platform_linux.cpp.

// --- Mutex ---
typedef void *platform_mutex_t;
platform_mutex_t platform_mutex_create();
bool platform_mutex_lock(platform_mutex_t m, uint32_t timeout_ms);
void platform_mutex_unlock(platform_mutex_t m);

// --- Time ---
uint32_t platform_millis();

// --- HTTP ---
// Synchronous GET. Returns true on 2xx with body written into out
// (replacing its contents); false on any failure (caller shouldn't rely on
// out's contents when false is returned).
bool platform_http_get(const char *url, char *out, size_t out_size, size_t *out_len);

// Optional marketplace rate-limit headers (RapidAPI / similar). All fields
// start false/0; callers pass nullptr when they do not care. Header names are
// matched case-insensitively (x-ratelimit-api-units-* and
// x-ratelimit-requests-*).
struct PlatformHttpRateLimit {
    bool have_units;
    bool have_requests;
    int units_limit;
    int units_remaining;
    int requests_limit;
    int requests_remaining;
};

// Synchronous GET with optional extra headers and raw HTTP status.
// extra_headers: nullptr, or a nullptr-terminated list of "Name: value"
// strings (libcurl CURLOPT_HTTPHEADER form).
// rate_limit: optional; when non-null, cleared then filled from response
// headers when present (any HTTP status, including 4xx/429).
// Returns true when an HTTP response was received (any status); false on
// transport failure. When true, *http_status is set (if non-null) and out
// receives the body (truncated to out_size-1). Callers that only want 2xx
// should keep using platform_http_get().
bool platform_http_get_ex(const char *url, char *out, size_t out_size, size_t *out_len,
                          long *http_status, const char *const *extra_headers,
                          PlatformHttpRateLimit *rate_limit = nullptr);

// --- Config storage ---
// Raw byte blob load/save, keyed by name — Linux maps this onto a JSON file
// under ~/.config/flightlevel314/.
bool platform_config_load(const char *key, void *buf, size_t buf_size, size_t *out_len);
bool platform_config_save(const char *key, const void *buf, size_t len);

// --- Log ---
// Levels: DEBUG < INFO < WARN < ERROR. Default minimum is INFO (quiet kiosk
// journal). WARN/ERROR go to stderr; DEBUG/INFO to stdout (both → journald).
typedef enum {
    PLATFORM_LOG_DEBUG = 0,
    PLATFORM_LOG_INFO  = 1,
    PLATFORM_LOG_WARN  = 2,
    PLATFORM_LOG_ERROR = 3,
} platform_log_level_t;

void platform_log_set_min_level(platform_log_level_t level);
platform_log_level_t platform_log_get_min_level(void);

void platform_log_at(platform_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define platform_log_debug(...) platform_log_at(PLATFORM_LOG_DEBUG, __VA_ARGS__)
#define platform_log_info(...)  platform_log_at(PLATFORM_LOG_INFO, __VA_ARGS__)
#define platform_log_warn(...)  platform_log_at(PLATFORM_LOG_WARN, __VA_ARGS__)
#define platform_log_error(...) platform_log_at(PLATFORM_LOG_ERROR, __VA_ARGS__)

// Compat: same as platform_log_info (prefer the leveled macros at new sites).
void platform_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// --- Compatibility shims ---
// Shared UI/data still call millis()/pdMS_TO_TICKS()/strlcpy() from the
// Arduino-era code; these stand in on Linux.
inline uint32_t millis() { return platform_millis(); }

#define pdMS_TO_TICKS(ms) (ms)

#if !defined(__APPLE__)
// glibc has no strlcpy — macOS libc does.
inline size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size) {
        size_t n = (len < size - 1) ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}
#endif
