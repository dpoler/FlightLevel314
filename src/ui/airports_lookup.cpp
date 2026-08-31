#include "airports_lookup.h"
#include "airport_display_names.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#if HAS_AIRPORTS_DB

static void icao_upper(const char *in, char *out, int out_sz) {
    int i = 0;
    for (; in && in[i] && i < out_sz - 1; i++)
        out[i] = (char)toupper((unsigned char)in[i]);
    out[i] = '\0';
}

static bool str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
        ++a; ++b;
    }
    return !*a && !*b;
}

static bool str_istr(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return true;
    if (!hay) return false;
    for (const char *h = hay; *h; ++h) {
        const char *a = h, *b = needle;
        while (*a && *b &&
               toupper((unsigned char)*a) == toupper((unsigned char)*b)) {
            ++a; ++b;
        }
        if (!*b) return true;
    }
    return false;
}

static bool word_boundary_before(const char *s, size_t pos) {
    return pos == 0 || !isalnum((unsigned char)s[pos - 1]);
}

static bool word_boundary_after(const char *s, size_t pos, size_t word_len) {
    char c = s[pos + word_len];
    return c == '\0' || !isalnum((unsigned char)c);
}

// Replace whole-word `from` with `to` in-place (buf sized buflen).
static void replace_word(char *buf, size_t buflen, const char *from, const char *to) {
    if (!buf || !from || !to || !from[0]) return;
    size_t from_len = strlen(from);
    size_t to_len = strlen(to);
    char tmp[160];
    size_t out = 0;
    size_t i = 0;
    size_t n = strlen(buf);
    while (i < n && out + 1 < sizeof(tmp)) {
        if (word_boundary_before(buf, i) &&
            i + from_len <= n &&
            strncmp(buf + i, from, from_len) == 0 &&
            word_boundary_after(buf, i, from_len)) {
            if (out + to_len >= sizeof(tmp)) break;
            memcpy(tmp + out, to, to_len);
            out += to_len;
            i += from_len;
        } else {
            tmp[out++] = buf[i++];
        }
    }
    tmp[out] = '\0';
    snprintf(buf, buflen, "%s", tmp);
}

// Cut at the first whole-word Airport / Aerodrome / Field (drop that word
// and after). "Field" is US-common for the same role as Airport; curated
// overrides cover cases where Field is the *brand* (Love Field) or a
// surname mid-name (James T. Field Memorial).
static void cut_at_airport_word(char *buf) {
    if (!buf || !buf[0]) return;
    static const char *const CUT[] = {"Airport", "Aerodrome", "Field", nullptr};
    size_t n = strlen(buf);
    size_t best = n;
    for (int c = 0; CUT[c]; c++) {
        size_t wlen = strlen(CUT[c]);
        for (size_t i = 0; i + wlen <= n; i++) {
            if (!word_boundary_before(buf, i)) continue;
            if (strncmp(buf + i, CUT[c], wlen) != 0) continue;
            if (!word_boundary_after(buf, i, wlen)) continue;
            if (i < best) best = i;
            break; // first occurrence of this word; still scan other CUT words
        }
    }
    if (best < n) {
        // Trim spaces/punctuation just before the cut.
        while (best > 0 && (buf[best - 1] == ' ' || buf[best - 1] == '-' ||
                            buf[best - 1] == ',' || buf[best - 1] == '/')) {
            best--;
        }
        buf[best] = '\0';
    }
}

// Abbreviate type words, then drop Airport/Aerodrome and everything after.
static void abbreviate_airport_name(const char *full, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!full || !full[0]) return;

    char buf[160];
    snprintf(buf, sizeof(buf), "%s", full);
    replace_word(buf, sizeof(buf), "International", "Int'l");
    replace_word(buf, sizeof(buf), "Regional", "Rgl");
    replace_word(buf, sizeof(buf), "Municipal", "Muni");
    replace_word(buf, sizeof(buf), "National", "Nat'l");
    cut_at_airport_word(buf);

    // Trim trailing junk left by abbreviation-only names.
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '-' ||
                     buf[n - 1] == ',' || buf[n - 1] == '/')) {
        buf[--n] = '\0';
    }
    snprintf(out, out_sz, "%s", buf[0] ? buf : full);
}

// LVGL LABEL_LONG_DOT wraps on whitespace. Names and overrides often glue
// city/airport with '/' and no spaces ("Sao Paulo/Guarulhos…",
// "Montreal/Trudeau"), so the slash-joined token never breaks. Insert spaces
// around '/' so wrap can split there (e.g. "Sao Paulo / Guarulhos …").
static void loosen_slash_breaks(char *buf, size_t buflen) {
    if (!buf || !buf[0] || buflen < 2) return;
    char tmp[160];
    size_t o = 0;
    for (size_t i = 0; buf[i] && o + 1 < sizeof(tmp); i++) {
        if (buf[i] == '/') {
            if (o > 0 && tmp[o - 1] != ' ') {
                if (o + 1 >= sizeof(tmp)) break;
                tmp[o++] = ' ';
            }
            if (o + 1 >= sizeof(tmp)) break;
            tmp[o++] = '/';
            if (buf[i + 1] != '\0' && buf[i + 1] != ' ') {
                if (o + 1 >= sizeof(tmp)) break;
                tmp[o++] = ' ';
            }
        } else {
            tmp[o++] = buf[i];
        }
    }
    tmp[o] = '\0';
    snprintf(buf, buflen, "%s", tmp);
}

