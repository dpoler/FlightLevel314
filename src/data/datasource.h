#pragma once
#include "aircraft.h"

// Data source abstraction -- fetch aircraft without callers knowing the
// backend. RemoteApiDataSource talks to adsb.lol or adsb.fi (see
// UserConfig::traffic_provider / pi/platform_linux/datasource_remote.cpp).
// LocalSdrDataSource is a stub for a future RTL-SDR + dump1090/readsb feed.

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
    const char *name() const override;
};

// Stub only -- not implemented.
class LocalSdrDataSource : public AircraftDataSource {
public:
    bool fetch(AircraftList *) override { return false; }
    const char *name() const override { return "local-sdr (stub)"; }
};
