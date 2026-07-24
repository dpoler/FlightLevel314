#pragma once
#include <cstddef>
#include "aircraft.h"

#define MAX_LOCATIONS 15   // saved airports, not counting Home
#define MAX_RUNWAYS   12   // KORD has exactly 8 active runways (11 total, 3
                            // closed) -- was capped at 8, right at the edge;
                            // bumped for headroom now that closed runways are
                            // filtered out during parsing (locations.cpp)
#define LOC_ICAO_LEN  8

// Safety ceiling for cached nearby LARGE airports per location (see the
// nearby-runways declarations below). Large-only rarely approaches this even
// in dense metro areas (KJFK at 50nm has ~20 airports total, but only a
// handful are classified `large_airport` -- JFK/LGA/EWR-class) -- this just
// bounds the pathological case.
#define NEARBY_MAX 20

struct LocRunway {
    float le_lat, le_lon;
    float he_lat, he_lon;
    char le_id[4];
    char he_id[4];
};

struct Location {
    char icao[LOC_ICAO_LEN];
    float lat, lon;
    int elevation_ft;
    LocRunway runways[MAX_RUNWAYS];
    int runway_count;
    bool nearby_enabled; // "show nearby large airports' runways" toggle
    int nearby_count;    // cached count -- cheap to read for any row's badge
                          // without loading the actual runway data (see
                          // locations_nearby_count())
};

// Home is not stored here — it's the existing g_config.home_lat/home_lon,
// treated as the implicit location at index -1 everywhere in this API.

// Load saved airports from NVS. Call once at boot.
void locations_init();

int locations_count();                     // number of saved (non-home) airports
const Location* locations_get(int idx);    // idx in [0, locations_count())

// Fetch runway/elevation data for `icao` from airportdb.io and persist it.
// Blocking network call — call from a background task, not the UI thread.
// Returns true on success; on failure, if err is non-null, writes a short reason.
bool locations_add_from_icao(const char *icao, char *err, size_t err_size);

// Request/response pair for adding a location from the UI thread without
// spawning a new task — a dedicated task's stack was enough extra internal-DRAM
// pressure to crash the SDIO driver on this board (see
// project_p4_heap_constraints memory). Instead: the UI calls
// locations_request_add(), and locations_add_poll() — called from
// location_poll_task's existing loop in fetcher.cpp — picks it up and does the
// actual fetch on that task's already-allocated stack.
void locations_request_add(const char *icao);
void locations_add_poll();
// Non-blocking: returns true (and clears the pending result) if an add
// completed since the last call. *ok/err are only valid when this returns true.
bool locations_add_result(bool *ok, char *err, size_t err_size);

void locations_remove(int idx);

// Moves the location at `from` to position `to`, shifting everything between
// them by one slot (same semantics as a list drag-to-reorder). No-op if
// either index is out of range or they're equal. The active selection (if
// any) is remapped to keep pointing at the same airport, not the same slot.
void locations_reorder(int from, int to);

// Currently selected location. -1 = Home.
int locations_active_index();
void locations_set_active(int idx);

// Convenience — resolves the active selection (Home or a saved Location) into
// a lat/lon/elevation triple. Returns false if idx is out of range.
bool locations_get_active_coords(float *lat, float *lon, int *elevation_ft);

// Resolves the active selection into whichever AircraftList currently holds
// its data: `home_list` when Home is active, or the shared on-demand list
// (fetcher_location_list()) when a saved airport is active. Views should call
// this each time they need the list, rather than caching the result — the
// active location can change out from under them at any time via the picker.
AircraftList* locations_active_list(AircraftList *home_list);

// "Nearby large airports" cache: draws full runway geometry (not just a
// glyph) for large airports near a saved location or Home, on top of that
// location's own runways. idx follows locations_active_index()'s convention:
// -1 = Home, [0, locations_count()) = saved. Large airports only (from the
// static DB's `large` flag) -- medium airports stay glyph-only. A dense
// 50nm radius (e.g. KJFK) can have ~20 airports total, but rarely more than
// a handful of LARGE ones, which is both the cheaper set to fetch and the
// one worth a full runway diagram at a glance.
//
// Fetched once, at the moment the toggle is first turned on -- same
// fetch-and-cache-at-a-discrete-action reasoning as locations_add_from_icao()
// itself, not lazily as airports scroll into view, so it never needs to
// re-fetch across zoom levels. Only the *active* location's cache is ever
// resident in DRAM at once (loaded from NVS on demand) -- embedding it
// inline in all MAX_LOCATIONS slots at all times would multiply DRAM use by
// 15x for data that's only ever drawn for whichever one location is active.
bool locations_nearby_enabled(int idx);
int locations_nearby_count(int idx);                  // cheap: persisted header only, safe for any row (e.g. the picker's "+N nearby" badge)
void locations_nearby_set_enabled(int idx, bool on);   // turning on triggers a fetch if nothing's cached yet; turning off just stops drawing it (cached data stays on disk)

// Currently active location's cached nearby-airport list (lazily (re)loaded
// from NVS whenever the active location changes). Empty if the toggle is off
// or nothing's been fetched yet.
const Location* locations_nearby_get_active(int *count);

// Poll counterpart to locations_add_poll() -- call from the same
// location_poll_task loop. Drains one queued nearby-airport fetch per call.
void locations_nearby_poll();
