// Scoped-down Pi implementation of src/ui/settings.h -- deliberately NOT
// a port of src/ui/settings.cpp. Pi Settings is a three-tab panel:
// Display (range presets, metric, brightness), Services (traffic source +
// keyed API status/enable), System (OTA, host info, diagnostics, cache /
// factory reset). Keys are hand-edited in config.json / set_api_keys.py;
// the UI only shows presence / validity / enable. WiFi/Ethernet and ESP32
// heap/PSRAM UI stay dropped.

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
#include <sys/utsname.h>
#include <unistd.h>

static lv_obj_t *_overlay = nullptr;
static lv_obj_t *_panel = nullptr;
static lv_obj_t *_tabview = nullptr; // Display | Services | System
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
static lv_obj_t *_sys_uptime_val = nullptr; // host /proc/uptime (System tab)
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

// Fits under status bar on 1280x800. Narrow — each tab is a single column.
#define PANEL_W 560
#define PANEL_H 660
#define TITLE_H 36
#define TAB_BAR_H 40
#define ACTION_H 52
#define LABEL_COLOR lv_color_hex(0x8888aa)
#define BG_COLOR lv_color_hex(0x12122a)
#define ACCENT_COLOR lv_color_hex(0x00cc66)
#define SYS_COLOR lv_color_hex(0x44cc88)
#define WARN_COLOR lv_color_hex(0xffaa44)
#define ERR_COLOR lv_color_hex(0xff6666)
#define ROW_BG lv_color_hex(0x1a1a3a)
#define BORDER_COLOR lv_color_hex(0x333366)
#define TAB_IDLE_BG lv_color_hex(0x1a1a3a)
#define TAB_IDLE_FG lv_color_hex(0x8888aa)
#define TAB_ACTIVE_BG lv_color_hex(0x1e2a24)

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
        // " | resets ~31st of every month" ≈ 34 chars
        // Prefer ASCII "|" — montserrat lacks U+00B7 (·) and shows tofu.
        if (used + 40 >= dst_sz) return;
        // e.g. " | resets ~9th of every month"
        snprintf(dst + used, dst_sz - used, " | resets ~%d%s of every month",
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

    // While Checking, keep the draft Enable state and only lock the switch.
    // Previously we cleared CHECKED + _cfg.*_enabled on every non-Valid poll,
    // which left g_config still enabled — UI showed Off while APIs kept running.
    auto sync_enable_sw = [](lv_obj_t *sw, KeyValid v, bool *draft_en, bool *live_en,
                             bool clear_enrich_on_force_off) {
        if (!sw || !draft_en || !live_en) return;
        if (v == KeyValid::Valid) {
            lv_obj_clear_state(sw, LV_STATE_DISABLED);
        } else if (v == KeyValid::Checking || v == KeyValid::Unknown) {
            lv_obj_add_state(sw, LV_STATE_DISABLED);
        } else {
            // Missing / Invalid — force off in UI, draft, and live config.
            lv_obj_add_state(sw, LV_STATE_DISABLED);
            lv_obj_clear_state(sw, LV_STATE_CHECKED);
            const bool was_live = *live_en;
            *draft_en = false;
            *live_en = false;
            if (clear_enrich_on_force_off && was_live) enrichment_clear_cache();
            return;
        }
        if (*draft_en) lv_obj_add_state(sw, LV_STATE_CHECKED);
        else lv_obj_clear_state(sw, LV_STATE_CHECKED);
    };
    sync_enable_sw(_sw_apt_en, _apt_valid, &_cfg.airportdb_enabled, &g_config.airportdb_enabled, false);
    sync_enable_sw(_sw_adbox_en, _adbox_valid, &_cfg.aerodatabox_enabled, &g_config.aerodatabox_enabled, true);
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

// Enable toggles preview live (like traffic); Save persists, Cancel reverts.
static void on_service_enable_changed(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target_obj(e);
    if (!sw) return;
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if (sw == _sw_apt_en) {
        if (on && _apt_valid != KeyValid::Valid) {
            lv_obj_clear_state(sw, LV_STATE_CHECKED);
            return;
        }
        _cfg.airportdb_enabled = on;
        g_config.airportdb_enabled = on;
        return;
    }
    if (sw == _sw_adbox_en) {
        if (on && _adbox_valid != KeyValid::Valid) {
            lv_obj_clear_state(sw, LV_STATE_CHECKED);
            return;
        }
        const bool was = g_config.aerodatabox_enabled;
        _cfg.aerodatabox_enabled = on;
        g_config.aerodatabox_enabled = on;
        if (on && g_config.adbox_rate_limited) {
            aerodatabox_clear_rate_limit();
            _cfg.adbox_rate_limited = false;
            g_config.adbox_rate_limited = false;
        }
        if (was != on) enrichment_clear_cache();
    }
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
static uint32_t system_uptime_s();
static void fmt_hms(char *out, size_t out_sz, uint32_t sec);

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
    char app_up[16];
    fmt_hms(app_up, sizeof(app_up), uptime_s);
    lv_label_set_text(_uptime_val, app_up);

    if (_sys_uptime_val) {
        char sys_up[16];
        fmt_hms(sys_up, sizeof(sys_up), system_uptime_s());
        lv_label_set_text(_sys_uptime_val, sys_up);
    }

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
        if (_factory_lbl) lv_label_set_text(_factory_lbl, "Reset to defaults");
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
    // traffic/ADB provider, service Enable). Disk is untouched for draft
    // fields; brightness may have been preview-only (no mid-edit writes).
    const int live_traffic = g_config.traffic_provider;
    const bool live_adbox = g_config.aerodatabox_enabled;
    g_config.display_brightness_pct = _cfg_at_open.display_brightness_pct;
    g_config.traffic_provider = _cfg_at_open.traffic_provider;
    g_config.aerodatabox_provider = _cfg_at_open.aerodatabox_provider;
    g_config.airportdb_enabled = _cfg_at_open.airportdb_enabled;
    g_config.aerodatabox_enabled = _cfg_at_open.aerodatabox_enabled;
    g_config.adbox_rate_limited = _cfg_at_open.adbox_rate_limited;
    backlight_set_percent(g_config.display_brightness_pct);
    if (live_traffic != g_config.traffic_provider)
        fetcher_request_immediate_fetch();
    if (live_adbox != g_config.aerodatabox_enabled)
        enrichment_clear_cache();
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
    if (_factory_lbl) lv_label_set_text(_factory_lbl, "Reset to defaults");

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

static void style_dropdown(lv_obj_t *dd, int w) {
    lv_obj_set_size(dd, w, 36);
    lv_obj_set_style_bg_color(dd, ROW_BG, 0);
    lv_obj_set_style_text_color(dd, lv_color_white(), 0);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_color(dd, BORDER_COLOR, 0);
    lv_obj_set_style_border_width(dd, 1, 0);
}

static void style_textarea(lv_obj_t *ta) {
    lv_obj_set_style_bg_color(ta, ROW_BG, 0);
    lv_obj_set_style_text_color(ta, lv_color_white(), 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_color(ta, BORDER_COLOR, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, ACCENT_COLOR, LV_STATE_FOCUSED);
}

static void help_btn_cb(lv_event_t *e) {
    const char *const *pack = (const char *const *)lv_event_get_user_data(e);
    if (!pack || !pack[0] || !pack[1] || !_overlay) return;

    lv_obj_t *mbox = lv_msgbox_create(_overlay);
    lv_obj_set_width(mbox, 520);
    lv_obj_set_style_bg_color(mbox, BG_COLOR, 0);
    lv_obj_set_style_border_color(mbox, BORDER_COLOR, 0);
    lv_obj_set_style_border_width(mbox, 1, 0);
    lv_obj_set_style_text_color(mbox, lv_color_white(), 0);
    lv_obj_set_style_text_font(mbox, &lv_font_montserrat_14, 0);
    lv_msgbox_add_title(mbox, pack[0]);
    lv_obj_t *txt = lv_msgbox_add_text(mbox, pack[1]);
    if (txt) {
        lv_obj_set_style_text_color(txt, LABEL_COLOR, 0);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(txt, 480);
    }
    lv_msgbox_add_close_button(mbox);
    lv_obj_center(mbox);
}

static lv_obj_t *make_help_btn(lv_obj_t *parent, int x, int y,
                               const char *const *pack) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 28, 28);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, ROW_BG, 0);
    lv_obj_set_style_border_color(btn, BORDER_COLOR, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "?");
    lv_obj_set_style_text_color(lbl, ACCENT_COLOR, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, help_btn_cb, LV_EVENT_CLICKED, (void *)pack);
    return btn;
}

