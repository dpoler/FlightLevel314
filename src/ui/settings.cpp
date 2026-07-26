#include <Arduino.h>
#include "settings.h"
#include "screensaver.h"
#include "../pins_config.h"
#include "../data/storage.h"
#include <cstdio>

static lv_obj_t *_overlay = nullptr;
static lv_obj_t *_panel = nullptr;
static lv_obj_t *_keyboard = nullptr;
static bool _visible = false;

// Text areas
static lv_obj_t *_ta_ssid = nullptr;
static lv_obj_t *_ta_pass = nullptr;
static lv_obj_t *_ta_airportdb_token = nullptr;

// Controls
static lv_obj_t *_ta_radius[4] = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t *_sw_metric = nullptr;
static lv_obj_t *_sw_ethernet = nullptr;
static lv_obj_t *_btn_show_pass = nullptr;
static lv_obj_t *_sw_alert_mil = nullptr;
static lv_obj_t *_sw_alert_emg = nullptr;

static UserConfig _cfg;

// Callback for config changes (set by main)
static settings_changed_cb_t _on_change = nullptr;

#define PANEL_W 820
#define PANEL_H (LCD_V_RES - 40)
#define FIELD_W 240
#define LABEL_COLOR lv_color_hex(0x8888aa)
#define BG_COLOR lv_color_hex(0x12122a)
#define ACCENT_COLOR lv_color_hex(0x00cc66)

static lv_obj_t *_focused_ta = nullptr;

static void show_keyboard_for(lv_obj_t *ta) {
    _focused_ta = ta;
    lv_keyboard_set_textarea(_keyboard, ta);
    lv_obj_clear_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void ta_focus_cb(lv_event_t *e) {
    show_keyboard_for(lv_event_get_target_obj(e));
    // Range Presets pass LV_KEYBOARD_MODE_NUMBER as user_data (digits, "+/-",
    // "." on one layout instead of buried on the alpha keyboard); every
    // other field here leaves user_data null, which is also
    // LV_KEYBOARD_MODE_TEXT_LOWER (0).
    lv_keyboard_set_mode(_keyboard, (lv_keyboard_mode_t)(intptr_t)lv_event_get_user_data(e));
}

static void keyboard_ready_cb(lv_event_t *e) {
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    _focused_ta = nullptr;
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, int x, int y) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, LABEL_COLOR, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

static lv_obj_t *create_textarea(lv_obj_t *parent, const char *placeholder,
                                  const char *value, int x, int y, bool password = false) {
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, FIELD_W, 36);
    lv_obj_set_pos(ta, x, y);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_text(ta, value);
    if (password) lv_textarea_set_password_mode(ta, true);

    lv_obj_set_style_bg_color(ta, lv_color_hex(0x1a1a3a), 0);
    lv_obj_set_style_text_color(ta, lv_color_white(), 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(0x333366), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, ACCENT_COLOR, LV_STATE_FOCUSED);

    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, nullptr);
    return ta;
}

static lv_obj_t *create_switch(lv_obj_t *parent, int x, int y, bool checked) {
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_pos(sw, x, y);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x333366), 0);
    lv_obj_set_style_bg_color(sw, ACCENT_COLOR, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    return sw;
}

static void save_and_close(lv_event_t *e) {
    bool old_use_ethernet = _cfg.use_ethernet;

    // Read values from text areas
    strncpy(_cfg.wifi_ssid, lv_textarea_get_text(_ta_ssid), sizeof(_cfg.wifi_ssid) - 1);
    _cfg.wifi_ssid[sizeof(_cfg.wifi_ssid) - 1] = '\0';
    for (char *p = _cfg.wifi_ssid; *p; p++) if (*p == '\r' || *p == '\n') *p = '\0';
    strncpy(_cfg.wifi_pass, lv_textarea_get_text(_ta_pass), sizeof(_cfg.wifi_pass) - 1);
    _cfg.wifi_pass[sizeof(_cfg.wifi_pass) - 1] = '\0';
    for (char *p = _cfg.wifi_pass; *p; p++) if (*p == '\r' || *p == '\n') *p = '\0';
    strncpy(_cfg.airportdb_token, lv_textarea_get_text(_ta_airportdb_token), sizeof(_cfg.airportdb_token) - 1);
    _cfg.airportdb_token[sizeof(_cfg.airportdb_token) - 1] = '\0';
    for (char *p = _cfg.airportdb_token; *p; p++) if (*p == '\r' || *p == '\n') *p = '\0';
    for (int i = 0; i < 4; i++) {
        int v = atoi(lv_textarea_get_text(_ta_radius[i]));
        if (v < 1) v = 1;
        if (v > 500) v = 500;
        _cfg.radius_presets[i] = v;
    }
    // Sort ascending so range module receives them in order
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 4; j++)
            if (_cfg.radius_presets[i] > _cfg.radius_presets[j]) {
                int tmp = _cfg.radius_presets[i];
                _cfg.radius_presets[i] = _cfg.radius_presets[j];
                _cfg.radius_presets[j] = tmp;
            }
    _cfg.radius_nm = _cfg.radius_presets[3]; // max preset = API query radius
    _cfg.use_metric = lv_obj_has_state(_sw_metric, LV_STATE_CHECKED);
    _cfg.use_ethernet = lv_obj_has_state(_sw_ethernet, LV_STATE_CHECKED);
    _cfg.alert_military = lv_obj_has_state(_sw_alert_mil, LV_STATE_CHECKED);
    _cfg.alert_emergency = lv_obj_has_state(_sw_alert_emg, LV_STATE_CHECKED);

    storage_save_config(_cfg);
    Serial.println("Config saved to NVS");

    if (_on_change) _on_change(&_cfg);

    settings_hide();

    // Network mode change requires reboot (can't switch ETH/WiFi at runtime)
    if (_cfg.use_ethernet != old_use_ethernet) {
        Serial.println("Network mode changed, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP.restart();
    }
}

