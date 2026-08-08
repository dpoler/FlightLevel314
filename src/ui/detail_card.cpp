#include "../platform/platform.h" // millis()/strlcpy compatibility shims on non-Arduino builds
#include "detail_card.h"
#include "geo.h"
#include "../data/storage.h"
#include "../data/locations.h"
#include "../data/enrichment.h"
#include "../data/airlines.h"
#include "airports_lookup.h"
#include "../pins_config.h"
#include <cstdio> // snprintf -- not reliably transitive under libstdc++ (Pi build)
#include <cstring>

static lv_obj_t *_card = nullptr;
#if LCD_H_RES >= 1280
static lv_obj_t *_summary_box = nullptr; // identity panel, upper-left on wide cards
#endif

// Header
static lv_obj_t *_callsign_label = nullptr;
static lv_obj_t *_badge_label = nullptr;

// Sub-header
static lv_obj_t *_reg_label = nullptr;

// Identity
static lv_obj_t *_operator_label = nullptr;
static lv_obj_t *_type_label = nullptr;
static lv_obj_t *_aircraft_detail_label = nullptr;
// FROM / TO route block (boarding-pass style two columns)
static lv_obj_t *_route_from_hdr = nullptr;
static lv_obj_t *_route_to_hdr = nullptr;
static lv_obj_t *_route_from_icao = nullptr;
static lv_obj_t *_route_to_icao = nullptr;
static lv_obj_t *_route_from_name = nullptr;
static lv_obj_t *_route_to_name = nullptr;
static lv_obj_t *_photo_credit_label = nullptr;
#if !defined(ARDUINO)
static lv_obj_t *_photo_img = nullptr;
static lv_image_dsc_t _photo_dsc;
static char _photo_shown_icao[7] = {};
#endif
// Data grid row 1 — flight state
static lv_obj_t *_alt_label = nullptr;
static lv_obj_t *_spd_label = nullptr;
static lv_obj_t *_hdg_label = nullptr;
static lv_obj_t *_vrate_label = nullptr;
static lv_obj_t *_squawk_label = nullptr;
static lv_obj_t *_status_label = nullptr;

// Data grid row 2 — position & tracking
static lv_obj_t *_dist_label = nullptr;
static lv_obj_t *_bearing_label = nullptr;
static lv_obj_t *_lat_label = nullptr;
static lv_obj_t *_lon_label = nullptr;
static lv_obj_t *_track_label = nullptr;
static lv_obj_t *_signal_label = nullptr;

// Data grid row 3 — extended flight params
static lv_obj_t *_mach_label = nullptr;
static lv_obj_t *_ias_label = nullptr;
static lv_obj_t *_tas_label = nullptr;
static lv_obj_t *_nav_alt_label = nullptr;
static lv_obj_t *_roll_label = nullptr;
static lv_obj_t *_qnh_label = nullptr;

// Live update timer
static lv_timer_t *_update_timer = nullptr;

static bool _visible = false;
static Aircraft _current_ac;
static AircraftList *_list = nullptr; // the live list -- update_timer_cb re-syncs _current_ac from this every tick

// Layout: jc1060 keeps the compact full-width stack. On the Pi's 1280x800
// the card is three columns — summary box (left), telemetry (middle),
// photo (right) — so stats aren't crushed into a single row under the
// image with the last line kissing the screen edge.
#if LCD_H_RES >= 1280
#define CARD_H         340
#define CARD_PAD       16
// Wide enough for FROM/TO columns under the identity block.
#define SUMMARY_W      400
#define SUMMARY_H      250
#define PHOTO_SLOT_W   400
#define PHOTO_SLOT_H   220
// Telemetry labels/values are short ("ALTITUDE", "FL350") — no need to
// pack the grid against the summary. Center a fixed-width 3-col block in
// the gap between summary and photo.
#define GRID_COL_W     140
#define STATS_W        (GRID_COL_W * 3)
#define MID_AVAIL      (LCD_H_RES - 2 * CARD_PAD - SUMMARY_W - PHOTO_SLOT_W)
#define STATS_X        (SUMMARY_W + (MID_AVAIL - STATS_W) / 2)
#define IDENTITY_MAX_W (SUMMARY_W - 24)
#define GRID_Y0        10
#define GRID_ROW_H     42
#define GRID_COLS      3
#else
#define CARD_H         340
#define CARD_PAD       16
#define PHOTO_SLOT_W   0
#define PHOTO_SLOT_H   0
#define STATS_X        0
#define IDENTITY_MAX_W (LCD_H_RES - 32)
// Room for FROM/TO route block under identity.
#define GRID_Y0        200
#define GRID_ROW_H     42
#define GRID_COL_W     160
#define GRID_COLS      6
#endif

