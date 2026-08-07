// Scoped-down Pi implementation of src/ui/settings.h -- deliberately NOT
// a port of src/ui/settings.cpp. That file is CONFIG (WiFi SSID/password,
// Ethernet toggle + reboot-on-change, OTA firmware update) plus STATUS
// (ESP32 heap/PSRAM/temperature/FreeRTOS task count/flash %) -- almost
// none of which applies to a Pi whose networking, firmware updates, and
// system stats work completely differently. Kept: range presets, metric
// units (still meaningful config), and a read-only status strip using
// what's actually already real on Pi -- fetcher_get_stats() and
// error_log.cpp, both ported for real (see project_pi_port memory).
//
// Dropped entirely for now: WiFi/Ethernet UI (Pi's networking is
// OS-managed), OTA (Pi would need an entirely different update mechanism,
// not designed yet), heap/PSRAM/temp/tasks/flash (ESP32-specific
// concepts with no Pi equivalent worth faking).

#include "../src/ui/settings.h"
#include "../src/data/error_log.h"
#include "../src/data/fetcher.h"
#include "../src/data/locations.h"
#include "../src/platform/platform.h"
#include "../src/ui/location_picker.h"
#include "../src/ui/map_view.h"
#include "../src/ui/range.h"
#include "basemap.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static lv_obj_t *_overlay = nullptr;
static lv_obj_t *_panel = nullptr;
static lv_obj_t *_keyboard = nullptr;
static bool _visible = false;
static uint32_t _shown_at_ms = 0;
static uint32_t _boot_time_ms = 0;

static lv_obj_t *_ta_radius[4] = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t *_sw_metric = nullptr;
static lv_obj_t *_fetch_val = nullptr;
static lv_obj_t *_latency_val = nullptr;
static lv_obj_t *_uptime_val = nullptr;
static lv_obj_t *_err_count_lbl = nullptr;
static lv_obj_t *_err_list_lbl = nullptr;
static lv_obj_t *_factory_lbl = nullptr;
static uint32_t _factory_confirm_until_ms = 0;

static UserConfig _cfg;
static settings_changed_cb_t _on_change = nullptr;

#define PANEL_W 370
#define PANEL_H 540
#define FIELD_W 280
#define LABEL_COLOR lv_color_hex(0x8888aa)
#define BG_COLOR lv_color_hex(0x12122a)
#define ACCENT_COLOR lv_color_hex(0x00cc66)
#define SYS_COLOR lv_color_hex(0x44cc88)

