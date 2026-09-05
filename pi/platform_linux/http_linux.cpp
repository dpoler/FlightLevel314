#include "../../src/platform/platform.h"
#include <curl/curl.h>
#include <string>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <mutex>

namespace {

size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

bool hdr_name_eq(const char *line, size_t line_len, const char *name) {
    // Match "Name: value" case-insensitively on the name only.
    size_t nlen = strlen(name);
    if (line_len < nlen + 1) return false;
    for (size_t i = 0; i < nlen; i++) {
        if (tolower((unsigned char)line[i]) != tolower((unsigned char)name[i])) return false;
    }
    return line[nlen] == ':';
}

int hdr_int_value(const char *line) {
    const char *colon = strchr(line, ':');
    if (!colon) return -1;
    colon++;
    while (*colon == ' ' || *colon == '\t') colon++;
    char *end = nullptr;
    long v = strtol(colon, &end, 10);
    if (end == colon) return -1;
    if (v < 0) v = 0;
    if (v > 1000000000L) v = 1000000000L;
    return (int)v;
}

struct RateLimitParse {
    PlatformHttpRateLimit *out = nullptr;
    bool got_units_lim = false;
    bool got_units_rem = false;
    bool got_req_lim = false;
    bool got_req_rem = false;
};

size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    auto *st = static_cast<RateLimitParse *>(userdata);
    size_t len = size * nitems;
    if (!st || !st->out || len == 0) return len;

    // Final empty header line — publish only complete pairs.
    if (len == 2 && buffer[0] == '\r' && buffer[1] == '\n') {
        st->out->have_units = st->got_units_lim && st->got_units_rem;
        st->out->have_requests = st->got_req_lim && st->got_req_rem;
        return len;
    }
    if (memchr(buffer, ':', len) == nullptr) return len;

    auto take = [&](const char *name, int *dst, bool *got) {
        if (!hdr_name_eq(buffer, len, name)) return;
        int v = hdr_int_value(buffer);
        if (v < 0) return;
        *dst = v;
        *got = true;
    };

    // RapidAPI AeroDataBox: API-Units = real Basic quota; Requests = looser
    // HTTP counter. Names matched case-insensitively.
    take("x-ratelimit-api-units-limit", &st->out->units_limit, &st->got_units_lim);
    take("x-ratelimit-api-units-remaining", &st->out->units_remaining, &st->got_units_rem);
    take("x-ratelimit-requests-limit", &st->out->requests_limit, &st->got_req_lim);
    take("x-ratelimit-requests-remaining", &st->out->requests_remaining, &st->got_req_rem);

    return len;
}

std::once_flag curl_init_flag;

bool http_get_internal(const char *url, char *out, size_t out_size, size_t *out_len,
                       long *http_status, const char *const *extra_headers,
                       PlatformHttpRateLimit *rate_limit, bool require_2xx) {
    std::call_once(curl_init_flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    if (rate_limit) {
        rate_limit->have_units = false;
        rate_limit->have_requests = false;
        rate_limit->units_limit = 0;
        rate_limit->units_remaining = 0;
        rate_limit->requests_limit = 0;
        rate_limit->requests_remaining = 0;
    }

    RateLimitParse hdr_parse;
    hdr_parse.out = rate_limit;

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    // Planespotters (and some other APIs) reject generic library UAs with
    // HTTP 403 -- identify the app and include a contact URL.
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "flightlevel314/0.1 (+https://github.com/dpoler/FlightLevel314)");

    if (rate_limit) {
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hdr_parse);
    }

    struct curl_slist *hdrs = nullptr;
    if (extra_headers) {
        for (const char *const *h = extra_headers; *h; ++h) {
            hdrs = curl_slist_append(hdrs, *h);
        }
        if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    // Finalize even if the trailing blank header line was skipped.
    if (rate_limit) {
        rate_limit->have_units = hdr_parse.got_units_lim && hdr_parse.got_units_rem;
        rate_limit->have_requests = hdr_parse.got_req_lim && hdr_parse.got_req_rem;
    }

    if (res != CURLE_OK) {
        platform_log_warn("HTTP GET %s failed: curl=%d\n", url, (int)res);
        return false;
    }
    if (http_status) *http_status = http_code;

    if (require_2xx && (http_code < 200 || http_code >= 300)) {
        platform_log_warn("HTTP GET %s failed: http=%ld\n", url, http_code);
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

} // namespace

bool platform_http_get(const char *url, char *out, size_t out_size, size_t *out_len) {
    return http_get_internal(url, out, out_size, out_len, nullptr, nullptr, nullptr, true);
}

bool platform_http_get_ex(const char *url, char *out, size_t out_size, size_t *out_len,
                          long *http_status, const char *const *extra_headers,
                          PlatformHttpRateLimit *rate_limit) {
    return http_get_internal(url, out, out_size, out_len, http_status, extra_headers,
                             rate_limit, false);
}
