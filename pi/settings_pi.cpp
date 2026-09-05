// Scoped-down Pi implementation of src/ui/settings.h -- deliberately NOT
// a port of src/ui/settings.cpp. That file is CONFIG (WiFi SSID/password,
// Ethernet toggle + reboot-on-change, OTA firmware update) plus STATUS
// (ESP32 heap/PSRAM/temperature/FreeRTOS task count/flash %) -- almost
// none of which applies to a Pi whose networking, firmware updates, and
// system stats work completely differently. Kept: range presets, metric
// units (still meaningful config), optional API key status/enable toggles
// (keys themselves are hand-edited in config.json), and a read-only status
// strip using what's actually already real on Pi -- fetcher_get_stats() and
// error_log.cpp, both ported for real (see project_pi_port memory).
//
// Dropped entirely for now: WiFi/Ethernet UI (Pi's networking is
// OS-managed), heap/PSRAM/temp/tasks/flash (ESP32-specific). Online
// app updates are implemented via GitHub Releases (ota_linux.cpp).

#include "../src/ui/settings.h"
#include "../src/data/enrichment.h"
#include "../src/data/error_log.h"
#include "../src/data/fetcher.h"
#include "../src/data/locations.h"
#include "../src/data/ota.h"
#include "../src/platform/platform.h"
#include "../src/ui/location_picker.h"
#include "../src/ui/map_view.h"
#include "../src/ui/range.h"
#include "../src/version.h"
#include "basemap.h"
#include "weather.h"
#include "backlight.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

static lv_obj_t *_overlay = nullptr;
static lv_obj_t *_panel = nullptr;
static lv_obj_t *_content = nullptr; // body above the action row (no scroll)
static lv_obj_t *_keyboard = nullptr;
static bool _visible = false;
static uint32_t _shown_at_ms = 0;
static uint32_t _boot_time_ms = 0;

static lv_obj_t *_ta_radius[4] = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t *_sw_metric = nullptr;
static lv_obj_t *_dd_traffic_prov = nullptr;
static lv_obj_t *_fetch_val = nullptr;
static lv_obj_t *_latency_val = nullptr;
static lv_obj_t *_uptime_val = nullptr;
static lv_obj_t *_err_count_lbl = nullptr;
static lv_obj_t *_err_list_lbl = nullptr;
static lv_obj_t *_factory_lbl = nullptr;
static uint32_t _factory_confirm_until_ms = 0;

// API KEYS section -- keys are never typed here; only presence / live
// validity / enable / AeroDataBox provider. Validity checked on open.
static lv_obj_t *_apt_key_val = nullptr;
static lv_obj_t *_apt_valid_val = nullptr;
static lv_obj_t *_sw_apt_en = nullptr;
static lv_obj_t *_adbox_key_val = nullptr;
static lv_obj_t *_adbox_valid_val = nullptr;
static lv_obj_t *_sw_adbox_en = nullptr;
static lv_obj_t *_dd_adbox_prov = nullptr;
static lv_obj_t *_adbox_usage_val = nullptr;
static lv_obj_t *_carto_key_val = nullptr;
static lv_obj_t *_ota_ver_val = nullptr;
static lv_obj_t *_ota_status_lbl = nullptr;
static lv_obj_t *_ota_btn_lbl = nullptr;
static OtaStatus _last_ota_ui = OTA_IDLE;
static lv_obj_t *_bright_label = nullptr;
static lv_obj_t *_bright_slider = nullptr;

enum class KeyValid : uint8_t { Unknown, Checking, Valid, Invalid, Missing };
static KeyValid _apt_valid = KeyValid::Unknown;
static KeyValid _adbox_valid = KeyValid::Unknown;
static bool _apt_verify_pending = false;
static bool _adbox_verify_pending = false;

static UserConfig _cfg;           // draft while Settings is open
static UserConfig _cfg_at_open;   // snapshot for Cancel restore
static settings_changed_cb_t _on_change = nullptr;

// Wide enough for three columns on 1280x800 (fits under status bar + margin).
#define PANEL_W 1100
#define PANEL_H 700
#define TITLE_H 36
#define ACTION_H 100
#define COL_GAP 20
#define COL_W ((PANEL_W - 40 - 2 * COL_GAP) / 3) // pad_all ~20 each side
#define LABEL_COLOR lv_color_hex(0x8888aa)
#define BG_COLOR lv_color_hex(0x12122a)
#define ACCENT_COLOR lv_color_hex(0x00cc66)
#define SYS_COLOR lv_color_hex(0x44cc88)
#define WARN_COLOR lv_color_hex(0xffaa44)
#define ERR_COLOR lv_color_hex(0xff6666)

static const char *const ADBOX_PROVIDER_OPTS =
    "RapidAPI\nAPI.Market\nDirect (aerodatabox.com)";
static const char *const TRAFFIC_PROVIDER_OPTS =
    "adsb.lol\nadsb.fi";

