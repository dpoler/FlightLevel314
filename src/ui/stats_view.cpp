#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_chip_info.h>
#include "stats_view.h"
#include "stats.h"
#include "views.h"
#include "status_bar.h"
#include "../pins_config.h"
#include "../data/fetcher.h"
#include "../data/error_log.h"
#include "../data/locations.h"
#include "geo.h" // altitude_color()

#define STATS_W LCD_H_RES
#define STATS_H (LCD_V_RES - STATUS_BAR_HEIGHT)
#define BG_COLOR lv_color_hex(0x0a0a1a)
#define DIM_COLOR lv_color_hex(0x9999bb) // brightened from 0x666688 -- low contrast against BG_COLOR was hard to read
#define ACCENT_COLOR lv_color_hex(0x4488ff)
#define SYS_COLOR lv_color_hex(0x44cc88)
#define WARN_COLOR lv_color_hex(0xccaa00)
// Column-identity accent for the "LOCATION" (session) column -- lets
// UNIQUE/PEAK and the column header itself read as visually distinct from
// the "CURRENT TRAFFIC" (live) column's ACCENT_COLOR, reinforcing the reset-on-
// switch grouping without needing a caption to explain it every time.
#define SESSION_COLOR lv_color_hex(0xaa88ff)

static AircraftList *_list = nullptr;      // the one aircraft list -- fetch_task always fetches for whichever location is currently active
static lv_obj_t *_container = nullptr;
static lv_obj_t *_traffic_total_lbl = nullptr; // "Total: N" caption under CURRENT TRAFFIC

// Category rows
struct BarRow {
    lv_obj_t *name_lbl;
    lv_obj_t *count_lbl;
    lv_obj_t *bar;
};

static BarRow _cat_rows[5];
static const char *CAT_NAMES[] = {"JETS", "GA", "HELI", "MIL", "EMRG"};
static const uint32_t CAT_COLORS[] = {0x4488ff, 0x88aacc, 0x44ddaa, 0xffaa00, 0xff3333};

// Altitude rows -- colors sourced from altitude_color() (geo.h) at a
// representative altitude in each band, rather than a second hardcoded
// palette, so this can't drift from what the trails on Map/Radar draw.
// GND dropped -- ground traffic is excluded from this whole screen (see
// stats.cpp), so that bucket would always read zero.
static BarRow _alt_rows[5];
static const char *ALT_NAMES[] = {"<5k", "<15k", "<25k", "<35k", "35k+"};
static const int32_t ALT_SAMPLES[] = {2500, 10000, 20000, 30000, 45000};

// Speed rows -- GND dropped, same reason as altitude above
static BarRow _spd_rows[5];
static const char *SPD_NAMES[] = {"<200", "<300", "<400", "<500", "500+"};
static const uint32_t SPD_COLORS[] = {0x4488cc, 0x4488ff, 0x8844ff, 0xcc44ff, 0xff44aa};

// Records
static lv_obj_t *_fastest_val = nullptr;
static lv_obj_t *_slowest_val = nullptr;
static lv_obj_t *_highest_val = nullptr;
static lv_obj_t *_lowest_val = nullptr;
static lv_obj_t *_closest_val = nullptr;

// Session stats -- per-location (see stats.cpp)
static lv_obj_t *_unique_val = nullptr;
static lv_obj_t *_peak_val = nullptr;

// Top airlines
static lv_obj_t *_airline_labels[5] = {};

// Top types
static lv_obj_t *_type_labels[5] = {};

// System health -- genuinely device-global, doesn't reset on location switch
static lv_obj_t *_uptime_val = nullptr;
static lv_obj_t *_heap_val = nullptr;
static lv_obj_t *_psram_val = nullptr;
static lv_obj_t *_temp_val = nullptr;
static lv_obj_t *_fps_val = nullptr;
static lv_obj_t *_tasks_val = nullptr;
static lv_obj_t *_lvgl_objs_val = nullptr;
static lv_obj_t *_flash_val = nullptr;

