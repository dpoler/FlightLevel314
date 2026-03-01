#include <Arduino.h>
#include "radar_view.h"
#include "views.h"
#include "../config.h"
#include "../pins_config.h"
#include "geo.h"

static AircraftList *_list = nullptr;
static lv_obj_t *_radar_obj = nullptr;

static float _sweep_angle = 0.0f; // current sweep angle in degrees
static uint32_t _last_sweep_ms = 0;

#define RADAR_W LCD_H_RES
#define RADAR_H (LCD_V_RES - 30)
#define RADAR_CX (RADAR_W / 2)
#define RADAR_CY (RADAR_H / 2)
#define RADAR_R (RADAR_H / 2 - 10)  // max radius in pixels

#define SWEEP_PERIOD_MS 30000  // one full rotation = 30 seconds

#define COLOR_SWEEP lv_color_hex(0x00ff44)
#define COLOR_RING lv_color_hex(0x0a2a0a)
#define COLOR_TEXT lv_color_hex(0x00cc33)
#define COLOR_BG lv_color_hex(0x000800)
#define COLOR_BLIP lv_color_hex(0x00ff66)
#define COLOR_MILITARY lv_color_hex(0xffaa00)

#define SWEEP_BRIGHT_DEG 60.0f        // full brightness boost zone
#define SWEEP_FADE_DEG   180.0f       // fade boost back to normal
#define BLIP_NORMAL_OPA  LV_OPA_90    // normal blip brightness
#define LABEL_VISIBLE_DEG 240.0f      // callsign label visibility zone

static MapProjection _proj;

// Convert lat/lon to radar-relative screen coords
static bool to_radar_screen(float lat, float lon, int &sx, int &sy) {
    float dx_nm = (lon - _proj.center_lon) * 60.0f * cosf(_proj.center_lat * M_PI / 180.0f);
    float dy_nm = (lat - _proj.center_lat) * 60.0f;
    float dist_nm = sqrtf(dx_nm * dx_nm + dy_nm * dy_nm);

    if (dist_nm > _proj.radius_nm) return false;

    float scale = (float)RADAR_R / _proj.radius_nm;
    sx = RADAR_CX + (int)(dx_nm * scale);
    sy = RADAR_CY - (int)(dy_nm * scale);
    return true;
}

// Get bearing angle (0=N, 90=E) from radar center to a screen point
static float blip_angle(int sx, int sy) {
    float dx = (float)(sx - RADAR_CX);
    float dy = (float)(RADAR_CY - sy); // invert Y (screen Y goes down)
    float angle = atan2f(dx, dy) * 180.0f / M_PI;
    if (angle < 0) angle += 360.0f;
    return angle;
}

// Angular distance from sweep to blip (how far behind the sweep the blip is)
static float angle_behind_sweep(float blip_deg) {
    float diff = _sweep_angle - blip_deg;
    if (diff < 0) diff += 360.0f;
    return diff; // 0 = just swept, 359 = about to be swept
}

static void draw_rings(lv_layer_t *layer) {
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = COLOR_RING;
    arc.width = 1;
    arc.start_angle = 0;
    arc.end_angle = 360;
    arc.center.x = RADAR_CX;
    arc.center.y = RADAR_CY;

    for (int i = 1; i <= 4; i++) {
        arc.radius = RADAR_R * i / 4;
        lv_draw_arc(layer, &arc);
    }

    // Cardinal direction labels
    const char *dirs[] = {"N", "E", "S", "W"};
    int dx[] = {0, 1, 0, -1};
    int dy[] = {-1, 0, 1, 0};
    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.color = COLOR_RING;
    lbl.font = &lv_font_montserrat_14;
    for (int i = 0; i < 4; i++) {
        int lx = RADAR_CX + dx[i] * (RADAR_R + 2) - 5;
        int ly = RADAR_CY + dy[i] * (RADAR_R + 2) - 7;
        lv_area_t area = {(lv_coord_t)lx, (lv_coord_t)ly,
                          (lv_coord_t)(lx + 20), (lv_coord_t)(ly + 16)};
        lbl.text = dirs[i];
        lv_draw_label(layer, &lbl, &area);
    }

    // Cross lines
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = COLOR_RING;
    line.width = 1;
    line.opa = LV_OPA_30;
    line.p1 = {RADAR_CX, RADAR_CY - RADAR_R};
    line.p2 = {RADAR_CX, RADAR_CY + RADAR_R};
    lv_draw_line(layer, &line);
    line.p1 = {RADAR_CX - RADAR_R, RADAR_CY};
    line.p2 = {RADAR_CX + RADAR_R, RADAR_CY};
    lv_draw_line(layer, &line);
}

static void draw_sweep(lv_layer_t *layer) {
    float rad = _sweep_angle * M_PI / 180.0f;
    int ex = RADAR_CX + (int)(RADAR_R * sinf(rad));
    int ey = RADAR_CY - (int)(RADAR_R * cosf(rad));

    // Sweep line
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = COLOR_SWEEP;
    line.width = 2;
    line.opa = LV_OPA_60;
    line.p1 = {RADAR_CX, RADAR_CY};
    line.p2 = {(lv_value_precise_t)ex, (lv_value_precise_t)ey};
    lv_draw_line(layer, &line);

    // Fading trail (draw 6 faded lines behind sweep)
    for (int i = 1; i <= 6; i++) {
        float trail_rad = (_sweep_angle - i * 3.0f) * M_PI / 180.0f;
        int tx = RADAR_CX + (int)(RADAR_R * sinf(trail_rad));
        int ty = RADAR_CY - (int)(RADAR_R * cosf(trail_rad));
        line.opa = LV_OPA_40 - i * 6;
        line.width = 1;
        line.p1 = {RADAR_CX, RADAR_CY};
        line.p2 = {(lv_value_precise_t)tx, (lv_value_precise_t)ty};
        lv_draw_line(layer, &line);
    }
}

