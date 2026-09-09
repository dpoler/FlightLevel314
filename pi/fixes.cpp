// FAA named-fix overlay for Map (Pi).
// AIS DesignatedPoints + NAVAID (deduped vs airports) + CIFP terminal WPs.

#include "fixes.h"
#include "../src/platform/platform.h"
#include "../src/ui/display_prefs.h"
#include "airports_db_include.h"

#include <ArduinoJson.h>
#include <curl/curl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>

namespace {

constexpr int kMaxFixes = 250;
constexpr float kRelocNm = 2.0f;
constexpr float kAirportNavaidDupNm = 2.5f;

enum class FixKind : uint8_t { Designated = 0, Navaid = 1, Terminal = 2 };

struct FixPoint {
    char ident[12];
    float lat;
    float lon;
    FixKind kind;
    uint8_t area_charted;
};

struct Snapshot {
    FixPoint pts[kMaxFixes];
    int count = 0;
    float lat = 0;
    float lon = 0;
    float radius_nm = 0;
    char airport_icao[8]{};
};

std::mutex g_mu;
Snapshot g_live;
Snapshot g_pending;
bool g_pending_ready = false;
bool g_busy = false;
bool g_have_live = false;
float g_fetched_lat = 0;
float g_fetched_lon = 0;
float g_fetched_radius = 0;
char g_fetched_icao[8]{};
bool g_have_fetched = false;

// CIFP PC cache for one airport (parsed from FAACIFP18).
struct CifpWp {
    char ident[12];
    float lat;
    float lon;
};
std::mutex g_cifp_mu;
std::string g_cifp_icao;
std::vector<CifpWp> g_cifp_wps;
bool g_cifp_ready = false;

static float haversine_nm(float lat1, float lon1, float lat2, float lon2) {
    const float R_nm = 3440.065f;
    const float dlat = (lat2 - lat1) * (float)M_PI / 180.0f;
    const float dlon = (lon2 - lon1) * (float)M_PI / 180.0f;
    const float a = sinf(dlat * 0.5f) * sinf(dlat * 0.5f) +
                    cosf(lat1 * (float)M_PI / 180.0f) *
                        cosf(lat2 * (float)M_PI / 180.0f) *
                        sinf(dlon * 0.5f) * sinf(dlon * 0.5f);
    return 2.0f * R_nm * asinf(fminf(1.0f, sqrtf(a)));
}

static void envelope(float lat, float lon, float radius_nm,
                     double *xmin, double *ymin, double *xmax, double *ymax) {
    const float pad = radius_nm * 1.08f;
    const float dlat = pad / 60.0f;
    float clat = cosf(lat * (float)M_PI / 180.0f);
    if (clat < 0.2f) clat = 0.2f;
    const float dlon = pad / (60.0f * clat);
    *xmin = lon - dlon;
    *xmax = lon + dlon;
    *ymin = lat - dlat;
    *ymax = lat + dlat;
}

static std::string config_root() {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) return std::string(xdg) + "/flightlevel314";
    const char *home = getenv("HOME");
    return std::string(home && home[0] ? home : ".") + "/.config/flightlevel314";
}

static std::string cifp_dir() { return config_root() + "/cifp"; }

static void mkdir_p(const std::string &path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur.push_back(path[i]);
        if (path[i] == '/' || i + 1 == path.size()) {
            if (!cur.empty() && cur != "/")
                mkdir(cur.c_str(), 0755);
        }
    }
}

static size_t curl_file_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    FILE *f = static_cast<FILE *>(userdata);
    return fwrite(ptr, size, nmemb, f);
}

static bool http_download_file(const char *url, const char *path) {
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    FILE *f = fopen(path, "wb");
    if (!f) {
        curl_easy_cleanup(curl);
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_file_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "flightlevel314/0.1 (+https://github.com/dpoler/FlightLevel314)");
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    fclose(f);
    if (rc != CURLE_OK || code < 200 || code >= 300) {
        remove(path);
        return false;
    }
    return true;
}

static bool http_get_text(const char *url, std::string *out) {
    std::vector<char> buf(1024 * 1024);
    size_t len = 0;
    if (!platform_http_get(url, buf.data(), buf.size(), &len) || len == 0)
        return false;
    out->assign(buf.data(), len);
    return true;
}

static bool http_get_json(const char *url, std::string *out) {
    return http_get_text(url, out);
}

