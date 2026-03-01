#include <Arduino.h>
#include "arrivals_view.h"
#include "views.h"
#include "../config.h"
#include "../pins_config.h"
#include "geo.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

static AircraftList *_list = nullptr;
static lv_obj_t *_board_container = nullptr;

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
#define MAX_ROWS 14

// Sort modes
enum SortMode {
    SORT_NONE = 0,
    SORT_ASC,
    SORT_DESC,
};

// Sortable column indices (into columns[])
#define COL_FLIGHT 0
#define COL_TYPE   1
#define COL_ALT    2
#define COL_SPD    3
#define COL_DIST   4
#define COL_HDG    5
#define COL_STATUS 6

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
    {"ALT",      5,  270, true},
    {"SPD",      4,  380, true},
    {"DIST",     5,  470, true},
    {"HDG",      3,  580, false},
    {"STATUS",   7,  640, false},
};
#define NUM_COLS 7

// Per-cell animation state
struct FlipCell {
    lv_obj_t *label;
    char target;
    char current;
    int rolls_remaining; // 0 = settled, >0 = still flipping
};

struct BoardRow {
    FlipCell cells[40]; // max characters across all columns
    int total_cells;
    char icao_hex[7];   // to track which aircraft this row represents
    bool active;
};

static BoardRow _rows[MAX_ROWS];
static lv_obj_t *_header_labels[NUM_COLS];
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

// Set a row's target text — triggers flip animation for changed characters
static void set_row_text(int row, const char *texts[], lv_color_t color, bool is_new_row) {
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
                if (is_new_row) {
                    // Full roll for new aircraft appearing
                    fc.rolls_remaining = 2 + (rand() % 3); // 2-4 rolls
                } else {
                    // Instant update for incremental changes (speed/dist/hdg ticking)
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

// Update board data from aircraft list
static void update_board(lv_timer_t *t) {
    if (!_list->lock(pdMS_TO_TICKS(50))) return;

    int row = 0;
    for (int i = 0; i < _list->count && row < MAX_ROWS; i++) {
        Aircraft &ac = _list->aircraft[i];
        if (ac.lat == 0 && ac.lon == 0) continue;

        bool is_new_row = !_rows[row].active ||
                          strcmp(_rows[row].icao_hex, ac.icao_hex) != 0;
        _rows[row].active = true;
        strlcpy(_rows[row].icao_hex, ac.icao_hex, sizeof(_rows[row].icao_hex));

        // Format each column
        char flight[9], type[5], alt[6], spd[5], dist[6], hdg[4], status[8];
        snprintf(flight, sizeof(flight), "%-8s", ac.callsign[0] ? ac.callsign : ac.icao_hex);
        snprintf(type, sizeof(type), "%-4s", ac.type_code);
        if (ac.on_ground) snprintf(alt, sizeof(alt), " GND ");
        else snprintf(alt, sizeof(alt), "%5d", ac.altitude / 100); // flight level
        snprintf(spd, sizeof(spd), "%4d", ac.speed);

        float dist_nm = MapProjection::distance_nm(HOME_LAT, HOME_LON, ac.lat, ac.lon);
        if (dist_nm >= 99.95f) snprintf(dist, sizeof(dist), "%5.0f", dist_nm);
        else snprintf(dist, sizeof(dist), "%5.1f", dist_nm);
        snprintf(hdg, sizeof(hdg), "%03d", ac.heading);
        snprintf(status, sizeof(status), "%-7s", status_from_vert_rate(ac.vert_rate, ac.on_ground));

        const char *texts[] = {flight, type, alt, spd, dist, hdg, status};

        lv_color_t color = CELL_TEXT;
        if (ac.is_emergency) color = EMERGENCY_CLR;
        else if (ac.is_military) color = MILITARY_CLR;

        // Dim stale (ghost) aircraft
        if (ac.stale_since != 0) {
            uint32_t now = millis();
            uint8_t opa = compute_aircraft_opacity(ac.stale_since, now);
            if (opa == 0) continue;
            // Mix color toward dark background proportional to fade
            color = lv_color_make((color.red * opa) / 255,
                                  (color.green * opa) / 255,
                                  (color.blue * opa) / 255);
        }

        set_row_text(row, texts, color, is_new_row);
        row++;
    }

    // Clear remaining rows
    for (; row < MAX_ROWS; row++) {
        if (_rows[row].active) {
            const char *blanks[] = {"", "", "", "", "", "", ""};
            set_row_text(row, blanks, CELL_TEXT, false);
            _rows[row].active = false;
        }
    }

    // Update title with count
    lv_label_set_text_fmt(_title_label, "OVERHEAD TRAFFIC         %d", _list->count);

    _list->unlock();
}

void arrivals_view_init(lv_obj_t *parent, AircraftList *list) {
    _list = list;

    _board_container = lv_obj_create(parent);
    lv_obj_set_size(_board_container, BOARD_W, BOARD_H);
    lv_obj_set_pos(_board_container, 0, 0);
    lv_obj_set_style_bg_color(_board_container, BOARD_BG, 0);
    lv_obj_set_style_bg_opa(_board_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_board_container, 0, 0);
    lv_obj_set_style_radius(_board_container, 0, 0);
    lv_obj_set_style_pad_all(_board_container, 0, 0);
    lv_obj_clear_flag(_board_container, LV_OBJ_FLAG_SCROLLABLE);

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
    lv_label_set_text(_title_label, "OVERHEAD TRAFFIC");
    lv_obj_set_style_text_font(_title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_title_label, HEADER_TEXT, 0);
    lv_obj_align(_title_label, LV_ALIGN_LEFT_MID, 10, 0);

    // Column headers — below title bar
    for (int i = 0; i < NUM_COLS; i++) {
        lv_obj_t *lbl = lv_label_create(_board_container);
        lv_label_set_text(lbl, columns[i].name);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x666666), 0);
        lv_obj_set_pos(lbl, columns[i].x, TITLE_H + 2);
        _header_labels[i] = lbl;
    }

    // Create all flip cells
    init_rows(_board_container);

    // Flip animation timer (runs fast for smooth rolling effect)
    lv_timer_create(flip_animation_tick, 60, nullptr);

    // Data update timer (sync with fetch interval)
    lv_timer_create(update_board, 2000, nullptr);
}

void arrivals_view_update() {
    // Triggered externally if needed
}
