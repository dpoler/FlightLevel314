#include <Arduino.h>
#include "map_view.h"
#include "detail_card.h"
#include "views.h"
// #include "tile_cache.h" // disabled: tiles broken on ESP32-P4
#include "../config.h"
#include "../pins_config.h"

static AircraftList *_list = nullptr;
static lv_obj_t *_canvas = nullptr;
static MapProjection _proj;

// Zoom levels: 50nm, 20nm, 5nm
static const float ZOOM_LEVELS[] = {50.0f, 20.0f, 5.0f};
static int _zoom_idx = 0;

// Map filter — single-select, -1 = show all
#define FILT_NONE     -1
#define FILT_AIRLINE   0
#define FILT_MILITARY  1
#define FILT_EMERGENCY 2
#define FILT_HELI      3
#define FILT_FAST      4
#define FILT_SLOW      5
#define FILT_ODDBALL   6
#define NUM_FILTERS    7

static int _active_filter = FILT_NONE;
static bool _filter_just_clicked = false; // guard against zoom cycle

struct FilterDef {
    const char *label;
    lv_color_t color;
    lv_obj_t *btn;
    lv_obj_t *lbl;
};

static FilterDef _filters[] = {
    {"COM",  lv_color_hex(0x4488ff), nullptr, nullptr},
    {"MIL",  lv_color_hex(0xffaa00), nullptr, nullptr},
    {"EMG",  lv_color_hex(0xff3333), nullptr, nullptr},
    {"HELI", lv_color_hex(0x44ddaa), nullptr, nullptr},
    {"FAST", lv_color_hex(0xff66cc), nullptr, nullptr},
    {"SLOW", lv_color_hex(0x88aacc), nullptr, nullptr},
    {"ODD",  lv_color_hex(0xcc88ff), nullptr, nullptr},
};

// Helper: does callsign look like an airline ICAO code?
static bool is_airline_callsign(const char *cs) {
    return cs[0] >= 'A' && cs[0] <= 'Z' &&
           cs[1] >= 'A' && cs[1] <= 'Z' &&
           cs[2] >= 'A' && cs[2] <= 'Z' &&
           cs[3] >= '0' && cs[3] <= '9';
}

// Helper: check type_code against known helicopter types
static bool is_heli_type(const char *t) {
    static const char *heli_types[] = {
        "R22", "R44", "R66", "EC35", "EC45", "EC55",
        "A109", "A139", "A169", "B06", "B212", "B412",
        "S76", "S92", "B407", "B429", "B505",
        "H135", "H145", "H160", "H175", "H225",
        "AS50", "AS55", "AS65", "MD52", "MD60",
        "NH90", "CH47", "V22", "UH1", "BK17",
        nullptr
    };
    for (int i = 0; heli_types[i]; i++) {
        if (strcmp(t, heli_types[i]) == 0) return true;
    }
    return false;
}

// Single-filter match
static bool aircraft_passes_filter(const Aircraft &ac) {
    if (_active_filter == FILT_NONE) return true;

    switch (_active_filter) {
        case FILT_AIRLINE:
            if (is_airline_callsign(ac.callsign)) return true;
            if (ac.category[0] == 'A' && ac.category[1] >= '3') return true;
            return false;
        case FILT_MILITARY:
            return ac.is_military;
        case FILT_EMERGENCY:
            return ac.is_emergency;
        case FILT_HELI:
            if (ac.category[0] == 'A' && ac.category[1] == '7') return true;
            if (ac.type_code[0] && is_heli_type(ac.type_code)) return true;
            return false;
        case FILT_FAST:
            return ac.speed > 300 && !ac.on_ground;
        case FILT_SLOW:
            return ac.speed > 0 && ac.speed < 100 && !ac.on_ground;
        case FILT_ODDBALL:
            if (ac.category[0] == 'B') return true;
            if (ac.registration[0] == 'N' && !is_airline_callsign(ac.callsign)) {
                if (ac.category[0] == 'A' && (ac.category[1] == '1' || ac.category[1] == '2')) return true;
                if (!ac.category[0] && ac.speed < 200) return true;
            }
            return false;
    }
    return true;
}

