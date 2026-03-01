#include <Arduino.h>
#include "stats.h"
#include "geo.h"
#include "../config.h"
#include <cstring>
#include <cstdlib>

static SessionStats _stats;

// Simple hash set for unique ICAO tracking
#define MAX_UNIQUE 2000
static char _seen_icaos[MAX_UNIQUE][7];
static int _seen_count = 0;

static bool already_seen(const char *icao) {
    for (int i = 0; i < _seen_count; i++) {
        if (strcmp(_seen_icaos[i], icao) == 0) return true;
    }
    return false;
}

static void mark_seen(const char *icao) {
    if (_seen_count >= MAX_UNIQUE || already_seen(icao)) return;
    strlcpy(_seen_icaos[_seen_count], icao, 7);
    _seen_count++;
}

static bool is_airline_callsign(const char *cs) {
    return cs[0] >= 'A' && cs[0] <= 'Z' &&
           cs[1] >= 'A' && cs[1] <= 'Z' &&
           cs[2] >= 'A' && cs[2] <= 'Z' &&
           cs[3] >= '0' && cs[3] <= '9';
}

static bool is_heli_type(const char *t) {
    static const char *types[] = {
        "R22", "R44", "R66", "EC35", "EC45", "EC55",
        "A109", "A139", "A169", "B06", "B212", "B412",
        "S76", "S92", "B407", "B429", "B505",
        "H135", "H145", "H160", "H175", "H225",
        "AS50", "AS55", "AS65", "MD52", "MD60",
        "NH90", "CH47", "V22", "UH1", "BK17",
        nullptr
    };
    for (int i = 0; types[i]; i++) {
        if (strcmp(t, types[i]) == 0) return true;
    }
    return false;
}

#define MAX_TYPE_TRACK 100
struct TypeTracker {
    char type[5];
    int count;
};
static TypeTracker _type_counts[MAX_TYPE_TRACK];
static int _type_track_count = 0;

static void track_type(const char *type) {
    if (!type[0]) return;
    for (int i = 0; i < _type_track_count; i++) {
        if (strcmp(_type_counts[i].type, type) == 0) {
            _type_counts[i].count++;
            return;
        }
    }
    if (_type_track_count < MAX_TYPE_TRACK) {
        strlcpy(_type_counts[_type_track_count].type, type, 5);
        _type_counts[_type_track_count].count = 1;
        _type_track_count++;
    }
}

static void compute_top_types() {
    for (int i = 0; i < 5 && i < _type_track_count; i++) {
        int max_idx = i;
        for (int j = i + 1; j < _type_track_count; j++) {
            if (_type_counts[j].count > _type_counts[max_idx].count) max_idx = j;
        }
        if (max_idx != i) {
            TypeTracker tmp = _type_counts[i];
            _type_counts[i] = _type_counts[max_idx];
            _type_counts[max_idx] = tmp;
        }
        strlcpy(_stats.top_types[i].type, _type_counts[i].type, 5);
        _stats.top_types[i].count = _type_counts[i].count;
    }
}

void stats_init() {
    memset(&_stats, 0, sizeof(_stats));
    _stats.boot_time = millis();
    _stats.closest_dist = 9999.0f;
    _seen_count = 0;
    _type_track_count = 0;
}

void stats_update(AircraftList *list) {
    if (!list->lock(pdMS_TO_TICKS(50))) return;

    _stats.current_count = 0;
    _stats.jets = 0;
    _stats.ga = 0;
    _stats.heli = 0;
    _stats.military = 0;
    _stats.emergency = 0;
    _stats.alt_gnd = 0;
    _stats.alt_low = 0;
    _stats.alt_med_low = 0;
    _stats.alt_med = 0;
    _stats.alt_high = 0;
    _stats.alt_very_high = 0;
    _stats.fastest_speed = 0;
    _stats.closest_dist = 9999.0f;
    _stats.fastest_callsign[0] = 0;
    _stats.closest_callsign[0] = 0;

    uint32_t now = millis();

    for (int i = 0; i < list->count; i++) {
        Aircraft &ac = list->aircraft[i];
        if (compute_aircraft_opacity(ac.stale_since, now) == 0) continue;
        if (ac.lat == 0 && ac.lon == 0) continue;

        _stats.current_count++;
        mark_seen(ac.icao_hex);
        track_type(ac.type_code);

        if (ac.is_military) _stats.military++;
        if (ac.is_emergency) _stats.emergency++;

        bool is_h = (ac.category[0] == 'A' && ac.category[1] == '7') ||
                    (ac.type_code[0] && is_heli_type(ac.type_code));
        bool is_j = !is_h && (is_airline_callsign(ac.callsign) ||
                   (ac.category[0] == 'A' && ac.category[1] >= '3'));

        if (is_h) _stats.heli++;
        else if (is_j) _stats.jets++;
        else _stats.ga++;

        if (ac.on_ground) _stats.alt_gnd++;
        else if (ac.altitude < 5000) _stats.alt_low++;
        else if (ac.altitude < 15000) _stats.alt_med_low++;
        else if (ac.altitude < 25000) _stats.alt_med++;
        else if (ac.altitude < 35000) _stats.alt_high++;
        else _stats.alt_very_high++;

        if (ac.speed > _stats.fastest_speed) {
            _stats.fastest_speed = ac.speed;
            strlcpy(_stats.fastest_callsign,
                    ac.callsign[0] ? ac.callsign : ac.icao_hex, 9);
        }

        float d = MapProjection::distance_nm(HOME_LAT, HOME_LON, ac.lat, ac.lon);
        if (d < _stats.closest_dist) {
            _stats.closest_dist = d;
            strlcpy(_stats.closest_callsign,
                    ac.callsign[0] ? ac.callsign : ac.icao_hex, 9);
        }
    }

    if (_stats.current_count > _stats.peak_count) {
        _stats.peak_count = _stats.current_count;
    }
    _stats.unique_seen = _seen_count;
    compute_top_types();

    list->unlock();
}

const SessionStats* stats_get() {
    return &_stats;
}