// Network stats
static lv_obj_t *_ip_val = nullptr;
static lv_obj_t *_fetch_val = nullptr;
static lv_obj_t *_bytes_val = nullptr;
static lv_obj_t *_latency_val = nullptr;
static lv_obj_t *_rssi_val = nullptr;

// Error log
static lv_obj_t *_err_count_lbl = nullptr;
static lv_obj_t *_err_list_lbl = nullptr;

// FPS measurement
static uint32_t _frame_count = 0;
static uint32_t _fps_last_time = 0;
static uint16_t _fps = 0;

#define BAR_MAX_W 160
#define BAR_H 8 // thinner bars, tightened row pitch below to match

// Uniform vertical rhythm for the whole screen -- every section (bars or
// plain text) now uses one of these two pitches instead of each section
// having picked its own spacing independently.
#define ROW_H 18      // line pitch for 14pt rows (text or bars)
#define ROW_H_WIDE 20 // line pitch for 16pt rows (center column distributions)
#define SECTION_GAP 14 // gap from a section's last row to the next header
#define COL_HEADER_GAP 40 // gap from a column's own header down to its first subsection header -- deliberately more breathing room than SECTION_GAP, since the column header is now larger (20pt) and needs to read as a clear step above the subsection headers below it, not just another row

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

static void create_bar_row(lv_obj_t *parent, BarRow *row, const char *name,
                           uint32_t color_hex, int x, int y,
                           const lv_font_t *font = &lv_font_montserrat_14,
                           int name_off = 42, int bar_off = 70) {
    lv_color_t color = lv_color_hex(color_hex);

    row->name_lbl = lv_label_create(parent);
    lv_label_set_text(row->name_lbl, name);
    lv_obj_set_style_text_font(row->name_lbl, font, 0);
    lv_obj_set_style_text_color(row->name_lbl, color, 0);
    lv_obj_set_pos(row->name_lbl, x, y + 1);
    lv_obj_clear_flag(row->name_lbl, LV_OBJ_FLAG_CLICKABLE);

    row->count_lbl = lv_label_create(parent);
    lv_label_set_text(row->count_lbl, "0");
    lv_obj_set_style_text_font(row->count_lbl, font, 0);
    lv_obj_set_style_text_color(row->count_lbl, lv_color_hex(0xccccdd), 0);
    lv_obj_set_pos(row->count_lbl, x + name_off, y + 1);
    lv_obj_clear_flag(row->count_lbl, LV_OBJ_FLAG_CLICKABLE);

    // +3 keeps the now-thinner bar vertically centered against the label's
    // text line instead of hugging its top.
    row->bar = create_bar(parent, x + bar_off, y + 3, color);
}

static void update_bar(BarRow *row, int count, int total) {
    lv_label_set_text_fmt(row->count_lbl, "%d", count);
    int w = (total > 0) ? (count * BAR_MAX_W / total) : 0;
    if (w < 2 && count > 0) w = 2;
    lv_obj_set_width(row->bar, w);
}

// Helper for an inline "HEADER  value" row -- header and value share one
// line (unlike create_stat_pair below, which stacks value under header).
static lv_obj_t *create_inline_row(lv_obj_t *parent, const char *header, int x, int y,
                                    lv_color_t val_color, int val_off) {
    lv_obj_t *h = lv_label_create(parent);
    lv_label_set_text(h, header);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(h, DIM_COLOR, 0);
    lv_obj_set_pos(h, x, y);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *v = lv_label_create(parent);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(v, val_color, 0);
    lv_obj_set_pos(v, x + val_off, y);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
    return v;
}

// Helper to create a label pair (header + value)
static lv_obj_t *create_stat_pair(lv_obj_t *parent, const char *header, int x, int y,
                                   lv_color_t val_color = lv_color_hex(0x44cc88)) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, header);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, DIM_COLOR, 0);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *val = lv_label_create(parent);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_font(val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(val, val_color, 0);
    lv_obj_set_pos(val, x, y + 16);
    lv_obj_clear_flag(val, LV_OBJ_FLAG_CLICKABLE);
    return val;
}

