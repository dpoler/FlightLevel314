// Linux config storage -- JSON file at ~/.config/flightlevel314/config.json
// (or $XDG_CONFIG_HOME/flightlevel314/config.json). Field names match the
// historical ESP32 NVS keys from dpoler/adsb for easy config migration.

#include "../../src/data/storage.h"
#include "../../src/platform/platform.h"
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>

UserConfig g_config = {};

static std::string config_dir() {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) return std::string(xdg) + "/flightlevel314";
    const char *home = getenv("HOME");
    return std::string(home ? home : ".") + "/.config/flightlevel314";
}

static std::string config_file_path() {
    return config_dir() + "/config.json";
}

static UserConfig defaults() {
    UserConfig cfg{};
    cfg.wifi_ssid[0] = '\0';
    cfg.wifi_pass[0] = '\0';
    cfg.airportdb_token[0] = '\0';
    cfg.airportdb_enabled = true; // preserve pre-toggle behavior for existing tokens
    cfg.aerodatabox_key[0] = '\0';
    cfg.aerodatabox_provider = 0; // RapidAPI
    cfg.aerodatabox_enabled = false;
    cfg.adbox_usage_yyyymm = 0;
    cfg.adbox_usage_count = 0;
    cfg.adbox_soft_limit = 0;
    cfg.adbox_rate_limited = false;
    cfg.adbox_verify_ok = false;
    cfg.adbox_verify_prov = -1;
    cfg.adbox_verify_key_hash = 0;
    cfg.adbox_mkt_have_units = false;
    cfg.adbox_mkt_units_rem = 0;
    cfg.adbox_mkt_units_lim = 0;
    cfg.adbox_renew_day = 0;
    cfg.traffic_provider = 0; // adsb.lol
    cfg.radius_nm = 50;
    cfg.radius_presets[0] = 5;
    cfg.radius_presets[1] = 10;
    cfg.radius_presets[2] = 20;
    cfg.radius_presets[3] = 50;
    cfg.use_metric = false;
    cfg.use_ethernet = false;
    cfg.watchlist_count = 0;
    cfg.alert_military = true;
    cfg.alert_emergency = true;
    cfg.trail_style = 0;
    cfg.display_brightness_pct = 100;
    cfg.display_dim_after_min = 0;
    cfg.display_blank_after_min = 0;
    cfg.screensaver_enabled = false;
    cfg.screensaver_drift = true;
    for (int i = 0; i < 4; i++) {
        cfg.view_filter_mask[i] = 0;
        cfg.view_hide_ground[i] = true; // hide GND by default (fresh installs only)
    }
    for (int i = 0; i < 2; i++) {
        cfg.view_trails_enabled[i] = true;
        cfg.view_trail_max_points[i] = 30;
        cfg.view_show_tag_id[i] = true;
        cfg.view_show_tag_data[i] = false;
        cfg.view_show_tag_type[i] = false;
        cfg.view_show_secondary_locations[i] = true;
    }
    cfg.carto_basemap_key[0] = '\0';
    cfg.esri_basemap_key[0] = '\0';
    cfg.map_basemap_enabled = true;
    for (int i = 0; i < 7; i++) cfg.map_basemap_opa[i] = 50;
    cfg.map_basemap_style = 0;
    cfg.map_weather_enabled = false;
    cfg.map_weather_opa = 60;
    cfg.last_view_idx = 0;
    cfg.last_range_idx = 0;
    cfg.last_location_name[0] = '\0';
    return cfg;
}