static const char *const HELP_AIRPORTDB[2] = {
    "AirportDB.io",
    "Optional runway and airport details when adding locations. "
    "Turn Enable on only when VALID shows yes."
};

static const char *const HELP_ADBOX[2] = {
    "AeroDataBox",
    "Provides origin/destination in the detail pop-up. "
    "600 calls/month (each lookup uses 2). "
    "Billing month starts on signup; set renew day with "
    "set_api_keys.py --adbox-renew-day N."
};

static const char *const HELP_CARTO[2] = {
    "CARTO basemap",
    "Needed for dark / voyager basemap styles. "
    "Free key at carto.com/basemaps/apikey."
};

static const char *const HELP_TRAFFIC[2] = {
    "Traffic source",
    "Selects the ADS-B feed provider for live traffic."
};

static void style_tab_button(lv_obj_t *btn, bool active) {
    if (!btn) return;
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, active ? TAB_ACTIVE_BG : TAB_IDLE_BG, 0);
    lv_obj_set_style_text_color(btn, active ? ACCENT_COLOR : TAB_IDLE_FG, 0);
    lv_obj_set_style_text_font(btn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, active ? 2 : 0, 0);
    lv_obj_set_style_border_color(btn, ACCENT_COLOR, 0);
    lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_outline_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 4, 0);
    lv_obj_t *lab = lv_obj_get_child(btn, 0);
    if (lab) {
        lv_obj_set_style_text_color(lab, active ? ACCENT_COLOR : TAB_IDLE_FG, 0);
        lv_obj_set_style_text_font(lab, &lv_font_montserrat_14, 0);
    }
}

