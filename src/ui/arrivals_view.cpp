#include <Arduino.h>
#include "arrivals_view.h"
#include "views.h"
#include "detail_card.h"
#include "../config.h"
#include "../pins_config.h"
#include "geo.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "range.h"

static AircraftList *_list = nullptr;
static lv_obj_t *_board_container = nullptr;
static lv_obj_t *_range_label = nullptr;

#define BOARD_W LCD_H_RES
#define BOARD_H (LCD_V_RES - 30)

// Split-flap cell dimensions
#define CELL_W 18
#define CELL_H 28
#define CELL_GAP 2
#define CELL_RADIUS 3
#define ROW_H (CELL_H + 4)
#define TITLE_H 30
#define COL_HEADER_H 18
#define HEADER_H (TITLE_H + COL_HEADER_H)
#define MAX_ROWS 16

// Sort modes
enum SortMode {
    SORT_NONE = 0,
    SORT_ASC,
    SORT_DESC,
};

// Sortable column indices (into columns[])
#define COL_FLIGHT 0
#define COL_TYPE   1
#define COL_ROUTE  2
#define COL_ALT    3
#define COL_SPD    4
#define COL_DIST   5
#define COL_HDG    6
#define COL_STATUS 7

static int  _sort_col  = COL_DIST;   // which column is sorted
static SortMode _sort_dir = SORT_ASC; // ascending by default

// Colors
#define BOARD_BG      lv_color_hex(0x0c0c0c)
#define CELL_BG       lv_color_hex(0x1a1a1a)
#define CELL_TEXT     lv_color_hex(0xffdd00)  // classic Solari yellow
#define HEADER_TEXT   lv_color_hex(0xffffff)
#define HEADER_BG     lv_color_hex(0x222222)
#define EMERGENCY_CLR lv_color_hex(0xff3333)
#define MILITARY_CLR  lv_color_hex(0xffaa44)

// Column definitions: name, character width, x offset
struct Column {
    const char *name;
    int chars;
    int x;
    bool sortable;
};

static Column columns[] = {
    {"FLIGHT",   8,  10,  true},
    {"TYPE",     4,  180, false},
    {"ROUTE",    7,  280, false},
    {"ALT",      4,  430, true},
    {"SPD",      3,  520, true},
    {"DIST",     4,  600, true},
    {"HDG",      3,  700, false},
    {"STATUS",   7,  780, false},
};
#define NUM_COLS 8

// Per-cell animation state
struct FlipCell {
    lv_obj_t *label;
    char target;
    char current;
    int rolls_remaining; // 0 = settled, >0 = still flipping
};

struct BoardRow {
    FlipCell cells[48]; // max characters across all columns
    int total_cells;
    char icao_hex[7];   // to track which aircraft this row represents
    bool active;
};

static BoardRow _rows[MAX_ROWS];
static lv_obj_t *_header_labels[8];
static lv_obj_t *_title_label = nullptr;

static const char *status_from_vert_rate(int16_t vr, bool on_ground) {
    if (on_ground) return "GROUND ";
    if (vr > 300) return "CLIMB  ";
    if (vr < -300) return "DESCEND";
    return "CRUISE ";
}

