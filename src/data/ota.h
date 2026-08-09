#pragma once

// Application updates via GitHub Releases — compare the compiled-in
// FIRMWARE_VERSION_STR (version.h) to the latest release tag, download the
// matching asset, and install (see pi/platform_linux/ota_linux.cpp).
//
// ota_poll() is near-instant when idle; it does real work only after an
// explicit ota_request_check() / ota_request_update().

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

// Request a version check or update -- both set a pending flag; the actual
// work happens inside ota_poll() (or the Pi background thread). 
// ota_request_update() is a no-op unless ota_status is already OTA_AVAILABLE.
void ota_request_check();
void ota_request_update();

void ota_poll();
