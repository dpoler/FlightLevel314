#include "status_bar.h"
#include "../pins_config.h"

static lv_obj_t *wifi_icon;
static lv_obj_t *ac_count_label;
static lv_obj_t *update_label;
static lv_obj_t *view_dots[3];
static lv_obj_t *gear_icon;

#define STATUS_BAR_HEIGHT 30
#define STATUS_BG_COLOR lv_color_hex(0x0d0d1a)
#define STATUS_TEXT_COLOR lv_color_hex(0x888899)
#define STATUS_ACCENT_COLOR lv_color_hex(0x00cc66)

lv_obj_t *status_bar_create(lv_obj_t *parent) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LCD_H_RES, STATUS_BAR_HEIGHT);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, STATUS_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // WiFi indicator (left side)
    wifi_icon = lv_label_create(bar);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi_icon, STATUS_TEXT_COLOR, 0);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_14, 0);
    lv_obj_align(wifi_icon, LV_ALIGN_LEFT_MID, 8, 0);

    // Aircraft count
    ac_count_label = lv_label_create(bar);
    lv_label_set_text(ac_count_label, "0 aircraft");
    lv_obj_set_style_text_color(ac_count_label, STATUS_TEXT_COLOR, 0);
    lv_obj_set_style_text_font(ac_count_label, &lv_font_montserrat_14, 0);
    lv_obj_align(ac_count_label, LV_ALIGN_LEFT_MID, 36, 0);

    // View indicator dots (center)
    for (int i = 0; i < 3; i++) {
        view_dots[i] = lv_obj_create(bar);
        lv_obj_set_size(view_dots[i], 8, 8);
        lv_obj_set_style_radius(view_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(view_dots[i], STATUS_TEXT_COLOR, 0);
        lv_obj_set_style_bg_opa(view_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(view_dots[i], 0, 0);
        lv_obj_align(view_dots[i], LV_ALIGN_CENTER, (i - 1) * 16, 0);
    }
    // First dot active by default
    lv_obj_set_style_bg_color(view_dots[0], STATUS_ACCENT_COLOR, 0);

    // Gear icon (right side, before update label)
    gear_icon = lv_label_create(bar);
    lv_label_set_text(gear_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(gear_icon, STATUS_TEXT_COLOR, 0);
    lv_obj_set_style_text_font(gear_icon, &lv_font_montserrat_16, 0);
    lv_obj_align(gear_icon, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_flag(gear_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(gear_icon, 10); // easier to tap

    // Last update (right side, shifted left for gear icon)
    update_label = lv_label_create(bar);
    lv_label_set_text(update_label, "--");
    lv_obj_set_style_text_color(update_label, STATUS_TEXT_COLOR, 0);
    lv_obj_set_style_text_font(update_label, &lv_font_montserrat_14, 0);
    lv_obj_align(update_label, LV_ALIGN_RIGHT_MID, -36, 0);

    return bar;
}

void status_bar_update(bool wifi_connected, int aircraft_count, uint32_t last_update_ms) {
    // WiFi color
    lv_obj_set_style_text_color(wifi_icon,
        wifi_connected ? STATUS_ACCENT_COLOR : lv_color_hex(0xcc3333), 0);

    // Aircraft count
    lv_label_set_text_fmt(ac_count_label, "%d aircraft", aircraft_count);

    // Last update
    if (last_update_ms == 0) {
        lv_label_set_text(update_label, "No data");
    } else {
        uint32_t ago = (millis() - last_update_ms) / 1000;
        lv_label_set_text_fmt(update_label, "%lus ago", ago);
    }
}

void status_bar_set_gear_callback(lv_event_cb_t cb) {
    lv_obj_add_event_cb(gear_icon, cb, LV_EVENT_CLICKED, nullptr);
}

void status_bar_set_active_dot(int view_index) {
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(view_dots[i],
            i == view_index ? STATUS_ACCENT_COLOR : STATUS_TEXT_COLOR, 0);
    }
}