// Reads config.json over `cfg` (which should start as defaults()). Missing =
// no file; Bad = empty/oversized or doesn't parse (cfg may be partly set).
// Silent -- storage_load_config() does the logging.
enum class LoadResult { Ok, Missing, Bad };
static LoadResult load_file(UserConfig &cfg) {
    FILE *f = fopen(config_file_path().c_str(), "r");
    if (!f) return LoadResult::Missing;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 65536) {
        fclose(f);
        return LoadResult::Bad;
    }
    std::string buf(size, '\0');
    size_t read = fread(&buf[0], 1, size, f);
    fclose(f);
    buf.resize(read);

    JsonDocument doc;
    if (deserializeJson(doc, buf) != DeserializationError::Ok) return LoadResult::Bad;

    strlcpy(cfg.wifi_ssid, doc["ssid"] | cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass, doc["pass"] | cfg.wifi_pass, sizeof(cfg.wifi_pass));
    strlcpy(cfg.airportdb_token, doc["apt_tok"] | cfg.airportdb_token, sizeof(cfg.airportdb_token));
    cfg.airportdb_enabled = doc["apt_en"] | cfg.airportdb_enabled;
    strlcpy(cfg.aerodatabox_key, doc["adbox_key"] | cfg.aerodatabox_key, sizeof(cfg.aerodatabox_key));
    cfg.aerodatabox_provider = doc["adbox_prov"] | cfg.aerodatabox_provider;
    if (cfg.aerodatabox_provider < 0 || cfg.aerodatabox_provider > 2) cfg.aerodatabox_provider = 0;
    cfg.aerodatabox_enabled = doc["adbox_en"] | cfg.aerodatabox_enabled;
    cfg.adbox_usage_yyyymm = doc["adbox_ym"] | cfg.adbox_usage_yyyymm;
    cfg.adbox_usage_count = doc["adbox_n"] | cfg.adbox_usage_count;
    cfg.adbox_soft_limit = doc["adbox_lim"] | cfg.adbox_soft_limit;
    cfg.adbox_rate_limited = doc["adbox_rl"] | cfg.adbox_rate_limited;
    cfg.adbox_verify_ok = doc["adbox_vok"] | cfg.adbox_verify_ok;
    cfg.adbox_verify_prov = doc["adbox_vprov"] | cfg.adbox_verify_prov;
    cfg.adbox_verify_key_hash = (uint32_t)(doc["adbox_vhash"] | cfg.adbox_verify_key_hash);
    cfg.adbox_mkt_have_units = doc["adbox_mu"] | cfg.adbox_mkt_have_units;
    cfg.adbox_mkt_units_rem = doc["adbox_murem"] | cfg.adbox_mkt_units_rem;
    cfg.adbox_mkt_units_lim = doc["adbox_mulim"] | cfg.adbox_mkt_units_lim;
    cfg.adbox_renew_day = doc["adbox_renew_day"] | cfg.adbox_renew_day;
    if (cfg.adbox_renew_day < 0 || cfg.adbox_renew_day > 31) cfg.adbox_renew_day = 0;
    cfg.traffic_provider = doc["traffic_prov"] | cfg.traffic_provider;
    if (cfg.traffic_provider < 0 || cfg.traffic_provider > 1) cfg.traffic_provider = 0;
    cfg.radius_nm = doc["radius"] | cfg.radius_nm;
    cfg.radius_presets[0] = doc["rad0"] | cfg.radius_presets[0];
    cfg.radius_presets[1] = doc["rad1"] | cfg.radius_presets[1];
    cfg.radius_presets[2] = doc["rad2"] | cfg.radius_presets[2];
    cfg.radius_presets[3] = doc["rad3"] | cfg.radius_presets[3];
    cfg.use_metric = doc["metric"] | cfg.use_metric;
    cfg.use_ethernet = doc["use_eth"] | cfg.use_ethernet;
    cfg.alert_military = doc["alrt_mil"] | cfg.alert_military;
    cfg.alert_emergency = doc["alrt_emg"] | cfg.alert_emergency;
    cfg.trail_style = doc["trail_sty"] | cfg.trail_style;
    cfg.display_brightness_pct = doc["disp_bright"] | cfg.display_brightness_pct;
    if (cfg.display_brightness_pct < 10) cfg.display_brightness_pct = 10;
    if (cfg.display_brightness_pct > 100) cfg.display_brightness_pct = 100;
    cfg.display_dim_after_min = doc["disp_dimmin"] | cfg.display_dim_after_min;
    cfg.display_blank_after_min = doc["disp_blkmin"] | cfg.display_blank_after_min;
    cfg.screensaver_enabled = doc["ss_enabled"] | cfg.screensaver_enabled;
    cfg.screensaver_drift = doc["ss_drift"] | cfg.screensaver_drift;
    for (int i = 0; i < 4; i++) {
        char key[12];
        snprintf(key, sizeof(key), "filt_m%d", i);
        cfg.view_filter_mask[i] = doc[key] | cfg.view_filter_mask[i];
        snprintf(key, sizeof(key), "hide_gnd%d", i);
        cfg.view_hide_ground[i] = doc[key] | cfg.view_hide_ground[i];
    }
    cfg.view_trails_enabled[0] = doc["trail_on0"] | cfg.view_trails_enabled[0];
    cfg.view_trails_enabled[1] = doc["trail_on1"] | cfg.view_trails_enabled[1];
    cfg.view_trail_max_points[0] = doc["trail_pts0"] | cfg.view_trail_max_points[0];
    cfg.view_trail_max_points[1] = doc["trail_pts1"] | cfg.view_trail_max_points[1];
    cfg.view_show_tag_id[0] = doc["tag_id0"] | cfg.view_show_tag_id[0];
    cfg.view_show_tag_id[1] = doc["tag_id1"] | cfg.view_show_tag_id[1];
    cfg.view_show_tag_data[0] = doc["tag_data0"] | cfg.view_show_tag_data[0];
    cfg.view_show_tag_data[1] = doc["tag_data1"] | cfg.view_show_tag_data[1];
    cfg.view_show_tag_type[0] = doc["tag_type0"] | cfg.view_show_tag_type[0];
    cfg.view_show_tag_type[1] = doc["tag_type1"] | cfg.view_show_tag_type[1];
    cfg.view_show_secondary_locations[0] = doc["show2loc0"] | cfg.view_show_secondary_locations[0];
    cfg.view_show_secondary_locations[1] = doc["show2loc1"] | cfg.view_show_secondary_locations[1];
    strlcpy(cfg.carto_basemap_key, doc["carto_key"] | cfg.carto_basemap_key,
            sizeof(cfg.carto_basemap_key));
    strlcpy(cfg.esri_basemap_key, doc["esri_key"] | cfg.esri_basemap_key,
            sizeof(cfg.esri_basemap_key));
    cfg.map_basemap_enabled = doc["bm_on"] | cfg.map_basemap_enabled;
    // Legacy single bm_opa seeds all styles if per-style keys are absent.
    int legacy_opa = doc["bm_opa"] | 50;
    if (legacy_opa < 10) legacy_opa = 10;
    if (legacy_opa > 100) legacy_opa = 100;
    for (int i = 0; i < 7; i++) {
        char key[12];
        snprintf(key, sizeof(key), "bm_opa%d", i);
        cfg.map_basemap_opa[i] = doc[key] | legacy_opa;
        if (cfg.map_basemap_opa[i] < 10) cfg.map_basemap_opa[i] = 10;
        if (cfg.map_basemap_opa[i] > 100) cfg.map_basemap_opa[i] = 100;
    }
    cfg.map_basemap_style = doc["bm_style"] | cfg.map_basemap_style;
    if (cfg.map_basemap_style < 0) cfg.map_basemap_style = 0;
    if (cfg.map_basemap_style > 6) cfg.map_basemap_style = 6;
    cfg.map_weather_enabled = doc["wx_on"] | cfg.map_weather_enabled;
    cfg.map_weather_opa = doc["wx_opa"] | cfg.map_weather_opa;
    if (cfg.map_weather_opa < 10) cfg.map_weather_opa = 10;
    if (cfg.map_weather_opa > 100) cfg.map_weather_opa = 100;
    cfg.last_view_idx = doc["last_view"] | cfg.last_view_idx;
    cfg.last_range_idx = doc["last_rng"] | cfg.last_range_idx;
    strlcpy(cfg.last_location_name, doc["last_loc"] | cfg.last_location_name, sizeof(cfg.last_location_name));
    return LoadResult::Ok;
}