// Discover current CIFP zip URL from the FAA download page.
static bool cifp_discover_zip_url(std::string *url_out) {
    std::string html;
    if (!http_get_text(
            "https://www.faa.gov/air_traffic/flight_info/aeronav/digital_products/cifp/download/",
            &html))
        return false;
    // Prefer aeronav absolute links: .../CIFP_YYMMDD.zip
    const char *key = "CIFP_";
    size_t pos = 0;
    std::string best;
    while ((pos = html.find(key, pos)) != std::string::npos) {
        size_t start = pos;
        while (start > 0 && html[start - 1] != '"' && html[start - 1] != '\'')
            start--;
        size_t end = html.find(".zip", pos);
        if (end == std::string::npos) break;
        end += 4;
        std::string cand = html.substr(start, end - start);
        if (cand.find("CIFP_") != std::string::npos && cand.size() < 200) {
            if (cand.rfind("http", 0) != 0) {
                if (cand.rfind("//", 0) == 0)
                    cand = "https:" + cand;
                else if (cand.rfind("Upload_313", 0) == 0)
                    cand = "https://aeronav.faa.gov/" + cand;
                else if (!cand.empty() && cand[0] == '/')
                    cand = "https://aeronav.faa.gov" + cand;
            }
            if (cand.find("aeronav.faa.gov") != std::string::npos) {
                best = cand;
                break;
            }
            if (best.empty()) best = cand;
        }
        pos = end;
    }
    if (best.empty()) return false;
    *url_out = best;
    return true;
}

static bool cifp_extract_faafile(const std::string &zip_path, const std::string &out_path) {
    // Avoid a libzip dependency — python3 is on the Pi image / cloud env.
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile,sys; "
             "z=zipfile.ZipFile(sys.argv[1]); "
             "z.extract('FAACIFP18', sys.argv[2])\" '%s' '%s'",
             zip_path.c_str(), cifp_dir().c_str());
    int rc = system(cmd);
    if (rc != 0) return false;
    // zipfile writes <cifp_dir>/FAACIFP18
    const std::string extracted = cifp_dir() + "/FAACIFP18";
    if (extracted != out_path) {
        remove(out_path.c_str());
        if (rename(extracted.c_str(), out_path.c_str()) != 0) return false;
    }
    struct stat st {};
    return stat(out_path.c_str(), &st) == 0 && st.st_size > 1000000;
}

static bool cifp_ensure_file(std::string *path_out) {
    mkdir_p(cifp_dir());
    const std::string data_path = cifp_dir() + "/FAACIFP18";
    const std::string meta_path = cifp_dir() + "/cycle.txt";
    struct stat st {};
    bool have = (stat(data_path.c_str(), &st) == 0 && st.st_size > 1000000);
    time_t now = time(nullptr);
    // Refresh about every chart cycle (~28d); keep a little slack.
    if (have && (now - st.st_mtime) < 25 * 24 * 3600) {
        *path_out = data_path;
        return true;
    }

    std::string url;
    if (!cifp_discover_zip_url(&url)) {
        // Fallback: last known pattern (may 404 when cycle rolls).
        url = "https://aeronav.faa.gov/Upload_313-d/cifp/CIFP_260903.zip";
        platform_log_warn("Fixes: CIFP catalog fetch failed; trying %s\n", url.c_str());
    } else {
        platform_log_info("Fixes: CIFP zip %s\n", url.c_str());
    }

    const std::string zip_path = cifp_dir() + "/cifp.zip";
    if (!http_download_file(url.c_str(), zip_path.c_str())) {
        platform_log_warn("Fixes: CIFP download failed\n");
        if (have) {
            *path_out = data_path;
            return true; // stale OK
        }
        return false;
    }
    if (!cifp_extract_faafile(zip_path, data_path)) {
        platform_log_warn("Fixes: CIFP extract failed\n");
        if (have) {
            *path_out = data_path;
            return true;
        }
        return false;
    }
    remove(zip_path.c_str());
    FILE *mf = fopen(meta_path.c_str(), "w");
    if (mf) {
        fprintf(mf, "%s\n", url.c_str());
        fclose(mf);
    }
    *path_out = data_path;
    return true;
}