static const char *display_override_for_icao(const char *icao_key) {
    // Linear scan is fine for a few dozen curated entries.
    for (int i = 0; i < k_airport_display_override_count; i++) {
        if (str_ieq(k_airport_display_overrides[i].icao, icao_key))
            return k_airport_display_overrides[i].label;
    }
    return nullptr;
}

const StaticAirport *airports_lookup_icao(const char *icao) {
    if (!icao || !icao[0]) return nullptr;
    char key[8];
    icao_upper(icao, key, sizeof(key));
    for (int i = 0; i < AIRPORTS_DB_COUNT; i++) {
        if (str_ieq(airports_db[i].icao, key)) return &airports_db[i];
        if (airports_db[i].alias[0] && str_ieq(airports_db[i].alias, key))
            return &airports_db[i];
    }
    return nullptr;
}

int airports_search(const char *query, const StaticAirport **out, int max_out) {
    if (!query || !query[0] || !out || max_out <= 0) return 0;
    while (*query && isspace((unsigned char)*query)) ++query;
    if (!*query) return 0;

    char key[48];
    int ki = 0;
    for (; query[ki] && ki < (int)sizeof(key) - 1; ki++)
        key[ki] = query[ki];
    key[ki] = '\0';
    while (ki > 0 && isspace((unsigned char)key[ki - 1])) key[--ki] = '\0';

    int n = 0;
    bool maybe_icao = (ki >= 3 && ki <= 4);
    if (maybe_icao) {
        for (int i = 0; i < ki; i++) {
            if (!isalnum((unsigned char)key[i])) { maybe_icao = false; break; }
        }
    }
    if (maybe_icao) {
        const StaticAirport *exact = airports_lookup_icao(key);
        if (exact) out[n++] = exact;
    }

    for (int i = 0; i < AIRPORTS_DB_COUNT && n < max_out; i++) {
        if (maybe_icao && n > 0 && out[0] == &airports_db[i]) continue;
        if (str_istr(airports_db[i].name, key) ||
            str_istr(airports_db[i].municipality, key) ||
            str_istr(airports_db[i].icao, key) ||
            (airports_db[i].alias[0] && str_istr(airports_db[i].alias, key))) {
            out[n++] = &airports_db[i];
        }
    }
    return n;
}

void airports_format_name(const char *icao, char *buf, int buf_size) {
    if (!buf || buf_size <= 0) return;
    buf[0] = '\0';
    const StaticAirport *ap = airports_lookup_icao(icao);
    if (ap && ap->name[0]) {
        strncpy(buf, ap->name, (size_t)buf_size - 1);
        buf[buf_size - 1] = '\0';
    }
}

void airports_format_place(const char *icao, char *buf, int buf_size) {
    if (!buf || buf_size <= 0) return;
    buf[0] = '\0';
    if (!icao || !icao[0]) return;

    char key[8];
    icao_upper(icao, key, sizeof(key));

    // Hosts-style override keyed by the caller's ICAO *or* the DB primary
    // when the caller used an alias.
    const char *ovr = display_override_for_icao(key);
    const StaticAirport *ap = airports_lookup_icao(key);
    if (!ovr && ap && ap->icao[0] && !str_ieq(ap->icao, key))
        ovr = display_override_for_icao(ap->icao);
    if (ovr) {
        strncpy(buf, ovr, (size_t)buf_size - 1);
        buf[buf_size - 1] = '\0';
        loosen_slash_breaks(buf, (size_t)buf_size);
        return;
    }

    if (ap && ap->name[0]) {
        abbreviate_airport_name(ap->name, buf, (size_t)buf_size);
        loosen_slash_breaks(buf, (size_t)buf_size);
    }
}

#else

const StaticAirport *airports_lookup_icao(const char *icao) {
    (void)icao;
    return nullptr;
}

int airports_search(const char *query, const StaticAirport **out, int max_out) {
    (void)query; (void)out; (void)max_out;
    return 0;
}

void airports_format_name(const char *icao, char *buf, int buf_size) {
    (void)icao;
    if (buf && buf_size > 0) buf[0] = '\0';
}

void airports_format_place(const char *icao, char *buf, int buf_size) {
    (void)icao;
    if (buf && buf_size > 0) buf[0] = '\0';
}

#endif
