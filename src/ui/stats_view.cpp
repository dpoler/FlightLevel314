#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include "stats_view.h"
#include "stats.h"
#include "views.h"
#include "../pins_config.h"

#define STATS_W LCD_H_RES
#define STATS_H (LCD_V_RES - 30)
#define BG_COLOR lv_color_hex(0x0a0a1a)
#define DIM_COLOR lv_color_hex(0x666688)
#define ACCENT_COLOR lv_color_hex(0x4488ff)

static AircraftList *_list = nullptr;
static lv_obj_t *_container = nullptr;

// Current count
static lv_obj_t *_count_label = nullptr;

// Category rows
struct BarRow {
    lv_obj_t *name_lbl;
    lv_obj_t *count_lbl;
    lv_obj_t *bar;
};

static BarRow _cat_rows[5];
static const char *CAT_NAMES[] = {"JETS", "GA", "HELI", "MIL", "EMRG"};
static const uint32_t CAT_COLORS[] = {0x4488ff, 0x88aacc, 0x44ddaa, 0xffaa00, 0xff3333};

// Altitude rows
static BarRow _alt_rows[6];
static const char *ALT_NAMES[] = {"GND", "<5k", "<15k", "<25k", "<35k", "35k+"};
static const uint32_t ALT_COLORS[] = {0x666666, 0x00cc44, 0x88cc00, 0xcccc00, 0xcc8800, 0xcc2200};

// Fastest / Closest
static lv_obj_t *_fastest_val = nullptr;
static lv_obj_t *_closest_val = nullptr;

// Session stats
static lv_obj_t *_unique_val = nullptr;
static lv_obj_t *_peak_val = nullptr;
static lv_obj_t *_uptime_val = nullptr;

// Top types
static lv_obj_t *_type_labels[5] = {};

// System health
static lv_obj_t *_heap_val = nullptr;
static lv_obj_t *_psram_val = nullptr;
static lv_obj_t *_watermark_val = nullptr;
static lv_obj_t *_rssi_val = nullptr;
static lv_obj_t *_fps_val = nullptr;
static lv_obj_t *_tasks_val = nullptr;

// FPS measurement
static uint32_t _frame_count = 0;
static uint32_t _fps_last_time = 0;
static uint16_t _fps = 0;

#define BAR_MAX_W 280
#define BAR_H 16

static lv_obj_t *create_bar(lv_obj_t *parent, int x, int y, lv_color_t color) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 0, BAR_H);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_style_bg_color(bar, color, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    return bar;
}

