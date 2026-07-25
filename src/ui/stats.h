#pragma once
#include "../data/aircraft.h"

struct SessionStats {
    // Live snapshot -- rebuilt from scratch on every stats_update() call,
    // from whichever aircraft are currently visible. Not accumulated over
    // time; describes only "right now" (see stats_view.cpp's "RIGHT NOW"
    // column).
    int current_count;
    int jets;
    int ga;
    int heli;
    int military;
    int emergency;
    int alt_low;       // < 5000
    int alt_med_low;   // < 15000
    int alt_med;       // < 25000
    int alt_high;      // < 35000
    int alt_very_high; // >= 35000

    // Session records -- the most extreme value seen since the active
    // location was last switched (see stats_view.cpp's "THIS LOCATION"
    // column), not the current instant. An aircraft that already left the
    // area can still be the record-holder here until the next switch.
    char fastest_callsign[9];
    int fastest_speed;
    char slowest_callsign[9];
    int slowest_speed;
    char highest_callsign[9];
    int32_t highest_alt;
    char lowest_callsign[9];
    int32_t lowest_alt;
    char closest_callsign[9];
    float closest_dist;

    // Session totals -- accumulated since the active location was last
    // switched, despite the name ("session" here means "time spent viewing
    // this location," not "since boot").
    int unique_seen;
    int peak_count;

    // Genuinely global -- never reset by a location switch.
    uint32_t boot_time;

    // Speed distribution
    int spd_slow;      // < 200kt
    int spd_med;       // < 300kt
    int spd_fast;      // < 400kt
    int spd_very_fast; // < 500kt
    int spd_extreme;   // >= 500kt

    // Top 5 types
    struct TypeCount {
        char type[5];
        int count;
    };
    TypeCount top_types[5];

    // Top 5 airlines (3-letter ICAO prefix)
    struct AirlineCount {
        char code[4];
        int count;
    };
    AirlineCount top_airlines[5];
};

void stats_init();
void stats_update(AircraftList *list);
const SessionStats* stats_get();