static void ta_focus_cb(lv_event_t *e) {
    lv_keyboard_set_textarea(_keyboard, lv_event_get_target_obj(e));
    lv_keyboard_set_mode(_keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_clear_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void keyboard_ready_cb(lv_event_t *e) {
    (void)e;
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, int x, int y) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, LABEL_COLOR, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

static lv_obj_t *create_inline_row(lv_obj_t *parent, const char *header, int x, int y, int val_off) {
    create_label(parent, header, x, y);
    lv_obj_t *v = lv_label_create(parent);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(v, SYS_COLOR, 0);
    lv_obj_set_pos(v, x + val_off, y);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
    return v;
}

static void set_valid_label(lv_obj_t *lbl, KeyValid v) {
    if (!lbl) return;
    switch (v) {
    case KeyValid::Missing:
        lv_obj_set_style_text_color(lbl, WARN_COLOR, 0);
        lv_label_set_text(lbl, "-");
        break;
    case KeyValid::Checking:
        lv_obj_set_style_text_color(lbl, LABEL_COLOR, 0);
        lv_label_set_text(lbl, "checking...");
        break;
    case KeyValid::Valid:
        lv_obj_set_style_text_color(lbl, SYS_COLOR, 0);
        lv_label_set_text(lbl, "yes");
        break;
    case KeyValid::Invalid:
        lv_obj_set_style_text_color(lbl, ERR_COLOR, 0);
        lv_label_set_text(lbl, "no");
        break;
    default:
        lv_obj_set_style_text_color(lbl, LABEL_COLOR, 0);
        lv_label_set_text(lbl, "--");
        break;
    }
}

static const char *day_ordinal_suffix(int d) {
    // 1st/2nd/3rd/4th … 11th/12th/13th … 21st/22nd/23rd/31st
    int mod100 = d % 100;
    if (mod100 >= 11 && mod100 <= 13) return "th";
    switch (d % 10) {
    case 1: return "st";
    case 2: return "nd";
    case 3: return "rd";
    default: return "th";
    }
}

static void refresh_adbox_usage_ui() {
    if (!_adbox_usage_val) return;
    int ym = 0, n = 0, lim = 0;
    bool rl = false;
    aerodatabox_usage_snapshot(&ym, &n, &lim, &rl);
    static const char *const MONTHS[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    int month = ym % 100;
    const char *mon = (month >= 1 && month <= 12) ? MONTHS[month - 1] : "???";

    int u_rem = -1, u_lim = -1, r_rem = -1, r_lim = -1;
    const bool have_mkt = aerodatabox_marketplace_quota(
        &u_rem, &u_lim, &r_rem, &r_lim);
    const int renew = _cfg.adbox_renew_day; // 1–31, 0 = unset

    auto append_renew = [&](char *dst, size_t dst_sz) {
        if (renew < 1 || renew > 31 || dst_sz == 0) return;
        size_t used = strlen(dst);
        // " · resets ~31st of every month" ≈ 34 chars
        if (used + 40 >= dst_sz) return;
        // e.g. " · resets ~9th of every month"
        snprintf(dst + used, dst_sz - used, " · resets ~%d%s of every month",
                 renew, day_ordinal_suffix(renew));
    };

    // Marketplace first: "used of limit" (matches RapidAPI dashboard).
    char buf[128];
    if (have_mkt && u_lim >= 0 && u_rem >= 0) {
        int used = u_lim - u_rem;
        if (used < 0) used = 0;
        if (rl || u_rem <= 0) {
            snprintf(buf, sizeof(buf), "%d of %d OFF", used, u_lim);
            lv_obj_set_style_text_color(_adbox_usage_val, ERR_COLOR, 0);
        } else {
            snprintf(buf, sizeof(buf), "%d of %d", used, u_lim);
            append_renew(buf, sizeof(buf));
            const bool high = (u_lim > 0 && used * 10 >= u_lim * 9);
            lv_obj_set_style_text_color(_adbox_usage_val, high ? WARN_COLOR : SYS_COLOR, 0);
        }
    } else if (have_mkt && r_lim >= 0 && r_rem >= 0) {
        int used = r_lim - r_rem;
        if (used < 0) used = 0;
        if (rl || r_rem <= 0) {
            snprintf(buf, sizeof(buf), "%d of %d r OFF", used, r_lim);
            lv_obj_set_style_text_color(_adbox_usage_val, ERR_COLOR, 0);
        } else {
            snprintf(buf, sizeof(buf), "%d of %d r", used, r_lim);
            append_renew(buf, sizeof(buf));
            lv_obj_set_style_text_color(_adbox_usage_val, SYS_COLOR, 0);
        }
    } else if (rl) {
        snprintf(buf, sizeof(buf), "%d (%s UTC) AUTO-OFF", n, mon);
        lv_obj_set_style_text_color(_adbox_usage_val, ERR_COLOR, 0);
    } else if (lim > 0) {
        snprintf(buf, sizeof(buf), "%d/%d (%s UTC)", n, lim, mon);
        lv_obj_set_style_text_color(_adbox_usage_val, n >= lim ? WARN_COLOR : SYS_COLOR, 0);
    } else {
        snprintf(buf, sizeof(buf), "%d (%s UTC)", n, mon);
        lv_obj_set_style_text_color(_adbox_usage_val, SYS_COLOR, 0);
    }
    lv_label_set_text(_adbox_usage_val, buf);
}

static void refresh_key_presence_ui() {
    if (_apt_key_val) {
        if (_cfg.airportdb_token[0]) {
            lv_obj_set_style_text_color(_apt_key_val, SYS_COLOR, 0);
            lv_label_set_text(_apt_key_val, "present");
        } else {
            lv_obj_set_style_text_color(_apt_key_val, WARN_COLOR, 0);
            lv_label_set_text(_apt_key_val, "missing");
        }
    }
    if (_adbox_key_val) {
        if (_cfg.aerodatabox_key[0]) {
            lv_obj_set_style_text_color(_adbox_key_val, SYS_COLOR, 0);
            lv_label_set_text(_adbox_key_val, "present");
        } else {
            lv_obj_set_style_text_color(_adbox_key_val, WARN_COLOR, 0);
            lv_label_set_text(_adbox_key_val, "missing");
        }
    }
    if (_carto_key_val) {
        if (_cfg.carto_basemap_key[0]) {
            lv_obj_set_style_text_color(_carto_key_val, SYS_COLOR, 0);
            lv_label_set_text(_carto_key_val, "present");
        } else {
            lv_obj_set_style_text_color(_carto_key_val, WARN_COLOR, 0);
            lv_label_set_text(_carto_key_val, "missing");
        }
    }

    if (_sw_apt_en) {
        if (_apt_valid == KeyValid::Valid) lv_obj_clear_state(_sw_apt_en, LV_STATE_DISABLED);
        else lv_obj_add_state(_sw_apt_en, LV_STATE_DISABLED);
    }
    if (_sw_adbox_en) {
        if (_adbox_valid == KeyValid::Valid) lv_obj_clear_state(_sw_adbox_en, LV_STATE_DISABLED);
        else lv_obj_add_state(_sw_adbox_en, LV_STATE_DISABLED);
    }
    refresh_adbox_usage_ui();
}

static void start_key_validation() {
    _apt_verify_pending = false;
    _adbox_verify_pending = false;

    if (!_cfg.airportdb_token[0]) {
        _apt_valid = KeyValid::Missing;
        set_valid_label(_apt_valid_val, _apt_valid);
    } else {
        _apt_valid = KeyValid::Checking;
        set_valid_label(_apt_valid_val, _apt_valid);
        strlcpy(g_config.airportdb_token, _cfg.airportdb_token, sizeof(g_config.airportdb_token));
        locations_request_verify_token();
        _apt_verify_pending = true;
    }

    if (!_cfg.aerodatabox_key[0]) {
        _adbox_valid = KeyValid::Missing;
        set_valid_label(_adbox_valid_val, _adbox_valid);
    } else {
        _adbox_valid = KeyValid::Checking;
        set_valid_label(_adbox_valid_val, _adbox_valid);
        strlcpy(g_config.aerodatabox_key, _cfg.aerodatabox_key, sizeof(g_config.aerodatabox_key));
        g_config.aerodatabox_provider = _cfg.aerodatabox_provider;
        aerodatabox_request_verify();
        _adbox_verify_pending = true;
    }
    refresh_key_presence_ui();
}

static void on_adbox_provider_changed(lv_event_t *e) {
    (void)e;
    if (!_dd_adbox_prov) return;
    int sel = (int)lv_dropdown_get_selected(_dd_adbox_prov);
    if (sel < 0 || sel > 2) sel = 0;
    if (sel == _cfg.aerodatabox_provider) return;
    _cfg.aerodatabox_provider = sel;
    g_config.aerodatabox_provider = sel;
    // Re-validate against the newly selected gateway.
    if (_cfg.aerodatabox_key[0]) {
        _adbox_valid = KeyValid::Checking;
        set_valid_label(_adbox_valid_val, _adbox_valid);
        aerodatabox_request_verify();
        _adbox_verify_pending = true;
        refresh_key_presence_ui();
    }
}

static void on_traffic_provider_changed(lv_event_t *e) {
    (void)e;
    if (!_dd_traffic_prov) return;
    int sel = (int)lv_dropdown_get_selected(_dd_traffic_prov);
    if (sel < 0 || sel > 1) sel = 0;
    if (sel == _cfg.traffic_provider) return;
    _cfg.traffic_provider = sel;
    g_config.traffic_provider = sel;
    // Live preview only — disk write is on Save (Settings requires Save or
    // Cancel to dismiss). Cancel restores the provider from open.
    fetcher_request_immediate_fetch();
}

static void poll_key_validation() {
    if (_apt_verify_pending) {
        bool ok = false;
        if (locations_verify_token_result(&ok, nullptr, 0)) {
            _apt_verify_pending = false;
            _apt_valid = ok ? KeyValid::Valid : KeyValid::Invalid;
            set_valid_label(_apt_valid_val, _apt_valid);
            if (!ok && _sw_apt_en) {
                lv_obj_clear_state(_sw_apt_en, LV_STATE_CHECKED);
                _cfg.airportdb_enabled = false;
            }
            refresh_key_presence_ui();
        }
    }
    if (_adbox_verify_pending) {
        bool ok = false;
        if (aerodatabox_verify_result(&ok, nullptr, 0)) {
            _adbox_verify_pending = false;
            _adbox_valid = ok ? KeyValid::Valid : KeyValid::Invalid;
            set_valid_label(_adbox_valid_val, _adbox_valid);
            if (!ok && _sw_adbox_en) {
                lv_obj_clear_state(_sw_adbox_en, LV_STATE_CHECKED);
                _cfg.aerodatabox_enabled = false;
            }
            refresh_key_presence_ui();
        }
    }
}

static void refresh_adbox_usage_ui();
static void refresh_ota_ui();

static void status_refresh(lv_timer_t *t) {
    (void)t;
    if (_visible) {
        poll_key_validation();
        refresh_adbox_usage_ui();
        if (_last_ota_ui != ota_status || ota_status == OTA_DOWNLOADING)
            refresh_ota_ui();
    }
    if (!_fetch_val) return;

    const FetcherStats *fs = fetcher_get_stats();
    lv_label_set_text_fmt(_fetch_val, "%lu ok / %lu err", (unsigned long)fs->fetch_ok, (unsigned long)fs->fetch_fail);
    if (fs->last_fetch_ms > 0) lv_label_set_text_fmt(_latency_val, "%lums", (unsigned long)fs->last_fetch_ms);

    uint32_t uptime_s = (platform_millis() - _boot_time_ms) / 1000;
    lv_label_set_text_fmt(_uptime_val, "%02d:%02d:%02d",
        (int)(uptime_s / 3600), (int)((uptime_s % 3600) / 60), (int)(uptime_s % 60));

    uint32_t err_total = error_log_total_count();
    lv_label_set_text_fmt(_err_count_lbl, "(%lu)", (unsigned long)err_total);
    ErrorSnapshot snap = error_log_snapshot();
    if (snap.count == 0) {
        lv_label_set_text(_err_list_lbl, "(none)");
    } else {
        static char buf[256];
        int pos = 0;
        uint32_t now = platform_millis();
        for (int i = snap.count - 1; i >= 0 && pos < (int)sizeof(buf) - 60; i--) {
            uint32_t age_s = (now - snap.entries[i].timestamp) / 1000;
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%dm%02ds %s\n",
                            (int)(age_s / 60), (int)(age_s % 60), snap.entries[i].msg);
        }
        if (pos > 0) buf[pos - 1] = '\0';
        lv_label_set_text(_err_list_lbl, buf);
    }

    if (_factory_confirm_until_ms && platform_millis() > _factory_confirm_until_ms) {
        _factory_confirm_until_ms = 0;
        if (_factory_lbl) lv_label_set_text(_factory_lbl, "Reset to factory defaults");
    }
}

static void apply_cfg_to_fields() {
    for (int i = 0; i < 4; i++) {
        char rbuf[8];
        snprintf(rbuf, sizeof(rbuf), "%d", _cfg.radius_presets[i]);
        lv_textarea_set_text(_ta_radius[i], rbuf);
    }
    if (_cfg.use_metric) lv_obj_add_state(_sw_metric, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_metric, LV_STATE_CHECKED);

    int tprov = _cfg.traffic_provider;
    if (tprov < 0 || tprov > 1) tprov = 0;
    if (_dd_traffic_prov) lv_dropdown_set_selected(_dd_traffic_prov, (uint16_t)tprov);

    if (_cfg.airportdb_enabled) lv_obj_add_state(_sw_apt_en, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_apt_en, LV_STATE_CHECKED);
    if (_cfg.aerodatabox_enabled) lv_obj_add_state(_sw_adbox_en, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_adbox_en, LV_STATE_CHECKED);

    int prov = _cfg.aerodatabox_provider;
    if (prov < 0 || prov > 2) prov = 0;
    if (_dd_adbox_prov) lv_dropdown_set_selected(_dd_adbox_prov, (uint16_t)prov);

    if (_bright_slider) {
        int b = _cfg.display_brightness_pct;
        if (b < 10) b = 10;
        if (b > 100) b = 100;
        lv_slider_set_value(_bright_slider, b, LV_ANIM_OFF);
        if (_bright_label) lv_label_set_text_fmt(_bright_label, "%d%%", b);
    }

    refresh_key_presence_ui();
}

static void cancel_and_close(lv_event_t *e) {
    (void)e;
    // Revert anything previewed live while the panel was open (brightness,
    // traffic/ADB provider). Disk is untouched for draft fields; brightness
    // may have been preview-only (no mid-edit writes).
    const int live_traffic = g_config.traffic_provider;
    g_config.display_brightness_pct = _cfg_at_open.display_brightness_pct;
    g_config.traffic_provider = _cfg_at_open.traffic_provider;
    g_config.aerodatabox_provider = _cfg_at_open.aerodatabox_provider;
    backlight_set_percent(g_config.display_brightness_pct);
    if (live_traffic != g_config.traffic_provider)
        fetcher_request_immediate_fetch();
    _cfg = _cfg_at_open;
    settings_hide();
}

static void save_and_close(lv_event_t *e) {
    (void)e;
    for (int i = 0; i < 4; i++) {
        int v = atoi(lv_textarea_get_text(_ta_radius[i]));
        if (v < 1) v = 1;
        if (v > 500) v = 500;
        _cfg.radius_presets[i] = v;
    }
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 4; j++)
            if (_cfg.radius_presets[i] > _cfg.radius_presets[j]) {
                int tmp = _cfg.radius_presets[i];
                _cfg.radius_presets[i] = _cfg.radius_presets[j];
                _cfg.radius_presets[j] = tmp;
            }
    _cfg.radius_nm = _cfg.radius_presets[3];
    _cfg.use_metric = lv_obj_has_state(_sw_metric, LV_STATE_CHECKED);

    if (_dd_traffic_prov) {
        int sel = (int)lv_dropdown_get_selected(_dd_traffic_prov);
        if (sel < 0 || sel > 1) sel = 0;
        _cfg.traffic_provider = sel;
    }

    if (_dd_adbox_prov) {
        int sel = (int)lv_dropdown_get_selected(_dd_adbox_prov);
        if (sel < 0 || sel > 2) sel = 0;
        _cfg.aerodatabox_provider = sel;
    }

    bool apt_en = lv_obj_has_state(_sw_apt_en, LV_STATE_CHECKED) && _apt_valid == KeyValid::Valid;
    bool adbox_en = lv_obj_has_state(_sw_adbox_en, LV_STATE_CHECKED) && _adbox_valid == KeyValid::Valid;
    bool clear_enrich = (adbox_en != g_config.aerodatabox_enabled)
                        || (_cfg.aerodatabox_provider != g_config.aerodatabox_provider);
    _cfg.airportdb_enabled = apt_en;
    _cfg.aerodatabox_enabled = adbox_en;
    // Re-enabling clears sticky rate-limit / soft-cap lockout.
    if (adbox_en && g_config.adbox_rate_limited) {
        aerodatabox_clear_rate_limit();
        _cfg.adbox_rate_limited = false;
    }

    if (_bright_slider) {
        int b = (int)lv_slider_get_value(_bright_slider);
        if (b < 10) b = 10;
        if (b > 100) b = 100;
        _cfg.display_brightness_pct = b;
    }

    // Settings never edits renewal day (set_api_keys.py). Keep on-disk value
    // so Save after a mid-open --adbox-renew-day does not wipe it.
    {
        UserConfig disk = storage_load_config();
        _cfg.adbox_renew_day = disk.adbox_renew_day;
    }

    storage_save_config(_cfg);
    if (clear_enrich) enrichment_clear_cache();
    if (_on_change) _on_change(&_cfg);
    settings_hide();
}

static void refresh_ota_ui() {
    if (!_ota_status_lbl || !_ota_btn_lbl) return;
    _last_ota_ui = ota_status;
    switch (ota_status) {
    case OTA_IDLE:
        lv_label_set_text(_ota_status_lbl, "Tap to check");
        lv_obj_set_style_text_color(_ota_status_lbl, LABEL_COLOR, 0);
        lv_label_set_text(_ota_btn_lbl, "Check for update");
        break;
    case OTA_CHECKING:
        lv_label_set_text(_ota_status_lbl, "Checking...");
        lv_obj_set_style_text_color(_ota_status_lbl, WARN_COLOR, 0);
        lv_label_set_text(_ota_btn_lbl, "Checking...");
        break;
    case OTA_UP_TO_DATE:
        lv_label_set_text_fmt(_ota_status_lbl, "Up to date (%s)",
                              ota_latest_tag[0] ? ota_latest_tag : FIRMWARE_VERSION_STR);
        lv_obj_set_style_text_color(_ota_status_lbl, SYS_COLOR, 0);
        lv_label_set_text(_ota_btn_lbl, "Check again");
        break;
    case OTA_AVAILABLE:
        lv_label_set_text_fmt(_ota_status_lbl, "Available: %s", ota_latest_tag);
        lv_obj_set_style_text_color(_ota_status_lbl, WARN_COLOR, 0);
        lv_label_set_text(_ota_btn_lbl, "Install & restart");
        break;
    case OTA_DOWNLOADING:
        lv_label_set_text_fmt(_ota_status_lbl, "Downloading %d%%", ota_progress);
        lv_obj_set_style_text_color(_ota_status_lbl, WARN_COLOR, 0);
        lv_label_set_text(_ota_btn_lbl, "Downloading...");
        break;
    case OTA_DONE:
        lv_label_set_text(_ota_status_lbl, "Installed - restarting");
        lv_obj_set_style_text_color(_ota_status_lbl, SYS_COLOR, 0);
        lv_label_set_text(_ota_btn_lbl, "Restarting...");
        break;
    case OTA_ERROR:
    default:
        lv_label_set_text(_ota_status_lbl, "Check failed (see log)");
        lv_obj_set_style_text_color(_ota_status_lbl, ERR_COLOR, 0);
        lv_label_set_text(_ota_btn_lbl, "Retry check");
        break;
    }
}

static void ota_btn_cb(lv_event_t *e) {
    (void)e;
    if (ota_status == OTA_AVAILABLE) ota_request_update();
    else if (ota_status != OTA_CHECKING && ota_status != OTA_DOWNLOADING) ota_request_check();
    refresh_ota_ui();
}

static void clear_all_caches_cb(lv_event_t *e) {
    (void)e;
    int n = basemap_cache_clear();
    int nw = weather_cache_clear();
    locations_nearby_cache_clear();
    enrichment_clear_cache();
    platform_log_info("Settings: cleared all caches (%d basemap + %d weather file(s) + nearby runways + enrichment)\n",
                 n, nw);
    map_view_on_show();
}

static void factory_reset_cb(lv_event_t *e) {
    (void)e;
    uint32_t now = platform_millis();
    if (!_factory_confirm_until_ms || now > _factory_confirm_until_ms) {
        _factory_confirm_until_ms = now + 4000;
        if (_factory_lbl) lv_label_set_text(_factory_lbl, "Tap again to confirm");
        return;
    }

    _factory_confirm_until_ms = 0;
    if (_factory_lbl) lv_label_set_text(_factory_lbl, "Reset to factory defaults");

    storage_factory_reset();
    locations_factory_reset();
    basemap_cache_clear();
    weather_cache_clear();
    enrichment_clear_cache();

    _cfg = storage_load_config();
    g_config = _cfg;
    storage_save_config(g_config);
    apply_cfg_to_fields();
    if (_on_change) _on_change(&g_config);

    location_picker_close();
    map_view_on_show();
    platform_log_info("Settings: ADS-B factory defaults restored (config + locations + caches)\n");
    settings_hide();
}

static lv_obj_t *make_enable_switch(lv_obj_t *parent, int x, int y) {
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_pos(sw, x, y);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x333366), 0);
    lv_obj_set_style_bg_color(sw, ACCENT_COLOR, LV_PART_INDICATOR | LV_STATE_CHECKED);
    return sw;
}

