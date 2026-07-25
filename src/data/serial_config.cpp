#include "serial_config.h"
#include "storage.h"
#include "locations.h"
#include <Arduino.h>
#include <cstring>

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
    } else if (strncmp(line, "TOKEN=", 6) == 0) {
        const char *token = line + 6;
        strlcpy(g_config.airportdb_token, token, sizeof(g_config.airportdb_token));
        storage_save_config(g_config);
        Serial.printf("OK Saved airportdb.io token (%d chars)\n", (int)strlen(g_config.airportdb_token));
    } else if (strncmp(line, "WIFI_SSID=", 10) == 0) {
        strlcpy(g_config.wifi_ssid, line + 10, sizeof(g_config.wifi_ssid));
        storage_save_config(g_config);
        Serial.printf("OK WiFi SSID saved (%d chars) -- reboot to apply\n", (int)strlen(g_config.wifi_ssid));
    } else if (strncmp(line, "WIFI_PASS=", 10) == 0) {
        strlcpy(g_config.wifi_pass, line + 10, sizeof(g_config.wifi_pass));
        storage_save_config(g_config);
        Serial.printf("OK WiFi password saved (%d chars) -- reboot to apply\n", (int)strlen(g_config.wifi_pass));
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
        Serial.println("ERR Unknown command. Supported: PING, TOKEN=, WIFI_SSID=, WIFI_PASS=, FACTORY_RESET=CONFIRM");
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