// Create a single flip cell (character tile)
static FlipCell create_cell(lv_obj_t *parent, int x, int y) {
    FlipCell cell;
    cell.target = ' ';
    cell.current = ' ';
    cell.rolls_remaining = 0;

    // Background tile
    lv_obj_t *bg = lv_obj_create(parent);
    lv_obj_set_size(bg, CELL_W, CELL_H);
    lv_obj_set_pos(bg, x, y);
    lv_obj_set_style_bg_color(bg, CELL_BG, 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bg, CELL_RADIUS, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_pad_all(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE); // let clicks pass through to board_container

    // Character label
    cell.label = lv_label_create(bg);
    lv_label_set_text(cell.label, " ");
    lv_obj_set_style_text_font(cell.label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(cell.label, CELL_TEXT, 0);
    lv_obj_center(cell.label);

    return cell;
}

static void init_rows(lv_obj_t *parent) {
    for (int row = 0; row < MAX_ROWS; row++) {
        int y = HEADER_H + row * ROW_H + 4;
        int cell_idx = 0;
        _rows[row].active = false;
        memset(_rows[row].icao_hex, 0, sizeof(_rows[row].icao_hex));

        for (int col = 0; col < NUM_COLS; col++) {
            for (int ch = 0; ch < columns[col].chars; ch++) {
                int x = columns[col].x + ch * (CELL_W + CELL_GAP);
                _rows[row].cells[cell_idx] = create_cell(parent, x, y);
                cell_idx++;
            }
        }
        _rows[row].total_cells = cell_idx;
    }
}

// Set a row's target text — animate only for genuinely new aircraft
static void set_row_text(int row, const char *texts[], lv_color_t color, bool should_animate) {
    int cell_idx = 0;
    for (int col = 0; col < NUM_COLS; col++) {
        const char *text = texts[col];
        int len = strlen(text);
        for (int ch = 0; ch < columns[col].chars; ch++) {
            char target = (ch < len) ? text[ch] : ' ';
            FlipCell &fc = _rows[row].cells[cell_idx];

            // Set color
            lv_obj_set_style_text_color(fc.label, color, 0);

            if (target != fc.target) {
                fc.target = target;
                if (should_animate) {
                    // Flip animation for genuinely new aircraft
                    fc.rolls_remaining = 2 + (rand() % 3);
                } else {
                    // Instant update
                    fc.rolls_remaining = 0;
                    fc.current = target;
                    char buf[2] = {target, 0};
                    lv_label_set_text(fc.label, buf);
                }
            }
            cell_idx++;
        }
    }
}

// Animation tick — called frequently to advance flip animations
static void flip_animation_tick(lv_timer_t *t) {
    if (views_get_active_index() != VIEW_ARRIVALS) return;

    static const char flip_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -./";

    for (int row = 0; row < MAX_ROWS; row++) {
        if (!_rows[row].active) continue;
        for (int c = 0; c < _rows[row].total_cells; c++) {
            FlipCell &fc = _rows[row].cells[c];
            if (fc.rolls_remaining > 0) {
                // Show random character
                char random_char = flip_chars[rand() % (sizeof(flip_chars) - 1)];
                char buf[2] = {random_char, 0};
                lv_label_set_text(fc.label, buf);
                fc.rolls_remaining--;
            } else if (fc.current != fc.target) {
                // Settle on target
                char buf[2] = {fc.target, 0};
                lv_label_set_text(fc.label, buf);
                fc.current = fc.target;
            }
        }
    }
}

// Temporary struct for sorting aircraft indices
struct SortEntry {
    int index;
    float dist_nm;  // cached for SORT_DIST
};

static int sort_compare(const void *a, const void *b) {
    const SortEntry *ea = (const SortEntry *)a;
    const SortEntry *eb = (const SortEntry *)b;
    if (_sort_dir == SORT_NONE) return 0;

    int cmp = 0;
    const Aircraft &aa = _list->aircraft[ea->index];
    const Aircraft &ab = _list->aircraft[eb->index];

    switch (_sort_col) {
        case COL_FLIGHT:
            cmp = strcasecmp(
                aa.callsign[0] ? aa.callsign : aa.icao_hex,
                ab.callsign[0] ? ab.callsign : ab.icao_hex);
            break;
        case COL_ALT:
            cmp = (aa.altitude > ab.altitude) - (aa.altitude < ab.altitude);
            break;
        case COL_SPD:
            cmp = (aa.speed > ab.speed) - (aa.speed < ab.speed);
            break;
        case COL_DIST:
            if (ea->dist_nm < eb->dist_nm) cmp = -1;
            else if (ea->dist_nm > eb->dist_nm) cmp = 1;
            break;
    }

    return (_sort_dir == SORT_DESC) ? -cmp : cmp;
}

// Fill all rows with random gibberish (split-flap reset effect)
// When true, next update_board renders instantly (no flip animation) — gibberish→data transition
static bool _awaiting_data = true;

static void reset_board_gibberish() {
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int row = 0; row < MAX_ROWS; row++) {
        _rows[row].active = true;
        memset(_rows[row].icao_hex, 0, sizeof(_rows[row].icao_hex));
        for (int c = 0; c < _rows[row].total_cells; c++) {
            FlipCell &fc = _rows[row].cells[c];
            char ch = chars[rand() % (sizeof(chars) - 1)];
            char buf[2] = {ch, 0};
            lv_label_set_text(fc.label, buf);
            fc.current = ch;
            fc.target = ch;
            fc.rolls_remaining = 0;
        }
    }
    _awaiting_data = true;
}

static float _last_range_nm = -1;

// Update board data from aircraft list
static void update_board(lv_timer_t *t) {
    if (views_get_active_index() != VIEW_ARRIVALS) return;
    if (!_list->lock(pdMS_TO_TICKS(5))) return; // short timeout: skip update if data locked

    // Detect range change — reset board with gibberish and skip this cycle
    float current_range = range_get_nm();
    if (_last_range_nm >= 0 && current_range != _last_range_nm) {
        _last_range_nm = current_range;
        reset_board_gibberish();
        lv_label_set_text_fmt(_title_label, "OVERHEAD TRAFFIC  <%s  Loading...", range_label());
        lv_label_set_text(_range_label, range_label());
        _list->unlock();
        return; // show gibberish this cycle, real data next cycle
    }
    _last_range_nm = current_range;

    // First render after gibberish: instant overwrite, no flip animation
    bool instant = _awaiting_data;
    if (instant) _awaiting_data = false;

    // Build sortable index of aircraft with valid positions
    SortEntry entries[MAX_AIRCRAFT];
    int n_entries = 0;
    for (int i = 0; i < _list->count && n_entries < MAX_AIRCRAFT; i++) {
        Aircraft &ac = _list->aircraft[i];
        if (ac.lat == 0 && ac.lon == 0) continue;
        float d = MapProjection::distance_nm(HOME_LAT, HOME_LON, ac.lat, ac.lon);
        if (d > range_get_nm()) continue;
        entries[n_entries].index = i;
        entries[n_entries].dist_nm = d;
        n_entries++;
    }

    // Sort if a mode is active
    if (_sort_dir != SORT_NONE) {
        qsort(entries, n_entries, sizeof(SortEntry), sort_compare);
    }

    int row = 0;
    for (int e = 0; e < n_entries && row < MAX_ROWS; e++) {
        Aircraft &ac = _list->aircraft[entries[e].index];

        // Skip fully faded ghosts
        if (ac.stale_since != 0) {
            uint32_t now = millis();
            uint8_t opa = compute_aircraft_opacity(ac.stale_since, now);
            if (opa == 0) continue;
        }

        _rows[row].active = true;
        strlcpy(_rows[row].icao_hex, ac.icao_hex, sizeof(_rows[row].icao_hex));

        // Format each column
        char flight[9], type[5], route[8], alt[5], spd[4], dist[5], hdg[4], status[8];
        snprintf(flight, sizeof(flight), "%-8s", ac.callsign[0] ? ac.callsign : ac.icao_hex);
        snprintf(type, sizeof(type), "%-4s", ac.type_code);

        // Route: "BNA-MDW" or blank if not yet enriched
        if (ac.origin[0] && ac.origin[0] != '-' && ac.dest[0] && ac.dest[0] != '-') {
            snprintf(route, sizeof(route), "%-3s-%-3s", ac.origin, ac.dest);
        } else {
            snprintf(route, sizeof(route), "       ");
        }

        if (ac.on_ground) snprintf(alt, sizeof(alt), " GND");
        else snprintf(alt, sizeof(alt), "%4d", ac.altitude / 100);
        snprintf(spd, sizeof(spd), "%3d", ac.speed);

        float dist_nm = entries[e].dist_nm;
        if (dist_nm >= 99.95f) snprintf(dist, sizeof(dist), "%4.0f", dist_nm);
        else snprintf(dist, sizeof(dist), "%4.1f", dist_nm);
        snprintf(hdg, sizeof(hdg), "%03d", ac.heading);
        snprintf(status, sizeof(status), "%-7s", status_from_vert_rate(ac.vert_rate, ac.on_ground));

        const char *texts[] = {flight, type, route, alt, spd, dist, hdg, status};

        lv_color_t color = CELL_TEXT;
        if (ac.is_emergency) color = EMERGENCY_CLR;
        else if (ac.is_military) color = MILITARY_CLR;

        // Dim stale (ghost) aircraft
        if (ac.stale_since != 0) {
            uint32_t now = millis();
            uint8_t opa = compute_aircraft_opacity(ac.stale_since, now);
            color = lv_color_make((color.red * opa) / 255,
                                  (color.green * opa) / 255,
                                  (color.blue * opa) / 255);
        }

        set_row_text(row, texts, color, !instant);
        row++;
    }

    int displayed_count = row;  // save before clear loop overwrites row

    // Clear remaining rows — unconditional when instant (wipes leftover gibberish)
    for (; row < MAX_ROWS; row++) {
        if (instant || _rows[row].active) {
            const char *blanks[] = {"", "", "", "", "", "", "", ""};
            set_row_text(row, blanks, CELL_TEXT, false);
            _rows[row].active = false;
            memset(_rows[row].icao_hex, 0, sizeof(_rows[row].icao_hex));
        }
    }

    // Update title with range-filtered count
    lv_label_set_text_fmt(_title_label, "OVERHEAD TRAFFIC  <%s    %d", range_label(), displayed_count);
    lv_label_set_text(_range_label, range_label());

    _list->unlock();
}

// Update header label text to show sort indicator
static void update_header_labels() {
    for (int i = 0; i < NUM_COLS; i++) {
        if (i == _sort_col && _sort_dir != SORT_NONE) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%s %c", columns[i].name,
                     _sort_dir == SORT_ASC ? '^' : 'v');
            lv_label_set_text(_header_labels[i], buf);
            lv_obj_set_style_text_color(_header_labels[i], lv_color_hex(0xffffff), 0);
        } else {
            lv_label_set_text(_header_labels[i], columns[i].name);
            lv_obj_set_style_text_color(_header_labels[i],
                columns[i].sortable ? lv_color_hex(0x888888) : lv_color_hex(0x666666), 0);
        }
    }
}