#define CARD_BG lv_color_hex(0x141428)
#define CARD_SUMMARY_BG lv_color_hex(0x1a1a32)
#define CARD_SUMMARY_BORDER lv_color_hex(0x2a2a4a)
#define CARD_TEXT lv_color_hex(0xccccdd)
#define CARD_ACCENT lv_color_hex(0x4488ff)
#define CARD_DIM lv_color_hex(0x666688)

// ASCII separator — montserrat doesn't include U+00B7 (·), which rendered
// as empty rectangular tofu between summary fields.
#define SEP "  |  "

#define COL1 (STATS_X + 0 * GRID_COL_W)
#define COL2 (STATS_X + 1 * GRID_COL_W)
#define COL3 (STATS_X + 2 * GRID_COL_W)
#if GRID_COLS >= 6
#define COL4 (STATS_X + 3 * GRID_COL_W)
#define COL5 (STATS_X + 4 * GRID_COL_W)
#define COL6 (STATS_X + 5 * GRID_COL_W)
#endif

static const char *decode_category(const char *cat) {
    if (!cat[0]) return "";
    if (cat[0] == 'A') {
        switch (cat[1]) {
            case '0': return "Uncat";
            case '1': return "Light";
            case '2': return "Small";
            case '3': return "Large";
            case '4': return "High vortex";
            case '5': return "Heavy";
            case '6': return "High perf";
            case '7': return "Rotorcraft";
        }
    } else if (cat[0] == 'B') {
        switch (cat[1]) {
            case '1': return "Glider";
            case '2': return "Balloon";
            case '3': return "Parachute";
            case '4': return "Ultralight";
            case '6': return "UAV";
            case '7': return "Space";
        }
    } else if (cat[0] == 'C') {
        switch (cat[1]) {
            case '1': return "Emergency veh";
            case '2': return "Service veh";
            case '3': return "Obstruction";
        }
    }
    return "";
}

static const char *decode_squawk(uint16_t sq) {
    switch (sq) {
        case 7500: return "HIJACK";
        case 7600: return "RADIO FAIL";
        case 7700: return "EMERGENCY";
        case 1200: return "VFR";
        case 1000: return "IFR";
        case 2000: return "IFR unassigned";
        case 7000: return "VFR (EU)";
        default: return "";
    }
}

// Type line: "Desc (CODE)  |  Cat A3: Large" — keeps category with the
// aircraft identity instead of floating alone at a fixed x=500.
static void set_type_and_category(const char *desc_or_type, const char *type_code,
                                  const char *category) {
    char buf[96];
    int pos = 0;
    if (desc_or_type && desc_or_type[0]) {
        if (type_code && type_code[0] && strcmp(desc_or_type, type_code) != 0)
            pos = snprintf(buf, sizeof(buf), "%s (%s)", desc_or_type, type_code);
        else
            pos = snprintf(buf, sizeof(buf), "%s", desc_or_type);
    } else if (type_code && type_code[0]) {
        pos = snprintf(buf, sizeof(buf), "%s", type_code);
    } else {
        buf[0] = '\0';
    }

    const char *cat_desc = decode_category(category ? category : "");
    if (category && category[0]) {
        if (pos > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, SEP);
        if (cat_desc[0])
            snprintf(buf + pos, sizeof(buf) - pos, "Cat %s: %s", category, cat_desc);
        else
            snprintf(buf + pos, sizeof(buf) - pos, "Cat %s", category);
    }
    lv_label_set_text(_type_label, buf);
}

static void route_set_hidden(bool hidden) {
    auto apply = [&](lv_obj_t *o) {
        if (!o) return;
        if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    apply(_route_from_hdr);
    apply(_route_to_hdr);
    apply(_route_from_icao);
    apply(_route_to_icao);
    apply(_route_from_name);
    apply(_route_to_name);
}

// Drop common trailing words so FROM/TO name columns stay readable at
// half-width ("John F. Kennedy International Airport" → "John F. Kennedy").
static void shorten_airport_name(const char *full, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!full || !full[0]) return;
    static const char *const SUFFIXES[] = {
        " International Airport",
        " International",
        " Regional Airport",
        " Municipal Airport",
        " Airport",
        " Airfield",
        " Air Base",
        " AFB",
        nullptr
    };
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", full);
    for (int i = 0; SUFFIXES[i]; i++) {
        size_t bl = strlen(buf);
        size_t sl = strlen(SUFFIXES[i]);
        if (bl > sl && strcmp(buf + bl - sl, SUFFIXES[i]) == 0) {
            buf[bl - sl] = '\0';
            break;
        }
    }
    // Trim trailing spaces after stripping.
    size_t n = strlen(buf);
    while (n > 0 && buf[n - 1] == ' ') buf[--n] = '\0';
    snprintf(out, out_sz, "%s", buf[0] ? buf : full);
}

