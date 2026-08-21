// Linux implementation of src/data/airlines.h — OpenTravelData airline table
// (ICAO 3-letter → name). Replaces the old hand-curated AirlinesCSV feed.

#include "../../src/data/airlines.h"
#include "../../src/data/error_log.h"
#include "../../src/platform/platform.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

#define AIRLINES_URL \
    "https://raw.githubusercontent.com/opentraveldata/opentraveldata/master/" \
    "opentraveldata/optd_airline_best_known_so_far.csv"

namespace {

std::mutex _mutex;
AirlineEntry _airlines[AIRLINES_MAX];
int _airline_count = 0;

// OPTD uses '^' as the field separator. Field indices:
// 0 pk, 2 validity_from, 3 validity_to, 4 3char_code (ICAO), 7 name
bool nth_field(const char *line, size_t len, int want, const char **out, size_t *out_len) {
    int idx = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || line[i] == '^') {
            if (idx == want) {
                *out = line + start;
                *out_len = i - start;
                return true;
            }
            idx++;
            start = i + 1;
        }
    }
    return false;
}

void today_utc_ymd(char *out, size_t out_sz) {
    time_t now = time(nullptr);
    struct tm tm_utc {};
    gmtime_r(&now, &tm_utc);
    snprintf(out, out_sz, "%04d-%02d-%02d",
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday);
}

bool code_already_loaded(const char *code) {
    for (int i = 0; i < _airline_count; i++) {
        if (strcmp(_airlines[i].code, code) == 0) return true;
    }
    return false;
}

// Parses one OPTD line. Skips header, expired rows, and duplicate ICAO codes.
void parse_line(const char *line, size_t len, const char *today) {
    if (len == 0 || _airline_count >= AIRLINES_MAX) return;
    // Header row starts with "pk^"
    if (len >= 3 && line[0] == 'p' && line[1] == 'k' && line[2] == '^') return;

    const char *code_f = nullptr, *name_f = nullptr, *vt_f = nullptr;
    size_t code_len = 0, name_len = 0, vt_len = 0;
    if (!nth_field(line, len, 4, &code_f, &code_len)) return;
    if (!nth_field(line, len, 7, &name_f, &name_len)) return;
    nth_field(line, len, 3, &vt_f, &vt_len); // optional

    if (code_len != 3 || name_len == 0) return;
    char code[4] = {};
    for (int i = 0; i < 3; i++) {
        unsigned char c = (unsigned char)code_f[i];
        if (!isalpha(c)) return;
        code[i] = (char)toupper(c);
    }
    // Drop expired designators (validity_to < today). Empty = still valid.
    if (vt_f && vt_len >= 10) {
        char vt[11] = {};
        memcpy(vt, vt_f, 10);
        if (strcmp(vt, today) < 0) return;
    }
    if (code_already_loaded(code)) return;

    AirlineEntry &e = _airlines[_airline_count];
    memset(&e, 0, sizeof(e));
    memcpy(e.code, code, 3);
    size_t copy_n = name_len < sizeof(e.name) - 1 ? name_len : sizeof(e.name) - 1;
    memcpy(e.name, name_f, copy_n);
    // Telephony callsign not in this OPTD file — leave empty.
    _airline_count++;
}

} // namespace

bool airlines_load() {
    // Upstream file is ~170–200 KB; leave headroom.
    std::vector<char> buf(512 * 1024);
    size_t total = 0;
    uint32_t t0 = platform_millis();
    if (!platform_http_get(AIRLINES_URL, buf.data(), buf.size(), &total)) {
        platform_log_warn("Airlines: HTTP fetch failed in %lums\n",
                     (unsigned long)(platform_millis() - t0));
        error_log_add("Airlines load failed");
        return false;
    }
    platform_log_debug("Airlines: HTTP OK (%zu bytes) in %lums\n",
                 total, (unsigned long)(platform_millis() - t0));

    char today[16];
    today_utc_ymd(today, sizeof(today));

    std::lock_guard<std::mutex> lock(_mutex);
    _airline_count = 0;
    size_t line_start = 0;
    for (size_t i = 0; i <= total; i++) {
        if (i == total || buf[i] == '\n') {
            size_t line_len = i - line_start;
            if (line_len > 0 && buf[line_start + line_len - 1] == '\r') line_len--;
            parse_line(buf.data() + line_start, line_len, today);
            line_start = i + 1;
        }
    }

    bool ok = _airline_count > 0;
    platform_log_info("Airlines: Loaded %d entries (OpenTravelData)\n", _airline_count);
    if (!ok) error_log_add("Airlines load failed (empty table)");
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
    // Prefer exact 3-letter ICAO match (ADS-B airline callsigns).
    if (plen == 3) {
        for (int i = 0; i < _airline_count; i++) {
            if (strcmp(prefix, _airlines[i].code) == 0) return &_airlines[i];
        }
        return nullptr;
    }
    // Rare 2-letter prefix: only match if the table entry is also 2 chars.
    for (int i = 0; i < _airline_count; i++) {
        if (_airlines[i].code[2] == '\0' && strcmp(prefix, _airlines[i].code) == 0)
            return &_airlines[i];
    }
    return nullptr;
}
