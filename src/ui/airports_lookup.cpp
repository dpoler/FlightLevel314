#include "airports_lookup.h"

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

// Drop common trailing words so a fallback place label stays readable at
// half-width ("John F. Kennedy International Airport" → "John F. Kennedy").
static void shorten_airport_name(const char *full, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!full || !full[0]) return;
    static const char *const SUFFIXES[] = {
        " International Airport",
        " International",
        " Regional Airport",
        " Municipal Airport",
        " Airport",
        " Airfield",
        " Air Base",
        " AFB",
        nullptr
    };
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", full);
    for (int i = 0; SUFFIXES[i]; i++) {
        size_t bl = strlen(buf);
        size_t sl = strlen(SUFFIXES[i]);
        if (bl > sl && strcmp(buf + bl - sl, SUFFIXES[i]) == 0) {
            buf[bl - sl] = '\0';
            break;
        }
    }
    size_t n = strlen(buf);
    while (n > 0 && buf[n - 1] == ' ') buf[--n] = '\0';
    snprintf(out, out_sz, "%s", buf[0] ? buf : full);
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
    const StaticAirport *ap = airports_lookup_icao(icao);
    if (!ap) return;
    if (ap->municipality[0]) {
        strncpy(buf, ap->municipality, (size_t)buf_size - 1);
        buf[buf_size - 1] = '\0';
        return;
    }
    if (ap->name[0]) {
        shorten_airport_name(ap->name, buf, (size_t)buf_size);
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