static void update_filter_visuals() {
    for (int i = 0; i < NUM_FILTERS; i++) {
        bool active = (_active_filter == i);
        if (active) {
            lv_obj_set_style_bg_color(_filters[i].btn, _filters[i].color, 0);
            lv_obj_set_style_bg_opa(_filters[i].btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(_filters[i].btn, lv_color_hex(0xffffff), 0);
            lv_obj_set_style_border_width(_filters[i].btn, 2, 0);
            lv_obj_set_style_border_opa(_filters[i].btn, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(_filters[i].lbl, lv_color_hex(0x000000), 0);
        } else {
            lv_obj_set_style_bg_color(_filters[i].btn, lv_color_hex(0x0a0a1a), 0);
            lv_obj_set_style_bg_opa(_filters[i].btn, LV_OPA_70, 0);
            lv_obj_set_style_border_color(_filters[i].btn, _filters[i].color, 0);
            lv_obj_set_style_border_width(_filters[i].btn, 1, 0);
            lv_obj_set_style_border_opa(_filters[i].btn, LV_OPA_40, 0);
            lv_obj_set_style_text_color(_filters[i].lbl, lv_color_hex(0x666666), 0);
        }
    }
    if (_canvas) lv_obj_invalidate(_canvas);
}

static void filter_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    _filter_just_clicked = true; // prevent zoom cycle
    if (_active_filter == idx) {
        _active_filter = FILT_NONE; // toggle off
    } else {
        _active_filter = idx; // switch to this filter
    }
    update_filter_visuals();
}

#define CANVAS_W LCD_H_RES
#define CANVAS_H (LCD_V_RES - 30)  // minus status bar
#define BG_COLOR lv_color_hex(0x0a0a1a)

static void draw_grid(lv_layer_t *layer) {
    float radius_nm = ZOOM_LEVELS[_zoom_idx];
    float cos_lat = cosf(_proj.center_lat * M_PI / 180.0f);

    float grid_deg = radius_nm >= 30 ? 1.0f : (radius_nm >= 10 ? 0.5f : 0.1f);

    float half_h_nm = radius_nm;
    float half_w_nm = radius_nm * (float)CANVAS_W / (float)CANVAS_H;
    float north = _proj.center_lat + half_h_nm / 60.0f;
    float south = _proj.center_lat - half_h_nm / 60.0f;
    float east = _proj.center_lon + half_w_nm / (60.0f * cos_lat);
    float west = _proj.center_lon - half_w_nm / (60.0f * cos_lat);

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_hex(0x0d1a2a);
    line_dsc.width = 1;
    line_dsc.opa = LV_OPA_COVER;

    float lon_start = floorf(west / grid_deg) * grid_deg;
    for (float lon = lon_start; lon <= east; lon += grid_deg) {
        int x1, y1, x2, y2;
        _proj.to_screen(north, lon, x1, y1);
        _proj.to_screen(south, lon, x2, y2);
        line_dsc.p1 = {(lv_value_precise_t)x1, (lv_value_precise_t)y1};
        line_dsc.p2 = {(lv_value_precise_t)x2, (lv_value_precise_t)y2};
        lv_draw_line(layer, &line_dsc);
    }

    float lat_start = floorf(south / grid_deg) * grid_deg;
    for (float lat = lat_start; lat <= north; lat += grid_deg) {
        int x1, y1, x2, y2;
        _proj.to_screen(lat, west, x1, y1);
        _proj.to_screen(lat, east, x2, y2);
        line_dsc.p1 = {(lv_value_precise_t)x1, (lv_value_precise_t)y1};
        line_dsc.p2 = {(lv_value_precise_t)x2, (lv_value_precise_t)y2};
        lv_draw_line(layer, &line_dsc);
    }
}

static void draw_range_rings(lv_layer_t *layer) {
    lv_draw_arc_dsc_t arc_dsc;
    lv_draw_arc_dsc_init(&arc_dsc);
    arc_dsc.color = lv_color_hex(0x1a2a3a);
    arc_dsc.width = 1;
    arc_dsc.start_angle = 0;
    arc_dsc.end_angle = 360;

    float radius_nm = ZOOM_LEVELS[_zoom_idx];
    float scale = (float)CANVAS_H / (radius_nm * 2.0f);

    float ring_interval = radius_nm <= 10 ? 2.0f : (radius_nm <= 25 ? 5.0f : 10.0f);
    for (float r = ring_interval; r <= radius_nm; r += ring_interval) {
        int pixel_r = (int)(r * scale);
        arc_dsc.center.x = CANVAS_W / 2 + _proj.offset_x;
        arc_dsc.center.y = CANVAS_H / 2 + _proj.offset_y;
        arc_dsc.radius = pixel_r;
        lv_draw_arc(layer, &arc_dsc);
    }
}

static void draw_home_marker(lv_layer_t *layer) {
    int hx, hy;
    if (!_proj.to_screen(HOME_LAT, HOME_LON, hx, hy)) return;

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_hex(0x4488ff);
    line_dsc.width = 1;

    line_dsc.p1 = {(lv_value_precise_t)(hx - 8), (lv_value_precise_t)hy};
    line_dsc.p2 = {(lv_value_precise_t)(hx + 8), (lv_value_precise_t)hy};
    lv_draw_line(layer, &line_dsc);

    line_dsc.p1 = {(lv_value_precise_t)hx, (lv_value_precise_t)(hy - 8)};
    line_dsc.p2 = {(lv_value_precise_t)hx, (lv_value_precise_t)(hy + 8)};
    lv_draw_line(layer, &line_dsc);
}

static void draw_aircraft(lv_layer_t *layer) {
    if (!_list->lock(pdMS_TO_TICKS(50))) return;

    uint32_t now = millis();
    for (int i = 0; i < _list->count; i++) {
        Aircraft &ac = _list->aircraft[i];
        uint8_t ac_opa = compute_aircraft_opacity(ac.stale_since, now);
        if (ac_opa == 0) continue;
        if (!aircraft_passes_filter(ac)) continue;

        int sx, sy;
        if (!_proj.to_screen(ac.lat, ac.lon, sx, sy)) continue;

        lv_color_t color = ac.is_military ? lv_color_hex(0xffaa00) :
                           ac.is_emergency ? lv_color_hex(0xff3333) :
                           altitude_color(ac.altitude);

        // Draw trail
        if (ac.trail_count > 1) {
            lv_draw_line_dsc_t trail_dsc;
            lv_draw_line_dsc_init(&trail_dsc);
            trail_dsc.width = 1;

            for (int t = 1; t < ac.trail_count; t++) {
                int tx1, ty1, tx2, ty2;
                if (_proj.to_screen(ac.trail[t - 1].lat, ac.trail[t - 1].lon, tx1, ty1) &&
                    _proj.to_screen(ac.trail[t].lat, ac.trail[t].lon, tx2, ty2)) {
                    trail_dsc.color = altitude_color(ac.trail[t].alt);
                    trail_dsc.opa = LV_OPA_50 + (t * LV_OPA_50 / ac.trail_count);
                    trail_dsc.p1 = {(lv_value_precise_t)tx1, (lv_value_precise_t)ty1};
                    trail_dsc.p2 = {(lv_value_precise_t)tx2, (lv_value_precise_t)ty2};
                    lv_draw_line(layer, &trail_dsc);
                }
            }
        }

        // Draw heading line
        float heading_rad = ac.heading * M_PI / 180.0f;
        int hdg_x = sx + (int)(14 * sinf(heading_rad));
        int hdg_y = sy - (int)(14 * cosf(heading_rad));
        lv_draw_line_dsc_t hdg_dsc;
        lv_draw_line_dsc_init(&hdg_dsc);
        hdg_dsc.color = color;
        hdg_dsc.width = 2;
        hdg_dsc.opa = ac_opa;
        hdg_dsc.p1 = {(lv_value_precise_t)sx, (lv_value_precise_t)sy};
        hdg_dsc.p2 = {(lv_value_precise_t)hdg_x, (lv_value_precise_t)hdg_y};
        lv_draw_line(layer, &hdg_dsc);

        // Draw aircraft marker (diamond for military, dot for civilian)
        if (ac.is_military) {
            lv_draw_line_dsc_t dm;
            lv_draw_line_dsc_init(&dm);
            dm.color = color;
            dm.width = 2;
            dm.opa = ac_opa;
            int d = 5;
            lv_point_precise_t pts[] = {
                {(lv_value_precise_t)sx, (lv_value_precise_t)(sy - d)},
                {(lv_value_precise_t)(sx + d), (lv_value_precise_t)sy},
                {(lv_value_precise_t)sx, (lv_value_precise_t)(sy + d)},
                {(lv_value_precise_t)(sx - d), (lv_value_precise_t)sy},
            };
            for (int j = 0; j < 4; j++) {
                dm.p1 = {pts[j].x, pts[j].y};
                dm.p2 = {pts[(j + 1) % 4].x, pts[(j + 1) % 4].y};
                lv_draw_line(layer, &dm);
            }
        } else {
            lv_draw_rect_dsc_t dot_dsc;
            lv_draw_rect_dsc_init(&dot_dsc);
            dot_dsc.bg_color = color;
            dot_dsc.bg_opa = ac_opa;
            dot_dsc.radius = 3;
            lv_area_t dot_area = {(lv_coord_t)(sx - 3), (lv_coord_t)(sy - 3),
                                   (lv_coord_t)(sx + 3), (lv_coord_t)(sy + 3)};
            lv_draw_rect(layer, &dot_dsc, &dot_area);
        }

        // Draw callsign label
        const char *label_text = ac.callsign[0] ? ac.callsign : ac.icao_hex;
        lv_draw_label_dsc_t lbl_dsc;
        lv_draw_label_dsc_init(&lbl_dsc);
        lbl_dsc.color = color;
        lbl_dsc.font = &lv_font_montserrat_14;
        lbl_dsc.opa = (uint8_t)((ac_opa * LV_OPA_80) / 255);
        lv_area_t lbl_area = {(lv_coord_t)(sx + 8), (lv_coord_t)(sy - 7),
                               (lv_coord_t)(sx + 120), (lv_coord_t)(sy + 10)};
        lbl_dsc.text = label_text;
        lv_draw_label(layer, &lbl_dsc, &lbl_area);
    }

    _list->unlock();
}

static void draw_altitude_legend(lv_layer_t *layer) {
    struct { const char *label; lv_color_t color; } entries[] = {
        {"GND",  lv_color_hex(0x666666)},
        {"<5k",  lv_color_hex(0x00cc44)},
        {"<15k", lv_color_hex(0x88cc00)},
        {"<25k", lv_color_hex(0xcccc00)},
        {"<35k", lv_color_hex(0xcc8800)},
        {"35k+", lv_color_hex(0xcc2200)},
    };

    int x = 8;
    int y = CANVAS_H - 16;

    for (int i = 0; i < 6; i++) {
        lv_draw_rect_dsc_t swatch;
        lv_draw_rect_dsc_init(&swatch);
        swatch.bg_color = entries[i].color;
        swatch.bg_opa = LV_OPA_COVER;
        swatch.radius = 2;
        lv_area_t sa = {(lv_coord_t)x, (lv_coord_t)y,
                        (lv_coord_t)(x + 8), (lv_coord_t)(y + 8)};
        lv_draw_rect(layer, &swatch, &sa);

        lv_draw_label_dsc_t lbl;
        lv_draw_label_dsc_init(&lbl);
        lbl.color = entries[i].color;
        lbl.font = &lv_font_montserrat_14;
        lbl.opa = LV_OPA_80;
        lbl.text = entries[i].label;
        lv_area_t la = {(lv_coord_t)(x + 12), (lv_coord_t)(y - 2),
                        (lv_coord_t)(x + 60), (lv_coord_t)(y + 12)};
        lv_draw_label(layer, &lbl, &la);

        x += 58;
    }
}

// Draw active filter indicator on canvas
static void draw_filter_label(lv_layer_t *layer) {
    if (_active_filter == FILT_NONE) return;

    const char *names[] = {"COMMERCIAL", "MILITARY", "EMERGENCY", "HELICOPTERS", "FAST >300kt", "SLOW <100kt", "ODDBALL"};
    char buf[32];
    snprintf(buf, sizeof(buf), "FILTER: %s", names[_active_filter]);

    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.color = _filters[_active_filter].color;
    lbl.font = &lv_font_montserrat_14;
    lbl.opa = LV_OPA_COVER;
    lbl.text = buf;
    lv_area_t la = {(lv_coord_t)8, (lv_coord_t)4,
                    (lv_coord_t)300, (lv_coord_t)20};
    lv_draw_label(layer, &lbl, &la);
}

static void canvas_draw_cb(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);

    draw_grid(layer);
    draw_range_rings(layer);
    draw_home_marker(layer);
    draw_aircraft(layer);
    draw_altitude_legend(layer);
    draw_filter_label(layer);
}

