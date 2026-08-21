// Pi online app updates via GitHub Releases (implements src/data/ota.h).
// Checks latest release tag vs FIRMWARE_VERSION_STR, downloads the arch-
// matching asset next to the running binary, atomically replaces it, then
// exits so systemd Restart=always brings up the new build.
//
// Network work runs on a dedicated thread (same pattern as enrichment).
// UI only reads ota_status / ota_latest_tag / ota_progress.

#include "../../src/data/ota.h"
#include "../../src/platform/platform.h"
#include "../../src/version.h"

#include <ArduinoJson.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define GITHUB_OWNER "dpoler"
#define GITHUB_REPO  "FlightLevel314"

volatile OtaStatus ota_status = OTA_IDLE;
char ota_latest_tag[16] = "";
volatile int ota_progress = 0;

namespace {

std::mutex g_mu;
std::atomic<bool> g_check_req{false};
std::atomic<bool> g_update_req{false};
std::atomic<bool> g_worker_busy{false};
std::string g_asset_url;
std::string g_install_path; // absolute path of the running binary
uint32_t g_last_auto_check_ms = 0;
constexpr uint32_t AUTO_CHECK_INTERVAL_MS = 24u * 60u * 60u * 1000u;

struct CurlBuf {
    std::vector<uint8_t> data;
};

size_t curl_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *b = static_cast<CurlBuf *>(userdata);
    size_t n = size * nmemb;
    b->data.insert(b->data.end(), ptr, ptr + n);
    return n;
}

struct ProgressCtx {
    curl_off_t last_log = -1;
};

int curl_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                  curl_off_t, curl_off_t) {
    auto *ctx = static_cast<ProgressCtx *>(clientp);
    if (dltotal > 0) {
        int pct = (int)((dlnow * 100) / dltotal);
        if (pct > 100) pct = 100;
        ota_progress = pct;
        if (pct / 10 != ctx->last_log / 10) {
            ctx->last_log = pct;
            platform_log_debug("OTA: download %d%%\n", pct);
        }
    }
    return 0;
}

bool http_get_string(const std::string &url, std::string &out, long *http_status) {
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    CurlBuf buf;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "FlightLevel314-OTA/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    struct curl_slist *hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Accept: application/vnd.github+json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (http_status) *http_status = code;
    if (rc != CURLE_OK || code != 200) return false;
    out.assign(reinterpret_cast<const char *>(buf.data.data()), buf.data.size());
    return true;
}

bool http_download_file(const std::string &url, const std::string &path) {
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp) {
        platform_log_error("OTA: cannot write %s\n", path.c_str());
        return false;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        return false;
    }
    ProgressCtx pctx;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "FlightLevel314-OTA/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &pctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    fclose(fp);
    if (rc != CURLE_OK || code != 200) {
        unlink(path.c_str());
        platform_log_warn("OTA: download failed rc=%d http=%ld\n", (int)rc, code);
        return false;
    }
    return true;
}

const char *asset_name_for_host() {
    utsname u{};
    if (uname(&u) != 0) return "flightlevel314-linux-x86_64";
    if (strcmp(u.machine, "aarch64") == 0 || strcmp(u.machine, "arm64") == 0)
        return "flightlevel314-linux-aarch64";
    if (strcmp(u.machine, "armv7l") == 0 || strcmp(u.machine, "armv6l") == 0)
        return "flightlevel314-linux-armv7";
    return "flightlevel314-linux-x86_64";
}

std::string resolve_install_path() {
    char buf[512];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return std::string(buf);
    }
    return "/opt/flightlevel314/flightlevel314";
}