static bool parse_arinc_latlon(const char *s, size_t len, float *lat, float *lon) {
    // NDDMMSSSS WDDDMMSSSS (SSSS = seconds * 100)
    auto find_ns = [&](char ns) -> const char * {
        for (size_t i = 0; i + 19 < len; i++) {
            if (s[i] == ns && (s[i + 9] == 'W' || s[i + 9] == 'E'))
                return s + i;
        }
        return nullptr;
    };
    const char *p = find_ns('N');
    if (!p) p = find_ns('S');
    if (!p) return false;
    int ad = (p[1] - '0') * 10 + (p[2] - '0');
    int am = (p[3] - '0') * 10 + (p[4] - '0');
    int as_ = (p[5] - '0') * 1000 + (p[6] - '0') * 100 + (p[7] - '0') * 10 + (p[8] - '0');
    char ew = p[9];
    int od = (p[10] - '0') * 100 + (p[11] - '0') * 10 + (p[12] - '0');
    int om = (p[13] - '0') * 10 + (p[14] - '0');
    int os_ = (p[15] - '0') * 1000 + (p[16] - '0') * 100 + (p[17] - '0') * 10 + (p[18] - '0');
    float la = (float)ad + (float)am / 60.0f + ((float)as_ / 100.0f) / 3600.0f;
    float lo = (float)od + (float)om / 60.0f + ((float)os_ / 100.0f) / 3600.0f;
    if (p[0] == 'S') la = -la;
    if (ew == 'W') lo = -lo;
    *lat = la;
    *lon = lo;
    return true;
}

static void cifp_load_airport_pc(const char *icao) {
    if (!icao || !icao[0]) {
        std::lock_guard<std::mutex> lock(g_cifp_mu);
        g_cifp_icao.clear();
        g_cifp_wps.clear();
        g_cifp_ready = false;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_cifp_mu);
        if (g_cifp_ready && g_cifp_icao == icao) return;
    }

    std::string path;
    if (!cifp_ensure_file(&path)) {
        platform_log_warn("Fixes: no CIFP file; terminal waypoints unavailable\n");
        return;
    }

    std::vector<CifpWp> wps;
    wps.reserve(512);
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return;
    char line[160];
    const size_t icao_len = strlen(icao);
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        // FAA CIFP PC: SUSA P_ IIII RR C IDENT... with lat/lon packed.
        // Indices: [4]=P, [6:10]=airport, [10:12]=region, [12]=C, [13:18]=ident
        if (n < 40 || line[4] != 'P' || line[12] != 'C') continue;
        if (strncmp(line + 6, icao, icao_len) != 0 || icao_len != 4) continue;
        CifpWp wp{};
        // Ident may be 5 chars; trim trailing spaces.
        memcpy(wp.ident, line + 13, 5);
        wp.ident[5] = '\0';
        for (int i = 4; i >= 0 && wp.ident[i] == ' '; i--) wp.ident[i] = '\0';
        if (!wp.ident[0]) continue;
        if (!parse_arinc_latlon(line, n, &wp.lat, &wp.lon)) continue;
        wps.push_back(wp);
    }
    fclose(f);

    {
        std::lock_guard<std::mutex> lock(g_cifp_mu);
        g_cifp_icao = icao;
        g_cifp_wps.swap(wps);
        g_cifp_ready = true;
    }
    platform_log_info("Fixes: CIFP terminal WPs for %s: %zu\n", icao,
                      (size_t)g_cifp_wps.size());
}

static bool ident_dup_in_snap(const Snapshot *snap, const char *ident) {
    for (int i = 0; i < snap->count; i++)
        if (strcmp(snap->pts[i].ident, ident) == 0) return true;
    return false;
}

// True when a navaid IDENT is essentially the same as a nearby airport
// (LAX ↔ KLAX, SLI ↔ KSLI within a couple of nm).
static bool navaid_duplicates_nearby_airport(const char *ident, float lat, float lon) {
    if (!ident || !ident[0]) return false;
#if HAS_AIRPORTS_DB
    char cand[8];
    const char *prefixes[] = {"", "K", "P", "C"};
    for (const char *pre : prefixes) {
        snprintf(cand, sizeof(cand), "%s%s", pre, ident);
        if (strlen(cand) < 3 || strlen(cand) > 4) continue;
        for (int i = 0; i < AIRPORTS_DB_COUNT; i++) {
            const StaticAirport &ap = airports_db[i];
            if (strcmp(ap.icao, cand) != 0 &&
                !(ap.alias[0] && strcmp(ap.alias, cand) == 0))
                continue;
            if (haversine_nm(lat, lon, ap.lat, ap.lon) <= kAirportNavaidDupNm)
                return true;
        }
    }
#else
    (void)lat;
    (void)lon;
#endif
    return false;
}

