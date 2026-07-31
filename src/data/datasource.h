#pragma once
#include "aircraft.h"

// Data source abstraction -- lets the app fetch aircraft data from
// different backends without callers needing to know which. Today only
// RemoteApiDataSource (adsb.lol) is real, implemented Pi-only for now (see
// pi/platform_linux/datasource_remote.cpp -- calls platform_http_get(),
// not yet implemented on the ESP32 side). LocalSdrDataSource is a
// deliberate stub, wired now so a future RTL-SDR + dump1090/readsb local
// feed is a new class later, not a fetcher rewrite. See project_pi_port
// memory.
//
// jc1060 doesn't use this yet -- src/data/fetcher.cpp's existing WiFi/C6-
// co-processor recovery loop is untouched, deliberately: it's exactly the
// kind of hard-won, extensively-commented hardware recovery code (see
// project_p4_heap_constraints / project_platform_pin memories) not worth
// risking a shared-abstraction refactor on for this port. This header is
// safe to sit in src/data/ regardless -- it's declarations only, so it
// costs nothing at ESP32 link time as long as nothing there instantiates
// RemoteApiDataSource or calls its fetch().

class AircraftDataSource {
public:
    virtual ~AircraftDataSource() = default;
    // Fetches once, merges results into list. Returns true on success.
    virtual bool fetch(AircraftList *list) = 0;
    virtual const char *name() const = 0;
};

class RemoteApiDataSource : public AircraftDataSource {
public:
    bool fetch(AircraftList *list) override;
    const char *name() const override { return "adsb.lol"; }
};

// Stub only -- not implemented. See project_pi_port memory.
class LocalSdrDataSource : public AircraftDataSource {
public:
    bool fetch(AircraftList *) override { return false; }
    const char *name() const override { return "local-sdr (stub)"; }
};
