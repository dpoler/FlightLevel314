#include "ota.h"
#include "http_mutex.h"
#include "../version.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <cstring>

#define GITHUB_OWNER "dpoler"
#define GITHUB_REPO  "adsb"

// Two board targets, one release -- each release must attach both assets.
#ifdef BOARD_CROWPANEL
#define OTA_ASSET_NAME "firmware-crowpanel.bin"
#else
#define OTA_ASSET_NAME "firmware-jc1060.bin"
#endif

// Same reasoning as fetcher.cpp/enrichment.cpp: NetworkClientSecure's
// default TLS handshake timeout (120s) isn't bounded by
// HTTPClient::setTimeout(), which only covers the read phase after a
// connection succeeds -- must set this on the WiFiClientSecure directly.
#define TLS_HANDSHAKE_TIMEOUT_S 8

volatile OtaStatus ota_status = OTA_IDLE;
char ota_latest_tag[16] = "";
volatile int ota_progress = 0;

static volatile bool _check_requested = false;
static volatile bool _update_requested = false;

// PSRAM allocator for ArduinoJson -- same pattern as locations.cpp/
// enrichment.cpp, keeps internal RAM free for SDIO/WiFi buffers.
struct OtaPsramAlloc : ArduinoJson::Allocator {
    void* allocate(size_t size) override {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }
    void deallocate(void* p) override {
        heap_caps_free(p);
    }
    void* reallocate(void* p, size_t size) override {
        return heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM);
    }
};
static OtaPsramAlloc _ota_alloc;

void ota_request_check() {
    if (ota_status == OTA_CHECKING || ota_status == OTA_DOWNLOADING) return;
    _check_requested = true;
}

void ota_request_update() {
    if (ota_status != OTA_AVAILABLE) return;
    _update_requested = true;
}

static void do_check() {
    ota_status = OTA_CHECKING;

    if (!http_mutex_acquire(pdMS_TO_TICKS(15000))) {
        ota_status = OTA_ERROR;
        Serial.println("[OTA] Check failed -- network busy, try again");
        return;
    }
    {
        WiFiClientSecure client;
        client.setInsecure(); // matches this app's other third-party HTTPS calls
        client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
        HTTPClient http;
        http.begin(client, "https://api.github.com/repos/" GITHUB_OWNER "/" GITHUB_REPO "/releases/latest");
        http.addHeader("User-Agent", GITHUB_REPO); // GitHub's API 403s requests with no User-Agent
        http.addHeader("Accept", "application/vnd.github+json");
        http.setTimeout(10000);

        int code = http.GET();
        if (code == HTTP_CODE_OK) {
            // Only pull tag_name out of what's otherwise a large payload
            // (full release notes, every asset's metadata, etc) -- no need
            // to parse or hold any of that in memory just to compare a tag.
            JsonDocument filter(&_ota_alloc);
            filter["tag_name"] = true;
            JsonDocument doc(&_ota_alloc);
            if (!deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter))) {
                const char *tag = doc["tag_name"];
                if (tag && tag[0]) {
                    strlcpy(ota_latest_tag, tag, sizeof(ota_latest_tag));
                    ota_status = (strcmp(ota_latest_tag, FIRMWARE_VERSION_STR) == 0)
                                 ? OTA_UP_TO_DATE : OTA_AVAILABLE;
                    Serial.printf("[OTA] %s (latest=%s running=%s)\n",
                        ota_status == OTA_UP_TO_DATE ? "Up to date" : "Update available",
                        ota_latest_tag, FIRMWARE_VERSION_STR);
                } else {
                    ota_status = OTA_ERROR;
                    Serial.println("[OTA] Check failed -- no tag_name in response");
                }
            } else {
                ota_status = OTA_ERROR;
                Serial.println("[OTA] Check failed -- JSON parse error");
            }
        } else {
            ota_status = OTA_ERROR;
            Serial.printf("[OTA] Check failed -- HTTP %d\n", code);
        }
        http.end();
    }
    http_mutex_release();
}

static void do_update() {
    ota_status = OTA_DOWNLOADING;
    ota_progress = 0;

    if (!http_mutex_acquire(pdMS_TO_TICKS(15000))) {
        ota_status = OTA_ERROR;
        Serial.println("[OTA] Update failed -- network busy, try again");
        return;
    }
    {
        char url[192];
        snprintf(url, sizeof(url),
            "https://github.com/" GITHUB_OWNER "/" GITHUB_REPO "/releases/download/%s/" OTA_ASSET_NAME,
            ota_latest_tag);

        WiFiClientSecure client;
        client.setInsecure();
        client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
        HTTPClient http;
        http.begin(client, url);
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); // GitHub release assets 302 to S3
        http.setTimeout(30000);

        Serial.printf("[OTA] Downloading %s...\n", url);
        int code = http.GET();
        if (code == HTTP_CODE_OK) {
            int content_len = http.getSize();
            Stream &stream = http.getStream();

            if (Update.begin(content_len > 0 ? content_len : UPDATE_SIZE_UNKNOWN)) {
                uint8_t buf[512];
                int written = 0;
                int last_log_pct = -1;
                uint32_t idle_start = millis();
                bool write_failed = false;

                while (http.connected() && (content_len < 0 || written < content_len)) {
                    size_t avail = stream.available();
                    if (avail) {
                        idle_start = millis();
                        int r = stream.readBytes(buf, min(avail, sizeof(buf)));
                        if (Update.write(buf, r) != (size_t)r) {
                            write_failed = true;
                            break;
                        }
                        written += r;
                        if (content_len > 0) {
                            ota_progress = written * 100 / content_len;
                            int log_pct = (ota_progress / 10) * 10;
                            if (log_pct > last_log_pct) {
                                last_log_pct = log_pct;
                                Serial.printf("[OTA] %d%%\n", log_pct);
                            }
                        }
                    } else {
                        if (millis() - idle_start > 10000) break; // stalled connection
                        vTaskDelay(pdMS_TO_TICKS(1));
                    }
                }

                bool size_ok = (content_len < 0) || (written == content_len);
                ota_status = (!write_failed && size_ok && Update.end(true)) ? OTA_DONE : OTA_ERROR;
                Serial.printf("[OTA] %s\n", ota_status == OTA_DONE ? "Complete -- restarting" : "FAILED");
            } else {
                ota_status = OTA_ERROR;
                Serial.println("[OTA] Update.begin() failed");
            }
        } else {
            ota_status = OTA_ERROR;
            Serial.printf("[OTA] Download failed -- HTTP %d\n", code);
        }
        http.end();
    }
    http_mutex_release();

    if (ota_status == OTA_DONE) {
        Serial.flush();
        delay(200); // let the log line above actually flush before reset
        ESP.restart();
    }
}

void ota_poll() {
    if (_check_requested) {
        _check_requested = false;
        do_check();
    }
    if (_update_requested) {
        _update_requested = false;
        do_update();
    }
}