static void append_designated(Snapshot *snap, const char *json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        platform_log_warn("Fixes: DesignatedPoints JSON parse failed: %s\n", err.c_str());
        return;
    }
    JsonArray feats = doc["features"].as<JsonArray>();
    if (feats.isNull()) return;
    for (JsonObject f : feats) {
        if (snap->count >= kMaxFixes) break;
        const char *ident = f["attributes"]["IDENT"] | "";
        if (!ident[0]) continue;
        // AIS mirrors navaids as TYPE_CODE OTHER at the same coords — skip;
        // real navaids come from NAVAIDSystem (then airport-deduped).
        const char *type = f["attributes"]["TYPE_CODE"] | "";
        if (strcmp(type, "OTHER") == 0) continue;
        double x = f["geometry"]["x"] | 0.0;
        double y = f["geometry"]["y"] | 0.0;
        if (x == 0.0 && y == 0.0) continue;
        float lat = (float)y, lon = (float)x;
        if (navaid_duplicates_nearby_airport(ident, lat, lon)) continue;
        if (ident_dup_in_snap(snap, ident)) continue;
        FixPoint &p = snap->pts[snap->count];
        strlcpy(p.ident, ident, sizeof(p.ident));
        p.lon = lon;
        p.lat = lat;
        p.kind = FixKind::Designated;
        p.area_charted = (uint8_t)((int)(f["attributes"]["US_AREA"] | 0) != 0);
        snap->count++;
    }
}

static void append_navaids(Snapshot *snap, const char *json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        platform_log_warn("Fixes: NAVAID JSON parse failed: %s\n", err.c_str());
        return;
    }
    JsonArray feats = doc["features"].as<JsonArray>();
    if (feats.isNull()) return;
    for (JsonObject f : feats) {
        if (snap->count >= kMaxFixes) break;
        const char *ident = f["attributes"]["IDENT"] | "";
        if (!ident[0]) continue;
        double x = f["geometry"]["x"] | 0.0;
        double y = f["geometry"]["y"] | 0.0;
        if (x == 0.0 && y == 0.0) continue;
        float lat = (float)y, lon = (float)x;
        if (navaid_duplicates_nearby_airport(ident, lat, lon)) continue;
        if (ident_dup_in_snap(snap, ident)) continue;
        FixPoint &p = snap->pts[snap->count];
        strlcpy(p.ident, ident, sizeof(p.ident));
        p.lon = lon;
        p.lat = lat;
        p.kind = FixKind::Navaid;
        p.area_charted = 1;
        snap->count++;
    }
}

static void append_cifp_terminal(Snapshot *snap, const char *icao,
                                 float center_lat, float center_lon, float radius_nm) {
    if (!icao || !icao[0]) return;
    cifp_load_airport_pc(icao);
    std::vector<CifpWp> local;
    {
        std::lock_guard<std::mutex> lock(g_cifp_mu);
        if (!g_cifp_ready || g_cifp_icao != icao) return;
        local = g_cifp_wps;
    }
    for (const CifpWp &wp : local) {
        if (snap->count >= kMaxFixes) break;
        if (haversine_nm(center_lat, center_lon, wp.lat, wp.lon) > radius_nm * 1.05f)
            continue;
        if (ident_dup_in_snap(snap, wp.ident)) continue;
        FixPoint &p = snap->pts[snap->count];
        strlcpy(p.ident, wp.ident, sizeof(p.ident));
        p.lat = wp.lat;
        p.lon = wp.lon;
        p.kind = FixKind::Terminal;
        p.area_charted = 1; // prefer labeling these
        snap->count++;
    }
}

static void sort_prefer_area_and_distance(Snapshot *snap) {
    auto score = [&](const FixPoint &p) {
        float d = haversine_nm(snap->lat, snap->lon, p.lat, p.lon);
        // Prefer terminal (approach) fixes, then charted area, then closer.
        float kind_pen = (p.kind == FixKind::Terminal) ? 0.0f
                         : (p.kind == FixKind::Designated) ? 50.0f
                                                           : 100.0f;
        return kind_pen + (p.area_charted ? 0.0f : 200.0f) + d;
    };
    for (int i = 1; i < snap->count; i++) {
        FixPoint key = snap->pts[i];
        float ks = score(key);
        int j = i - 1;
        while (j >= 0 && score(snap->pts[j]) > ks) {
            snap->pts[j + 1] = snap->pts[j];
            j--;
        }
        snap->pts[j + 1] = key;
    }
}

