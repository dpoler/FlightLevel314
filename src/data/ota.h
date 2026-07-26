#pragma once

// Application-firmware OTA updates via GitHub Releases -- checks the repo's
// latest release tag against the compiled-in FIRMWARE_VERSION_STR
// (version.h), downloads the matching board-specific release asset, and
// flashes it to the inactive OTA partition (partitions.csv now has two,
// app0/app1, added specifically for this). Does NOT touch the ESP32-C6
// co-processor's own firmware -- that's a deliberately separate, harder
// problem (see project_platform_pin/project_p4_heap_constraints memory for
// why) and out of scope here.
//
// Driven by ota_poll(), called from location_poll_task's existing loop
// (fetcher.cpp) -- not a dedicated FreeRTOS task, even though the reference
// implementation this was adapted from (dpoler/FlightRadarCYD) uses one.
// New tasks doing network work already crashed this board's SDIO driver
// once (project_p4_heap_constraints memory) -- same reasoning that already
// moved enrichment.cpp off a per-tap task and onto this same poll loop.
// ota_poll() is near-instant when idle; it blocks for the duration of an
// actual check or download when one has been requested, which is
// acceptable since both are rare, explicitly user-triggered events, not
// routine/automatic ones -- unlike a per-tap enrichment fetch, an OTA check
// or update happening a few seconds later than the ideal instant makes no
// practical difference.

enum OtaStatus {
    OTA_IDLE,
    OTA_CHECKING,
    OTA_UP_TO_DATE,
    OTA_AVAILABLE,
    OTA_DOWNLOADING,
    OTA_DONE,
    OTA_ERROR,
};

extern volatile OtaStatus ota_status;
extern char ota_latest_tag[16];
extern volatile int ota_progress; // 0-100, meaningful only while OTA_DOWNLOADING

// Request a version check or update -- both just set a pending flag; the
// actual work happens inside ota_poll(). ota_request_update() is a no-op
// unless ota_status is already OTA_AVAILABLE (i.e. a check has already
// found a newer release).
void ota_request_check();
void ota_request_update();

// Called every location_poll_task tick (fetcher.cpp).
void ota_poll();