void do_check() {
    ota_status = OTA_CHECKING;
    ota_progress = 0;
    g_asset_url.clear();

    std::string body;
    long status = 0;
    const char *url = "https://api.github.com/repos/" GITHUB_OWNER "/" GITHUB_REPO "/releases/latest";
    if (!http_get_string(url, body, &status)) {
        ota_status = OTA_ERROR;
        platform_log_warn("OTA: check failed (http=%ld)\n", status);
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        ota_status = OTA_ERROR;
        platform_log_warn("OTA: check JSON parse failed\n");
        return;
    }
    const char *tag = doc["tag_name"] | "";
    if (!tag[0]) {
        ota_status = OTA_ERROR;
        platform_log_warn("OTA: check: no tag_name\n");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        strlcpy(ota_latest_tag, tag, sizeof(ota_latest_tag));
    }

    const char *want = asset_name_for_host();
    std::string browser_url;
    JsonArray assets = doc["assets"].as<JsonArray>();
    for (JsonObject a : assets) {
        const char *name = a["name"] | "";
        if (strcmp(name, want) == 0) {
            browser_url = a["browser_download_url"] | "";
            break;
        }
    }

    if (strcmp(tag, FIRMWARE_VERSION_STR) == 0) {
        ota_status = OTA_UP_TO_DATE;
        platform_log_info("OTA: up to date (running=%s)\n", FIRMWARE_VERSION_STR);
        return;
    }

    if (browser_url.empty()) {
        // Newer tag exists but no binary for this arch yet.
        ota_status = OTA_ERROR;
        platform_log_warn("OTA: update %s available but no asset '%s' on the release\n",
                     tag, want);
        return;
    }

    g_asset_url = browser_url;
    ota_status = OTA_AVAILABLE;
    platform_log_info("OTA: update available: %s (running %s) asset=%s\n",
                 tag, FIRMWARE_VERSION_STR, want);
}

void do_update() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        url = g_asset_url;
        if (g_install_path.empty()) g_install_path = resolve_install_path();
    }
    if (url.empty()) {
        ota_status = OTA_ERROR;
        platform_log_warn("OTA: update: no asset URL (run check first)\n");
        return;
    }

    ota_status = OTA_DOWNLOADING;
    ota_progress = 0;

    const std::string dest = g_install_path;
    const std::string tmp = dest + ".new";
    const std::string bak = dest + ".bak";

    if (!http_download_file(url, tmp)) {
        ota_status = OTA_ERROR;
        return;
    }
    if (chmod(tmp.c_str(), 0755) != 0) {
        platform_log_error("OTA: chmod failed on %s\n", tmp.c_str());
        unlink(tmp.c_str());
        ota_status = OTA_ERROR;
        return;
    }

    // Keep previous binary as .bak; atomic replace via rename.
    unlink(bak.c_str());
    if (access(dest.c_str(), F_OK) == 0 && rename(dest.c_str(), bak.c_str()) != 0) {
        platform_log_error("OTA: cannot backup %s (permission?)\n", dest.c_str());
        unlink(tmp.c_str());
        ota_status = OTA_ERROR;
        return;
    }
    if (rename(tmp.c_str(), dest.c_str()) != 0) {
        platform_log_error("OTA: cannot install %s — restoring backup\n", dest.c_str());
        rename(bak.c_str(), dest.c_str());
        unlink(tmp.c_str());
        ota_status = OTA_ERROR;
        return;
    }

    ota_progress = 100;
    ota_status = OTA_DONE;
    platform_log_info("OTA: installed %s — exiting for systemd restart\n", dest.c_str());
    // Give journals a moment; systemd Restart=always relaunches.
    fflush(nullptr);
    _exit(0);
}

void worker_main() {
    g_worker_busy = true;
    if (g_update_req.exchange(false)) {
        do_update();
    } else if (g_check_req.exchange(false)) {
        do_check();
    }
    g_worker_busy = false;
}

void kick_worker() {
    if (g_worker_busy.load()) return;
    std::thread(worker_main).detach();
}

} // namespace

void ota_request_check() {
    if (ota_status == OTA_CHECKING || ota_status == OTA_DOWNLOADING) return;
    g_check_req = true;
    kick_worker();
}

void ota_request_update() {
    if (ota_status != OTA_AVAILABLE) return;
    g_update_req = true;
    kick_worker();
}

void ota_poll() {
    // Periodic quiet check (once per day after boot grace).
    const uint32_t now = platform_millis();
    if (g_last_auto_check_ms == 0) g_last_auto_check_ms = now;
    if (!g_worker_busy.load() &&
        ota_status != OTA_CHECKING && ota_status != OTA_DOWNLOADING &&
        now - g_last_auto_check_ms >= AUTO_CHECK_INTERVAL_MS) {
        g_last_auto_check_ms = now;
        g_check_req = true;
        kick_worker();
    }
    if ((g_check_req.load() || g_update_req.load()) && !g_worker_busy.load()) {
        kick_worker();
    }
}