// Per-label click handler for sort column headers
static void header_label_click_cb(lv_event_t *e) {
    int col = (int)(intptr_t)lv_event_get_user_data(e);
    if (col < 0 || col >= NUM_COLS || !columns[col].sortable) return;

    if (_sort_col == col) {
        if (_sort_dir == SORT_ASC) _sort_dir = SORT_DESC;
        else if (_sort_dir == SORT_DESC) { _sort_dir = SORT_NONE; _sort_col = -1; }
    } else {
        _sort_col = col;
        _sort_dir = SORT_ASC;
    }
    update_header_labels();
}

void arrivals_view_init(lv_obj_t *parent, AircraftList *list) {
    _list = list;

    // Make tile fully opaque so radar/map don't bleed through
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_bg_color(parent, BOARD_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    _board_container = lv_obj_create(parent);
    lv_obj_set_size(_board_container, BOARD_W, BOARD_H);
    lv_obj_set_pos(_board_container, 0, 0);
    lv_obj_set_style_bg_color(_board_container, BOARD_BG, 0);
    lv_obj_set_style_bg_opa(_board_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_board_container, 0, 0);
    lv_obj_set_style_radius(_board_container, 0, 0);
    lv_obj_set_style_pad_all(_board_container, 0, 0);
    lv_obj_clear_flag(_board_container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_clear_flag(_board_container, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(_board_container, [](lv_event_t *e) {
        if (views_get_active_index() != VIEW_ARRIVALS) return;

        if (detail_card_is_visible()) {
            detail_card_hide();
            return;
        }

        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        int ty = point.y - 30; // offset for status bar

        // Determine which row was tapped
        int row = (ty - HEADER_H - 4) / ROW_H;
        if (row < 0 || row >= MAX_ROWS) return;
        if (!_rows[row].active) return;

        Serial.printf("ARR tap: row=%d icao=%s\n", row, _rows[row].icao_hex);

        // Look up aircraft by ICAO hex
        if (!_list->lock(pdMS_TO_TICKS(10))) { Serial.println("ARR: lock failed"); return; }
        for (int i = 0; i < _list->count; i++) {
            if (strcmp(_list->aircraft[i].icao_hex, _rows[row].icao_hex) == 0) {
                Aircraft ac_copy = _list->aircraft[i];
                _list->unlock();
                Serial.printf("ARR: hit %s\n", ac_copy.callsign);
                detail_card_show(&ac_copy);
                return;
            }
        }
        Serial.println("ARR: no match in list");
        _list->unlock();
    }, LV_EVENT_CLICKED, nullptr);

    // Title bar
    lv_obj_t *title_bg = lv_obj_create(_board_container);
    lv_obj_set_size(title_bg, BOARD_W, TITLE_H);
    lv_obj_set_pos(title_bg, 0, 0);
    lv_obj_set_style_bg_color(title_bg, HEADER_BG, 0);
    lv_obj_set_style_bg_opa(title_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(title_bg, 0, 0);
    lv_obj_set_style_radius(title_bg, 0, 0);
    lv_obj_clear_flag(title_bg, LV_OBJ_FLAG_SCROLLABLE);

    _title_label = lv_label_create(title_bg);
    lv_label_set_text(_title_label, "OVERHEAD TRAFFIC  Loading...");
    lv_obj_set_style_text_font(_title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_title_label, HEADER_TEXT, 0);
    lv_obj_align(_title_label, LV_ALIGN_LEFT_MID, 10, 0);

    // Column header labels — sortable ones get wide clickable containers
    for (int i = 0; i < NUM_COLS; i++) {
        // Calculate column width (to next column, or to board edge)
        int col_w = (i < NUM_COLS - 1) ? (columns[i + 1].x - columns[i].x) : (BOARD_W - columns[i].x);

        if (columns[i].sortable) {
            // Clickable container spanning full column width for easy tap target
            lv_obj_t *btn = lv_obj_create(_board_container);
            lv_obj_set_size(btn, col_w, COL_HEADER_H);
            lv_obj_set_pos(btn, columns[i].x, TITLE_H);
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_set_style_pad_all(btn, 0, 0);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLL_CHAIN); // prevent tileview from stealing clicks
            lv_obj_add_event_cb(btn, header_label_click_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);

            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, columns[i].name);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), 0);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
            _header_labels[i] = lbl;
        } else {
            // Non-sortable: plain label, no click handling
            lv_obj_t *lbl = lv_label_create(_board_container);
            lv_label_set_text(lbl, columns[i].name);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
            lv_obj_set_pos(lbl, columns[i].x, TITLE_H + 2);
            // Prevent non-sortable labels from eating touch events
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
            _header_labels[i] = lbl;
        }
    }

    // Set initial sort indicator
    update_header_labels();

    // Create all flip cells
    init_rows(_board_container);

    // Pre-load: fill all cells with random static characters until real data arrives
    reset_board_gibberish();

    // Range label — bottom-right, tappable
    _range_label = lv_label_create(parent);
    lv_label_set_text(_range_label, range_label());
    lv_obj_set_style_text_font(_range_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_range_label, lv_color_hex(0xffdd00), 0);
    lv_obj_set_pos(_range_label, BOARD_W - 80, BOARD_H - 28);
    lv_obj_add_flag(_range_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_range_label, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(_range_label, [](lv_event_t *e) {
        range_cycle();
        lv_label_set_text(_range_label, range_label());
    }, LV_EVENT_CLICKED, nullptr);

    // Flip animation timer (runs fast for smooth rolling effect)
    lv_timer_create(flip_animation_tick, 60, nullptr);

    // Data update timer (sync with fetch interval)
    lv_timer_create(update_board, 2000, nullptr);
}

void arrivals_view_update() {
    // Triggered externally if needed
}
