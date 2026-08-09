#include "../platform/platform.h" // millis()/strlcpy compatibility shims on non-Arduino builds
#include "stats_view.h"
#include "stats.h"
#include "views.h"
#include "status_bar.h"
#include "../pins_config.h"
#include "../data/locations.h"
#include "../data/metar.h"
#include "../data/atis.h"
#include "geo.h" // altitude_color()
#include <cstdio>

#define STATS_W LCD_H_RES
#define STATS_H (LCD_V_RES - STATUS_BAR_HEIGHT)
#define BG_COLOR lv_color_hex(0x0a0a1a)
#define DIM_COLOR lv_color_hex(0x9999bb)
#define ACCENT_COLOR lv_color_hex(0x4488ff)
#define SESSION_COLOR lv_color_hex(0xaa88ff)
#define TEXT_COLOR lv_color_hex(0xccccdd)

static AircraftList *_list = nullptr;
static lv_obj_t *_container = nullptr;
static lv_obj_t *_traffic_total_lbl = nullptr;
static lv_obj_t *_metar_lbl = nullptr;
static lv_obj_t *_atis_combined_lbl = nullptr;
static lv_obj_t *_atis_arr_hdr = nullptr;
static lv_obj_t *_atis_arr_lbl = nullptr;
static lv_obj_t *_atis_dep_hdr = nullptr;
static lv_obj_t *_atis_dep_lbl = nullptr;
static lv_obj_t *_atis_status_lbl = nullptr;
static lv_obj_t *_atis_scroll = nullptr;

struct BarRow {
    lv_obj_t *name_lbl;
    lv_obj_t *count_lbl;
    lv_obj_t *bar;
};

static BarRow _cat_rows[5];
static const char *CAT_NAMES[] = {"JETS", "GA", "HELI", "MIL", "EMRG"};
static const uint32_t CAT_COLORS[] = {0x4488ff, 0x88aacc, 0x44ddaa, 0xffaa00, 0xff3333};

static BarRow _alt_rows[5];
static const char *ALT_NAMES[] = {"<5k", "<15k", "<25k", "<35k", "35k+"};
static const int32_t ALT_SAMPLES[] = {2500, 10000, 20000, 30000, 45000};

static BarRow _spd_rows[5];
static const char *SPD_NAMES[] = {"<200", "<300", "<400", "<500", "500+"};
static const uint32_t SPD_COLORS[] = {0x4488cc, 0x4488ff, 0x8844ff, 0xcc44ff, 0xff44aa};

static lv_obj_t *_fastest_val = nullptr;
static lv_obj_t *_slowest_val = nullptr;
static lv_obj_t *_highest_val = nullptr;
static lv_obj_t *_lowest_val = nullptr;
static lv_obj_t *_closest_val = nullptr;
static lv_obj_t *_unique_val = nullptr;
static lv_obj_t *_peak_val = nullptr;
static lv_obj_t *_airline_labels[5] = {};
static lv_obj_t *_type_labels[5] = {};

#if LCD_H_RES >= 1280
#define BAR_MAX_W 200
#define ROW_H 16
#define ROW_H_WIDE 16
#define SECTION_GAP 8
#define COL_HEADER_GAP 28
#else
#define BAR_MAX_W 220
#define ROW_H 18
#define ROW_H_WIDE 20
#define SECTION_GAP 14
#define COL_HEADER_GAP 40
#endif

static lv_obj_t *create_bar(lv_obj_t *parent, int x, int y, lv_color_t color) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 0, 8);
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
    lv_obj_set_style_text_color(row->count_lbl, TEXT_COLOR, 0);
    lv_obj_set_pos(row->count_lbl, x + name_off, y + 1);
    lv_obj_clear_flag(row->count_lbl, LV_OBJ_FLAG_CLICKABLE);

    row->bar = create_bar(parent, x + bar_off, y + 3, color);
}

static void update_bar(BarRow *row, int count, int total) {
    lv_label_set_text_fmt(row->count_lbl, "%d", count);
    int w = (total > 0) ? (count * BAR_MAX_W / total) : 0;
    if (w < 2 && count > 0) w = 2;
    lv_obj_set_width(row->bar, w);
}

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

static void create_section_header(lv_obj_t *parent, const char *text, int x, int y) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, DIM_COLOR, 0);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

