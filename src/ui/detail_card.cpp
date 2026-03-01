#include "detail_card.h"
#include "geo.h"
#include "../config.h"
#include "../data/enrichment.h"
#include "../pins_config.h"

static lv_obj_t *_card = nullptr;
static lv_obj_t *_callsign_label = nullptr;
static lv_obj_t *_reg_label = nullptr;
static lv_obj_t *_type_label = nullptr;
static lv_obj_t *_airline_label = nullptr;
static lv_obj_t *_route_label = nullptr;
static lv_obj_t *_alt_label = nullptr;
static lv_obj_t *_spd_label = nullptr;
static lv_obj_t *_hdg_label = nullptr;
static lv_obj_t *_vrate_label = nullptr;
static lv_obj_t *_dist_label = nullptr;
static lv_obj_t *_squawk_label = nullptr;
static lv_obj_t *_bearing_label = nullptr;
static lv_obj_t *_status_label = nullptr;
static lv_obj_t *_photo_placeholder = nullptr;
static lv_obj_t *_loading_spinner = nullptr;

static bool _visible = false;
static Aircraft _current_ac;

#define CARD_H 350
#define CARD_BG lv_color_hex(0x141428)
#define CARD_TEXT lv_color_hex(0xccccdd)
#define CARD_ACCENT lv_color_hex(0x4488ff)
#define CARD_DIM lv_color_hex(0x666688)