static void show_only_settings_tab(lv_obj_t *tv, uint32_t idx) {
    if (!tv) return;
    lv_obj_t *cont = lv_tabview_get_content(tv);
    uint32_t n = lv_obj_get_child_count(cont);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *page = lv_obj_get_child(cont, i);
        if (!page) continue;
        // Stack pages at the same origin; hide inactive ones so nothing peeks
        // from a neighboring horizontally-scrolled page.
        lv_obj_set_pos(page, 0, 0);
        lv_obj_set_size(page, lv_pct(100), lv_pct(100));
        if (i == idx) lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_scroll_to_x(cont, 0, LV_ANIM_OFF);
    lv_obj_scroll_to_y(cont, 0, LV_ANIM_OFF);
}

static void refresh_tab_button_styles(lv_obj_t *tv) {
    if (!tv) return;
    uint32_t active = lv_tabview_get_tab_active(tv);
    uint32_t n = lv_tabview_get_tab_count(tv);
    for (uint32_t i = 0; i < n; i++) {
        style_tab_button(lv_tabview_get_tab_button(tv, (int32_t)i), i == active);
    }
    show_only_settings_tab(tv, active);
}

static void on_tabview_changed(lv_event_t *e) {
    lv_obj_t *tv = (lv_obj_t *)lv_event_get_user_data(e);
    if (!tv) tv = lv_event_get_target_obj(e);
    // Button click already updates tabview active index via LVGL; sync our
    // stacked visibility + button chrome after it settles.
    refresh_tab_button_styles(tv);
}