void settings_init(lv_obj_t *parent) {
    // Semi-transparent overlay
    _overlay = lv_obj_create(parent);
    lv_obj_set_size(_overlay, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(_overlay, 0, 0);
    lv_obj_set_style_bg_color(_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(_overlay, 0, 0);
    lv_obj_set_style_radius(_overlay, 0, 0);
    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);

    // Tap overlay background to close
    lv_obj_add_event_cb(_overlay, [](lv_event_t *e) {
        if (lv_event_get_target_obj(e) == _overlay) settings_hide();
    }, LV_EVENT_CLICKED, nullptr);

    // Settings panel (centered)
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

    // Title
    lv_obj_t *title = lv_label_create(_panel);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(title, 0, 0);

    // Load current config
    _cfg = storage_load_config();

    // === LEFT SIDE (x=0) ===

    // WiFi
    create_label(_panel, "WiFi SSID", 0, 36);
    _ta_ssid = create_textarea(_panel, "SSID", _cfg.wifi_ssid, 0, 54);

    create_label(_panel, "WiFi Password", 0, 96);
    _ta_pass = create_textarea(_panel, "Password", _cfg.wifi_pass, 0, 114, true);

    _btn_show_pass = lv_button_create(_panel);
    lv_obj_set_size(_btn_show_pass, 34, 36);
    lv_obj_set_pos(_btn_show_pass, FIELD_W + 4, 114);
    lv_obj_set_style_bg_color(_btn_show_pass, lv_color_hex(0x1a1a3a), 0);
    lv_obj_set_style_border_color(_btn_show_pass, lv_color_hex(0x333366), 0);
    lv_obj_set_style_border_width(_btn_show_pass, 1, 0);
    lv_obj_set_style_radius(_btn_show_pass, 4, 0);
    lv_obj_set_style_shadow_width(_btn_show_pass, 0, 0);
    { lv_obj_t *lbl = lv_label_create(_btn_show_pass);
      lv_label_set_text(lbl, LV_SYMBOL_EYE_OPEN);
      lv_obj_set_style_text_color(lbl, lv_color_hex(0x8888aa), 0);
      lv_obj_center(lbl); }
    lv_obj_add_event_cb(_btn_show_pass, [](lv_event_t *e) {
        bool pw = lv_textarea_get_password_mode(_ta_pass);
        lv_textarea_set_password_mode(_ta_pass, !pw);
        lv_obj_t *lbl = lv_obj_get_child(lv_event_get_target_obj(e), 0);
        lv_label_set_text(lbl, pw ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
    }, LV_EVENT_CLICKED, nullptr);

    // Range Presets — 4 configurable text fields (nm, 1-500)
    create_label(_panel, "Range Presets (nm, 1-500)", 0, 158);
    for (int i = 0; i < 4; i++) {
        char rbuf[8];
        snprintf(rbuf, sizeof(rbuf), "%d", _cfg.radius_presets[i]);
        _ta_radius[i] = lv_textarea_create(_panel);
        lv_obj_set_size(_ta_radius[i], 60, 36);
        lv_obj_set_pos(_ta_radius[i], i * 66, 178);
        lv_textarea_set_one_line(_ta_radius[i], true);
        lv_textarea_set_text(_ta_radius[i], rbuf);
        lv_obj_set_style_bg_color(_ta_radius[i], lv_color_hex(0x1a1a3a), 0);
        lv_obj_set_style_text_color(_ta_radius[i], lv_color_white(), 0);
        lv_obj_set_style_text_font(_ta_radius[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_border_color(_ta_radius[i], lv_color_hex(0x333366), 0);
        lv_obj_set_style_border_width(_ta_radius[i], 1, 0);
        lv_obj_set_style_border_color(_ta_radius[i], ACCENT_COLOR, LV_STATE_FOCUSED);
        lv_obj_add_event_cb(_ta_radius[i], ta_focus_cb, LV_EVENT_FOCUSED,
                            (void *)(intptr_t)LV_KEYBOARD_MODE_NUMBER);
    }

    // Metric
    create_label(_panel, "Metric Units", 0, 222);
    _sw_metric = create_switch(_panel, 110, 220, _cfg.use_metric);

    // Network mode (Ethernet toggle — off=WiFi, on=Ethernet)
    create_label(_panel, "Ethernet", 0, 258);
    _sw_ethernet = create_switch(_panel, 110, 256, _cfg.use_ethernet);
    lv_obj_t *net_hint = lv_label_create(_panel);
    lv_label_set_text(net_hint, "(reboot)");
    lv_obj_set_style_text_color(net_hint, lv_color_hex(0x666688), 0);
    lv_obj_set_style_text_font(net_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(net_hint, 164, 260);

    // Alert toggles
    create_label(_panel, "Mil Alerts", 0, 292);
    _sw_alert_mil = create_switch(_panel, 110, 290, _cfg.alert_military);

    create_label(_panel, "Emg Alerts", 0, 326);
    _sw_alert_emg = create_switch(_panel, 110, 324, _cfg.alert_emergency);

    // === RIGHT SIDE (x=420) ===
    int rx = 420;

    // Location entry (Home lat/lon/elevation, and airport-by-ICAO) now
    // happens entirely through the location picker's add-flow
    // (location_picker.cpp) instead of here -- there's no single
    // distinguished "Home" location anymore, just saved locations like any
    // other. Aircraft Trails on/off, length, and clear are similarly
    // consolidated in the status bar's VIEW chip popover
    // (status_bar.cpp -> view_menu.cpp) instead of living here.

    // Hide ground aircraft -- moved to a quick-access GND button in the
    // right-edge filter column (map/radar/arrivals), same control as this
    // used to be, just not buried in Settings anymore.

    // Display / Screensaver button -- deactivated 2026-07-23 along with the
    // rest of screensaver.cpp (see the #if 0 block there for why). Left
    // commented out rather than deleted so it's a one-step re-enable.
#if 0
    lv_obj_t *display_btn = lv_button_create(_panel);
    lv_obj_set_size(display_btn, FIELD_W, 40);
    lv_obj_set_pos(display_btn, rx, 118);
    lv_obj_set_style_bg_color(display_btn, lv_color_hex(0x1a1a3a), 0);
    lv_obj_set_style_border_color(display_btn, lv_color_hex(0x333366), 0);
    lv_obj_set_style_border_width(display_btn, 1, 0);
    lv_obj_set_style_radius(display_btn, 6, 0);
    { lv_obj_t *lbl = lv_label_create(display_btn);
      lv_label_set_text(lbl, "Display / Screensaver...");
      lv_obj_set_style_text_color(lbl, lv_color_hex(0xccccdd), 0);
      lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
      lv_obj_center(lbl); }
    lv_obj_add_event_cb(display_btn, [](lv_event_t *e) {
        screensaver_show_settings();
    }, LV_EVENT_CLICKED, nullptr);
#endif

    // airportdb.io token — used by the location picker's "Add Airport" flow.
    // Shifted up into the space Auto-Cycle Views used to occupy (removed).
    create_label(_panel, "Airport DB Token (airportdb.io)", rx, 36);
    _ta_airportdb_token = create_textarea(_panel, "token", _cfg.airportdb_token, rx, 54);

    // === Save button (centered at bottom) ===
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

    // === On-screen keyboard (hidden by default) ===
    _keyboard = lv_keyboard_create(_overlay);
    lv_obj_set_size(_keyboard, LCD_H_RES, 200);
    lv_obj_align(_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(_keyboard, keyboard_ready_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(_keyboard, keyboard_ready_cb, LV_EVENT_CANCEL, nullptr);
}

void settings_show() {
    if (_visible) return;
    _visible = true;

    // Reload config in case it changed
    _cfg = storage_load_config();
    lv_textarea_set_text(_ta_ssid, _cfg.wifi_ssid);
    lv_textarea_set_text(_ta_pass, _cfg.wifi_pass);
    lv_textarea_set_password_mode(_ta_pass, true);
    lv_textarea_set_text(_ta_airportdb_token, _cfg.airportdb_token);
    lv_label_set_text(lv_obj_get_child(_btn_show_pass, 0), LV_SYMBOL_EYE_OPEN);

    for (int i = 0; i < 4; i++) {
        char rbuf[8];
        snprintf(rbuf, sizeof(rbuf), "%d", _cfg.radius_presets[i]);
        lv_textarea_set_text(_ta_radius[i], rbuf);
    }

    if (_cfg.use_metric) lv_obj_add_state(_sw_metric, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_metric, LV_STATE_CHECKED);

    if (_cfg.alert_military) lv_obj_add_state(_sw_alert_mil, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_alert_mil, LV_STATE_CHECKED);

    if (_cfg.alert_emergency) lv_obj_add_state(_sw_alert_emg, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_alert_emg, LV_STATE_CHECKED);

    if (_cfg.use_ethernet) lv_obj_add_state(_sw_ethernet, LV_STATE_CHECKED);
    else lv_obj_clear_state(_sw_ethernet, LV_STATE_CHECKED);

    lv_obj_clear_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
}

void settings_hide() {
    if (!_visible) return;
    _visible = false;
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_overlay, LV_OBJ_FLAG_HIDDEN);
}

bool settings_is_visible() {
    return _visible;
}

void settings_set_change_callback(settings_changed_cb_t cb) {
    _on_change = cb;
}
