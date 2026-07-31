#include "../../src/platform/platform.h"
#include <curl/curl.h>
#include <string>
#include <cstring>
#include <mutex>

namespace {

size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::once_flag curl_init_flag;

} // namespace

bool platform_http_get(const char *url, char *out, size_t out_size, size_t *out_len) {
    std::call_once(curl_init_flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "adsb-pi-port/0.1");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code < 200 || http_code >= 300) {
        platform_log("HTTP GET %s failed: curl=%d http=%ld\n", url, (int)res, http_code);
        return false;
    }

    size_t n = body.size();
    if (out_size > 0 && n >= out_size) n = out_size - 1;
    if (out_size > 0) {
        memcpy(out, body.data(), n);
        out[n] = '\0';
    }
    if (out_len) *out_len = n;
    return true;
}
