#include <Arduino.h>
#include "map_view.h"
#include "detail_card.h"
#include "views.h"
#include "range.h"
#include "filters.h"
// #include "tile_cache.h" // disabled: tiles broken on ESP32-P4
#include "../config.h"
#include "../pins_config.h"

static AircraftList *_list = nullptr;
static lv_obj_t *_canvas = nullptr;
static lv_obj_t *_range_label = nullptr;
static MapProjection _proj;

static bool _filter_just_clicked = false; // guard against zoom cycle

// Per-view button/label pointers for filter buttons
static lv_obj_t *_filter_btns[NUM_FILTERS] = {};
static lv_obj_t *_filter_lbls[NUM_FILTERS] = {};

static void update_filter_visuals() {
    int active = filter_get_active();
    for (int i = 0; i < NUM_FILTERS; i++) {
        if (active == i) {
            lv_obj_set_style_bg_color(_filter_btns[i], filter_defs[i].color, 0);
            lv_obj_set_style_bg_opa(_filter_btns[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(_filter_btns[i], lv_color_hex(0xffffff), 0);
            lv_obj_set_style_border_width(_filter_btns[i], 2, 0);
            lv_obj_set_style_border_opa(_filter_btns[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(_filter_lbls[i], lv_color_hex(0x000000), 0);
        } else {
            lv_obj_set_style_bg_color(_filter_btns[i], lv_color_hex(0x0a0a1a), 0);
            lv_obj_set_style_bg_opa(_filter_btns[i], LV_OPA_70, 0);
            lv_obj_set_style_border_color(_filter_btns[i], filter_defs[i].color, 0);
            lv_obj_set_style_border_width(_filter_btns[i], 1, 0);
            lv_obj_set_style_border_opa(_filter_btns[i], LV_OPA_40, 0);
            lv_obj_set_style_text_color(_filter_lbls[i], lv_color_hex(0x666666), 0);
        }
    }
    if (_canvas) lv_obj_invalidate(_canvas);
}

static void filter_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    _filter_just_clicked = true; // prevent zoom cycle
    filter_toggle(idx);
    update_filter_visuals();
}

#define CANVAS_W LCD_H_RES
#define CANVAS_H (LCD_V_RES - 30)  // minus status bar
#define BG_COLOR lv_color_hex(0x0a0a1a)

static void draw_grid(lv_layer_t *layer) {
    float radius_nm = range_get_nm();
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

    float radius_nm = range_get_nm();
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

// Rotate a local-coordinate point by heading (0=north) around origin, translate to screen pos
static inline void rotate_pt(float lx, float ly, float sin_h, float cos_h,
                              int cx, int cy, lv_point_precise_t &out) {
    out.x = (lv_value_precise_t)(cx + lx * cos_h - ly * sin_h);
    out.y = (lv_value_precise_t)(cy + lx * sin_h + ly * cos_h);
}

// Draw a filled triangle rotated by heading
static void draw_tri(lv_layer_t *layer, int cx, int cy, float sin_h, float cos_h,
                     float x0, float y0, float x1, float y1, float x2, float y2,
                     lv_color_t color, uint8_t opa) {
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = color;
    tri.opa = opa;
    rotate_pt(x0, y0, sin_h, cos_h, cx, cy, tri.p[0]);
    rotate_pt(x1, y1, sin_h, cos_h, cx, cy, tri.p[1]);
    rotate_pt(x2, y2, sin_h, cos_h, cx, cy, tri.p[2]);
    lv_draw_triangle(layer, &tri);
}

// Airliner icon: long fuselage, long straight wings (~24px long, 40px wingspan)
//   Think 737/A320 top-down — wide straight wings, clearly distinct from fighter
static void draw_icon_airliner(lv_layer_t *layer, int cx, int cy,
                                float sin_h, float cos_h, lv_color_t color, uint8_t opa) {
    // Fuselage — long, narrow tube (30px nose to tail)
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, -15,  1.5f, 0,  -1.5f, 0, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, -15,  1.5f, 0,  0, 15, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, -15,  -1.5f, 0,  0, 15, color, opa);
    // Wings — long, nearly straight, minimal sweep (40px tip to tip)
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, -1,  20, 1,  0, 3, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, -1,  -20, 1,  0, 3, color, opa);
    // Horizontal stabilizer at tail
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, 11,  7, 14,  0, 15, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, 11,  -7, 14,  0, 15, color, opa);
}

// Military jet icon: swept delta wings, narrow fuselage, tail fins (~16px long)
//   Fighter/fast-jet silhouette
static void draw_icon_jet(lv_layer_t *layer, int cx, int cy,
                          float sin_h, float cos_h, lv_color_t color, uint8_t opa) {
    // Fuselage (narrow diamond)
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, -8,  1.5f, 4,  -1.5f, 4, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, -8,  1.5f, 4,  0, 8, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, -8,  -1.5f, 4,  0, 8, color, opa);
    // Main wings (swept back)
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, -2,  9, 4,  0, 3, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, -2,  -9, 4,  0, 3, color, opa);
    // Tail fins
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, 5,  4, 8,  0, 8, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, 5,  -4, 8,  0, 8, color, opa);
}

