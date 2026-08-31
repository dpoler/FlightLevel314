// Linux implementation of src/data/airlines.h — OpenTravelData airline table
// (ICAO 3-letter → name). Replaces the old hand-curated AirlinesCSV feed.

#include "../../src/data/airlines.h"
#include "../../src/data/error_log.h"
#include "../../src/platform/platform.h"
#include <cctype>
#include <cstdint>
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

// Montserrat (and our other UI fonts) are ASCII-only. Fold accents then drop
// anything outside 0x20–0x7E so "LATAM Perú" becomes "LATAM Peru" instead of
// "LATAM Per" + a missing-glyph box. Handles multi-byte UTF-8 in OPTD names.
void copy_name_ascii(char *dst, size_t dst_sz, const char *src, size_t src_len) {
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src || src_len == 0) return;

    size_t out = 0;
    size_t i = 0;
    while (i < src_len && out + 1 < dst_sz) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x80) {
            if (c >= 0x20 && c < 0x7F) dst[out++] = (char)c;
            i++;
            continue;
        }
        // Decode one UTF-8 codepoint, NFKD-ish via a tiny Latin-1 / common
        // supplement fold table for airline names (é→e, ø→o, …).
        uint32_t cp = 0;
        int need = 0;
        if ((c & 0xE0) == 0xC0 && i + 1 < src_len) {
            need = 2;
            cp = (c & 0x1F);
        } else if ((c & 0xF0) == 0xE0 && i + 2 < src_len) {
            need = 3;
            cp = (c & 0x0F);
        } else if ((c & 0xF8) == 0xF0 && i + 3 < src_len) {
            need = 4;
            cp = (c & 0x07);
        } else {
            i++; // invalid lead — skip
            continue;
        }
        bool ok = true;
        for (int k = 1; k < need; k++) {
            unsigned char cc = (unsigned char)src[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { i++; continue; }
        i += (size_t)need;

        // Common accented Latin letters → ASCII base (NFC forms from OPTD).
        auto base = [](uint32_t u) -> char {
            if (u >= 'A' && u <= 'Z') return (char)u;
            if (u >= 'a' && u <= 'z') return (char)u;
            // Latin-1 supplement + a few extras seen in OPTD airline names.
            switch (u) {
            case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5:
            case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5:
                return (u < 0x00E0) ? 'A' : 'a';
            case 0x00C7: case 0x00E7: return (u < 0x00E0) ? 'C' : 'c';
            case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB:
            case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB:
                return (u < 0x00E0) ? 'E' : 'e';
            case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF:
            case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF:
                return (u < 0x00E0) ? 'I' : 'i';
            case 0x00D1: case 0x00F1: return (u < 0x00E0) ? 'N' : 'n';
            case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: case 0x00D8:
            case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8:
                return (u < 0x00E0) ? 'O' : 'o';
            case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC:
            case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC:
                return (u < 0x00E0) ? 'U' : 'u';
            case 0x00DD: case 0x00FD: case 0x00FF: return (u == 0x00DD) ? 'Y' : 'y';
            case 0x00DF: return 's'; // ß → s (good enough for UI)
            case 0x0152: return 'O'; // Œ
            case 0x0153: return 'o'; // œ
            case 0x0178: return 'Y'; // Ÿ
            default: return 0;
            }
        };
        char ch = base(cp);
        if (ch) dst[out++] = ch;
    }
    dst[out] = '\0';
    // Collapse runs of spaces left by dropped glyphs.
    size_t r = 0, w = 0;
    bool sp = false;
    while (dst[r]) {
        if (dst[r] == ' ') {
            if (!sp && w > 0) dst[w++] = ' ';
            sp = true;
        } else {
            dst[w++] = dst[r];
            sp = false;
        }
        r++;
    }
    while (w > 0 && dst[w - 1] == ' ') w--;
    dst[w] = '\0';
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
    copy_name_ascii(e.name, sizeof(e.name), name_f, name_len);
    if (!e.name[0]) return; // nothing left after folding
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