static void worker_fetch(float lat, float lon, float radius_nm, std::string icao) {
    double xmin, ymin, xmax, ymax;
    envelope(lat, lon, radius_nm, &xmin, &ymin, &xmax, &ymax);

    Snapshot snap{};
    snap.lat = lat;
    snap.lon = lon;
    snap.radius_nm = radius_nm;
    strlcpy(snap.airport_icao, icao.c_str(), sizeof(snap.airport_icao));

    // Terminal approach/SID/STAR fixes for the active airport first.
    if (!icao.empty())
        append_cifp_terminal(&snap, icao.c_str(), lat, lon, radius_nm);

    char url[640];
    snprintf(url, sizeof(url),
             "https://services6.arcgis.com/ssFJjBXIUyZDrSYZ/arcgis/rest/services/"
             "DesignatedPoints/FeatureServer/0/query"
             "?where=1%%3D1"
             "&geometry=%.5f%%2C%.5f%%2C%.5f%%2C%.5f"
             "&geometryType=esriGeometryEnvelope&inSR=4326"
             "&spatialRel=esriSpatialRelIntersects"
             "&outFields=IDENT%%2CTYPE_CODE%%2CUS_AREA"
             "&returnGeometry=true&outSR=4326"
             "&resultRecordCount=%d&f=json",
             xmin, ymin, xmax, ymax, kMaxFixes);

    std::string body;
    if (http_get_json(url, &body))
        append_designated(&snap, body.c_str());
    else
        platform_log_warn("Fixes: DesignatedPoints fetch failed\n");

    snprintf(url, sizeof(url),
             "https://services6.arcgis.com/ssFJjBXIUyZDrSYZ/arcgis/rest/services/"
             "NAVAIDSystem/FeatureServer/0/query"
             "?where=1%%3D1"
             "&geometry=%.5f%%2C%.5f%%2C%.5f%%2C%.5f"
             "&geometryType=esriGeometryEnvelope&inSR=4326"
             "&spatialRel=esriSpatialRelIntersects"
             "&outFields=IDENT%%2CTYPE_CODE"
             "&returnGeometry=true&outSR=4326"
             "&resultRecordCount=100&f=json",
             xmin, ymin, xmax, ymax);

    body.clear();
    if (http_get_json(url, &body))
        append_navaids(&snap, body.c_str());
    else
        platform_log_warn("Fixes: NAVAID fetch failed\n");

    sort_prefer_area_and_distance(&snap);

    int w = 0;
    for (int i = 0; i < snap.count; i++) {
        if (haversine_nm(lat, lon, snap.pts[i].lat, snap.pts[i].lon) <= radius_nm * 1.05f)
            snap.pts[w++] = snap.pts[i];
    }
    snap.count = w;

    int term = 0;
    for (int i = 0; i < snap.count; i++)
        if (snap.pts[i].kind == FixKind::Terminal) term++;

    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_pending = snap;
        g_pending_ready = true;
        g_fetched_lat = lat;
        g_fetched_lon = lon;
        g_fetched_radius = radius_nm;
        strlcpy(g_fetched_icao, icao.c_str(), sizeof(g_fetched_icao));
        g_have_fetched = true;
        g_busy = false;
    }
    platform_log_info("Fixes: loaded %d near %.2f,%.2f r=%.0fnm icao=%s (terminal=%d)\n",
                      snap.count, lat, lon, radius_nm,
                      icao.empty() ? "-" : icao.c_str(), term);
}

} // namespace

void fixes_clear(void) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_live = Snapshot{};
    g_pending = Snapshot{};
    g_pending_ready = false;
    g_have_live = false;
    g_have_fetched = false;
    g_fetched_icao[0] = '\0';
}