void map_view_init(lv_obj_t *parent, AircraftList *list) {
    _list = list;

    _proj.center_lat = HOME_LAT;
    _proj.center_lon = HOME_LON;
    _proj.radius_nm = ZOOM_LEVELS[_zoom_idx];
    _proj.screen_w = CANVAS_W;
    _proj.screen_h = CANVAS_H;
    _proj.offset_x = 0;
    _proj.offset_y = 0;

    _canvas = lv_obj_create(parent);
    lv_obj_set_size(_canvas, CANVAS_W, CANVAS_H);
    lv_obj_set_pos(_canvas, 0, 0);
    lv_obj_set_style_bg_color(_canvas, BG_COLOR, 0);
    lv_obj_set_style_bg_opa(_canvas, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_canvas, 0, 0);
    lv_obj_set_style_radius(_canvas, 0, 0);
    lv_obj_set_style_pad_all(_canvas, 0, 0);
    lv_obj_clear_flag(_canvas, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(_canvas, canvas_draw_cb, LV_EVENT_DRAW_MAIN_END, nullptr);

    // Tap: aircraft hit-test -> detail card, or cycle zoom
    lv_obj_add_event_cb(_canvas, [](lv_event_t *e) {
        if (views_get_active_index() != VIEW_MAP) return;

        // Guard: skip if a filter button was just clicked
        if (_filter_just_clicked) {
            _filter_just_clicked = false;
            return;
        }

        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);

        int tx = point.x;
        int ty = point.y - 30;

        if (detail_card_is_visible()) {
            detail_card_hide();
            return;
        }

        // Hit test against aircraft (20px radius)
        if (!_list->lock(pdMS_TO_TICKS(50))) return;
        for (int i = 0; i < _list->count; i++) {
            int sx, sy;
            if (_proj.to_screen(_list->aircraft[i].lat, _list->aircraft[i].lon, sx, sy)) {
                int dx = tx - sx;
                int dy = ty - sy;
                if (dx * dx + dy * dy < 400) {
                    Aircraft ac_copy = _list->aircraft[i];
                    _list->unlock();
                    detail_card_show(&ac_copy);
                    return;
                }
            }
        }
        _list->unlock();

        // No aircraft hit — cycle zoom
        _zoom_idx = (_zoom_idx + 1) % 3;
        _proj.radius_nm = ZOOM_LEVELS[_zoom_idx];
        lv_obj_invalidate(_canvas);
    }, LV_EVENT_CLICKED, nullptr);

    // Filter toggle buttons — top-right, single-select exclusive
    int btn_x = CANVAS_W - (NUM_FILTERS * 54) - 4;
    for (int i = 0; i < NUM_FILTERS; i++) {
        lv_obj_t *btn = lv_obj_create(parent);
        lv_obj_set_size(btn, 50, 22);
        lv_obj_set_pos(btn, btn_x + i * 54, 4);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x0a0a1a), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
        lv_obj_set_style_border_color(btn, _filters[i].color, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(btn, filter_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, _filters[i].label);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
        lv_obj_center(lbl);

        _filters[i].btn = btn;
        _filters[i].lbl = lbl;
    }

    // Periodic refresh
    lv_timer_create([](lv_timer_t *t) {
        if (views_get_active_index() == VIEW_MAP) {
            lv_obj_invalidate(_canvas);
        }
    }, 1000, nullptr);
}

void map_view_update() {
    if (_canvas) lv_obj_invalidate(_canvas);
}