// GA/prop icon: straight high wings, short body — Cessna silhouette (~10px long)
static void draw_icon_ga(lv_layer_t *layer, int cx, int cy,
                         float sin_h, float cos_h, lv_color_t color, uint8_t opa) {
    // Fuselage — shorter, stubbier
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, -5,  1, 0,  -1, 0, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h,
             1, 0,  -1, 0,  0, 5, color, opa);
    // High straight wings — nearly zero sweep
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, -2,  8, -1,  0, 0, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, -2,  -8, -1,  0, 0, color, opa);
    // Simple horizontal tail
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, 3,  3, 5,  -3, 5, color, opa);
}

// Helicopter icon: teardrop body + rotor disc
static void draw_icon_heli(lv_layer_t *layer, int cx, int cy,
                           float sin_h, float cos_h, lv_color_t color, uint8_t opa) {
    // Body (teardrop pointing in heading direction)
    draw_tri(layer, cx, cy, sin_h, cos_h,
             0, -5,  3, 1,  -3, 1, color, opa);
    draw_tri(layer, cx, cy, sin_h, cos_h,
             3, 1,  -3, 1,  0, 6, color, opa);
    // Rotor disc (two crossing lines)
    lv_draw_line_dsc_t rotor;
    lv_draw_line_dsc_init(&rotor);
    rotor.color = color;
    rotor.width = 1;
    rotor.opa = (uint8_t)(opa * 3 / 4);
    lv_point_precise_t r0, r1, r2, r3;
    rotate_pt(-7, -2, sin_h, cos_h, cx, cy, r0);
    rotate_pt( 7, -2, sin_h, cos_h, cx, cy, r1);
    rotate_pt( 0, -8, sin_h, cos_h, cx, cy, r2);
    rotate_pt( 0,  4, sin_h, cos_h, cx, cy, r3);
    rotor.p1 = {r0.x, r0.y}; rotor.p2 = {r1.x, r1.y};
    lv_draw_line(layer, &rotor);
    rotor.p1 = {r2.x, r2.y}; rotor.p2 = {r3.x, r3.y};
    lv_draw_line(layer, &rotor);
}

// Classify aircraft into icon type
enum IconType { ICON_AIRLINER, ICON_JET, ICON_GA, ICON_HELI };

static IconType classify_icon(const Aircraft &ac) {
    // Helicopters: category A7 or known heli type codes
    if (ac.category[0] == 'A' && ac.category[1] == '7') return ICON_HELI;
    if (ac.type_code[0] && is_heli_type(ac.type_code)) return ICON_HELI;
    // Military → fighter jet silhouette
    if (ac.is_military) return ICON_JET;
    // Airliners: airline callsigns or large aircraft categories (A3+)
    if (is_airline_callsign(ac.callsign)) return ICON_AIRLINER;
    if (ac.category[0] == 'A' && ac.category[1] >= '3') return ICON_AIRLINER;
    // Everything else: GA/prop
    return ICON_GA;
}

