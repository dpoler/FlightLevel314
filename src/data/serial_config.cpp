#include "serial_config.h"
#include "storage.h"
#include "locations.h"
#include "ota.h"
#include "fetcher.h" // fetcher_wifi_connected()/fetcher_connection_type() -- WIFI_STATUS
#include "../version.h"
#include <Arduino.h>
#include <cstring>
#include <cstdlib>

// Plain Serial (jc1060 runs ARDUINO_USB_MODE=1, a single USB port -- no
// board-specific routing needed here).

#define LINE_BUF_SIZE 200

static char _line[LINE_BUF_SIZE];
static size_t _len = 0;

// Every command here is a "set to X" (or "clear everything") operation, not
// an append/increment -- running the same command twice always converges to
// the same end state. That's deliberate: tools/configure_device.{sh,ps1}
// (the USB-serial config scripts this protocol was built for) need to be
// safe to re-run without side effects if a step gets repeated or the script
// is run again later.
static void handle_line(char *line) {
    // Trim trailing CR/LF/whitespace
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) {
        line[--n] = '\0';
    }
    if (n == 0) return;

    // Connectivity/protocol-version check for the config scripts -- lets
    // them confirm they're actually talking to this firmware (and pick the
    // right port among several connected USB-serial devices) before sending
    // anything real.
    if (strcmp(line, "PING") == 0) {
        Serial.println("OK PONG");
    } else if (strcmp(line, "STATUS") == 0) {
        // Lets tools/configure_device.{sh,ps1} tell an unconfigured device
        // (fresh flash/factory reset) from an already-set-up one, so it can
        // decide whether to run the guided first-time setup automatically
        // or go straight to the menu. wifi is the deciding signal for
        // "configured" -- token/locations are optional/incremental even on
        // a fully set-up device, but a device that's never had WiFi
        // credentials saved has never been touched at all.
        Serial.printf("OK wifi=%d token=%d locations=%d\n",
            g_config.wifi_ssid[0] ? 1 : 0,
            g_config.airportdb_token[0] ? 1 : 0,
            locations_count());
    } else if (strncmp(line, "TOKEN=", 6) == 0) {
        // Deliberately does NOT validate against airportdb.io here -- saving
        // always succeeds regardless of whether the token actually works
        // (matches every other command's "set to X" idempotent-and-safe
        // contract, see the file header comment). A token typed before WiFi
        // credentials have ever taken effect (the common case during
        // first-time setup, which sets WiFi before this) couldn't be
        // validated here anyway -- there's no network yet. TOKEN_VERIFY
        // below is the explicit, separate way to actually test it once
        // online.
        const char *token = line + 6;
        strlcpy(g_config.airportdb_token, token, sizeof(g_config.airportdb_token));
        storage_save_config(g_config);
        Serial.printf("OK Saved airportdb.io token (%d chars)\n", (int)strlen(g_config.airportdb_token));
    } else if (strcmp(line, "TOKEN_VERIFY") == 0) {
        // Real validation, unlike the plain presence-check STATUS reports --
        // does a live fetch against a known-good ICAO using the saved token
        // and reports whether it actually authenticates. Same async
        // request/poll split as OTA_CHECK/OTA_STATUS: the real HTTPS round
        // trip happens on location_poll_task (locations_verify_token_poll(),
        // fetcher.cpp), not here -- this handler runs on the main render
        // loop (see serial_config_poll()'s call site in main.cpp), so
        // blocking here for however long that request takes would freeze
        // the display the whole time.
        locations_request_verify_token();
        Serial.println("OK Verifying token...");
    } else if (strcmp(line, "TOKEN_VERIFY_STATUS") == 0) {
        bool ok;
        char err[48];
        if (!locations_verify_token_result(&ok, err, sizeof(err))) {
            Serial.println("OK pending");
        } else if (ok) {
            Serial.println("OK valid");
        } else {
            Serial.printf("OK invalid (%s)\n", err);
        }
    } else if (strncmp(line, "WIFI_SSID=", 10) == 0) {
        strlcpy(g_config.wifi_ssid, line + 10, sizeof(g_config.wifi_ssid));
        storage_save_config(g_config);
        Serial.printf("OK WiFi SSID saved (%d chars) -- reboot to apply\n", (int)strlen(g_config.wifi_ssid));
    } else if (strncmp(line, "WIFI_PASS=", 10) == 0) {
        strlcpy(g_config.wifi_pass, line + 10, sizeof(g_config.wifi_pass));
        storage_save_config(g_config);
        Serial.printf("OK WiFi password saved (%d chars) -- reboot to apply\n", (int)strlen(g_config.wifi_pass));
    } else if (strcmp(line, "WIFI_STATUS") == 0) {
        // Instant, unlike TOKEN_VERIFY -- just reads already-computed
        // connection state (fetcher.cpp), no new network activity, so no
        // request/poll split needed here.
        if (fetcher_wifi_connected()) {
            Serial.println("OK connected");
        } else {
            Serial.println("OK disconnected");
        }
    } else if (strncmp(line, "ADD_WAYPOINT=", 13) == 0) {
        // <name>|<lat>|<lon>|<elevation_ft> -- pipe-delimited (not comma) so
        // a name can contain a comma if a user wants one. locations_add_waypoint()
        // still strips any literal '|' from the parsed name defensively.
        char args[LINE_BUF_SIZE];
        strlcpy(args, line + 13, sizeof(args));
        char *name = args;
        char *lat_s = strchr(name, '|');
        char *lon_s = lat_s ? strchr(lat_s + 1, '|') : nullptr;
        char *elev_s = lon_s ? strchr(lon_s + 1, '|') : nullptr;
        if (!lat_s || !lon_s || !elev_s) {
            Serial.println("ERR ADD_WAYPOINT needs <name>|<lat>|<lon>|<elevation_ft>");
        } else {
            *lat_s++ = '\0';
            *lon_s++ = '\0';
            *elev_s++ = '\0';
            char err[64] = {};
            if (locations_add_waypoint(name, atof(lat_s), atof(lon_s), atoi(elev_s), err, sizeof(err))) {
                Serial.printf("OK Added location \"%s\"\n", name);
            } else {
                Serial.printf("ERR %s\n", err);
            }
        }
    } else if (strcmp(line, "REBOOT") == 0) {
        // Non-destructive restart -- distinct from FACTORY_RESET=CONFIRM,
        // which also wipes settings. Exists so config scripts can apply
        // WiFi-credential changes (only read at boot) without asking the
        // user to physically power-cycle the device.
        Serial.println("OK Rebooting...");
        Serial.flush();
        delay(200); // let the OK line actually flush over USB CDC before reset
        ESP.restart();
    } else if (strcmp(line, "OTA_CHECK") == 0) {
        // Application-firmware update check via GitHub Releases -- does NOT
        // touch the ESP32-C6 co-processor's own firmware, see ota.h. Just
        // requests the check; OTA_STATUS reports the result once
        // ota_poll() (fetcher.cpp's location_poll_task) has actually run it.
        ota_request_check();
        Serial.printf("OK Checking for updates (currently running %s)...\n", FIRMWARE_VERSION_STR);
    } else if (strcmp(line, "OTA_STATUS") == 0) {
        const char *state =
            ota_status == OTA_IDLE        ? "idle" :
            ota_status == OTA_CHECKING    ? "checking" :
            ota_status == OTA_UP_TO_DATE  ? "up to date" :
            ota_status == OTA_AVAILABLE   ? "update available" :
            ota_status == OTA_DOWNLOADING ? "downloading" :
            ota_status == OTA_DONE        ? "done" : "error";
        if (ota_status == OTA_DOWNLOADING) {
            Serial.printf("OK %s (%d%%)\n", state, ota_progress);
        } else if (ota_status == OTA_AVAILABLE || ota_status == OTA_UP_TO_DATE) {
            Serial.printf("OK %s (latest=%s running=%s)\n", state, ota_latest_tag, FIRMWARE_VERSION_STR);
        } else {
            Serial.printf("OK %s\n", state);
        }
    } else if (strcmp(line, "OTA_UPDATE") == 0) {
        // Only proceeds if a prior OTA_CHECK actually found a newer release
        // -- ota_request_update() itself no-ops otherwise, matching the
        // natural check-then-install flow rather than needing a separate
        // confirm string like FACTORY_RESET does.
        if (ota_status != OTA_AVAILABLE) {
            Serial.println("ERR No update available -- run OTA_CHECK first");
        } else {
            ota_request_update();
            Serial.printf("OK Downloading %s -- device will reboot automatically when done\n", ota_latest_tag);
        }
    } else if (strcmp(line, "FACTORY_RESET=CONFIRM") == 0) {
        // Requires the exact confirm string, not just "FACTORY_RESET" --
        // this is destructive (wipes every saved setting and location) and
        // the whole point of this protocol is to be easy to script, so it
        // shouldn't also be easy to trigger by accident.
        Serial.println("OK Factory reset -- erasing all settings and saved locations, rebooting...");
        Serial.flush();
        storage_factory_reset();
        locations_factory_reset();
        delay(200); // let the OK line actually flush over USB CDC before reset
        ESP.restart();
    } else {
        Serial.println("ERR Unknown command. Supported: PING, STATUS, TOKEN=, TOKEN_VERIFY, TOKEN_VERIFY_STATUS, WIFI_SSID=, WIFI_PASS=, WIFI_STATUS, ADD_WAYPOINT=, OTA_CHECK, OTA_STATUS, OTA_UPDATE, REBOOT, FACTORY_RESET=CONFIRM");
    }
}

void serial_config_poll() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n') {
            _line[_len] = '\0';
            handle_line(_line);
            _len = 0;
        } else if (_len < LINE_BUF_SIZE - 1) {
            _line[_len++] = c;
        }
        // else: silently drop overlong input rather than overflow
    }
}