static void on_enrichment_ready(AircraftEnrichment *data) {
    if (!_visible) return;

    if (data->airline[0]) {
        lv_label_set_text(_airline_label, data->airline);
    }
    if (data->origin_airport[0] && data->destination_airport[0]) {
        lv_label_set_text_fmt(_route_label, "%s  ->  %s",
                              data->origin_airport, data->destination_airport);
    }
    if (data->photo_url[0]) {
        lv_label_set_text(_photo_placeholder, "Photo available");
        lv_obj_set_style_text_color(_photo_placeholder, CARD_ACCENT, 0);
    }

    if (_loading_spinner) lv_obj_add_flag(_loading_spinner, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_data_row(lv_obj_t *parent, const char *label_text,
                                int x, int y, lv_obj_t **value_label) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, CARD_DIM, 0);
    lv_obj_set_pos(lbl, x, y);

    *value_label = lv_label_create(parent);
    lv_label_set_text(*value_label, "--");
    lv_obj_set_style_text_font(*value_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(*value_label, CARD_TEXT, 0);
    lv_obj_set_pos(*value_label, x, y + 16);

    return lbl;
}

void detail_card_init(lv_obj_t *parent) {
    _card = lv_obj_create(parent);
    lv_obj_set_size(_card, LCD_H_RES, CARD_H);
    lv_obj_set_pos(_card, 0, LCD_V_RES); // start off-screen (below)
    lv_obj_set_style_bg_color(_card, CARD_BG, 0);
    lv_obj_set_style_bg_opa(_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_card, 0, 0);
    lv_obj_set_style_radius(_card, 12, 0);
    lv_obj_set_style_pad_all(_card, 16, 0);
    lv_obj_clear_flag(_card, LV_OBJ_FLAG_SCROLLABLE);

    // Drag handle indicator
    lv_obj_t *handle = lv_obj_create(_card);
    lv_obj_set_size(handle, 40, 4);
    lv_obj_set_style_bg_color(handle, CARD_DIM, 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(handle, 2, 0);
    lv_obj_set_style_border_width(handle, 0, 0);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, -8);

    // Callsign (large)
    _callsign_label = lv_label_create(_card);
    lv_obj_set_style_text_font(_callsign_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_callsign_label, lv_color_white(), 0);
    lv_obj_set_pos(_callsign_label, 0, 8);

    // Registration
    _reg_label = lv_label_create(_card);
    lv_obj_set_style_text_font(_reg_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_reg_label, CARD_DIM, 0);
    lv_obj_set_pos(_reg_label, 0, 40);

    // Airline
    _airline_label = lv_label_create(_card);
    lv_obj_set_style_text_font(_airline_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_airline_label, CARD_ACCENT, 0);
    lv_obj_set_pos(_airline_label, 0, 60);

    // Type
    _type_label = lv_label_create(_card);
    lv_obj_set_style_text_font(_type_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_type_label, CARD_TEXT, 0);
    lv_obj_set_pos(_type_label, 0, 82);

    // Route
    _route_label = lv_label_create(_card);
    lv_obj_set_style_text_font(_route_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_route_label, CARD_TEXT, 0);
    lv_obj_set_pos(_route_label, 0, 104);

    // Photo placeholder (right side)
    _photo_placeholder = lv_label_create(_card);
    lv_label_set_text(_photo_placeholder, "");
    lv_obj_set_style_text_font(_photo_placeholder, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_photo_placeholder, CARD_DIM, 0);
    lv_obj_set_pos(_photo_placeholder, 600, 8);

    // Loading spinner
    _loading_spinner = lv_spinner_create(_card);
    lv_obj_set_size(_loading_spinner, 30, 30);
    lv_obj_set_pos(_loading_spinner, 700, 16);
    lv_obj_add_flag(_loading_spinner, LV_OBJ_FLAG_HIDDEN);

    // Data grid — 2 rows
    int y1 = 140;
    make_data_row(_card, "ALTITUDE", 0, y1, &_alt_label);
    make_data_row(_card, "SPEED", 160, y1, &_spd_label);
    make_data_row(_card, "HEADING", 320, y1, &_hdg_label);
    make_data_row(_card, "V/S", 480, y1, &_vrate_label);
    make_data_row(_card, "SQUAWK", 640, y1, &_squawk_label);

    int y2 = 190;
    make_data_row(_card, "DISTANCE", 0, y2, &_dist_label);
    make_data_row(_card, "BEARING", 160, y2, &_bearing_label);
    make_data_row(_card, "STATUS", 320, y2, &_status_label);

    // Tap to close
    lv_obj_add_event_cb(_card, [](lv_event_t *e) {
        detail_card_hide();
    }, LV_EVENT_CLICKED, nullptr);

    _visible = false;
}

void detail_card_show(const Aircraft *ac) {
    memcpy(&_current_ac, ac, sizeof(Aircraft));

    // Populate labels
    lv_label_set_text(_callsign_label, ac->callsign[0] ? ac->callsign : ac->icao_hex);
    lv_label_set_text_fmt(_reg_label, "%s  |  %s", ac->registration, ac->icao_hex);
    lv_label_set_text(_type_label, ac->type_code);
    lv_label_set_text(_airline_label, "");
    lv_label_set_text(_route_label, "");
    lv_label_set_text(_photo_placeholder, "");

    // Show route from aircraft struct if available
    if (ac->origin[0] && ac->origin[0] != '-' && ac->dest[0] && ac->dest[0] != '-') {
        lv_label_set_text_fmt(_route_label, "%s -> %s", ac->origin, ac->dest);
    }

    // Live data
    if (ac->on_ground) lv_label_set_text(_alt_label, "GND");
    else lv_label_set_text_fmt(_alt_label, "%d ft", ac->altitude);
    lv_label_set_text_fmt(_spd_label, "%d kts", ac->speed);
    lv_label_set_text_fmt(_hdg_label, "%03d\xC2\xB0", ac->heading);
    lv_label_set_text_fmt(_vrate_label, "%+d fpm", ac->vert_rate);
    lv_label_set_text_fmt(_squawk_label, "%04d", ac->squawk);

    float dist = MapProjection::distance_nm(HOME_LAT, HOME_LON, ac->lat, ac->lon);
    lv_label_set_text_fmt(_dist_label, "%.1f nm", dist);

    // Bearing from home to aircraft
    float dlat = (ac->lat - HOME_LAT) * M_PI / 180.0f;
    float dlon = (ac->lon - HOME_LON) * M_PI / 180.0f;
    float y = sinf(dlon) * cosf(ac->lat * M_PI / 180.0f);
    float x = cosf(HOME_LAT * M_PI / 180.0f) * sinf(ac->lat * M_PI / 180.0f) -
              sinf(HOME_LAT * M_PI / 180.0f) * cosf(ac->lat * M_PI / 180.0f) * cosf(dlon);
    int bearing = (int)(atan2f(y, x) * 180.0f / M_PI + 360.0f) % 360;
    lv_label_set_text_fmt(_bearing_label, "%03d\xC2\xB0", bearing);

    // Flight status
    const char *status;
    if (ac->on_ground) status = "On Ground";
    else if (ac->vert_rate > 300) status = "Climbing";
    else if (ac->vert_rate < -300) status = "Descending";
    else status = "Cruising";
    lv_label_set_text(_status_label, status);

    // Slide in
    _visible = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, _card);
    lv_anim_set_values(&a, LCD_V_RES, LCD_V_RES - CARD_H);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
        lv_obj_set_y((lv_obj_t *)obj, v);
    });
    lv_anim_start(&a);

    // Fetch enrichment
    lv_obj_clear_flag(_loading_spinner, LV_OBJ_FLAG_HIDDEN);
    enrichment_fetch(ac->icao_hex, ac->registration, on_enrichment_ready);
}

void detail_card_hide() {
    if (!_visible) return;
    _visible = false;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, _card);
    lv_anim_set_values(&a, lv_obj_get_y(_card), LCD_V_RES);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
        lv_obj_set_y((lv_obj_t *)obj, v);
    });
    lv_anim_start(&a);
}

bool detail_card_is_visible() {
    return _visible;
}