static void on_enrichment_ready(AircraftEnrichment *data) {
    if (!_visible) return;

    // Model — more detailed than bulk API desc; keep category on the same line.
    if (data->model[0]) {
        set_type_and_category(data->model, _current_ac.type_code, _current_ac.category);
    }

    // Aircraft details line: manufacturer | country year | engines
    // (registered owner dropped -- for airline aircraft it's almost always
    // just the same airline name already shown above, from airline_lookup())
    char detail[160] = {};
    int pos = 0;
    if (data->manufacturer[0]) {
        pos += snprintf(detail + pos, sizeof(detail) - pos, "%s", data->manufacturer);
    }
    if (data->registered_country[0]) {
        if (pos > 0) pos += snprintf(detail + pos, sizeof(detail) - pos, SEP);
        pos += snprintf(detail + pos, sizeof(detail) - pos, "%s", data->registered_country);
        if (data->year_built > 0) {
            pos += snprintf(detail + pos, sizeof(detail) - pos, " %d", data->year_built);
        }
    } else if (data->year_built > 0) {
        if (pos > 0) pos += snprintf(detail + pos, sizeof(detail) - pos, SEP);
        pos += snprintf(detail + pos, sizeof(detail) - pos, "Built %d", data->year_built);
    }
    if (data->engine_count > 0 && data->engine_type[0]) {
        if (pos > 0) pos += snprintf(detail + pos, sizeof(detail) - pos, SEP);
        snprintf(detail + pos, sizeof(detail) - pos, "%dx %s", data->engine_count, data->engine_type);
    }
    if (detail[0]) lv_label_set_text(_aircraft_detail_label, detail);

    if (data->origin_icao[0] || data->dest_icao[0]) {
        const char *o = data->origin_icao[0] ? data->origin_icao : "----";
        const char *d = data->dest_icao[0] ? data->dest_icao : "----";
        char oname[64] = {}, dname[64] = {};
        char oshort[48] = {}, dshort[48] = {};
        airports_format_name(data->origin_icao, oname, sizeof(oname));
        airports_format_name(data->dest_icao, dname, sizeof(dname));
        shorten_airport_name(oname, oshort, sizeof(oshort));
        shorten_airport_name(dname, dshort, sizeof(dshort));

        lv_label_set_text(_route_from_icao, o);
        lv_label_set_text(_route_to_icao, d);
        lv_label_set_text(_route_from_name, oshort[0] ? oshort : "—");
        lv_label_set_text(_route_to_name, dshort[0] ? dshort : "—");
        route_set_hidden(false);
    }

#if !defined(ARDUINO)
    if (data->photo_rgb565 && data->photo_w > 0 && data->photo_h > 0 && _photo_img) {
        memset(&_photo_dsc, 0, sizeof(_photo_dsc));
        _photo_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        _photo_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        _photo_dsc.header.w = data->photo_w;
        _photo_dsc.header.h = data->photo_h;
        _photo_dsc.header.stride = (uint32_t)data->photo_w * 2;
        _photo_dsc.data_size = (uint32_t)data->photo_w * (uint32_t)data->photo_h * 2;
        _photo_dsc.data = data->photo_rgb565;
        lv_image_set_src(_photo_img, &_photo_dsc);
        lv_obj_set_size(_photo_img, data->photo_w, data->photo_h);
        lv_obj_align(_photo_img, LV_ALIGN_TOP_RIGHT, 0, 8);
        lv_obj_clear_flag(_photo_img, LV_OBJ_FLAG_HIDDEN);
        strlcpy(_photo_shown_icao, _current_ac.icao_hex, sizeof(_photo_shown_icao));

        if (data->photo_photographer[0]) {
            lv_label_set_text_fmt(_photo_credit_label, "Photo: %s", data->photo_photographer);
            lv_obj_clear_flag(_photo_credit_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align_to(_photo_credit_label, _photo_img, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 4);
            lv_obj_set_style_text_align(_photo_credit_label, LV_TEXT_ALIGN_RIGHT, 0);
        }
    } else
#endif
    if (data->photo_photographer[0]) {
        // ESP32 (or Pi with metadata but no pixels): credit under identity.
        lv_label_set_text_fmt(_photo_credit_label, "Photo: %s", data->photo_photographer);
        lv_obj_clear_flag(_photo_credit_label, LV_OBJ_FLAG_HIDDEN);
#if LCD_H_RES >= 1280
        lv_obj_set_pos(_photo_credit_label, 10, SUMMARY_H - 22);
#else
        lv_obj_set_pos(_photo_credit_label, 0, 118);
#endif
        lv_obj_set_style_text_align(_photo_credit_label, LV_TEXT_ALIGN_LEFT, 0);
    }
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

// Renders everything in the data grid (rows 1-3) from `ac`. Shared by
// detail_card_show()'s initial paint and update_timer_cb()'s per-tick
// refresh -- previously this only ever ran once, at the moment the card was
// tapped open, so DIST/BEARING/ALT/SPD/position/etc. all froze at whatever
// they were then. That was most visible as "switching to a different active
// location doesn't update DIST/BEARING for an aircraft visible from both"
// (reported), but it was really a general staleness bug, not something
// specific to switching -- none of this ever tracked live telemetry updates
// either, switching or not.
static void render_grid(const Aircraft *ac) {
    // === DATA GRID ROW 1 — flight state ===
    if (ac->on_ground) {
        lv_label_set_text(_alt_label, "GND");
    } else if (ac->altitude >= 18000) {
        lv_label_set_text_fmt(_alt_label, "FL%d", ac->altitude / 100);
    } else {
        lv_label_set_text_fmt(_alt_label, "%d ft", ac->altitude);
    }

    lv_label_set_text_fmt(_spd_label, "%d kts", ac->speed);
    lv_label_set_text_fmt(_hdg_label, "%03d", ac->heading);
    lv_label_set_text_fmt(_vrate_label, "%+d fpm", ac->vert_rate);
    lv_label_set_text_fmt(_squawk_label, "%04d", ac->squawk);

    const char *status;
    if (ac->on_ground) status = "On Ground";
    else if (ac->vert_rate > 300) status = "Climbing";
    else if (ac->vert_rate < -300) status = "Descending";
    else status = "Cruising";
    lv_label_set_text(_status_label, status);

    // === DATA GRID ROW 2 — position & tracking ===
    // DIST/BEARING measure from whichever location is actually active, not
    // a hardcoded Home -- this was a real, latent bug found while removing
    // Home as a special case: unlike stats.cpp's CLOSEST record (already
    // fixed for exactly this in an earlier session), this always read
    // g_config.home_lat/lon regardless of which saved location was being
    // viewed. Stays 0,0 if nothing's selected (harmless -- the detail card
    // isn't reachable with an empty aircraft list anyway). Recomputed every
    // call (not just once at tap time) so switching the active location
    // updates these for whatever aircraft the card is already showing.
    float ref_lat = 0, ref_lon = 0;
    locations_get_active_coords(&ref_lat, &ref_lon, nullptr);

    float dist = MapProjection::distance_nm(ref_lat, ref_lon, ac->lat, ac->lon);
    lv_label_set_text_fmt(_dist_label, "%.1f nm", dist);

    float dlon = (ac->lon - ref_lon) * M_PI / 180.0f;
    float y = sinf(dlon) * cosf(ac->lat * M_PI / 180.0f);
    float x = cosf(ref_lat * M_PI / 180.0f) * sinf(ac->lat * M_PI / 180.0f) -
              sinf(ref_lat * M_PI / 180.0f) * cosf(ac->lat * M_PI / 180.0f) * cosf(dlon);
    int bearing = (int)(atan2f(y, x) * 180.0f / M_PI + 360.0f) % 360;
    lv_label_set_text_fmt(_bearing_label, "%03d", bearing);

    lv_label_set_text_fmt(_lat_label, "%.4f", ac->lat);
    lv_label_set_text_fmt(_lon_label, "%.4f", ac->lon);

    // Trail stats
    if (ac->trail_count >= 2) {
        uint32_t dur_ms = ac->trail[ac->trail_count - 1].timestamp - ac->trail[0].timestamp;
        uint32_t secs = dur_ms / 1000;
        if (secs < 60) {
            lv_label_set_text_fmt(_track_label, "%d pts %lus",
                ac->trail_count, (unsigned long)secs);
        } else {
            lv_label_set_text_fmt(_track_label, "%d pts %lum%lus",
                ac->trail_count, (unsigned long)(secs / 60), (unsigned long)(secs % 60));
        }
    } else if (ac->trail_count == 1) {
        lv_label_set_text(_track_label, "1 pt");
    } else {
        lv_label_set_text(_track_label, "new");
    }

    // Signal age
    uint32_t age_ms = millis() - ac->last_seen;
    if (age_ms < 1000) {
        lv_label_set_text(_signal_label, "live");
    } else if (age_ms < 60000) {
        lv_label_set_text_fmt(_signal_label, "%lus", (unsigned long)(age_ms / 1000));
    } else {
        lv_label_set_text_fmt(_signal_label, "%lum%lus",
            (unsigned long)(age_ms / 60000), (unsigned long)((age_ms / 1000) % 60));
    }

    // === DATA GRID ROW 3 — extended flight params ===
    if (ac->mach > 0.01f) {
        lv_label_set_text_fmt(_mach_label, "%.3f", ac->mach);
    } else {
        lv_label_set_text(_mach_label, "--");
    }

    if (ac->ias > 0) {
        lv_label_set_text_fmt(_ias_label, "%d kts", ac->ias);
    } else {
        lv_label_set_text(_ias_label, "--");
    }

    if (ac->tas > 0) {
        lv_label_set_text_fmt(_tas_label, "%d kts", ac->tas);
    } else {
        lv_label_set_text(_tas_label, "--");
    }

    if (ac->nav_altitude > 0) {
        if (ac->nav_altitude >= 18000) {
            lv_label_set_text_fmt(_nav_alt_label, "FL%d", ac->nav_altitude / 100);
        } else {
            lv_label_set_text_fmt(_nav_alt_label, "%d ft", ac->nav_altitude);
        }
    } else {
        lv_label_set_text(_nav_alt_label, "--");
    }

    // Roll angle
    if (ac->roll != 0.0f) {
        lv_label_set_text_fmt(_roll_label, "%.1f%s", ac->roll, ac->roll > 0 ? " R" : " L");
    } else {
        lv_label_set_text(_roll_label, "--");
    }

    // Altimeter QNH
    if (ac->nav_qnh > 0.0f) {
        lv_label_set_text_fmt(_qnh_label, "%.1f hPa", ac->nav_qnh);
    } else {
        lv_label_set_text(_qnh_label, "--");
    }
}

static void update_timer_cb(lv_timer_t *timer) {
    if (!_visible) return;

    // Re-sync _current_ac from the live list every tick, by icao_hex --
    // without this the card was a frozen snapshot of whatever the aircraft
    // looked like the instant it was tapped, never reflecting its actual
    // live telemetry (position, altitude, speed...) or a since-changed
    // active location's DIST/BEARING. Left as the last-known snapshot (not
    // cleared/dismissed) if the aircraft is no longer in the live list --
    // e.g. it went fully stale, or a location switch just cleared the list
    // and a fresh fetch for the new location hasn't landed yet -- so the
    // card still shows something rather than going blank, and signal age
    // (computed from the frozen _current_ac.last_seen in that case) keeps
    // climbing to make clear it is not current.
    if (_list && _list->lock(pdMS_TO_TICKS(20))) {
        for (int i = 0; i < _list->count; i++) {
            if (strcmp(_list->aircraft[i].icao_hex, _current_ac.icao_hex) == 0) {
                memcpy(&_current_ac, &_list->aircraft[i], sizeof(Aircraft));
                break;
            }
        }
        _list->unlock();
    }

    render_grid(&_current_ac);
}

void detail_card_init(lv_obj_t *parent, AircraftList *list) {
    _list = list;
    _card = lv_obj_create(parent);
    lv_obj_set_size(_card, LCD_H_RES, CARD_H);
    lv_obj_set_pos(_card, 0, LCD_V_RES); // start off-screen (below)
    lv_obj_set_style_bg_color(_card, CARD_BG, 0);
    lv_obj_set_style_bg_opa(_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_card, 0, 0);
    lv_obj_set_style_radius(_card, 12, 0);
    lv_obj_set_style_pad_all(_card, CARD_PAD, 0);
    lv_obj_set_style_clip_corner(_card, true, 0);
    // Sized to fit — no scroll/scrollbar.
    lv_obj_clear_flag(_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(_card, LV_SCROLLBAR_MODE_OFF);

    // Drag handle indicator
    lv_obj_t *handle = lv_obj_create(_card);
    lv_obj_set_size(handle, 40, 4);
    lv_obj_set_style_bg_color(handle, CARD_DIM, 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(handle, 2, 0);
    lv_obj_set_style_border_width(handle, 0, 0);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, -8);

#if LCD_H_RES >= 1280
    // Summary box — visually separates identity from the telemetry column.
    _summary_box = lv_obj_create(_card);
    lv_obj_set_size(_summary_box, SUMMARY_W, SUMMARY_H);
    lv_obj_set_pos(_summary_box, 0, 8);
    lv_obj_set_style_bg_color(_summary_box, CARD_SUMMARY_BG, 0);
    lv_obj_set_style_bg_opa(_summary_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_summary_box, CARD_SUMMARY_BORDER, 0);
    lv_obj_set_style_border_width(_summary_box, 1, 0);
    lv_obj_set_style_radius(_summary_box, 8, 0);
    lv_obj_set_style_pad_all(_summary_box, 10, 0);
    lv_obj_clear_flag(_summary_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(_summary_box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(_summary_box, LV_OBJ_FLAG_CLICKABLE); // taps fall through to card close

    lv_obj_t *id_parent = _summary_box;
    const int id_x = 0;
    const int y_call = 0;
    const int y_reg = 32;
    const int y_op = 54;
    const int y_type = 76;
    const int y_detail = 98;
    const int y_route_hdr = 122;
    const int y_route_icao = 140;
    const int y_route_name = 162;
#else
    lv_obj_t *id_parent = _card;
    const int id_x = 0;
    const int y_call = 0;
    const int y_reg = 34;
    const int y_op = 56;
    const int y_type = 78;
    const int y_detail = 98;
    const int y_route_hdr = 120;
    const int y_route_icao = 138;
    const int y_route_name = 160;
#endif

    // === HEADER / IDENTITY ===
    _callsign_label = lv_label_create(id_parent);
    lv_obj_set_style_text_font(_callsign_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_callsign_label, lv_color_white(), 0);
    lv_obj_set_pos(_callsign_label, id_x, y_call);
    lv_obj_set_width(_callsign_label, IDENTITY_MAX_W - 120);
    lv_label_set_long_mode(_callsign_label, LV_LABEL_LONG_CLIP);

    _badge_label = lv_label_create(id_parent);
    lv_obj_set_style_text_font(_badge_label, &lv_font_montserrat_14, 0);
    lv_obj_align_to(_badge_label, _callsign_label, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
    lv_label_set_text(_badge_label, "");

    _reg_label = lv_label_create(id_parent);
    lv_obj_set_style_text_font(_reg_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_reg_label, CARD_DIM, 0);
    lv_obj_set_pos(_reg_label, id_x, y_reg);
    lv_obj_set_width(_reg_label, IDENTITY_MAX_W);
    lv_label_set_long_mode(_reg_label, LV_LABEL_LONG_CLIP);

    _operator_label = lv_label_create(id_parent);
    lv_obj_set_style_text_font(_operator_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(_operator_label, CARD_ACCENT, 0);
    lv_obj_set_pos(_operator_label, id_x, y_op);
    lv_obj_set_width(_operator_label, IDENTITY_MAX_W);
    lv_label_set_long_mode(_operator_label, LV_LABEL_LONG_CLIP);

    _type_label = lv_label_create(id_parent);
    lv_obj_set_style_text_font(_type_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_type_label, CARD_TEXT, 0);
    lv_obj_set_pos(_type_label, id_x, y_type);
    lv_obj_set_width(_type_label, IDENTITY_MAX_W);
    lv_label_set_long_mode(_type_label, LV_LABEL_LONG_CLIP);

    _aircraft_detail_label = lv_label_create(id_parent);
    lv_label_set_text(_aircraft_detail_label, "");
    lv_obj_set_style_text_font(_aircraft_detail_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_aircraft_detail_label, CARD_DIM, 0);
    lv_obj_set_pos(_aircraft_detail_label, id_x, y_detail);
    lv_obj_set_width(_aircraft_detail_label, IDENTITY_MAX_W);
    lv_label_set_long_mode(_aircraft_detail_label, LV_LABEL_LONG_CLIP);

    // FROM / TO — two columns, boarding-pass style (no arrow).
    const int route_gap = 12;
    const int route_col_w = (IDENTITY_MAX_W - route_gap) / 2;
    const int route_to_x = id_x + route_col_w + route_gap;

    auto make_route_hdr = [&](const char *text, int x) {
        lv_obj_t *lbl = lv_label_create(id_parent);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, CARD_DIM, 0);
        lv_obj_set_pos(lbl, x, y_route_hdr);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        return lbl;
    };
    auto make_route_icao = [&](int x) {
        lv_obj_t *lbl = lv_label_create(id_parent);
        lv_label_set_text(lbl, "");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl, CARD_ACCENT, 0);
        lv_obj_set_pos(lbl, x, y_route_icao);
        lv_obj_set_width(lbl, route_col_w);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        return lbl;
    };
    auto make_route_name = [&](int x) {
        lv_obj_t *lbl = lv_label_create(id_parent);
        lv_label_set_text(lbl, "");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, CARD_DIM, 0);
        lv_obj_set_pos(lbl, x, y_route_name);
        lv_obj_set_width(lbl, route_col_w);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        return lbl;
    };

    _route_from_hdr = make_route_hdr("FROM", id_x);
    _route_to_hdr = make_route_hdr("TO", route_to_x);
    _route_from_icao = make_route_icao(id_x);
    _route_to_icao = make_route_icao(route_to_x);
    _route_from_name = make_route_name(id_x);
    _route_to_name = make_route_name(route_to_x);

    _photo_credit_label = lv_label_create(_card);
    lv_label_set_text(_photo_credit_label, "");
    lv_obj_set_style_text_font(_photo_credit_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_photo_credit_label, CARD_DIM, 0);
    lv_obj_set_pos(_photo_credit_label, 0, 118);
    lv_obj_add_flag(_photo_credit_label, LV_OBJ_FLAG_HIDDEN);

#if !defined(ARDUINO) && LCD_H_RES >= 1280
    _photo_img = lv_image_create(_card);
    lv_obj_set_size(_photo_img, PHOTO_SLOT_W, PHOTO_SLOT_H);
    lv_obj_align(_photo_img, LV_ALIGN_TOP_RIGHT, 0, 8);
    lv_obj_add_flag(_photo_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_photo_img, LV_OBJ_FLAG_CLICKABLE);
#endif

    // === DATA GRID ===
    // Wide: 3 columns × 6 rows in the middle band (beside summary + photo).
    // Narrow: original 6 columns × 3 rows under the identity block.
#if LCD_H_RES >= 1280
    int y0 = GRID_Y0;
    make_data_row(_card, "ALTITUDE", COL1, y0 + 0 * GRID_ROW_H, &_alt_label);
    make_data_row(_card, "GND SPD",  COL2, y0 + 0 * GRID_ROW_H, &_spd_label);
    make_data_row(_card, "HEADING",  COL3, y0 + 0 * GRID_ROW_H, &_hdg_label);

    make_data_row(_card, "V/S",      COL1, y0 + 1 * GRID_ROW_H, &_vrate_label);
    make_data_row(_card, "SQUAWK",   COL2, y0 + 1 * GRID_ROW_H, &_squawk_label);
    make_data_row(_card, "STATUS",   COL3, y0 + 1 * GRID_ROW_H, &_status_label);

    make_data_row(_card, "DISTANCE", COL1, y0 + 2 * GRID_ROW_H, &_dist_label);
    make_data_row(_card, "BEARING",  COL2, y0 + 2 * GRID_ROW_H, &_bearing_label);
    make_data_row(_card, "LAT",      COL3, y0 + 2 * GRID_ROW_H, &_lat_label);

    make_data_row(_card, "LON",      COL1, y0 + 3 * GRID_ROW_H, &_lon_label);
    make_data_row(_card, "TRACKED",  COL2, y0 + 3 * GRID_ROW_H, &_track_label);
    make_data_row(_card, "SIGNAL",   COL3, y0 + 3 * GRID_ROW_H, &_signal_label);

    make_data_row(_card, "MACH",     COL1, y0 + 4 * GRID_ROW_H, &_mach_label);
    make_data_row(_card, "IAS",      COL2, y0 + 4 * GRID_ROW_H, &_ias_label);
    make_data_row(_card, "TAS",      COL3, y0 + 4 * GRID_ROW_H, &_tas_label);

    make_data_row(_card, "NAV ALT",  COL1, y0 + 5 * GRID_ROW_H, &_nav_alt_label);
    make_data_row(_card, "ROLL",     COL2, y0 + 5 * GRID_ROW_H, &_roll_label);
    make_data_row(_card, "QNH",      COL3, y0 + 5 * GRID_ROW_H, &_qnh_label);
#else
    int y1 = GRID_Y0;
    make_data_row(_card, "ALTITUDE", COL1, y1, &_alt_label);
    make_data_row(_card, "GND SPD", COL2, y1, &_spd_label);
    make_data_row(_card, "HEADING", COL3, y1, &_hdg_label);
    make_data_row(_card, "V/S", COL4, y1, &_vrate_label);
    make_data_row(_card, "SQUAWK", COL5, y1, &_squawk_label);
    make_data_row(_card, "STATUS", COL6, y1, &_status_label);

    int y2 = GRID_Y0 + GRID_ROW_H;
    make_data_row(_card, "DISTANCE", COL1, y2, &_dist_label);
    make_data_row(_card, "BEARING", COL2, y2, &_bearing_label);
    make_data_row(_card, "LAT", COL3, y2, &_lat_label);
    make_data_row(_card, "LON", COL4, y2, &_lon_label);
    make_data_row(_card, "TRACKED", COL5, y2, &_track_label);
    make_data_row(_card, "SIGNAL", COL6, y2, &_signal_label);

    int y3 = GRID_Y0 + 2 * GRID_ROW_H;
    make_data_row(_card, "MACH", COL1, y3, &_mach_label);
    make_data_row(_card, "IAS", COL2, y3, &_ias_label);
    make_data_row(_card, "TAS", COL3, y3, &_tas_label);
    make_data_row(_card, "NAV ALT", COL4, y3, &_nav_alt_label);
    make_data_row(_card, "ROLL", COL5, y3, &_roll_label);
    make_data_row(_card, "QNH", COL6, y3, &_qnh_label);
#endif

    // Tap to close
    lv_obj_add_event_cb(_card, [](lv_event_t *e) {
        detail_card_hide();
    }, LV_EVENT_CLICKED, nullptr);

    // Live update timer (1s) — starts paused
    _update_timer = lv_timer_create(update_timer_cb, 1000, nullptr);
    lv_timer_pause(_update_timer);

    _visible = false;
}

void detail_card_show(const Aircraft *ac) {
    memcpy(&_current_ac, ac, sizeof(Aircraft));

    // === HEADER ===
    lv_label_set_text(_callsign_label, ac->callsign[0] ? ac->callsign : ac->icao_hex);
    // Re-anchor badge after callsign text width changes
    lv_obj_align_to(_badge_label, _callsign_label, LV_ALIGN_OUT_RIGHT_MID, 12, 0);

    // Military / Emergency badge
    if (ac->is_emergency) {
        const char *emg = ac->squawk == 7500 ? "HIJACK" : ac->squawk == 7600 ? "RADIO FAIL" : "EMERGENCY";
        lv_label_set_text(_badge_label, emg);
        lv_obj_set_style_text_color(_badge_label, lv_color_hex(0xff3333), 0);
    } else if (ac->is_military) {
        lv_label_set_text(_badge_label, "MILITARY");
        lv_obj_set_style_text_color(_badge_label, lv_color_hex(0xffaa00), 0);
    } else {
        lv_label_set_text(_badge_label, "");
    }

    // Sub-header: Reg | ICAO | Squawk with decode
    const char *sq_decode = decode_squawk(ac->squawk);
    if (sq_decode[0]) {
        lv_label_set_text_fmt(_reg_label, "%s" SEP "%s" SEP "Sq %04d (%s)",
                              ac->registration, ac->icao_hex, ac->squawk, sq_decode);
    } else {
        lv_label_set_text_fmt(_reg_label, "%s" SEP "%s" SEP "Sq %04d",
                              ac->registration, ac->icao_hex, ac->squawk);
    }

    // === IDENTITY ===
    // Airline name from callsign prefix (airlines.h) preferred — more
    // consistently present than the ADS-B owner_op field, and reflects who's
    // actually operating this flight (relevant for leased/wet-leased
    // aircraft where owner_op may show a leasing company instead).
    const AirlineEntry *airline = airline_lookup(ac->callsign);
    if (airline) {
        lv_label_set_text(_operator_label, airline->name);
    } else {
        lv_label_set_text(_operator_label, ac->owner_op[0] ? ac->owner_op : "");
    }

    if (ac->desc[0]) {
        set_type_and_category(ac->desc, ac->type_code, ac->category);
    } else {
        set_type_and_category(ac->type_code, nullptr, ac->category);
    }

    // Clear enrichment fields
    lv_label_set_text(_aircraft_detail_label, "");
    if (_route_from_icao) lv_label_set_text(_route_from_icao, "");
    if (_route_to_icao) lv_label_set_text(_route_to_icao, "");
    if (_route_from_name) lv_label_set_text(_route_from_name, "");
    if (_route_to_name) lv_label_set_text(_route_to_name, "");
    route_set_hidden(true);
    lv_label_set_text(_photo_credit_label, "");
    lv_obj_add_flag(_photo_credit_label, LV_OBJ_FLAG_HIDDEN);
#if !defined(ARDUINO)
    if (_photo_img) {
        lv_obj_add_flag(_photo_img, LV_OBJ_FLAG_HIDDEN);
        _photo_shown_icao[0] = '\0';
    }
#endif

    render_grid(ac);

    // === Slide in ===
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

    // Start live update timer
    lv_timer_resume(_update_timer);

    // Enrichment (adsbdb + planespotters; on Pi also photo decode + optional
    // AeroDataBox origin/destination). ESP32 only gets text fields /
    // photographer credit (image path is broken).
    enrichment_fetch(ac->icao_hex, ac->registration, ac->callsign, on_enrichment_ready);
}

void detail_card_hide() {
    if (!_visible) return;
    _visible = false;

#if !defined(ARDUINO)
    // Drop the image src before any cache slot can free photo_rgb565.
    if (_photo_img) {
        lv_obj_add_flag(_photo_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(_photo_img, nullptr);
        _photo_shown_icao[0] = '\0';
    }
#endif

    // Pause live updates
    lv_timer_pause(_update_timer);

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

void detail_card_scroll(int delta) {
    // Card is no longer scrollable — kept for API compat (swipe may still call).
    (void)delta;
}