static void ta_focus_cb(lv_event_t *e) {
    lv_keyboard_set_textarea(_keyboard, lv_event_get_target_obj(e));
    lv_keyboard_set_mode(_keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_clear_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void keyboard_ready_cb(lv_event_t *e) {
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

static void status_refresh(lv_timer_t *t) {
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

    // Expire the two-tap factory-reset confirm if the window elapsed.
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
}

static void save_and_close(lv_event_t *e) {
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

    storage_save_config(_cfg);
    if (_on_change) _on_change(&_cfg);
    settings_hide();
}

static void clear_all_caches_cb(lv_event_t *e) {
    int n = basemap_cache_clear();
    locations_nearby_cache_clear();
    platform_log("Settings: cleared all caches (%d basemap file(s) + nearby runways)\n", n);
    map_view_on_show(); // re-request basemap for current projection if Map is live
}

static void factory_reset_cb(lv_event_t *e) {
    uint32_t now = platform_millis();
    if (!_factory_confirm_until_ms || now > _factory_confirm_until_ms) {
        // First tap: arm a short confirm window (does not wipe the Pi OS --
        // only ADS-B config.json, locations.json, and basemap cache).
        _factory_confirm_until_ms = now + 4000;
        if (_factory_lbl) lv_label_set_text(_factory_lbl, "Tap again to confirm");
        return;
    }

    _factory_confirm_until_ms = 0;
    if (_factory_lbl) lv_label_set_text(_factory_lbl, "Reset to factory defaults");

    storage_factory_reset();
    locations_factory_reset();
    basemap_cache_clear();

    _cfg = storage_load_config(); // compiled defaults (no config.json)
    g_config = _cfg;
    storage_save_config(g_config); // persist clean defaults
    apply_cfg_to_fields();
    if (_on_change) _on_change(&g_config);

    location_picker_close();
    map_view_on_show();
    platform_log("Settings: ADS-B factory defaults restored (config + locations + caches)\n");
    settings_hide();
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
    lv_obj_add_event_cb(_overlay, [](lv_event_t *e) {
        if (lv_event_get_target_obj(e) == _overlay && platform_millis() - _shown_at_ms > 400) settings_hide();
    }, LV_EVENT_CLICKED, nullptr);

    _panel = lv_obj_create(_overlay);
    lv_obj_set_size(_panel, PANEL_W, PANEL_H);
    lv_obj_align(_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_panel, BG_COLOR, 0);
    lv_obj_set_style_bg_opa(_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(_panel, 12, 0);
    lv_obj_set_style_border_color(_panel, lv_color_hex(0x333366), 0);
    lv_obj_set_style_border_width(_panel, 1, 0);
    lv_obj_set_style_pad_all(_panel, 20, 0);
    lv_obj_clear_flag(_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(_panel);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(title, 0, 0);

    _cfg = storage_load_config();

    create_label(_panel, "Range Presets (nm, 1-500)", 0, 44);
    for (int i = 0; i < 4; i++) {
        char rbuf[8];
        snprintf(rbuf, sizeof(rbuf), "%d", _cfg.radius_presets[i]);
        _ta_radius[i] = lv_textarea_create(_panel);
        lv_obj_set_size(_ta_radius[i], 60, 36);
        lv_obj_set_pos(_ta_radius[i], i * 66, 64);
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

    create_label(_panel, "Metric Units", 0, 128);
    _sw_metric = lv_switch_create(_panel);
    lv_obj_set_pos(_sw_metric, 110, 126);
    lv_obj_set_style_bg_color(_sw_metric, lv_color_hex(0x333366), 0);
    lv_obj_set_style_bg_color(_sw_metric, ACCENT_COLOR, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (_cfg.use_metric) lv_obj_add_state(_sw_metric, LV_STATE_CHECKED);

    int sy = 172;
    create_label(_panel, "STATUS", 0, sy);
    _fetch_val = create_inline_row(_panel, "FETCHES", 0, sy + 20, 90);
    _latency_val = create_inline_row(_panel, "LATENCY", 0, sy + 38, 90);
    _uptime_val = create_inline_row(_panel, "UPTIME", 0, sy + 56, 90);

    int ey = sy + 82;
    create_label(_panel, "ERRORS", 0, ey);
    _err_count_lbl = lv_label_create(_panel);
    lv_label_set_text(_err_count_lbl, "(0)");
    lv_obj_set_style_text_font(_err_count_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_err_count_lbl, LABEL_COLOR, 0);
    lv_obj_set_pos(_err_count_lbl, 70, ey);
    lv_obj_clear_flag(_err_count_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *clr_btn = lv_obj_create(_panel);
    lv_obj_set_size(clr_btn, 40, 22);
    lv_obj_set_pos(clr_btn, 120, ey - 2);
    lv_obj_set_style_bg_color(clr_btn, lv_color_hex(0x1a1a2a), 0);
    lv_obj_set_style_bg_opa(clr_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(clr_btn, lv_color_hex(0x444466), 0);
    lv_obj_set_style_border_width(clr_btn, 1, 0);
    lv_obj_set_style_radius(clr_btn, 4, 0);
    lv_obj_set_style_pad_all(clr_btn, 0, 0);
    lv_obj_clear_flag(clr_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(clr_btn, [](lv_event_t *e) { error_log_clear(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *clr_lbl = lv_label_create(clr_btn);
    lv_label_set_text(clr_lbl, "CLR");
    lv_obj_set_style_text_font(clr_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(clr_lbl, lv_color_hex(0xff6666), 0);
    lv_obj_center(clr_lbl);

    _err_list_lbl = lv_label_create(_panel);
    lv_label_set_text(_err_list_lbl, "(none)");
    lv_obj_set_style_text_font(_err_list_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_err_list_lbl, lv_color_hex(0xff6666), 0);
    lv_obj_set_pos(_err_list_lbl, 0, ey + 18);
    lv_obj_set_width(_err_list_lbl, FIELD_W + 30);
    lv_obj_clear_flag(_err_list_lbl, LV_OBJ_FLAG_CLICKABLE);

    status_refresh(nullptr);
    lv_timer_create(status_refresh, 2000, nullptr);

    // Basemap mosaics + nearby-runway lists. Instant — not part of Save.
    lv_obj_t *cache_btn = lv_button_create(_panel);
    lv_obj_set_size(cache_btn, FIELD_W + 30, 34);
    lv_obj_align(cache_btn, LV_ALIGN_BOTTOM_MID, 0, -106);
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

    // ADS-B app only (config + locations + caches) — never touches the Pi OS.
    // Two-tap confirm within 4s to avoid an accidental wipe.
    lv_obj_t *factory_btn = lv_button_create(_panel);
    lv_obj_set_size(factory_btn, FIELD_W + 30, 34);
    lv_obj_align(factory_btn, LV_ALIGN_BOTTOM_MID, 0, -58);
    lv_obj_set_style_bg_color(factory_btn, lv_color_hex(0x2a1a1a), 0);
    lv_obj_set_style_border_color(factory_btn, lv_color_hex(0x664444), 0);
    lv_obj_set_style_border_width(factory_btn, 1, 0);
    lv_obj_set_style_radius(factory_btn, 6, 0);
    _factory_lbl = lv_label_create(factory_btn);
    lv_label_set_text(_factory_lbl, "Reset to factory defaults");
    lv_obj_set_style_text_color(_factory_lbl, lv_color_hex(0xff6666), 0);
    lv_obj_set_style_text_font(_factory_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(_factory_lbl);
    lv_obj_add_event_cb(factory_btn, factory_reset_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *save_btn = lv_button_create(_panel);
    lv_obj_set_size(save_btn, 120, 40);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
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
    if (_factory_lbl) lv_label_set_text(_factory_lbl, "Reset to factory defaults");
    _cfg = storage_load_config();
    apply_cfg_to_fields();
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
}

void settings_hide() {
    if (!_visible) return;
    _visible = false;
    _factory_confirm_until_ms = 0;
    if (_factory_lbl) lv_label_set_text(_factory_lbl, "Reset to factory defaults");
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
}

bool settings_is_visible() { return _visible; }
void settings_set_change_callback(settings_changed_cb_t cb) { _on_change = cb; }
