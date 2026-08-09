#pragma once

// ICAO ↔ name helpers over the generated airports_db[] (OurAirports
// large/medium). Safe to call when the DB header is missing — lookups
// return nullptr / 0 matches.

#include "airports_db_include.h"

#if !HAS_AIRPORTS_DB
// Stub so call sites compile without regenerating the DB.
struct StaticAirport {
    char icao[5];
    char alias[5];
    char name[64];
    float lat, lon;
    unsigned char large;
};
#endif

// Exact ICAO or alias match (case-insensitive). nullptr if unknown / DB absent.
const StaticAirport *airports_lookup_icao(const char *icao);

// Case-insensitive substring match on name/ICAO. Writes up to max_out
// pointers into out[]; returns the number written. Exact ICAO hits first.
int airports_search(const char *query, const StaticAirport **out, int max_out);

// Convenience: copy truncated name for an ICAO into buf, or "" if unknown.
void airports_format_name(const char *icao, char *buf, int buf_size);