void fixes_request(float lat, float lon, float radius_nm, const char *airport_icao) {
    if (!map_fixes_shown()) {
        fixes_clear();
        return;
    }
    if (radius_nm < 1.0f) radius_nm = 1.0f;
    if (radius_nm > 200.0f) radius_nm = 200.0f;

    char icao[8] = {};
    if (airport_icao && airport_icao[0]) {
        // Normalize to up to 4 chars uppercase.
        size_t n = 0;
        for (const char *p = airport_icao; *p && n < 4; p++) {
            char c = *p;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                icao[n++] = c;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_busy) return;
        if (g_have_fetched) {
            float moved = haversine_nm(g_fetched_lat, g_fetched_lon, lat, lon);
            float rdelta = fabsf(g_fetched_radius - radius_nm);
            if (moved < kRelocNm && rdelta < 1.0f &&
                strcmp(g_fetched_icao, icao) == 0)
                return;
        }
        g_busy = true;
    }

    platform_log_info("Fixes: fetching near %.2f,%.2f r=%.0fnm icao=%s\n", lat, lon,
                      radius_nm, icao[0] ? icao : "-");
    std::string icao_str = icao;
    std::thread([lat, lon, radius_nm, icao_str]() {
        worker_fetch(lat, lon, radius_nm, icao_str);
    }).detach();
}

bool fixes_poll_swap(void) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_pending_ready) return false;
    g_live = g_pending;
    g_pending_ready = false;
    g_have_live = true;
    return true;
}

int fixes_count(void) {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_live.count;
}

void fixes_draw(lv_layer_t *layer,
                bool (*to_screen)(float lat, float lon, int *sx, int *sy),
                lv_color_t color, lv_opa_t opa) {
    if (!layer || !to_screen || !map_fixes_shown()) return;

    Snapshot local;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_have_live || g_live.count <= 0) return;
        local = g_live;
    }

    const bool show_labels = local.radius_nm <= 25.0f;
    // Generous cap: KDEN alone has ~40 terminal WPs inside 8 nm; FAFs like
    // GRASP (~7.7 nm) must still label at a 20 nm range setting.
    const int label_cap = (local.radius_nm <= 12.0f) ? local.count : 100;

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = color;
    line.width = 1;
    line.opa = opa;

    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.color = color;
    lbl.font = &lv_font_montserrat_10;
    lbl.opa = opa;
    lbl.text_local = 1;

    constexpr int kMark = 3;
    int labeled = 0;
    for (int i = 0; i < local.count; i++) {
        const FixPoint &p = local.pts[i];
        int sx = 0, sy = 0;
        if (!to_screen(p.lat, p.lon, &sx, &sy)) continue;

        if (p.kind == FixKind::Navaid) {
            line.p1 = {(lv_value_precise_t)sx, (lv_value_precise_t)(sy - kMark - 1)};
            line.p2 = {(lv_value_precise_t)(sx + kMark + 1), (lv_value_precise_t)sy};
            lv_draw_line(layer, &line);
            line.p1 = {(lv_value_precise_t)(sx + kMark + 1), (lv_value_precise_t)sy};
            line.p2 = {(lv_value_precise_t)sx, (lv_value_precise_t)(sy + kMark + 1)};
            lv_draw_line(layer, &line);
            line.p1 = {(lv_value_precise_t)sx, (lv_value_precise_t)(sy + kMark + 1)};
            line.p2 = {(lv_value_precise_t)(sx - kMark - 1), (lv_value_precise_t)sy};
            lv_draw_line(layer, &line);
            line.p1 = {(lv_value_precise_t)(sx - kMark - 1), (lv_value_precise_t)sy};
            line.p2 = {(lv_value_precise_t)sx, (lv_value_precise_t)(sy - kMark - 1)};
            lv_draw_line(layer, &line);
        } else {
            // Designated + terminal approach fixes share a small +.
            line.p1 = {(lv_value_precise_t)(sx - kMark), (lv_value_precise_t)sy};
            line.p2 = {(lv_value_precise_t)(sx + kMark), (lv_value_precise_t)sy};
            lv_draw_line(layer, &line);
            line.p1 = {(lv_value_precise_t)sx, (lv_value_precise_t)(sy - kMark)};
            line.p2 = {(lv_value_precise_t)sx, (lv_value_precise_t)(sy + kMark)};
            lv_draw_line(layer, &line);
        }

        if (show_labels && labeled < label_cap) {
            lbl.text = p.ident;
            lv_area_t area = {(lv_coord_t)(sx + kMark + 2), (lv_coord_t)(sy - 6),
                              (lv_coord_t)(sx + kMark + 52), (lv_coord_t)(sy + 6)};
            lv_draw_label(layer, &lbl, &area);
            labeled++;
        }
    }
}