static void style_tabview(lv_obj_t *tv) {
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tv, 0, 0);
    lv_obj_set_style_pad_all(tv, 0, 0);

    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0e0e1c), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 4, 0);
    lv_obj_set_style_pad_gap(bar, 6, 0);

    uint32_t n = lv_tabview_get_tab_count(tv);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *btn = lv_tabview_get_tab_button(tv, (int32_t)i);
        if (!btn) continue;
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(btn, TAB_IDLE_BG, 0);
        lv_obj_set_style_bg_color(btn, TAB_ACTIVE_BG, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(btn, TAB_IDLE_FG, 0);
        lv_obj_set_style_text_color(btn, ACCENT_COLOR, LV_STATE_CHECKED);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 2, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(btn, ACCENT_COLOR, LV_STATE_CHECKED);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, LV_STATE_CHECKED);
        lv_obj_set_style_outline_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, on_tabview_changed, LV_EVENT_CLICKED, tv);
    }

    lv_obj_t *cont = lv_tabview_get_content(tv);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_column(cont, 0, 0);
    lv_obj_set_style_pad_row(cont, 0, 0);
    lv_obj_set_style_bg_color(cont, BG_COLOR, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_clip_corner(cont, true, 0);
    // Disable sideways paging entirely — we show one stacked page at a time.
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(cont, LV_DIR_NONE);
    lv_obj_set_scroll_snap_x(cont, LV_SCROLL_SNAP_NONE);

    refresh_tab_button_styles(tv);
}

// Read a short one-line file into out (trimmed). Returns false if empty/missing.
static bool read_oneline(const char *path, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return false;
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return false;
    if (!fgets(out, (int)out_sz, f)) {
        fclose(f);
        out[0] = '\0';
        return false;
    }
    fclose(f);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == '\0')) {
        out[--n] = '\0';
    }
    // Device-tree model strings are often NUL-padded; already stopped at first NUL via fgets? 
    // Actually fgets stops at NUL in the buffer when reading binary-ish — OK for model.
    return out[0] != '\0';
}

static void fill_sysinfo(char *hw, size_t hw_sz, char *os, size_t os_sz,
                         char *host, size_t host_sz, char *arch, size_t arch_sz) {
    hw[0] = os[0] = host[0] = arch[0] = '\0';

    // Prefer Pi model; else machine / virtualized host description.
    if (!read_oneline("/proc/device-tree/model", hw, hw_sz) &&
        !read_oneline("/sys/firmware/devicetree/base/model", hw, hw_sz)) {
        struct utsname u {};
        if (uname(&u) == 0) {
            snprintf(hw, hw_sz, "%s (%s)", u.sysname, u.machine);
        } else {
            snprintf(hw, hw_sz, "unknown");
        }
    }

    if (!read_oneline("/etc/os-release", os, os_sz)) {
        snprintf(os, os_sz, "Linux");
    } else {
        // Prefer PRETTY_NAME="..."
        char line[192];
        FILE *f = fopen("/etc/os-release", "r");
        os[0] = '\0';
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "PRETTY_NAME=", 12) == 0) {
                    char *v = line + 12;
                    if (*v == '"') v++;
                    size_t len = strlen(v);
                    while (len > 0 && (v[len - 1] == '\n' || v[len - 1] == '"' || v[len - 1] == '\r'))
                        v[--len] = '\0';
                    snprintf(os, os_sz, "%s", v);
                    break;
                }
            }
            fclose(f);
        }
        if (!os[0]) snprintf(os, os_sz, "Linux");
    }

    if (gethostname(host, host_sz) != 0 || !host[0])
        snprintf(host, host_sz, "-");
    host[host_sz - 1] = '\0';

    struct utsname u {};
    if (uname(&u) == 0) snprintf(arch, arch_sz, "%s", u.machine);
    else snprintf(arch, arch_sz, "-");
}