static int count_lvgl_objects(lv_obj_t *obj) {
    int n = 1;
    uint32_t cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cnt; i++) {
        n += count_lvgl_objects(lv_obj_get_child(obj, i));
    }
    return n;
}

static void refresh_stats(lv_timer_t *t) {
    // Deliberately not gated on "is Stats the active tab" -- UNIQUE/PEAK are
    // meant to describe the whole time a location has been selected, not
    // just however long you happen to have had the Stats screen open. Label
    // updates on an inactive tileview tile are cheap (nothing to redraw
    // until it's actually shown), so there's no real cost to always running.
    stats_update(_list);
    const SessionStats *s = stats_get();
    const FetcherStats *fs = fetcher_get_stats();

    lv_label_set_text_fmt(_traffic_total_lbl, "Total: %d", s->current_count);

    // Category bars
    int cat_counts[] = {s->jets, s->ga, s->heli, s->military, s->emergency};
    int cat_total = s->current_count > 0 ? s->current_count : 1;
    for (int i = 0; i < 5; i++) {
        update_bar(&_cat_rows[i], cat_counts[i], cat_total);
    }

    // Altitude bars (ground traffic excluded entirely -- see stats.cpp)
    int alt_counts[] = {s->alt_low, s->alt_med_low,
                        s->alt_med, s->alt_high, s->alt_very_high};
    int alt_max = 1;
    for (int i = 0; i < 5; i++) {
        if (alt_counts[i] > alt_max) alt_max = alt_counts[i];
    }
    for (int i = 0; i < 5; i++) {
        update_bar(&_alt_rows[i], alt_counts[i], alt_max);
    }

    // Speed bars (ground traffic excluded entirely -- see stats.cpp)
    int spd_counts[] = {s->spd_slow, s->spd_med,
                        s->spd_fast, s->spd_very_fast, s->spd_extreme};
    int spd_max = 1;
    for (int i = 0; i < 5; i++) {
        if (spd_counts[i] > spd_max) spd_max = spd_counts[i];
    }
    for (int i = 0; i < 5; i++) {
        update_bar(&_spd_rows[i], spd_counts[i], spd_max);
    }

    // Records
    if (s->fastest_callsign[0]) {
        lv_label_set_text_fmt(_fastest_val, "%s  %dkt", s->fastest_callsign, s->fastest_speed);
    } else {
        lv_label_set_text(_fastest_val, "--");
    }
    if (s->slowest_callsign[0] && s->slowest_speed < 99999) {
        lv_label_set_text_fmt(_slowest_val, "%s  %dkt", s->slowest_callsign, s->slowest_speed);
    } else {
        lv_label_set_text(_slowest_val, "--");
    }
    if (s->highest_callsign[0] && s->highest_alt > -9999) {
        if (s->highest_alt >= 18000) {
            lv_label_set_text_fmt(_highest_val, "%s  FL%d", s->highest_callsign, s->highest_alt / 100);
        } else {
            lv_label_set_text_fmt(_highest_val, "%s  %dft", s->highest_callsign, s->highest_alt);
        }
    } else {
        lv_label_set_text(_highest_val, "--");
    }
    if (s->lowest_callsign[0] && s->lowest_alt < 999999) {
        lv_label_set_text_fmt(_lowest_val, "%s  %dft", s->lowest_callsign, s->lowest_alt);
    } else {
        lv_label_set_text(_lowest_val, "--");
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

    // Top airlines
    for (int i = 0; i < 5; i++) {
        if (s->top_airlines[i].code[0]) {
            lv_label_set_text_fmt(_airline_labels[i], "%-3s %d", s->top_airlines[i].code, s->top_airlines[i].count);
        } else {
            lv_label_set_text(_airline_labels[i], "");
        }
    }

    // Top types
    for (int i = 0; i < 5; i++) {
        if (s->top_types[i].type[0]) {
            lv_label_set_text_fmt(_type_labels[i], "%-4s %d", s->top_types[i].type, s->top_types[i].count);
        } else {
            lv_label_set_text(_type_labels[i], "");
        }
    }

    // === SYSTEM HEALTH ===
    uint32_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    lv_label_set_text_fmt(_heap_val, "%luK / %luK min", (unsigned long)(heap_free / 1024), (unsigned long)(heap_min / 1024));
    lv_label_set_text_fmt(_psram_val, "%.1fM free", (double)psram_free / (1024.0 * 1024.0));

    // Temperature
    float temp = temperatureRead();
    if (temp > 0) {
        lv_color_t tc = temp > 70 ? lv_color_hex(0xff3333) : temp > 55 ? WARN_COLOR : SYS_COLOR;
        lv_obj_set_style_text_color(_temp_val, tc, 0);
        lv_label_set_text_fmt(_temp_val, "%.0fC", (double)temp);
    } else {
        lv_label_set_text(_temp_val, "N/A");
    }

    // FPS
    lv_label_set_text_fmt(_fps_val, "%d", _fps);

    // Tasks + LVGL objects
    lv_label_set_text_fmt(_tasks_val, "%lu", (unsigned long)uxTaskGetNumberOfTasks());

    static uint32_t last_obj_count_time = 0;
    static int cached_obj_count = 0;
    uint32_t now = millis();
    if (now - last_obj_count_time > 5000) {
        cached_obj_count = count_lvgl_objects(lv_screen_active());
        last_obj_count_time = now;
    }
    lv_label_set_text_fmt(_lvgl_objs_val, "%d", cached_obj_count);

    // Flash
    lv_label_set_text_fmt(_flash_val, "%.1f%%", 74.6); // static — compiled into binary

    // === NETWORK ===
    if (fs->ip_addr[0]) {
        lv_label_set_text(_ip_val, fs->ip_addr);
    }
    lv_label_set_text_fmt(_fetch_val, "%lu ok / %lu err", (unsigned long)fs->fetch_ok, (unsigned long)fs->fetch_fail);

    if (fs->bytes_received > 1048576) {
        lv_label_set_text_fmt(_bytes_val, "%.1fMB", (double)fs->bytes_received / 1048576.0);
    } else {
        lv_label_set_text_fmt(_bytes_val, "%luKB", (unsigned long)(fs->bytes_received / 1024));
    }
    if (fs->last_fetch_ms > 0) {
        lv_label_set_text_fmt(_latency_val, "%lums", (unsigned long)fs->last_fetch_ms);
    }

    // Link type + signal info
    NetType net = fetcher_connection_type();
    if (net == NET_ETHERNET) {
        lv_label_set_text(_rssi_val, "ETH 100M");
    } else if (net == NET_WIFI) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            lv_label_set_text_fmt(_rssi_val, "WiFi %d dBm", ap_info.rssi);
        } else {
            lv_label_set_text(_rssi_val, "WiFi --");
        }
    } else {
        lv_label_set_text(_rssi_val, "No link");
    }

    // === ERROR LOG ===
    // "ERRORS" itself is now a static section header (create_section_header
    // in stats_view_init()) -- this label is just the "(N)" count next to it.
    uint32_t err_total = error_log_total_count();
    lv_label_set_text_fmt(_err_count_lbl, "(%lu)", (unsigned long)err_total);

    ErrorSnapshot snap = error_log_snapshot();
    if (snap.count == 0) {
        lv_label_set_text(_err_list_lbl, "(none)");
    } else {
        static char err_buf[512];
        int pos = 0;
        uint32_t now_ms = millis();
        // Show newest first
        for (int i = snap.count - 1; i >= 0 && pos < (int)sizeof(err_buf) - 60; i--) {
            uint32_t age_s = (now_ms - snap.entries[i].timestamp) / 1000;
            int m = age_s / 60;
            int s = age_s % 60;
            pos += snprintf(err_buf + pos, sizeof(err_buf) - pos,
                "%dm%02ds %s\n", m, s, snap.entries[i].msg);
        }
        if (pos > 0) err_buf[pos - 1] = '\0'; // strip trailing newline
        lv_label_set_text(_err_list_lbl, err_buf);
    }
}