void settings_init(lv_obj_t *parent) {
    _boot_time_ms = platform_millis();

    _overlay = lv_obj_create(parent);
    lv_obj_set_size(_overlay, lv_obj_get_width(parent), lv_obj_get_height(parent));
    lv_obj_set_pos(_overlay, 0, 0);
    lv_obj_set_style_bg_color(_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_overlay, 0, 0);
    lv_obj_set_style_radius(_overlay, 0, 0);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
    // Dismiss only via Save or Cancel — tapping the dimmed backdrop does nothing.
    // (Earlier: click-outside hid without saving and made Save ambiguous.)

    _panel = lv_obj_create(_overlay);
    lv_obj_set_size(_panel, PANEL_W, PANEL_H);
    lv_obj_align(_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_panel, BG_COLOR, 0);
    lv_obj_set_style_bg_opa(_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_panel, 12, 0);
    lv_obj_set_style_border_color(_panel, lv_color_hex(0x333366), 0);
    lv_obj_set_style_border_width(_panel, 1, 0);
    lv_obj_set_style_pad_all(_panel, 16, 0);
    lv_obj_clear_flag(_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(_panel);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(title, 4, 0);

    // Non-scrolling body above a fixed action strip. Content height ends
    // strictly above the action strip (no overlap with Clear/Reset).
    const int content_h = PANEL_H - 32 /*pad*/ - TITLE_H - ACTION_H - 8 /*gap*/;
    _content = lv_obj_create(_panel);
    lv_obj_set_size(_content, PANEL_W - 32, content_h);
    lv_obj_set_pos(_content, 0, TITLE_H);
    lv_obj_set_style_bg_opa(_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_content, 0, 0);
    lv_obj_set_style_pad_all(_content, 4, 0);
    lv_obj_set_style_pad_bottom(_content, 16, 0);
    lv_obj_clear_flag(_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(_content, true, 0);

    _cfg = storage_load_config();

    const int col0 = 0;
    const int col1 = COL_W + COL_GAP;
    const int col2 = 2 * (COL_W + COL_GAP);

    // --- Column 0 (left): ranges + metric + traffic + status ---
    create_label(_content, "Range Presets (nm, 1-500)", col0, 0);
    for (int i = 0; i < 4; i++) {
        char rbuf[8];
        snprintf(rbuf, sizeof(rbuf), "%d", _cfg.radius_presets[i]);
        _ta_radius[i] = lv_textarea_create(_content);
        lv_obj_set_size(_ta_radius[i], 70, 36);
        lv_obj_set_pos(_ta_radius[i], col0 + i * 78, 22);
        lv_textarea_set_one_line(_ta_radius[i], true);
        lv_textarea_set_text(_ta_radius[i], rbuf);
        lv_obj_set_style_bg_color(_ta_radius[i], lv_color_hex(0x1a1a3a), 0);
        lv_obj_set_style_text_color(_ta_radius[i], lv_color_white(), 0);
        lv_obj_set_style_text_font(_ta_radius[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_border_color(_ta_radius[i], lv_color_hex(0x333366), 0);
        lv_obj_set_style_border_width(_ta_radius[i], 1, 0);
        lv_obj_set_style_border_color(_ta_radius[i], ACCENT_COLOR, LV_STATE_FOCUSED);
        lv_obj_add_event_cb(_ta_radius[i], ta_focus_cb, LV_EVENT_FOCUSED, nullptr);
    }

    create_label(_content, "Metric Units", col0, 72);
    _sw_metric = make_enable_switch(_content, col0 + 120, 70);
    if (_cfg.use_metric) lv_obj_add_state(_sw_metric, LV_STATE_CHECKED);

    create_label(_content, "TRAFFIC SOURCE", col0, 108);
    _dd_traffic_prov = lv_dropdown_create(_content);
    lv_dropdown_set_options(_dd_traffic_prov, TRAFFIC_PROVIDER_OPTS);
    lv_obj_set_size(_dd_traffic_prov, COL_W - 8, 36);
    lv_obj_set_pos(_dd_traffic_prov, col0, 128);
    lv_obj_set_style_bg_color(_dd_traffic_prov, lv_color_hex(0x1a1a3a), 0);
    lv_obj_set_style_text_color(_dd_traffic_prov, lv_color_white(), 0);
    lv_obj_set_style_text_font(_dd_traffic_prov, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_color(_dd_traffic_prov, lv_color_hex(0x333366), 0);
    lv_obj_set_style_border_width(_dd_traffic_prov, 1, 0);
    lv_dropdown_set_selected(_dd_traffic_prov, (uint16_t)(_cfg.traffic_provider == 1 ? 1 : 0));
    lv_obj_add_event_cb(_dd_traffic_prov, on_traffic_provider_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    int sy = 180;
    create_label(_content, "STATUS", col0, sy);
    _fetch_val = create_inline_row(_content, "FETCHES", col0, sy + 22, 90);
    _latency_val = create_inline_row(_content, "LATENCY", col0, sy + 42, 90);
    _uptime_val = create_inline_row(_content, "UPTIME", col0, sy + 62, 90);

    // --- Column 1 (middle): API keys ---
    create_label(_content, "API KEYS", col1, 0);
    lv_obj_t *hint = lv_label_create(_content);
    lv_label_set_text(hint, "Keys: tools/set_api_keys.py (or edit config.json)");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666688), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(hint, col1, 20);
    lv_obj_set_width(hint, COL_W - 8);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    create_label(_content, "AIRPORTDB.IO", col1, 48);
    _apt_key_val = create_inline_row(_content, "KEY", col1, 70, 70);
    _apt_valid_val = create_inline_row(_content, "VALID", col1, 90, 70);
    create_label(_content, "ENABLE", col1, 112);
    _sw_apt_en = make_enable_switch(_content, col1 + 80, 110);

    create_label(_content, "AERODATABOX", col1, 156);
    create_label(_content, "PROVIDER", col1, 178);
    _dd_adbox_prov = lv_dropdown_create(_content);
    lv_dropdown_set_options(_dd_adbox_prov, ADBOX_PROVIDER_OPTS);
    lv_obj_set_size(_dd_adbox_prov, COL_W - 8, 36);
    lv_obj_set_pos(_dd_adbox_prov, col1, 198);
    lv_obj_set_style_bg_color(_dd_adbox_prov, lv_color_hex(0x1a1a3a), 0);
    lv_obj_set_style_text_color(_dd_adbox_prov, lv_color_white(), 0);
    lv_obj_set_style_text_font(_dd_adbox_prov, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_color(_dd_adbox_prov, lv_color_hex(0x333366), 0);
    lv_obj_set_style_border_width(_dd_adbox_prov, 1, 0);
    lv_dropdown_set_selected(_dd_adbox_prov, (uint16_t)(_cfg.aerodatabox_provider >= 0 && _cfg.aerodatabox_provider <= 2
                                                         ? _cfg.aerodatabox_provider : 0));
    lv_obj_add_event_cb(_dd_adbox_prov, on_adbox_provider_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    _adbox_key_val = create_inline_row(_content, "KEY", col1, 246, 70);
    _adbox_valid_val = create_inline_row(_content, "VALID", col1, 266, 70);
    _adbox_usage_val = create_inline_row(_content, "USAGE", col1, 286, 70);
    create_label(_content, "ENABLE", col1, 308);
    _sw_adbox_en = make_enable_switch(_content, col1 + 80, 306);

    lv_obj_t *quota_note = lv_label_create(_content);
    // Show marketplace used/limit when headers exist; auto-off at 0
    // remaining (see adbox_note_rate_limit). Local soft-cap / 429 still apply.
    // Renewal day is user-set (not from API headers).
    lv_label_set_text(quota_note,
        "USAGE: used of limit · resets ~Nth of every month "
        "(set_api_keys --adbox-renew-day N). "
        "Auto-off at 0 remaining / 429 / soft-cap. "
        "Key check cached across restarts.");
    lv_obj_set_style_text_color(quota_note, lv_color_hex(0x666688), 0);
    lv_obj_set_style_text_font(quota_note, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(quota_note, col1, 340);
    lv_obj_set_width(quota_note, COL_W - 8);
    lv_obj_clear_flag(quota_note, LV_OBJ_FLAG_CLICKABLE);

    create_label(_content, "CARTO BASEMAP", col1, 430);
    _carto_key_val = create_inline_row(_content, "KEY", col1, 452, 70);
    lv_obj_t *carto_hint = lv_label_create(_content);
    lv_label_set_text(carto_hint, "Free key: carto.com/basemaps/apikey\n"
                                  "(needed for dark/voyager styles)");
    lv_obj_set_style_text_color(carto_hint, lv_color_hex(0x666688), 0);
    lv_obj_set_style_text_font(carto_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(carto_hint, col1, 474);
    lv_obj_set_width(carto_hint, COL_W - 8);
    lv_obj_clear_flag(carto_hint, LV_OBJ_FLAG_CLICKABLE);

    // --- Column 2 (right): DEVICE + ERRORS ---
    create_label(_content, "DEVICE", col2, 0);
    _ota_ver_val = create_inline_row(_content, "VERSION", col2, 22, 90);
    lv_label_set_text(_ota_ver_val, FIRMWARE_VERSION_STR);
    _ota_status_lbl = lv_label_create(_content);
    lv_label_set_text(_ota_status_lbl, "Tap to check");
    lv_obj_set_style_text_font(_ota_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_ota_status_lbl, LABEL_COLOR, 0);
    lv_obj_set_pos(_ota_status_lbl, col2, 44);
    lv_obj_set_width(_ota_status_lbl, COL_W - 8);
    lv_obj_clear_flag(_ota_status_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ota_btn = lv_button_create(_content);
    lv_obj_set_size(ota_btn, COL_W - 8, 34);
    lv_obj_set_pos(ota_btn, col2, 68);
    lv_obj_set_style_bg_color(ota_btn, lv_color_hex(0x1a1a2a), 0);
    lv_obj_set_style_border_color(ota_btn, lv_color_hex(0x444466), 0);
    lv_obj_set_style_border_width(ota_btn, 1, 0);
    lv_obj_set_style_radius(ota_btn, 6, 0);
    _ota_btn_lbl = lv_label_create(ota_btn);
    lv_label_set_text(_ota_btn_lbl, "Check for update");
    lv_obj_set_style_text_color(_ota_btn_lbl, ACCENT_COLOR, 0);
    lv_obj_set_style_text_font(_ota_btn_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(_ota_btn_lbl);
    lv_obj_add_event_cb(ota_btn, ota_btn_cb, LV_EVENT_CLICKED, nullptr);
    refresh_ota_ui();

    // Brightness — live-applies via sysfs; persisted on Save (and slider release).
    create_label(_content, "Brightness", col2, 112);
    _bright_label = lv_label_create(_content);
    lv_label_set_text_fmt(_bright_label, "%d%%", _cfg.display_brightness_pct);
    lv_obj_set_style_text_font(_bright_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_bright_label, SYS_COLOR, 0);
    lv_obj_set_pos(_bright_label, col2 + COL_W - 56, 112);
    lv_obj_clear_flag(_bright_label, LV_OBJ_FLAG_CLICKABLE);

    _bright_slider = lv_slider_create(_content);
    lv_obj_set_size(_bright_slider, COL_W - 8, 12);
    lv_obj_set_pos(_bright_slider, col2, 136);
    lv_slider_set_range(_bright_slider, 10, 100);
    {
        int b = _cfg.display_brightness_pct;
        if (b < 10) b = 10;
        if (b > 100) b = 100;
        lv_slider_set_value(_bright_slider, b, LV_ANIM_OFF);
    }
    lv_obj_set_style_bg_color(_bright_slider, lv_color_hex(0x333366), 0);
    lv_obj_set_style_bg_color(_bright_slider, ACCENT_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_bright_slider, ACCENT_COLOR, LV_PART_KNOB);
    lv_obj_add_event_cb(_bright_slider, [](lv_event_t *e) {
        int v = (int)lv_slider_get_value(lv_event_get_target_obj(e));
        if (v < 10) v = 10;
        if (v > 100) v = 100;
        _cfg.display_brightness_pct = v;
        if (_bright_label) lv_label_set_text_fmt(_bright_label, "%d%%", v);
        // Preview only — persist on Save; Cancel restores _cfg_at_open.
        backlight_set_percent(v);
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    const int ey = 168;
    create_label(_content, "ERRORS", col2, ey);
    _err_count_lbl = lv_label_create(_content);
    lv_label_set_text(_err_count_lbl, "(0)");
    lv_obj_set_style_text_font(_err_count_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_err_count_lbl, LABEL_COLOR, 0);
    lv_obj_set_pos(_err_count_lbl, col2 + 70, ey);
    lv_obj_clear_flag(_err_count_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *clr_btn = lv_obj_create(_content);
    lv_obj_set_size(clr_btn, 40, 22);
    lv_obj_set_pos(clr_btn, col2 + 120, ey - 2);
    lv_obj_set_style_bg_color(clr_btn, lv_color_hex(0x1a1a2a), 0);
    lv_obj_set_style_bg_opa(clr_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(clr_btn, lv_color_hex(0x444466), 0);
    lv_obj_set_style_border_width(clr_btn, 1, 0);
    lv_obj_set_style_radius(clr_btn, 4, 0);
    lv_obj_set_style_pad_all(clr_btn, 0, 0);
    lv_obj_clear_flag(clr_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(clr_btn, [](lv_event_t *ev) { (void)ev; error_log_clear(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *clr_lbl = lv_label_create(clr_btn);
    lv_label_set_text(clr_lbl, "CLR");
    lv_obj_set_style_text_font(clr_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(clr_lbl, ERR_COLOR, 0);
    lv_obj_center(clr_lbl);

    _err_list_lbl = lv_label_create(_content);
    lv_label_set_text(_err_list_lbl, "(none)");
    lv_obj_set_style_text_font(_err_list_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_err_list_lbl, ERR_COLOR, 0);
    lv_obj_set_pos(_err_list_lbl, col2, ey + 24);
    lv_obj_set_width(_err_list_lbl, COL_W - 8);
    lv_obj_clear_flag(_err_list_lbl, LV_OBJ_FLAG_CLICKABLE);

    status_refresh(nullptr);
    lv_timer_create(status_refresh, 500, nullptr);

    // Fixed action strip: Clear/Reset span col0+col1; Cancel + Save on the right.
    // Opaque band + clipped content prevent bleed-through.
    lv_obj_t *actions = lv_obj_create(_panel);
    lv_obj_set_size(actions, PANEL_W - 32, ACTION_H);
    lv_obj_align(actions, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(actions, BG_COLOR, 0);
    lv_obj_set_style_bg_opa(actions, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_set_style_pad_top(actions, 8, 0);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(actions);

    // Span first two columns for Clear/Reset; leave a gap before Cancel/Save.
    const int left_btn_w = 2 * COL_W + COL_GAP;
    const int actions_inner_w = PANEL_W - 32;
    const int exit_btn_w = 110;
    const int exit_gap = 10;

    lv_obj_t *cache_btn = lv_button_create(actions);
    lv_obj_set_size(cache_btn, left_btn_w, 34);
    lv_obj_set_pos(cache_btn, 0, 4);
    lv_obj_set_style_bg_color(cache_btn, lv_color_hex(0x1a1a2a), 0);
    lv_obj_set_style_border_color(cache_btn, lv_color_hex(0x444466), 0);
    lv_obj_set_style_border_width(cache_btn, 1, 0);
    lv_obj_set_style_radius(cache_btn, 6, 0);
    lv_obj_t *cache_lbl = lv_label_create(cache_btn);
    lv_label_set_text(cache_lbl, "Clear all caches");
    lv_obj_set_style_text_color(cache_lbl, lv_color_hex(0xffaa66), 0);
    lv_obj_set_style_text_font(cache_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(cache_lbl);
    lv_obj_add_event_cb(cache_btn, clear_all_caches_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *cancel_btn = lv_button_create(actions);
    lv_obj_set_size(cancel_btn, exit_btn_w, 34);
    lv_obj_set_pos(cancel_btn, actions_inner_w - 2 * exit_btn_w - exit_gap, 4);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x1a1a2a), 0);
    lv_obj_set_style_border_color(cancel_btn, lv_color_hex(0x666688), 0);
    lv_obj_set_style_border_width(cancel_btn, 1, 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, LABEL_COLOR, 0);
    lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_16, 0);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel_btn, cancel_and_close, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *save_btn = lv_button_create(actions);
    lv_obj_set_size(save_btn, exit_btn_w, 34);
    lv_obj_set_pos(save_btn, actions_inner_w - exit_btn_w, 4);
    lv_obj_set_style_bg_color(save_btn, ACCENT_COLOR, 0);
    lv_obj_set_style_radius(save_btn, 8, 0);
    lv_obj_t *save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save");
    lv_obj_set_style_text_color(save_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(save_label, &lv_font_montserrat_16, 0);
    lv_obj_center(save_label);
    lv_obj_add_event_cb(save_btn, save_and_close, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *factory_btn = lv_button_create(actions);
    lv_obj_set_size(factory_btn, left_btn_w, 34);
    lv_obj_set_pos(factory_btn, 0, 46);
    lv_obj_set_style_bg_color(factory_btn, lv_color_hex(0x2a1a1a), 0);
    lv_obj_set_style_border_color(factory_btn, lv_color_hex(0x664444), 0);
    lv_obj_set_style_border_width(factory_btn, 1, 0);
    lv_obj_set_style_radius(factory_btn, 6, 0);
    _factory_lbl = lv_label_create(factory_btn);
    lv_label_set_text(_factory_lbl, "Reset to factory defaults");
    lv_obj_set_style_text_color(_factory_lbl, ERR_COLOR, 0);
    lv_obj_set_style_text_font(_factory_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(_factory_lbl);
    lv_obj_add_event_cb(factory_btn, factory_reset_cb, LV_EVENT_CLICKED, nullptr);

    _keyboard = lv_keyboard_create(_overlay);
    lv_obj_set_size(_keyboard, lv_obj_get_width(parent), 200);
    lv_obj_align(_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(_keyboard, keyboard_ready_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(_keyboard, keyboard_ready_cb, LV_EVENT_CANCEL, nullptr);
}

void settings_show() {
    if (_visible) return;
    _visible = true;
    _shown_at_ms = platform_millis();
    _factory_confirm_until_ms = 0;
    if (_factory_lbl) lv_label_set_text(_factory_lbl, "Reset to factory defaults");
    _cfg = storage_load_config();
    _cfg_at_open = _cfg;
    apply_cfg_to_fields();
    start_key_validation();
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
}

void settings_hide() {
    if (!_visible) return;
    _visible = false;
    _apt_verify_pending = false;
    _adbox_verify_pending = false;
    _factory_confirm_until_ms = 0;
    if (_factory_lbl) lv_label_set_text(_factory_lbl, "Reset to factory defaults");
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
}

bool settings_is_visible() { return _visible; }
void settings_set_change_callback(settings_changed_cb_t cb) { _on_change = cb; }