static void draw_blips(lv_layer_t *layer) {
    if (!_list->lock(pdMS_TO_TICKS(50))) return;

    uint32_t now = millis();

    for (int i = 0; i < _list->count; i++) {
        Aircraft &ac = _list->aircraft[i];
        uint8_t ghost_opa = compute_aircraft_opacity(ac.stale_since, now);
        if (ghost_opa == 0) continue;

        int sx, sy;
        if (!to_radar_screen(ac.lat, ac.lon, sx, sy)) continue;

        // Sweep boost: brief brightness flash when sweep passes, otherwise normal
        float behind = angle_behind_sweep(blip_angle(sx, sy));
        uint8_t sweep_opa;
        if (behind < SWEEP_BRIGHT_DEG) {
            sweep_opa = LV_OPA_COVER;
        } else if (behind < SWEEP_FADE_DEG) {
            float t = (behind - SWEEP_BRIGHT_DEG) / (SWEEP_FADE_DEG - SWEEP_BRIGHT_DEG);
            sweep_opa = LV_OPA_COVER - (uint8_t)(t * (LV_OPA_COVER - BLIP_NORMAL_OPA));
        } else {
            sweep_opa = BLIP_NORMAL_OPA;
        }

        // Combine sweep brightness with ghost fade (stale aircraft fade out)
        uint8_t opa = (uint8_t)((sweep_opa * ghost_opa) / 255);

        lv_color_t color = ac.is_military ? COLOR_MILITARY : COLOR_BLIP;

        // Blip dot — larger for better visibility
        lv_draw_rect_dsc_t dot;
        lv_draw_rect_dsc_init(&dot);
        dot.bg_color = color;
        dot.bg_opa = opa;
        dot.radius = 4;
        lv_area_t area = {(lv_coord_t)(sx - 4), (lv_coord_t)(sy - 4),
                          (lv_coord_t)(sx + 4), (lv_coord_t)(sy + 4)};
        lv_draw_rect(layer, &dot, &area);

        // Labels: callsign + alt/speed — show for recently swept blips
        if (behind < LABEL_VISIBLE_DEG) {
            uint8_t lbl_opa = opa > LV_OPA_50 ? LV_OPA_80 : opa;

            // Callsign
            const char *label_text = ac.callsign[0] ? ac.callsign : ac.icao_hex;
            lv_draw_label_dsc_t lbl;
            lv_draw_label_dsc_init(&lbl);
            lbl.color = color;
            lbl.font = &lv_font_montserrat_14;
            lbl.opa = lbl_opa;
            lbl.text = label_text;
            lv_area_t lbl_area = {(lv_coord_t)(sx + 8), (lv_coord_t)(sy - 8),
                                   (lv_coord_t)(sx + 120), (lv_coord_t)(sy + 6)};
            lv_draw_label(layer, &lbl, &lbl_area);

            // Altitude + speed (smaller, dimmer)
            char info[24];
            if (ac.on_ground) snprintf(info, sizeof(info), "GND %dkt", ac.speed);
            else snprintf(info, sizeof(info), "FL%03d %dkt", ac.altitude / 100, ac.speed);
            lv_draw_label_dsc_t info_lbl;
            lv_draw_label_dsc_init(&info_lbl);
            info_lbl.color = color;
            info_lbl.font = &lv_font_montserrat_14;
            info_lbl.opa = (uint8_t)(lbl_opa * 3 / 4);
            info_lbl.text = info;
            lv_area_t info_area = {(lv_coord_t)(sx + 8), (lv_coord_t)(sy + 6),
                                    (lv_coord_t)(sx + 140), (lv_coord_t)(sy + 20)};
            lv_draw_label(layer, &info_lbl, &info_area);
        }
    }

    _list->unlock();
}

static void radar_draw_cb(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    draw_rings(layer);
    draw_sweep(layer);
    draw_blips(layer);
}

void radar_view_init(lv_obj_t *parent, AircraftList *list) {
    _list = list;

    _proj.center_lat = HOME_LAT;
    _proj.center_lon = HOME_LON;
    _proj.radius_nm = 50.0f;

    _radar_obj = lv_obj_create(parent);
    lv_obj_set_size(_radar_obj, RADAR_W, RADAR_H);
    lv_obj_set_pos(_radar_obj, 0, 0);
    lv_obj_set_style_bg_color(_radar_obj, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(_radar_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_radar_obj, 0, 0);
    lv_obj_set_style_radius(_radar_obj, 0, 0);
    lv_obj_clear_flag(_radar_obj, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(_radar_obj, radar_draw_cb, LV_EVENT_DRAW_MAIN_END, nullptr);

    // Animate sweep — always update angle, but only redraw when visible
    _last_sweep_ms = millis();
    lv_timer_create([](lv_timer_t *t) {
        uint32_t now = millis();
        uint32_t dt = now - _last_sweep_ms;
        _last_sweep_ms = now;
        _sweep_angle += (360.0f * dt) / SWEEP_PERIOD_MS;
        if (_sweep_angle >= 360.0f) _sweep_angle -= 360.0f;

        // Only invalidate when radar view is active (saves frame budget for swipes)
        if (views_get_active_index() == VIEW_RADAR) {
            lv_obj_invalidate(_radar_obj);
        }
    }, 33, nullptr);  // ~30fps
}

void radar_view_update() {
    if (_radar_obj) lv_obj_invalidate(_radar_obj);
}