static void create_section_header(lv_obj_t *parent, const char *text, int x, int y) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, DIM_COLOR, 0);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
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
    lv_obj_clear_flag(_container, LV_OBJ_FLAG_SCROLL_CHAIN);
    views_attach_swipe(_container);

    // All three columns share one vertical rhythm: column header at y=8,
    // first subsection header at y=26, first subsection's rows starting at
    // y=44, SECTION_GAP(14) between subsections thereafter -- deliberately
    // identical structure in every column (one top-level header, several
    // evenly-spaced subsections under it) instead of the previous layout,
    // where the left column mixed a large unexplained number with
    // unlabeled bars while the other two columns used a header-plus-
    // subsections shape. No column-level caption text (e.g. "every ~2s")
    // either -- the header text itself ("CURRENT TRAFFIC" / "LOCATION" +
    // "since last switch" / "DEVICE") is the whole explanation, no large
    // hero number, no prose block anywhere on this screen.

    // ============================================================
    // LEFT COLUMN (x=15): "CURRENT TRAFFIC" -- everything here is recalculated
    // from scratch on every refresh_stats() tick (~2s) from whichever
    // aircraft are currently visible. Nothing in this column accumulates
    // over time or remembers anything from a previous tick -- that's the
    // "LOCATION" column (center) below, kept deliberately separate.
    // ============================================================
    int lx = 15;

    lv_obj_t *now_header = lv_label_create(_container);
    lv_label_set_text(now_header, "CURRENT TRAFFIC");
    lv_obj_set_style_text_font(now_header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(now_header, ACCENT_COLOR, 0);
    lv_obj_set_pos(now_header, lx, 8);
    lv_obj_clear_flag(now_header, LV_OBJ_FLAG_CLICKABLE);

    // "Total: N" caption, same position/style/purpose as LOCATION's "since
    // last switch" caption below -- lines the two column headers up with
    // matching header-plus-caption shapes instead of one having a caption
    // and the other just floating alone above TYPE.
    _traffic_total_lbl = lv_label_create(_container);
    lv_label_set_text(_traffic_total_lbl, "Total: 0");
    lv_obj_set_style_text_font(_traffic_total_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_traffic_total_lbl, DIM_COLOR, 0);
    lv_obj_set_pos(_traffic_total_lbl, lx, 32);
    lv_obj_clear_flag(_traffic_total_lbl, LV_OBJ_FLAG_CLICKABLE);

    // Category breakdown -- type_y matches LOCATION's rc_y (COL_HEADER_GAP +
    // ROW_H, not just COL_HEADER_GAP) so TYPE and RECORDS line up now that
    // both columns have a caption line under their header.
    int type_y = 8 + COL_HEADER_GAP + ROW_H;
    create_section_header(_container, "TYPE", lx, type_y);
    for (int i = 0; i < 5; i++) {
        // EMRG's wide "M" runs into the count digit at the default 42px
        // offset even though HELI/JETS (same 4 chars) don't -- give it a
        // few extra px.
        int name_off = (i == 4) ? 48 : 42;
        create_bar_row(_container, &_cat_rows[i], CAT_NAMES[i], CAT_COLORS[i],
                       lx, type_y + ROW_H + i * ROW_H, &lv_font_montserrat_14, name_off);
    }

    // Altitude distribution
    int alt_y = type_y + 6 * ROW_H + SECTION_GAP;
    create_section_header(_container, "ALTITUDE", lx, alt_y);
    for (int i = 0; i < 5; i++) {
        create_bar_row(_container, &_alt_rows[i], ALT_NAMES[i],
                       lv_color_to_u32(altitude_color(ALT_SAMPLES[i])),
                       lx, alt_y + ROW_H_WIDE + i * ROW_H_WIDE, &lv_font_montserrat_16, 52, 88);
    }

    // Speed distribution
    int spd_y = alt_y + 6 * ROW_H_WIDE + SECTION_GAP;
    create_section_header(_container, "SPEED", lx, spd_y);
    for (int i = 0; i < 5; i++) {
        create_bar_row(_container, &_spd_rows[i], SPD_NAMES[i], SPD_COLORS[i],
                       lx, spd_y + ROW_H_WIDE + i * ROW_H_WIDE, &lv_font_montserrat_16, 52, 88);
    }

    // ============================================================
    // CENTER COLUMN (x=340): "LOCATION (since last switch)" -- everything
    // here is accumulated since the active location was last switched (see
    // stats.cpp: reset happens once, the moment locations_active_index()
    // changes -- leaving location A and arriving at location B are the same
    // event, not two separate resets). RECORDS in particular changed
    // meaning from earlier versions of this screen: FASTEST/SLOWEST/
    // HIGHEST/LOWEST/CLOSEST used to be recalculated from scratch every
    // tick just like the CURRENT TRAFFIC column, which read as "session records"
    // but was actually "whatever's true this instant" -- misleading, and
    // part of why this screen was reorganized. They are now genuine running
    // extremes: the most extreme value seen since this location became
    // active, same as PEAK already was.
    // ============================================================
    int cx = 340;

    lv_obj_t *loc_header = lv_label_create(_container);
    lv_label_set_text(loc_header, "LOCATION");
    lv_obj_set_style_text_font(loc_header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(loc_header, SESSION_COLOR, 0);
    lv_obj_set_pos(loc_header, cx, 8);
    lv_obj_clear_flag(loc_header, LV_OBJ_FLAG_CLICKABLE);

    // "(since last switch)" split onto its own smaller line rather than
    // appended to the big 20pt header -- at that size the full phrase runs
    // long enough to risk crowding into the DEVICE column's own header.
    lv_obj_t *loc_caption = lv_label_create(_container);
    lv_label_set_text(loc_caption, "since last switch");
    lv_obj_set_style_text_font(loc_caption, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(loc_caption, DIM_COLOR, 0);
    lv_obj_set_pos(loc_caption, cx, 32);
    lv_obj_clear_flag(loc_caption, LV_OBJ_FLAG_CLICKABLE);

    // Records — compact rows with inline header + value
    int rc_y = 8 + COL_HEADER_GAP + ROW_H; // +ROW_H to clear the caption line above
    create_section_header(_container, "RECORDS", cx, rc_y);

    lv_color_t rec_hdr = DIM_COLOR;
    lv_color_t rec_val = lv_color_hex(0xccccdd);
    int rr = rc_y + ROW_H; // first row
    int rh = ROW_H;        // row height

    auto make_rec_row = [&](const char *hdr, int y) -> lv_obj_t* {
        lv_obj_t *h = lv_label_create(_container);
        lv_label_set_text(h, hdr);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(h, rec_hdr, 0);
        lv_obj_set_pos(h, cx, y);
        lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *v = lv_label_create(_container);
        lv_label_set_text(v, "--");
        lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(v, rec_val, 0);
        // 80px, not 68 -- "SLOWEST" (widest header, thanks to the W) was
        // running right up against the value column at the old offset.
        lv_obj_set_pos(v, cx + 80, y);
        lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
        return v;
    };

    _fastest_val = make_rec_row("FASTEST", rr);
    lv_obj_set_style_text_color(_fastest_val, lv_color_hex(0xff66cc), 0);
    _slowest_val = make_rec_row("SLOWEST", rr + rh);
    lv_obj_set_style_text_color(_slowest_val, lv_color_hex(0x66aaff), 0);
    _highest_val = make_rec_row("HIGHEST", rr + rh * 2);
    lv_obj_set_style_text_color(_highest_val, altitude_color(45000), 0); // high end of the altitude gradient
    _lowest_val  = make_rec_row("LOWEST",  rr + rh * 3);
    lv_obj_set_style_text_color(_lowest_val, altitude_color(1000), 0); // low end of the altitude gradient
    _closest_val = make_rec_row("CLOSEST", rr + rh * 4);
    lv_obj_set_style_text_color(_closest_val, lv_color_hex(0x44ddaa), 0);

    // AIRCRAFT SEEN -- UPTIME deliberately still lives in DEVICE (right
    // column), not here, despite looking like a natural fit -- it does not
    // reset when you switch locations (genuinely device-global), so
    // grouping it with UNIQUE/PEAK would misrepresent it as location-scoped.
    int ss_y = rr + rh * 5 + SECTION_GAP;
    create_section_header(_container, "AIRCRAFT SEEN", cx, ss_y);
    // Stacked vertically (one inline "HEADER  value" row per stat), matching
    // RECORDS above, rather than side by side. SESSION_COLOR (not
    // ACCENT_COLOR) -- these are LOCATION data, not CURRENT TRAFFIC data.
    _unique_val = create_inline_row(_container, "UNIQUE", cx, ss_y + ROW_H, SESSION_COLOR, 80);
    _peak_val = create_inline_row(_container, "PEAK", cx, ss_y + ROW_H * 2, SESSION_COLOR, 80);

    // Top airlines -- AIRCRAFT SEEN is 2 rows deep (ROW_H * 2) plus its own
    // header pitch (ROW_H), then the usual gap before the next header.
    int al_y = ss_y + ROW_H * 3 + SECTION_GAP;
    create_section_header(_container, "AIRLINES SEEN", cx, al_y);
    for (int i = 0; i < 5; i++) {
        _airline_labels[i] = lv_label_create(_container);
        lv_label_set_text(_airline_labels[i], "");
        lv_obj_set_style_text_font(_airline_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(_airline_labels[i], lv_color_hex(0xccccdd), 0);
        lv_obj_set_pos(_airline_labels[i], cx + (i % 3) * 80, al_y + ROW_H + (i / 3) * ROW_H);
        lv_obj_clear_flag(_airline_labels[i], LV_OBJ_FLAG_CLICKABLE);
    }

    // Top aircraft types -- same reasoning as above (2 rows of airlines)
    int ty_y = al_y + ROW_H * 3 + SECTION_GAP;
    create_section_header(_container, "TYPES SEEN", cx, ty_y);
    for (int i = 0; i < 5; i++) {
        _type_labels[i] = lv_label_create(_container);
        lv_label_set_text(_type_labels[i], "");
        lv_obj_set_style_text_font(_type_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(_type_labels[i], lv_color_hex(0xccccdd), 0);
        lv_obj_set_pos(_type_labels[i], cx + (i % 3) * 80, ty_y + ROW_H + (i / 3) * ROW_H);
        lv_obj_clear_flag(_type_labels[i], LV_OBJ_FLAG_CLICKABLE);
    }

    // ============================================================
    // RIGHT COLUMN (x=700): "DEVICE" -- genuinely global, none of it resets
    // on a location switch. SYS_COLOR (green) doubles as this column's
    // header color, same color already used for every NETWORK/SYSTEM value
    // below it.
    // ============================================================
    int rx = 700;

    lv_obj_t *device_header = lv_label_create(_container);
    lv_label_set_text(device_header, "DEVICE");
    lv_obj_set_style_text_font(device_header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(device_header, SYS_COLOR, 0);
    lv_obj_set_pos(device_header, rx, 8);
    lv_obj_clear_flag(device_header, LV_OBJ_FLAG_CLICKABLE);

    // Plenty of width in this column for "HEADER  value" on one line --
    // no need to stack the value under the header like SYSTEM below does.
    int net_y = 8 + COL_HEADER_GAP;
    create_section_header(_container, "NETWORK", rx, net_y);
    _ip_val = create_inline_row(_container, "IP", rx, net_y + ROW_H, SYS_COLOR, 90);
    _rssi_val = create_inline_row(_container, "LINK", rx, net_y + ROW_H * 2, SYS_COLOR, 90);
    _fetch_val = create_inline_row(_container, "FETCHES", rx, net_y + ROW_H * 3, SYS_COLOR, 90);
    _bytes_val = create_inline_row(_container, "RX DATA", rx, net_y + ROW_H * 4, SYS_COLOR, 90);
    _latency_val = create_inline_row(_container, "LATENCY", rx, net_y + ROW_H * 5, SYS_COLOR, 90);

    int sy = net_y + 6 * ROW_H + SECTION_GAP;
    create_section_header(_container, "SYSTEM", rx, sy);

    _heap_val = create_stat_pair(_container, "HEAP", rx, sy + 18, SYS_COLOR);
    _uptime_val = create_stat_pair(_container, "UPTIME", rx + 120, sy + 18, SYS_COLOR);
    _psram_val = create_stat_pair(_container, "PSRAM", rx, sy + 52, SYS_COLOR);

    // Compact row: TEMP / FPS / TASKS / OBJS / FLASH
    int sr2 = sy + 86;
    _temp_val = create_stat_pair(_container, "TEMP", rx, sr2, SYS_COLOR);
    _fps_val = create_stat_pair(_container, "FPS", rx + 60, sr2, SYS_COLOR);
    _tasks_val = create_stat_pair(_container, "TASKS", rx + 110, sr2, SYS_COLOR);
    _lvgl_objs_val = create_stat_pair(_container, "LVGL", rx + 170, sr2, SYS_COLOR);
    _flash_val = create_stat_pair(_container, "FLASH", rx + 230, sr2, SYS_COLOR);

    // Error log section -- 32 is the 2-line stat-pair block height (TEMP row)
    int ey = sr2 + 32 + SECTION_GAP;
    create_section_header(_container, "ERRORS", rx, ey);
    _err_count_lbl = lv_label_create(_container);
    lv_label_set_text(_err_count_lbl, "(0)");
    lv_obj_set_style_text_font(_err_count_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_err_count_lbl, DIM_COLOR, 0);
    lv_obj_set_pos(_err_count_lbl, rx + 70, ey);
    lv_obj_clear_flag(_err_count_lbl, LV_OBJ_FLAG_CLICKABLE);

    // CLR button
    lv_obj_t *clr_btn = lv_obj_create(_container);
    lv_obj_set_size(clr_btn, 40, 22);
    lv_obj_set_pos(clr_btn, rx + 120, ey - 2);
    lv_obj_set_style_bg_color(clr_btn, lv_color_hex(0x1a1a2a), 0);
    lv_obj_set_style_bg_opa(clr_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(clr_btn, lv_color_hex(0x444466), 0);
    lv_obj_set_style_border_width(clr_btn, 1, 0);
    lv_obj_set_style_radius(clr_btn, 4, 0);
    lv_obj_set_style_pad_all(clr_btn, 0, 0);
    lv_obj_clear_flag(clr_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(clr_btn, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(clr_btn, [](lv_event_t *e) {
        error_log_clear();
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *clr_lbl = lv_label_create(clr_btn);
    lv_label_set_text(clr_lbl, "CLR");
    lv_obj_set_style_text_font(clr_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(clr_lbl, lv_color_hex(0xff6666), 0);
    lv_obj_center(clr_lbl);

    // Error list label
    _err_list_lbl = lv_label_create(_container);
    lv_label_set_text(_err_list_lbl, "(none)");
    lv_obj_set_style_text_font(_err_list_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_err_list_lbl, lv_color_hex(0xff6666), 0);
    lv_obj_set_pos(_err_list_lbl, rx, ey + ROW_H);
    lv_obj_set_width(_err_list_lbl, 310);
    lv_obj_clear_flag(_err_list_lbl, LV_OBJ_FLAG_CLICKABLE);

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
    }, 33, nullptr);

    // Refresh timer -- stats_update() just reads whatever is currently in
    // the live aircraft list, so polling it more often than the ~20s fetch
    // cadence doesn't "recount stale data" (the reasoning this used to be
    // pinned to the fetch interval for); it just redraws the same numbers
    // an extra few times between fetches, which is cheap. The real cost of
    // the old 20s interval showed up right after a location switch: the
    // aircraft-list clear (fetcher.cpp's fetcher_request_immediate_fetch())
    // and the fresh fetch landing could both complete within a couple of
    // seconds, but the visible reset/repopulation of UNIQUE/PEAK/CLOSEST
    // stayed invisible for up to 20s until this timer's next tick happened
    // to fire -- reported as "stats do update but it takes a while."
    lv_timer_create(refresh_stats, 2000, nullptr);
}

void stats_view_update() {
    // triggered externally if needed
}