// Helper: create a label+count+bar row
static void create_bar_row(lv_obj_t *parent, BarRow *row, const char *name,
                           uint32_t color_hex, int x, int y) {
    lv_color_t color = lv_color_hex(color_hex);

    row->name_lbl = lv_label_create(parent);
    lv_label_set_text(row->name_lbl, name);
    lv_obj_set_style_text_font(row->name_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(row->name_lbl, color, 0);
    lv_obj_set_pos(row->name_lbl, x, y + 1);
    lv_obj_clear_flag(row->name_lbl, LV_OBJ_FLAG_CLICKABLE);

    row->count_lbl = lv_label_create(parent);
    lv_label_set_text(row->count_lbl, "0");
    lv_obj_set_style_text_font(row->count_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(row->count_lbl, lv_color_hex(0xccccdd), 0);
    lv_obj_set_pos(row->count_lbl, x + 50, y + 1);
    lv_obj_clear_flag(row->count_lbl, LV_OBJ_FLAG_CLICKABLE);

    row->bar = create_bar(parent, x + 90, y, color);
}

static void update_bar(BarRow *row, int count, int total) {
    lv_label_set_text_fmt(row->count_lbl, "%d", count);
    int w = (total > 0) ? (count * BAR_MAX_W / total) : 0;
    if (w < 2 && count > 0) w = 2; // minimum visible bar
    lv_obj_set_width(row->bar, w);
}

static void refresh_stats(lv_timer_t *t) {
    if (views_get_active_index() != VIEW_STATS) return;

    stats_update(_list);
    const SessionStats *s = stats_get();

    // Current count
    lv_label_set_text_fmt(_count_label, "%d", s->current_count);

    // Category bars
    int cat_counts[] = {s->jets, s->ga, s->heli, s->military, s->emergency};
    int cat_total = s->current_count > 0 ? s->current_count : 1;
    for (int i = 0; i < 5; i++) {
        update_bar(&_cat_rows[i], cat_counts[i], cat_total);
    }

    // Altitude bars
    int alt_counts[] = {s->alt_gnd, s->alt_low, s->alt_med_low,
                        s->alt_med, s->alt_high, s->alt_very_high};
    int alt_max = 1;
    for (int i = 0; i < 6; i++) {
        if (alt_counts[i] > alt_max) alt_max = alt_counts[i];
    }
    for (int i = 0; i < 6; i++) {
        update_bar(&_alt_rows[i], alt_counts[i], alt_max);
    }

    // Fastest / Closest
    if (s->fastest_callsign[0]) {
        lv_label_set_text_fmt(_fastest_val, "%s  %dkt", s->fastest_callsign, s->fastest_speed);
    } else {
        lv_label_set_text(_fastest_val, "--");
    }
    if (s->closest_callsign[0] && s->closest_dist < 9999.0f) {
        lv_label_set_text_fmt(_closest_val, "%s  %.1fnm", s->closest_callsign, (double)s->closest_dist);
    } else {
        lv_label_set_text(_closest_val, "--");
    }

    // Session stats
    lv_label_set_text_fmt(_unique_val, "%d", s->unique_seen);
    lv_label_set_text_fmt(_peak_val, "%d", s->peak_count);

    uint32_t uptime_s = (millis() - s->boot_time) / 1000;
    int hrs = uptime_s / 3600;
    int mins = (uptime_s % 3600) / 60;
    int secs = uptime_s % 60;
    lv_label_set_text_fmt(_uptime_val, "%02d:%02d:%02d", hrs, mins, secs);

    // Top types
    for (int i = 0; i < 5; i++) {
        if (s->top_types[i].type[0]) {
            lv_label_set_text_fmt(_type_labels[i], "%-4s  %d", s->top_types[i].type, s->top_types[i].count);
        } else {
            lv_label_set_text(_type_labels[i], "");
        }
    }

    // System health
    uint32_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    lv_label_set_text_fmt(_heap_val, "%luK", (unsigned long)(heap_free / 1024));
    lv_label_set_text_fmt(_psram_val, "%.1fM", (double)psram_free / (1024.0 * 1024.0));
    lv_label_set_text_fmt(_watermark_val, "%luK", (unsigned long)(heap_min / 1024));

    // WiFi RSSI
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        lv_label_set_text_fmt(_rssi_val, "%d dBm", ap_info.rssi);
    } else {
        lv_label_set_text(_rssi_val, "N/A");
    }

    // FPS
    lv_label_set_text_fmt(_fps_val, "%d", _fps);

    // Task count
    lv_label_set_text_fmt(_tasks_val, "%lu", (unsigned long)uxTaskGetNumberOfTasks());
}

void stats_view_init(lv_obj_t *parent, AircraftList *list) {
    _list = list;

    _container = lv_obj_create(parent);
    lv_obj_set_size(_container, STATS_W, STATS_H);
    lv_obj_set_pos(_container, 0, 0);
    lv_obj_set_style_bg_color(_container, BG_COLOR, 0);
    lv_obj_set_style_bg_opa(_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_container, 0, 0);
    lv_obj_set_style_radius(_container, 0, 0);
    lv_obj_set_style_pad_all(_container, 0, 0);
    lv_obj_clear_flag(_container, LV_OBJ_FLAG_SCROLLABLE);

    // === LEFT COLUMN ===
    int lx = 20;

    // Current count (large)
    _count_label = lv_label_create(_container);
    lv_label_set_text(_count_label, "0");
    lv_obj_set_style_text_font(_count_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_count_label, lv_color_white(), 0);
    lv_obj_set_pos(_count_label, lx, 10);
    lv_obj_clear_flag(_count_label, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *subtitle = lv_label_create(_container);
    lv_label_set_text(subtitle, "TRACKING NOW");
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(subtitle, DIM_COLOR, 0);
    lv_obj_set_pos(subtitle, lx, 44);
    lv_obj_clear_flag(subtitle, LV_OBJ_FLAG_CLICKABLE);

    // Category breakdown
    int cat_y = 70;
    for (int i = 0; i < 5; i++) {
        create_bar_row(_container, &_cat_rows[i], CAT_NAMES[i], CAT_COLORS[i],
                       lx, cat_y + i * 28);
    }

    // Fastest + Closest
    int fc_y = 230;

    lv_obj_t *fast_lbl = lv_label_create(_container);
    lv_label_set_text(fast_lbl, "FASTEST");
    lv_obj_set_style_text_font(fast_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(fast_lbl, DIM_COLOR, 0);
    lv_obj_set_pos(fast_lbl, lx, fc_y);
    lv_obj_clear_flag(fast_lbl, LV_OBJ_FLAG_CLICKABLE);

    _fastest_val = lv_label_create(_container);
    lv_label_set_text(_fastest_val, "--");
    lv_obj_set_style_text_font(_fastest_val, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_fastest_val, lv_color_hex(0xff66cc), 0);
    lv_obj_set_pos(_fastest_val, lx, fc_y + 18);
    lv_obj_clear_flag(_fastest_val, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *close_lbl = lv_label_create(_container);
    lv_label_set_text(close_lbl, "CLOSEST");
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(close_lbl, DIM_COLOR, 0);
    lv_obj_set_pos(close_lbl, lx, fc_y + 48);
    lv_obj_clear_flag(close_lbl, LV_OBJ_FLAG_CLICKABLE);

    _closest_val = lv_label_create(_container);
    lv_label_set_text(_closest_val, "--");
    lv_obj_set_style_text_font(_closest_val, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_closest_val, lv_color_hex(0x44ddaa), 0);
    lv_obj_set_pos(_closest_val, lx, fc_y + 66);
    lv_obj_clear_flag(_closest_val, LV_OBJ_FLAG_CLICKABLE);

    // === RIGHT COLUMN ===
    int rx = 530;

    // Altitude distribution
    int alt_y = 10;
    lv_obj_t *alt_header = lv_label_create(_container);
    lv_label_set_text(alt_header, "ALTITUDE");
    lv_obj_set_style_text_font(alt_header, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(alt_header, DIM_COLOR, 0);
    lv_obj_set_pos(alt_header, rx, alt_y);
    lv_obj_clear_flag(alt_header, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 6; i++) {
        create_bar_row(_container, &_alt_rows[i], ALT_NAMES[i], ALT_COLORS[i],
                       rx, alt_y + 22 + i * 28);
    }

    // Session stats
    int ss_y = 220;

    lv_obj_t *uniq_lbl = lv_label_create(_container);
    lv_label_set_text(uniq_lbl, "UNIQUE SEEN");
    lv_obj_set_style_text_font(uniq_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(uniq_lbl, DIM_COLOR, 0);
    lv_obj_set_pos(uniq_lbl, rx, ss_y);
    lv_obj_clear_flag(uniq_lbl, LV_OBJ_FLAG_CLICKABLE);

    _unique_val = lv_label_create(_container);
    lv_label_set_text(_unique_val, "0");
    lv_obj_set_style_text_font(_unique_val, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_unique_val, ACCENT_COLOR, 0);
    lv_obj_set_pos(_unique_val, rx, ss_y + 18);
    lv_obj_clear_flag(_unique_val, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *peak_lbl = lv_label_create(_container);
    lv_label_set_text(peak_lbl, "PEAK COUNT");
    lv_obj_set_style_text_font(peak_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(peak_lbl, DIM_COLOR, 0);
    lv_obj_set_pos(peak_lbl, rx, ss_y + 50);
    lv_obj_clear_flag(peak_lbl, LV_OBJ_FLAG_CLICKABLE);

    _peak_val = lv_label_create(_container);
    lv_label_set_text(_peak_val, "0");
    lv_obj_set_style_text_font(_peak_val, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_peak_val, ACCENT_COLOR, 0);
    lv_obj_set_pos(_peak_val, rx, ss_y + 68);
    lv_obj_clear_flag(_peak_val, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *up_lbl = lv_label_create(_container);
    lv_label_set_text(up_lbl, "UPTIME");
    lv_obj_set_style_text_font(up_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(up_lbl, DIM_COLOR, 0);
    lv_obj_set_pos(up_lbl, rx, ss_y + 100);
    lv_obj_clear_flag(up_lbl, LV_OBJ_FLAG_CLICKABLE);

    _uptime_val = lv_label_create(_container);
    lv_label_set_text(_uptime_val, "00:00:00");
    lv_obj_set_style_text_font(_uptime_val, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_uptime_val, ACCENT_COLOR, 0);
    lv_obj_set_pos(_uptime_val, rx, ss_y + 118);
    lv_obj_clear_flag(_uptime_val, LV_OBJ_FLAG_CLICKABLE);

    // Top types
    int ty = 370;
    lv_obj_t *types_hdr = lv_label_create(_container);
    lv_label_set_text(types_hdr, "TOP TYPES");
    lv_obj_set_style_text_font(types_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(types_hdr, DIM_COLOR, 0);
    lv_obj_set_pos(types_hdr, rx, ty);
    lv_obj_clear_flag(types_hdr, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 5; i++) {
        _type_labels[i] = lv_label_create(_container);
        lv_label_set_text(_type_labels[i], "");
        lv_obj_set_style_text_font(_type_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(_type_labels[i], lv_color_hex(0xccccdd), 0);
        lv_obj_set_pos(_type_labels[i], rx, ty + 20 + i * 20);
        lv_obj_clear_flag(_type_labels[i], LV_OBJ_FLAG_CLICKABLE);
    }

    // === SYSTEM HEALTH (left column, below fastest/closest) ===
    int sys_y = 320;
    lv_obj_t *sys_header = lv_label_create(_container);
    lv_label_set_text(sys_header, "SYSTEM");
    lv_obj_set_style_text_font(sys_header, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sys_header, DIM_COLOR, 0);
    lv_obj_set_pos(sys_header, lx, sys_y);
    lv_obj_clear_flag(sys_header, LV_OBJ_FLAG_CLICKABLE);

    // Row 1: HEAP / PSRAM / WATERMARK
    struct SysField { const char *name; int x; lv_obj_t **val; };
    SysField fields[] = {
        {"HEAP",      lx,       &_heap_val},
        {"PSRAM",     lx + 120, &_psram_val},
        {"HEAP MIN",  lx + 240, &_watermark_val},
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *lbl = lv_label_create(_container);
        lv_label_set_text(lbl, fields[i].name);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, DIM_COLOR, 0);
        lv_obj_set_pos(lbl, fields[i].x, sys_y + 20);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        *fields[i].val = lv_label_create(_container);
        lv_label_set_text(*fields[i].val, "--");
        lv_obj_set_style_text_font(*fields[i].val, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(*fields[i].val, lv_color_hex(0x44cc88), 0);
        lv_obj_set_pos(*fields[i].val, fields[i].x, sys_y + 36);
        lv_obj_clear_flag(*fields[i].val, LV_OBJ_FLAG_CLICKABLE);
    }

    // Row 2: RSSI / FPS / TASKS
    SysField fields2[] = {
        {"WIFI",   lx,       &_rssi_val},
        {"FPS",    lx + 120, &_fps_val},
        {"TASKS",  lx + 240, &_tasks_val},
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *lbl = lv_label_create(_container);
        lv_label_set_text(lbl, fields2[i].name);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, DIM_COLOR, 0);
        lv_obj_set_pos(lbl, fields2[i].x, sys_y + 62);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        *fields2[i].val = lv_label_create(_container);
        lv_label_set_text(*fields2[i].val, "--");
        lv_obj_set_style_text_font(*fields2[i].val, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(*fields2[i].val, lv_color_hex(0x44cc88), 0);
        lv_obj_set_pos(*fields2[i].val, fields2[i].x, sys_y + 78);
        lv_obj_clear_flag(*fields2[i].val, LV_OBJ_FLAG_CLICKABLE);
    }

    // FPS counter — increment each refresh, calculate every second
    _fps_last_time = millis();
    lv_timer_create([](lv_timer_t *t) {
        _frame_count++;
        uint32_t now = millis();
        if (now - _fps_last_time >= 1000) {
            _fps = (uint16_t)(_frame_count * 1000 / (now - _fps_last_time));
            _frame_count = 0;
            _fps_last_time = now;
        }
    }, 33, nullptr); // count at ~30fps rate

    // Refresh timer
    lv_timer_create(refresh_stats, 2000, nullptr);
}

void stats_view_update() {
    // triggered externally if needed
}
