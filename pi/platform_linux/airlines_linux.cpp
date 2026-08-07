// Linux implementation of src/data/airlines.h -- same CSV source and
// parse rules as src/data/airlines.cpp, but via platform_http_get()
// (libcurl) instead of Arduino HTTPClient. Loaded once at app start
// from pi/main.cpp on a background thread (Pi has no fetcher_init
// boot task the way ESP32 does).

#include "../../src/data/airlines.h"
#include "../../src/data/error_log.h"
#include "../../src/platform/platform.h"
#include <cstring>
#include <mutex>
#include <vector>

#define AIRLINES_URL "https://raw.githubusercontent.com/dpoler/AirlinesCSV/main/airlines.csv"

namespace {

std::mutex _mutex;
AirlineEntry _airlines[AIRLINES_MAX];
int _airline_count = 0;

// Parses one CSV line: ICAO,Name[,Callsign]  (# comments and blank lines skipped)
void parse_line(const char *line, size_t len) {
    if (len == 0 || line[0] == '#' || _airline_count >= AIRLINES_MAX) return;

    const char *c1 = (const char *)memchr(line, ',', len);
    if (!c1) return;
    size_t icao_len = (size_t)(c1 - line);
    if (icao_len < 2 || icao_len > 3) return;

    const char *rest = c1 + 1;
    size_t rest_len = len - icao_len - 1;
    const char *c2 = (const char *)memchr(rest, ',', rest_len);
    size_t name_len = c2 ? (size_t)(c2 - rest) : rest_len;
    if (name_len == 0) return;

    AirlineEntry &e = _airlines[_airline_count];
    memset(&e, 0, sizeof(e));
    memcpy(e.code, line, icao_len < 3 ? icao_len : 3);
    memcpy(e.name, rest, name_len < 25 ? name_len : 25);
    if (c2) {
        const char *cs = c2 + 1;
        size_t cs_len = rest_len - name_len - 1;
        memcpy(e.callsign, cs, cs_len < 15 ? cs_len : 15);
    }
    _airline_count++;
}

} // namespace

bool airlines_load() {
    // ~180 lines today; 64KB leaves plenty of headroom if the CSV grows.
    std::vector<char> buf(64 * 1024);
    size_t total = 0;
    uint32_t t0 = platform_millis();
    if (!platform_http_get(AIRLINES_URL, buf.data(), buf.size(), &total)) {
        platform_log("[Airlines] HTTP fetch failed in %lums\n",
                     (unsigned long)(platform_millis() - t0));
        error_log_add("Airlines load failed");
        return false;
    }
    platform_log("[Airlines] HTTP OK (%zu bytes) in %lums\n",
                 total, (unsigned long)(platform_millis() - t0));

    std::lock_guard<std::mutex> lock(_mutex);
    _airline_count = 0;
    size_t line_start = 0;
    for (size_t i = 0; i <= total; i++) {
        if (i == total || buf[i] == '\n') {
            size_t line_len = i - line_start;
            if (line_len > 0 && buf[line_start + line_len - 1] == '\r') line_len--;
            parse_line(buf.data() + line_start, line_len);
            line_start = i + 1;
        }
    }

    bool ok = _airline_count > 0;
    platform_log("[Airlines] Loaded %d entries\n", _airline_count);
    if (!ok) error_log_add("Airlines load failed (empty CSV)");
    return ok;
}

const AirlineEntry *airline_lookup(const char *callsign) {
    if (!callsign) return nullptr;
    char prefix[4] = {0, 0, 0, 0};
    int plen = 0;
    while (plen < 3 && callsign[plen] >= 'A' && callsign[plen] <= 'Z') {
        prefix[plen] = callsign[plen];
        plen++;
    }
    if (plen < 2) return nullptr;

    std::lock_guard<std::mutex> lock(_mutex);
    for (int i = 0; i < _airline_count; i++) {
        if (strcmp(prefix, _airlines[i].code) == 0) return &_airlines[i];
    }
    return nullptr;
}