static void set_atis_visibility(bool split, bool show_status) {
    if (_atis_status_lbl) {
        if (show_status) lv_obj_clear_flag(_atis_status_lbl, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(_atis_status_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    if (_atis_combined_lbl) {
        if (!split && !show_status) lv_obj_clear_flag(_atis_combined_lbl, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(_atis_combined_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    auto set_pair = [](lv_obj_t *hdr, lv_obj_t *lbl, bool on) {
        if (!hdr || !lbl) return;
        if (on) {
            lv_obj_clear_flag(hdr, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(hdr, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        }
    };
    set_pair(_atis_arr_hdr, _atis_arr_lbl, split && !show_status);
    set_pair(_atis_dep_hdr, _atis_dep_lbl, split && !show_status);
}

static void layout_atis_split_labels() {
    if (!_atis_arr_lbl || !_atis_dep_hdr || !_atis_dep_lbl || !_atis_scroll) return;
    lv_obj_update_layout(_atis_arr_lbl);
    int arr_h = lv_obj_get_height(_atis_arr_lbl);
    int dep_y = 20 + arr_h + 12;
    lv_obj_set_pos(_atis_dep_hdr, 0, dep_y);
    lv_obj_set_pos(_atis_dep_lbl, 0, dep_y + 20);
}

static void refresh_stats(lv_timer_t *t) {
    (void)t;
    stats_update(_list);
    const SessionStats *s = stats_get();

    static int _wx_last_loc = -2;
    int wx_loc = locations_active_index();
    if (wx_loc != _wx_last_loc) {
        _wx_last_loc = wx_loc;
        if (_metar_lbl) lv_label_set_text(_metar_lbl, "");
        set_atis_visibility(false, true);
        if (_atis_status_lbl) lv_label_set_text(_atis_status_lbl, "");
    } else if (_metar_lbl) {
        switch (metar_status) {
            case METAR_OK:
                lv_label_set_text(_metar_lbl, metar_raw);
                lv_obj_set_style_text_color(_metar_lbl, TEXT_COLOR, 0);
                break;
            case METAR_NO_STATION:
                lv_label_set_text(_metar_lbl, "No stations within 50nm");
                lv_obj_set_style_text_color(_metar_lbl, DIM_COLOR, 0);
                break;
            case METAR_IDLE:
                lv_label_set_text(_metar_lbl, "");
                break;
            case METAR_FETCHING:
            case METAR_ERROR:
                break;
        }

        if (_atis_status_lbl || _atis_combined_lbl) {
            switch (atis_status) {
                case ATIS_OK:
                    if (atis_split) {
                        set_atis_visibility(true, false);
                        if (_atis_arr_lbl)
                            lv_label_set_text(_atis_arr_lbl, atis_arr[0] ? atis_arr : "(none)");
                        if (_atis_dep_lbl)
                            lv_label_set_text(_atis_dep_lbl, atis_dep[0] ? atis_dep : "(none)");
                        layout_atis_split_labels();
                    } else {
                        set_atis_visibility(false, false);
                        if (_atis_combined_lbl)
                            lv_label_set_text(_atis_combined_lbl, atis_combined);
                    }
                    break;
                case ATIS_UNAVAILABLE:
                    set_atis_visibility(false, true);
                    if (_atis_status_lbl) {
                        if (atis_airport[0]) {
                            char buf[96];
                            snprintf(buf, sizeof(buf),
                                     "D-ATIS not available for %s\n(US major airports only)",
                                     atis_airport);
                            lv_label_set_text(_atis_status_lbl, buf);
                        } else {
                            lv_label_set_text(_atis_status_lbl,
                                              "D-ATIS not available\n(US major airports only)");
                        }
                        lv_obj_set_style_text_color(_atis_status_lbl, DIM_COLOR, 0);
                    }
                    break;
                case ATIS_IDLE:
                    set_atis_visibility(false, true);
                    if (_atis_status_lbl) lv_label_set_text(_atis_status_lbl, "");
                    break;
                case ATIS_FETCHING:
                    set_atis_visibility(false, true);
                    if (_atis_status_lbl) {
                        lv_label_set_text(_atis_status_lbl, "Fetching D-ATIS...");
                        lv_obj_set_style_text_color(_atis_status_lbl, ACCENT_COLOR, 0);
                    }
                    break;
                case ATIS_ERROR:
                    break;
            }
        }
    }

    lv_label_set_text_fmt(_traffic_total_lbl, "Total: %d", s->current_count);

    int cat_counts[] = {s->jets, s->ga, s->heli, s->military, s->emergency};
    int cat_total = s->current_count > 0 ? s->current_count : 1;
    for (int i = 0; i < 5; i++) update_bar(&_cat_rows[i], cat_counts[i], cat_total);

    int alt_counts[] = {s->alt_low, s->alt_med_low, s->alt_med, s->alt_high, s->alt_very_high};
    int alt_max = 1;
    for (int i = 0; i < 5; i++) if (alt_counts[i] > alt_max) alt_max = alt_counts[i];
    for (int i = 0; i < 5; i++) update_bar(&_alt_rows[i], alt_counts[i], alt_max);

    int spd_counts[] = {s->spd_slow, s->spd_med, s->spd_fast, s->spd_very_fast, s->spd_extreme};
    int spd_max = 1;
    for (int i = 0; i < 5; i++) if (spd_counts[i] > spd_max) spd_max = spd_counts[i];
    for (int i = 0; i < 5; i++) update_bar(&_spd_rows[i], spd_counts[i], spd_max);

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

    lv_label_set_text_fmt(_unique_val, "%d", s->unique_seen);
    lv_label_set_text_fmt(_peak_val, "%d", s->peak_count);

    for (int i = 0; i < 5; i++) {
        if (s->top_airlines[i].code[0]) {
            lv_label_set_text_fmt(_airline_labels[i], "%-3s %d", s->top_airlines[i].code, s->top_airlines[i].count);
        } else {
            lv_label_set_text(_airline_labels[i], "");
        }
    }
    for (int i = 0; i < 5; i++) {
        if (s->top_types[i].type[0]) {
            lv_label_set_text_fmt(_type_labels[i], "%-4s %d", s->top_types[i].type, s->top_types[i].count);
        } else {
            lv_label_set_text(_type_labels[i], "");
        }
    }
}

static void build_traffic_column(lv_obj_t *parent, int lx, int top_y) {
    lv_obj_t *now_header = lv_label_create(parent);
    lv_label_set_text(now_header, "CURRENT TRAFFIC");
    lv_obj_set_style_text_font(now_header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(now_header, ACCENT_COLOR, 0);
    lv_obj_set_pos(now_header, lx, top_y);
    lv_obj_clear_flag(now_header, LV_OBJ_FLAG_CLICKABLE);

    _traffic_total_lbl = lv_label_create(parent);
    lv_label_set_text(_traffic_total_lbl, "Total: 0");
    lv_obj_set_style_text_font(_traffic_total_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_traffic_total_lbl, DIM_COLOR, 0);
    lv_obj_set_pos(_traffic_total_lbl, lx, top_y + 22);
    lv_obj_clear_flag(_traffic_total_lbl, LV_OBJ_FLAG_CLICKABLE);

    int type_y = top_y + COL_HEADER_GAP + ROW_H;
    create_section_header(parent, "TYPE", lx, type_y);
    for (int i = 0; i < 5; i++) {
        int name_off = (i == 4) ? 48 : 42;
        create_bar_row(parent, &_cat_rows[i], CAT_NAMES[i], CAT_COLORS[i],
                       lx, type_y + ROW_H + i * ROW_H, &lv_font_montserrat_14, name_off);
    }

    int alt_y = type_y + 6 * ROW_H + SECTION_GAP;
    create_section_header(parent, "ALTITUDE", lx, alt_y);
    for (int i = 0; i < 5; i++) {
        create_bar_row(parent, &_alt_rows[i], ALT_NAMES[i],
                       lv_color_to_u32(altitude_color(ALT_SAMPLES[i])),
                       lx, alt_y + ROW_H_WIDE + i * ROW_H_WIDE, &lv_font_montserrat_14, 52, 88);
    }

    int spd_y = alt_y + 6 * ROW_H_WIDE + SECTION_GAP;
    create_section_header(parent, "SPEED", lx, spd_y);
    for (int i = 0; i < 5; i++) {
        create_bar_row(parent, &_spd_rows[i], SPD_NAMES[i], SPD_COLORS[i],
                       lx, spd_y + ROW_H_WIDE + i * ROW_H_WIDE, &lv_font_montserrat_14, 52, 88);
    }
}

static void build_location_column(lv_obj_t *parent, int cx, int top_y) {
    lv_obj_t *loc_header = lv_label_create(parent);
    lv_label_set_text(loc_header, "LOCATION");
    lv_obj_set_style_text_font(loc_header, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(loc_header, SESSION_COLOR, 0);
    lv_obj_set_pos(loc_header, cx, top_y);
    lv_obj_clear_flag(loc_header, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *loc_caption = lv_label_create(parent);
    lv_label_set_text(loc_caption, "since last switch");
    lv_obj_set_style_text_font(loc_caption, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(loc_caption, DIM_COLOR, 0);
    lv_obj_set_pos(loc_caption, cx, top_y + 22);
    lv_obj_clear_flag(loc_caption, LV_OBJ_FLAG_CLICKABLE);

    int rc_y = top_y + COL_HEADER_GAP + ROW_H;
    create_section_header(parent, "RECORDS", cx, rc_y);

    auto make_rec_row = [&](const char *hdr, int y) -> lv_obj_t * {
        lv_obj_t *h = lv_label_create(parent);
        lv_label_set_text(h, hdr);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(h, DIM_COLOR, 0);
        lv_obj_set_pos(h, cx, y);
        lv_obj_clear_flag(h, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *v = lv_label_create(parent);
        lv_label_set_text(v, "--");
        lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(v, TEXT_COLOR, 0);
        lv_obj_set_pos(v, cx + 100, y);
        lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
        return v;
    };

    int rr = rc_y + ROW_H;
    _fastest_val = make_rec_row("FASTEST", rr);
    lv_obj_set_style_text_color(_fastest_val, lv_color_hex(0xff66cc), 0);
    _slowest_val = make_rec_row("SLOWEST", rr + ROW_H);
    lv_obj_set_style_text_color(_slowest_val, lv_color_hex(0x66aaff), 0);
    _highest_val = make_rec_row("HIGHEST", rr + ROW_H * 2);
    lv_obj_set_style_text_color(_highest_val, altitude_color(45000), 0);
    _lowest_val = make_rec_row("LOWEST", rr + ROW_H * 3);
    lv_obj_set_style_text_color(_lowest_val, altitude_color(1000), 0);
    _closest_val = make_rec_row("CLOSEST", rr + ROW_H * 4);
    lv_obj_set_style_text_color(_closest_val, lv_color_hex(0x44ddaa), 0);

    int ss_y = rr + ROW_H * 5 + SECTION_GAP;
    create_section_header(parent, "AIRCRAFT SEEN", cx, ss_y);
    _unique_val = create_inline_row(parent, "UNIQUE", cx, ss_y + ROW_H, SESSION_COLOR, 100);
    _peak_val = create_inline_row(parent, "PEAK", cx, ss_y + ROW_H * 2, SESSION_COLOR, 100);

    int al_y = ss_y + ROW_H * 3 + SECTION_GAP;
    create_section_header(parent, "AIRLINES SEEN", cx, al_y);
    for (int i = 0; i < 5; i++) {
        _airline_labels[i] = lv_label_create(parent);
        lv_label_set_text(_airline_labels[i], "");
        lv_obj_set_style_text_font(_airline_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(_airline_labels[i], TEXT_COLOR, 0);
        lv_obj_set_pos(_airline_labels[i], cx + (i % 3) * 100, al_y + ROW_H + (i / 3) * ROW_H);
        lv_obj_clear_flag(_airline_labels[i], LV_OBJ_FLAG_CLICKABLE);
    }

    int ty_y = al_y + ROW_H * 3 + SECTION_GAP;
    create_section_header(parent, "TYPES SEEN", cx, ty_y);
    for (int i = 0; i < 5; i++) {
        _type_labels[i] = lv_label_create(parent);
        lv_label_set_text(_type_labels[i], "");
        lv_obj_set_style_text_font(_type_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(_type_labels[i], TEXT_COLOR, 0);
        lv_obj_set_pos(_type_labels[i], cx + (i % 3) * 100, ty_y + ROW_H + (i / 3) * ROW_H);
        lv_obj_clear_flag(_type_labels[i], LV_OBJ_FLAG_CLICKABLE);
    }
}

static lv_obj_t *make_wrap_label(lv_obj_t *parent, int w) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, TEXT_COLOR, 0);
    lv_obj_set_width(lbl, w);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

#if LCD_H_RES >= 1280
static void build_pi_quadrants() {
    const int half_w = STATS_W / 2;
    const int half_h = STATS_H / 2;
    const int pad = 20;
    const int col_w = half_w - pad * 2;

    build_traffic_column(_container, pad, 12);
    build_location_column(_container, pad, half_h + 8);

    int rx = half_w + pad;
    lv_obj_t *metar_hdr = lv_label_create(_container);
    lv_label_set_text(metar_hdr, "METAR");
    lv_obj_set_style_text_font(metar_hdr, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(metar_hdr, ACCENT_COLOR, 0);
    lv_obj_set_pos(metar_hdr, rx, 12);
    lv_obj_clear_flag(metar_hdr, LV_OBJ_FLAG_CLICKABLE);

    _metar_lbl = make_wrap_label(_container, col_w);
    lv_obj_set_pos(_metar_lbl, rx, 44);
    lv_obj_set_style_text_font(_metar_lbl, &lv_font_montserrat_16, 0);

    lv_obj_t *atis_hdr = lv_label_create(_container);
    lv_label_set_text(atis_hdr, "ATIS");
    lv_obj_set_style_text_font(atis_hdr, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(atis_hdr, ACCENT_COLOR, 0);
    lv_obj_set_pos(atis_hdr, rx, half_h + 8);
    lv_obj_clear_flag(atis_hdr, LV_OBJ_FLAG_CLICKABLE);

    int scroll_y = half_h + 40;
    int scroll_h = STATS_H - scroll_y - 12;
    _atis_scroll = lv_obj_create(_container);
    lv_obj_set_size(_atis_scroll, col_w, scroll_h);
    lv_obj_set_pos(_atis_scroll, rx, scroll_y);
    lv_obj_set_style_bg_opa(_atis_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_atis_scroll, 0, 0);
    lv_obj_set_style_pad_all(_atis_scroll, 0, 0);
    lv_obj_set_scroll_dir(_atis_scroll, LV_DIR_VER);
    lv_obj_add_flag(_atis_scroll, LV_OBJ_FLAG_SCROLLABLE);

    _atis_status_lbl = make_wrap_label(_atis_scroll, col_w);
    lv_obj_set_pos(_atis_status_lbl, 0, 0);

    _atis_combined_lbl = make_wrap_label(_atis_scroll, col_w);
    lv_obj_set_pos(_atis_combined_lbl, 0, 0);
    lv_obj_add_flag(_atis_combined_lbl, LV_OBJ_FLAG_HIDDEN);

    _atis_arr_hdr = lv_label_create(_atis_scroll);
    lv_label_set_text(_atis_arr_hdr, "ARRIVAL");
    lv_obj_set_style_text_font(_atis_arr_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_atis_arr_hdr, DIM_COLOR, 0);
    lv_obj_set_pos(_atis_arr_hdr, 0, 0);
    lv_obj_add_flag(_atis_arr_hdr, LV_OBJ_FLAG_HIDDEN);

    _atis_arr_lbl = make_wrap_label(_atis_scroll, col_w);
    lv_obj_set_pos(_atis_arr_lbl, 0, 20);
    lv_obj_add_flag(_atis_arr_lbl, LV_OBJ_FLAG_HIDDEN);

    _atis_dep_hdr = lv_label_create(_atis_scroll);
    lv_label_set_text(_atis_dep_hdr, "DEPARTURE");
    lv_obj_set_style_text_font(_atis_dep_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_atis_dep_hdr, DIM_COLOR, 0);
    lv_obj_set_pos(_atis_dep_hdr, 0, 0);
    lv_obj_add_flag(_atis_dep_hdr, LV_OBJ_FLAG_HIDDEN);

    _atis_dep_lbl = make_wrap_label(_atis_scroll, col_w);
    lv_obj_set_pos(_atis_dep_lbl, 0, 0);
    lv_obj_add_flag(_atis_dep_lbl, LV_OBJ_FLAG_HIDDEN);
}
#endif

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

#if LCD_H_RES >= 1280
    build_pi_quadrants();
#else
    _metar_lbl = lv_label_create(_container);
    lv_label_set_text(_metar_lbl, "");
    lv_obj_set_style_text_font(_metar_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_metar_lbl, DIM_COLOR, 0);
    lv_obj_set_style_text_align(_metar_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(_metar_lbl, STATS_W - 80);
    lv_obj_set_pos(_metar_lbl, 40, 40);
    lv_obj_clear_flag(_metar_lbl, LV_OBJ_FLAG_CLICKABLE);

    int top_y = 88;
    build_traffic_column(_container, 40, top_y);
    build_location_column(_container, 540, top_y);
#endif

    lv_timer_create(refresh_stats, 2000, nullptr);
}

void stats_view_update() {
}
