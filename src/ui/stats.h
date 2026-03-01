#pragma once
#include "../data/aircraft.h"

struct SessionStats {
    // Live snapshot (recalculated each update)
    int current_count;
    int jets;
    int ga;
    int heli;
    int military;
    int emergency;
    int alt_gnd;
    int alt_low;       // < 5000
    int alt_med_low;   // < 15000
    int alt_med;       // < 25000
    int alt_high;      // < 35000
    int alt_very_high; // >= 35000
    char fastest_callsign[9];
    int fastest_speed;
    char closest_callsign[9];
    float closest_dist;

    // Session totals (accumulated since boot)
    int unique_seen;
    int peak_count;
    uint32_t boot_time;

    // Top 5 types
    struct TypeCount {
        char type[5];
        int count;
    };
    TypeCount top_types[5];
};

void stats_init();
void stats_update(AircraftList *list);
const SessionStats* stats_get();