static uint32_t system_uptime_s() {
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return 0;
    double up = 0;
    if (fscanf(f, "%lf", &up) != 1) up = 0;
    fclose(f);
    if (up < 0) up = 0;
    return (uint32_t)up;
}

static void fmt_hms(char *out, size_t out_sz, uint32_t sec) {
    snprintf(out, out_sz, "%02u:%02u:%02u",
             (unsigned)(sec / 3600u),
             (unsigned)((sec % 3600u) / 60u),
             (unsigned)(sec % 60u));
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

    _panel = lv_obj_create(_overlay);
    lv_obj_set_size(_panel, PANEL_W, PANEL_H);
    lv_obj_align(_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_panel, BG_COLOR, 0);
    lv_obj_set_style_bg_opa(_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_panel, 12, 0);
    lv_obj_set_style_border_color(_panel, BORDER_COLOR, 0);
    lv_obj_set_style_border_width(_panel, 1, 0);
    lv_obj_set_style_pad_all(_panel, 16, 0);
    lv_obj_clear_flag(_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(_panel);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(title, 4, 0);

    const int content_h = PANEL_H - 32 /*pad*/ - TITLE_H - ACTION_H - 8 /*gap*/;
    const int body_w = PANEL_W - 32;

    _tabview = lv_tabview_create(_panel);
    lv_obj_set_size(_tabview, body_w, content_h);
    lv_obj_set_pos(_tabview, 0, TITLE_H);
    lv_tabview_set_tab_bar_position(_tabview, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(_tabview, TAB_BAR_H);

    lv_obj_t *tab_display = lv_tabview_add_tab(_tabview, "Display");
    lv_obj_t *tab_services = lv_tabview_add_tab(_tabview, "Services");
    lv_obj_t *tab_system = lv_tabview_add_tab(_tabview, "System");
    style_tabview(_tabview);

    auto prep_tab = [](lv_obj_t *tab, bool scrollable) {
        lv_obj_set_style_bg_color(tab, BG_COLOR, 0);
        lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(tab, 8, 0);
        lv_obj_set_style_pad_bottom(tab, 16, 0);
        lv_obj_set_style_border_width(tab, 0, 0);
        // Display fits; Services/System can overrun the page — allow vertical
        // scroll so CARTO / factory-reset aren't clipped by the footer.
        if (scrollable) lv_obj_add_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
        else lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(tab, LV_DIR_VER);
    };
    prep_tab(tab_display, false);
    prep_tab(tab_services, true);
    prep_tab(tab_system, true);

    _cfg = storage_load_config();
    const int field_w = 380;

    // --- Display: range presets, metric, brightness ---
    create_label(tab_display, "Range Presets (nm, 1-500)", 0, 4);
    for (int i = 0; i < 4; i++) {
        char rbuf[8];
        snprintf(rbuf, sizeof(rbuf), "%d", _cfg.radius_presets[i]);
        _ta_radius[i] = lv_textarea_create(tab_display);
        lv_obj_set_size(_ta_radius[i], 80, 36);
        lv_obj_set_pos(_ta_radius[i], i * 90, 28);
        lv_textarea_set_one_line(_ta_radius[i], true);
        lv_textarea_set_text(_ta_radius[i], rbuf);
        style_textarea(_ta_radius[i]);
        lv_obj_add_event_cb(_ta_radius[i], ta_focus_cb, LV_EVENT_FOCUSED, nullptr);
    }

    create_label(tab_display, "Metric Units", 0, 84);
    _sw_metric = make_enable_switch(tab_display, 130, 82);
    if (_cfg.use_metric) lv_obj_add_state(_sw_metric, LV_STATE_CHECKED);

    create_label(tab_display, "Brightness", 0, 130);
    _bright_label = lv_label_create(tab_display);
    lv_label_set_text_fmt(_bright_label, "%d%%", _cfg.display_brightness_pct);
    lv_obj_set_style_text_font(_bright_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_bright_label, SYS_COLOR, 0);
    lv_obj_set_pos(_bright_label, field_w - 48, 130);
    lv_obj_clear_flag(_bright_label, LV_OBJ_FLAG_CLICKABLE);

    _bright_slider = lv_slider_create(tab_display);
    lv_obj_set_size(_bright_slider, field_w, 12);
    lv_obj_set_pos(_bright_slider, 0, 158);
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
        backlight_set_percent(v);
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    // --- Services: traffic + keyed APIs (roomier vertical rhythm) ---
    lv_obj_t *keys_hint = lv_label_create(tab_services);
    lv_label_set_text(keys_hint, "API keys are set with set_api_keys.py");
    lv_obj_set_style_text_color(keys_hint, lv_color_hex(0x666688), 0);
    lv_obj_set_style_text_font(keys_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(keys_hint, 0, 0);
    lv_obj_set_width(keys_hint, field_w + 80);
    lv_obj_clear_flag(keys_hint, LV_OBJ_FLAG_CLICKABLE);

    create_label(tab_services, "TRAFFIC SOURCE", 0, 36);
    make_help_btn(tab_services, 150, 32, HELP_TRAFFIC);
    _dd_traffic_prov = lv_dropdown_create(tab_services);
    lv_dropdown_set_options(_dd_traffic_prov, TRAFFIC_PROVIDER_OPTS);
    style_dropdown(_dd_traffic_prov, field_w);
    lv_obj_set_pos(_dd_traffic_prov, 0, 64);
    lv_dropdown_set_selected(_dd_traffic_prov, (uint16_t)(_cfg.traffic_provider == 1 ? 1 : 0));
    lv_obj_add_event_cb(_dd_traffic_prov, on_traffic_provider_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    create_label(tab_services, "AIRPORTDB.IO", 0, 124);
    make_help_btn(tab_services, 130, 120, HELP_AIRPORTDB);
    _apt_key_val = create_inline_row(tab_services, "KEY", 0, 156, 60);
    _apt_valid_val = create_inline_row(tab_services, "VALID", 180, 156, 60);
    create_label(tab_services, "ENABLE", 360, 156);
    _sw_apt_en = make_enable_switch(tab_services, 430, 152);
    lv_obj_add_event_cb(_sw_apt_en, on_service_enable_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    create_label(tab_services, "AERODATABOX", 0, 208);
    make_help_btn(tab_services, 140, 204, HELP_ADBOX);
    create_label(tab_services, "PROVIDER", 0, 240);
    _dd_adbox_prov = lv_dropdown_create(tab_services);
    lv_dropdown_set_options(_dd_adbox_prov, ADBOX_PROVIDER_OPTS);
    style_dropdown(_dd_adbox_prov, field_w);
    lv_obj_set_pos(_dd_adbox_prov, 0, 264);
    lv_dropdown_set_selected(_dd_adbox_prov, (uint16_t)(_cfg.aerodatabox_provider >= 0 && _cfg.aerodatabox_provider <= 2
                                                         ? _cfg.aerodatabox_provider : 0));
    lv_obj_add_event_cb(_dd_adbox_prov, on_adbox_provider_changed, LV_EVENT_VALUE_CHANGED, nullptr);

    _adbox_key_val = create_inline_row(tab_services, "KEY", 0, 320, 60);
    _adbox_valid_val = create_inline_row(tab_services, "VALID", 180, 320, 60);
    create_label(tab_services, "ENABLE", 360, 320);
    _sw_adbox_en = make_enable_switch(tab_services, 430, 316);
    lv_obj_add_event_cb(_sw_adbox_en, on_service_enable_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    _adbox_usage_val = create_inline_row(tab_services, "USAGE", 0, 356, 60);
    lv_obj_set_width(_adbox_usage_val, field_w + 80);

    create_label(tab_services, "CARTO BASEMAP", 0, 412);
    make_help_btn(tab_services, 160, 408, HELP_CARTO);
    _carto_key_val = create_inline_row(tab_services, "KEY", 0, 444, 60);

    // --- System: version / OTA, host info, diagnostics, destructive actions ---
    // Compact vertical rhythm so Clear/Factory fit above the footer on 620px.
    create_label(tab_system, "UPDATE", 0, 0);
    _ota_ver_val = create_inline_row(tab_system, "VERSION", 0, 20, 90);
    lv_label_set_text(_ota_ver_val, FIRMWARE_VERSION_STR);
    _ota_status_lbl = lv_label_create(tab_system);
    lv_label_set_text(_ota_status_lbl, "Tap to check");
    lv_obj_set_style_text_font(_ota_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_ota_status_lbl, LABEL_COLOR, 0);
    lv_obj_set_pos(_ota_status_lbl, 200, 20);
    lv_obj_set_width(_ota_status_lbl, field_w - 200);
    lv_obj_clear_flag(_ota_status_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ota_btn = lv_button_create(tab_system);
    lv_obj_set_size(ota_btn, field_w, 30);
    lv_obj_set_pos(ota_btn, 0, 42);
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

    char hw[96], osname[96], host[64], arch[32];
    fill_sysinfo(hw, sizeof(hw), osname, sizeof(osname), host, sizeof(host), arch, sizeof(arch));
    create_label(tab_system, "HOST", 0, 82);
    lv_obj_t *hw_val = create_inline_row(tab_system, "HARDWARE", 0, 100, 100);
    lv_label_set_text(hw_val, hw);
    lv_obj_set_width(hw_val, field_w - 8);
    lv_obj_t *os_val = create_inline_row(tab_system, "OS", 0, 118, 100);
    lv_label_set_text(os_val, osname);
    lv_obj_set_width(os_val, field_w - 8);
    lv_obj_t *host_val = create_inline_row(tab_system, "HOSTNAME", 0, 136, 100);
    lv_label_set_text(host_val, host);
    lv_obj_t *arch_val = create_inline_row(tab_system, "ARCH", 0, 154, 100);
    lv_label_set_text(arch_val, arch);
    _sys_uptime_val = create_inline_row(tab_system, "SYS UPTIME", 0, 172, 100);

    create_label(tab_system, "DIAGNOSTICS", 0, 200);
    _fetch_val = create_inline_row(tab_system, "FETCHES", 0, 218, 100);
    _latency_val = create_inline_row(tab_system, "LATENCY", 0, 236, 100);
    _uptime_val = create_inline_row(tab_system, "APP UPTIME", 0, 254, 100);

    create_label(tab_system, "ERRORS", 0, 278);
    _err_count_lbl = lv_label_create(tab_system);
    lv_label_set_text(_err_count_lbl, "(0)");
    lv_obj_set_style_text_font(_err_count_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_err_count_lbl, LABEL_COLOR, 0);
    lv_obj_set_pos(_err_count_lbl, 80, 278);
    lv_obj_clear_flag(_err_count_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *clr_btn = lv_obj_create(tab_system);
    lv_obj_set_size(clr_btn, 40, 22);
    lv_obj_set_pos(clr_btn, 130, 276);
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

    _err_list_lbl = lv_label_create(tab_system);
    lv_label_set_text(_err_list_lbl, "(none)");
    lv_obj_set_style_text_font(_err_list_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_err_list_lbl, ERR_COLOR, 0);
    lv_obj_set_pos(_err_list_lbl, 180, 278);
    lv_obj_set_width(_err_list_lbl, field_w - 180);
    lv_obj_clear_flag(_err_list_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *cache_btn = lv_button_create(tab_system);
    const int half_w = (field_w - 10) / 2;
    lv_obj_set_size(cache_btn, half_w, 34);
    lv_obj_set_pos(cache_btn, 0, 308);
    lv_obj_set_style_bg_color(cache_btn, lv_color_hex(0x1a1a2a), 0);
    lv_obj_set_style_border_color(cache_btn, lv_color_hex(0x444466), 0);
    lv_obj_set_style_border_width(cache_btn, 1, 0);
    lv_obj_set_style_radius(cache_btn, 6, 0);
    lv_obj_t *cache_lbl = lv_label_create(cache_btn);
    lv_label_set_text(cache_lbl, "Clear caches");
    lv_obj_set_style_text_color(cache_lbl, lv_color_hex(0xffaa66), 0);
    lv_obj_set_style_text_font(cache_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(cache_lbl);
    lv_obj_add_event_cb(cache_btn, clear_all_caches_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *factory_btn = lv_button_create(tab_system);
    lv_obj_set_size(factory_btn, half_w, 34);
    lv_obj_set_pos(factory_btn, half_w + 10, 308);
    lv_obj_set_style_bg_color(factory_btn, lv_color_hex(0x2a1a1a), 0);
    lv_obj_set_style_border_color(factory_btn, lv_color_hex(0x664444), 0);
    lv_obj_set_style_border_width(factory_btn, 1, 0);
    lv_obj_set_style_radius(factory_btn, 6, 0);
    _factory_lbl = lv_label_create(factory_btn);
    lv_label_set_text(_factory_lbl, "Reset to defaults");
    lv_obj_set_style_text_color(_factory_lbl, ERR_COLOR, 0);
    lv_obj_set_style_text_font(_factory_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(_factory_lbl);
    lv_obj_add_event_cb(factory_btn, factory_reset_cb, LV_EVENT_CLICKED, nullptr);

    status_refresh(nullptr);
    lv_timer_create(status_refresh, 500, nullptr);

    // Footer: Cancel + Save only.
    lv_obj_t *actions = lv_obj_create(_panel);
    lv_obj_set_size(actions, body_w, ACTION_H);
    lv_obj_align(actions, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(actions, BG_COLOR, 0);
    lv_obj_set_style_bg_opa(actions, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_set_style_pad_top(actions, 8, 0);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(actions);

    const int exit_btn_w = 110;
    const int exit_gap = 10;

    lv_obj_t *cancel_btn = lv_button_create(actions);
    lv_obj_set_size(cancel_btn, exit_btn_w, 34);
    lv_obj_set_pos(cancel_btn, body_w - 2 * exit_btn_w - exit_gap, 4);
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
    lv_obj_set_pos(save_btn, body_w - exit_btn_w, 4);
    lv_obj_set_style_bg_color(save_btn, ACCENT_COLOR, 0);
    lv_obj_set_style_radius(save_btn, 8, 0);
    lv_obj_t *save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save");
    lv_obj_set_style_text_color(save_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(save_label, &lv_font_montserrat_16, 0);
    lv_obj_center(save_label);
    lv_obj_add_event_cb(save_btn, save_and_close, LV_EVENT_CLICKED, nullptr);

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
    if (_factory_lbl) lv_label_set_text(_factory_lbl, "Reset to defaults");
    _cfg = storage_load_config();
    _cfg_at_open = _cfg;
    apply_cfg_to_fields();
    start_key_validation();
    if (_tabview) {
        lv_tabview_set_active(_tabview, 0, LV_ANIM_OFF);
        refresh_tab_button_styles(_tabview);
    }
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_overlay);
}

void settings_hide() {
    if (!_visible) return;
    _visible = false;
    _apt_verify_pending = false;
    _adbox_verify_pending = false;
    _factory_confirm_until_ms = 0;
    if (_factory_lbl) lv_label_set_text(_factory_lbl, "Reset to defaults");
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
}

bool settings_is_visible() { return _visible; }
void settings_set_change_callback(settings_changed_cb_t cb) { _on_change = cb; }