UserConfig storage_load_config() {
    UserConfig cfg = defaults();
    switch (load_file(cfg)) {
    case LoadResult::Ok:
        platform_log_info("Storage: config loaded from %s\n", config_file_path().c_str());
        break;
    case LoadResult::Missing:
        platform_log_info("Storage: no config file yet at %s, using defaults\n",
                          config_file_path().c_str());
        break;
    case LoadResult::Bad:
        platform_log_warn("Storage: %s failed to parse, using defaults\n",
                          config_file_path().c_str());
        cfg = defaults();
        break;
    }
    return cfg;
}

// Saves come from the UI thread and background threads (AeroDataBox
// counters / verify, quota headers) -- serialize them so two writers can't
// interleave the disk-key read and the rename.
static std::mutex _save_mutex;

void storage_save_config(const UserConfig &cfg) {
    std::lock_guard<std::mutex> save_lock(_save_mutex);
    platform_mkdirs(config_dir().c_str());

    // API keys (and the billing renew day) are only ever written by
    // tools/set_api_keys.py -- the app never edits them. Take them from the
    // file on disk, not from `cfg`: the running app's in-memory copy predates
    // any key set (or cleared) over SSH since boot, and saving it used to
    // silently erase the new key on the next range tap / location switch.
    // Missing file (first run, factory reset) -> fall back to `cfg`.
    UserConfig disk = defaults();
    const bool have_disk = (load_file(disk) == LoadResult::Ok);
    const UserConfig &keys = have_disk ? disk : cfg;

    JsonDocument doc;
    doc["ssid"] = cfg.wifi_ssid;
    doc["pass"] = cfg.wifi_pass;
    doc["apt_tok"] = keys.airportdb_token;
    doc["apt_en"] = cfg.airportdb_enabled;
    doc["adbox_key"] = keys.aerodatabox_key;
    doc["adbox_prov"] = cfg.aerodatabox_provider;
    doc["adbox_en"] = cfg.aerodatabox_enabled;
    doc["adbox_ym"] = cfg.adbox_usage_yyyymm;
    doc["adbox_n"] = cfg.adbox_usage_count;
    doc["adbox_lim"] = cfg.adbox_soft_limit;
    doc["adbox_rl"] = cfg.adbox_rate_limited;
    doc["adbox_vok"] = cfg.adbox_verify_ok;
    doc["adbox_vprov"] = cfg.adbox_verify_prov;
    doc["adbox_vhash"] = cfg.adbox_verify_key_hash;
    doc["adbox_mu"] = cfg.adbox_mkt_have_units;
    doc["adbox_murem"] = cfg.adbox_mkt_units_rem;
    doc["adbox_mulim"] = cfg.adbox_mkt_units_lim;
    doc["adbox_renew_day"] = keys.adbox_renew_day;
    doc["traffic_prov"] = cfg.traffic_provider;
    doc["radius"] = cfg.radius_nm;
    doc["rad0"] = cfg.radius_presets[0];
    doc["rad1"] = cfg.radius_presets[1];
    doc["rad2"] = cfg.radius_presets[2];
    doc["rad3"] = cfg.radius_presets[3];
    doc["metric"] = cfg.use_metric;
    doc["use_eth"] = cfg.use_ethernet;
    doc["alrt_mil"] = cfg.alert_military;
    doc["alrt_emg"] = cfg.alert_emergency;
    doc["trail_sty"] = cfg.trail_style;
    doc["disp_bright"] = cfg.display_brightness_pct;
    doc["disp_dimmin"] = cfg.display_dim_after_min;
    doc["disp_blkmin"] = cfg.display_blank_after_min;
    doc["ss_enabled"] = cfg.screensaver_enabled;
    doc["ss_drift"] = cfg.screensaver_drift;
    for (int i = 0; i < 4; i++) {
        char key[12];
        snprintf(key, sizeof(key), "filt_m%d", i);
        doc[key] = cfg.view_filter_mask[i];
        snprintf(key, sizeof(key), "hide_gnd%d", i);
        doc[key] = cfg.view_hide_ground[i];
    }
    doc["trail_on0"] = cfg.view_trails_enabled[0];
    doc["trail_on1"] = cfg.view_trails_enabled[1];
    doc["trail_pts0"] = cfg.view_trail_max_points[0];
    doc["trail_pts1"] = cfg.view_trail_max_points[1];
    doc["tag_id0"] = cfg.view_show_tag_id[0];
    doc["tag_id1"] = cfg.view_show_tag_id[1];
    doc["tag_data0"] = cfg.view_show_tag_data[0];
    doc["tag_data1"] = cfg.view_show_tag_data[1];
    doc["tag_type0"] = cfg.view_show_tag_type[0];
    doc["tag_type1"] = cfg.view_show_tag_type[1];
    doc["show2loc0"] = cfg.view_show_secondary_locations[0];
    doc["show2loc1"] = cfg.view_show_secondary_locations[1];
    doc["carto_key"] = keys.carto_basemap_key;
    doc["esri_key"] = keys.esri_basemap_key;
    doc["bm_on"] = cfg.map_basemap_enabled;
    for (int i = 0; i < 7; i++) {
        char key[12];
        snprintf(key, sizeof(key), "bm_opa%d", i);
        doc[key] = cfg.map_basemap_opa[i];
    }
    doc["bm_opa"] = cfg.map_basemap_opa[0]; // legacy
    doc["bm_style"] = cfg.map_basemap_style;
    doc["wx_on"] = cfg.map_weather_enabled;
    doc["wx_opa"] = cfg.map_weather_opa;
    doc["last_view"] = cfg.last_view_idx;
    doc["last_rng"] = cfg.last_range_idx;
    doc["last_loc"] = cfg.last_location_name;

    // Atomic replace: a power cut mid-save used to leave a truncated file,
    // which loads as defaults -- losing every API key.
    std::string out;
    serializeJson(doc, out);
    if (!platform_write_file_atomic(config_file_path().c_str(), out.data(), out.size())) {
        platform_log_error("Storage: failed to write %s\n", config_file_path().c_str());
        return;
    }
    platform_log_debug("Storage: config saved to %s\n", config_file_path().c_str());
}

void storage_factory_reset() {
    remove(config_file_path().c_str());
    platform_log_info("Storage: config file removed (factory reset)\n");
}