static void draw_aircraft(lv_layer_t *layer) {
    if (!_list->lock(pdMS_TO_TICKS(5))) return; // short timeout: skip frame if data locked

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

        // Draw aircraft icon rotated to heading
        float heading_rad = ac.heading * M_PI / 180.0f;
        float sin_h = sinf(heading_rad);
        float cos_h = cosf(heading_rad);

        IconType icon = classify_icon(ac);
        switch (icon) {
            case ICON_AIRLINER: draw_icon_airliner(layer, sx, sy, sin_h, cos_h, color, ac_opa); break;
            case ICON_JET:      draw_icon_jet(layer, sx, sy, sin_h, cos_h, color, ac_opa); break;
            case ICON_GA:       draw_icon_ga(layer, sx, sy, sin_h, cos_h, color, ac_opa); break;
            case ICON_HELI:     draw_icon_heli(layer, sx, sy, sin_h, cos_h, color, ac_opa); break;
        }

        // Draw callsign label
        const char *label_text = ac.callsign[0] ? ac.callsign : ac.icao_hex;
        lv_draw_label_dsc_t lbl_dsc;
        lv_draw_label_dsc_init(&lbl_dsc);
        lbl_dsc.color = color;
        lbl_dsc.font = &lv_font_montserrat_14;
        lbl_dsc.opa = (uint8_t)((ac_opa * LV_OPA_80) / 255);
        lv_area_t lbl_area = {(lv_coord_t)(sx + 12), (lv_coord_t)(sy - 7),
                               (lv_coord_t)(sx + 130), (lv_coord_t)(sy + 10)};
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
    int y = CANVAS_H - 18;

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

// Draw icon type legend above altitude legend
static void draw_icon_legend(lv_layer_t *layer) {
    lv_color_t legend_color = lv_color_hex(0x669966);
    int y = CANVAS_H - 38;

    // Draw mini icons with labels
    struct { const char *label; IconType type; } entries[] = {
        {"AIR",  ICON_AIRLINER},
        {"GA",   ICON_GA},
        {"MIL",  ICON_JET},
        {"HELI", ICON_HELI},
    };

    int x = 8;
    for (int i = 0; i < 4; i++) {
        // Draw a small north-facing icon
        switch (entries[i].type) {
            case ICON_AIRLINER: draw_icon_airliner(layer, x + 6, y + 6, 0, 1, legend_color, LV_OPA_80); break;
            case ICON_JET:      draw_icon_jet(layer, x + 6, y + 6, 0, 1, legend_color, LV_OPA_80); break;
            case ICON_GA:       draw_icon_ga(layer, x + 6, y + 6, 0, 1, legend_color, LV_OPA_80); break;
            case ICON_HELI:     draw_icon_heli(layer, x + 6, y + 6, 0, 1, legend_color, LV_OPA_80); break;
        }

        lv_draw_label_dsc_t lbl;
        lv_draw_label_dsc_init(&lbl);
        lbl.color = legend_color;
        lbl.font = &lv_font_montserrat_14;
        lbl.opa = LV_OPA_80;
        lbl.text = entries[i].label;
        lv_area_t la = {(lv_coord_t)(x + 16), (lv_coord_t)(y - 1),
                        (lv_coord_t)(x + 60), (lv_coord_t)(y + 14)};
        lv_draw_label(layer, &lbl, &la);

        x += 58;
    }
}

// Draw active filter indicator on canvas
static void draw_filter_label(lv_layer_t *layer) {
    int af = filter_get_active();
    if (af == FILT_NONE) return;

    char buf[32];
    snprintf(buf, sizeof(buf), "FILTER: %s", filter_defs[af].full_name);

    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.color = filter_defs[af].color;
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
    draw_icon_legend(layer);
    draw_altitude_legend(layer);
    draw_filter_label(layer);
}

void map_view_init(lv_obj_t *parent, AircraftList *list) {
    _list = list;

    _proj.center_lat = HOME_LAT;
    _proj.center_lon = HOME_LON;
    _proj.radius_nm = range_get_nm();
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
    lv_obj_clear_flag(_canvas, LV_OBJ_FLAG_SCROLL_CHAIN); // prevent tileview from stealing clicks

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

        Serial.printf("MAP tap: (%d,%d)\n", tx, ty);

        // Hit test against aircraft (20px radius)
        if (!_list->lock(pdMS_TO_TICKS(10))) { Serial.println("MAP: lock failed"); return; }
        for (int i = 0; i < _list->count; i++) {
            int sx, sy;
            if (_proj.to_screen(_list->aircraft[i].lat, _list->aircraft[i].lon, sx, sy)) {
                int dx = tx - sx;
                int dy = ty - sy;
                if (dx * dx + dy * dy < 900) { // 30px hit radius for touchscreen
                    Aircraft ac_copy = _list->aircraft[i];
                    _list->unlock();
                    Serial.printf("MAP: hit %s at (%d,%d)\n", ac_copy.callsign, tx, ty);
                    detail_card_show(&ac_copy);
                    return;
                }
            }
        }
        Serial.println("MAP: no hit");
        _list->unlock();
    }, LV_EVENT_CLICKED, nullptr);

    // Filter toggle buttons — vertical stack on right edge
    int btn_w = 64;
    int btn_h = 48;
    int btn_gap = 10;
    int total_h = NUM_FILTERS * btn_h + (NUM_FILTERS - 1) * btn_gap;
    int btn_x = CANVAS_W - btn_w - 8;
    int btn_y0 = (CANVAS_H - total_h) / 2;
    for (int i = 0; i < NUM_FILTERS; i++) {
        lv_obj_t *btn = lv_obj_create(parent);
        lv_obj_set_size(btn, btn_w, btn_h);
        lv_obj_set_pos(btn, btn_x, btn_y0 + i * (btn_h + btn_gap));
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x0a0a1a), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
        lv_obj_set_style_border_color(btn, filter_defs[i].color, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_add_event_cb(btn, filter_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, filter_defs[i].label);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
        lv_obj_center(lbl);

        _filter_btns[i] = btn;
        _filter_lbls[i] = lbl;
    }

    // Range label — bottom-right, tappable
    _range_label = lv_label_create(parent);
    lv_label_set_text(_range_label, range_label());
    lv_obj_set_style_text_font(_range_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_range_label, lv_color_hex(0x4488ff), 0);
    lv_obj_set_pos(_range_label, CANVAS_W - 80, CANVAS_H - 28);
    lv_obj_add_flag(_range_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_range_label, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(_range_label, [](lv_event_t *e) {
        range_cycle();
        lv_label_set_text(_range_label, range_label());
        _proj.radius_nm = range_get_nm();
        lv_obj_invalidate(_canvas);
    }, LV_EVENT_CLICKED, nullptr);

    // Periodic refresh
    static int _last_synced_filter = FILT_NONE;
    lv_timer_create([](lv_timer_t *t) {
        // Sync filter button visuals if filter changed from another view
        int af = filter_get_active();
        if (af != _last_synced_filter) {
            _last_synced_filter = af;
            update_filter_visuals();
        }
        if (views_get_active_index() == VIEW_MAP) {
            _proj.radius_nm = range_get_nm();
            lv_label_set_text(_range_label, range_label());
            lv_obj_invalidate(_canvas);
        }
    }, 1000, nullptr);
}

void map_view_update() {
    if (_canvas) lv_obj_invalidate(_canvas);
}
