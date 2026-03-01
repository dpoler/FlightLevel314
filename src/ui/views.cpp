#include <Arduino.h>
#include "views.h"
#include "status_bar.h"
#include "map_view.h"
#include "radar_view.h"
#include "arrivals_view.h"
#include "../pins_config.h"

static lv_obj_t *tileview;
static lv_obj_t *tiles[4];
static int _active_index = VIEW_MAP;

// View cycle state
#define CYCLE_DWELL_MS   60000  // 60s per view
#define CYCLE_PAUSE_MS   60000  // 60s pause after touch
static uint32_t _last_touch_time = 0;
static uint32_t _last_cycle_time = 0;
static bool _cycle_paused = false;

#define STATUS_BAR_HEIGHT 30
#define CONTENT_Y STATUS_BAR_HEIGHT
#define CONTENT_H (LCD_V_RES - STATUS_BAR_HEIGHT)

static void tileview_changed_cb(lv_event_t *e) {
    lv_obj_t *tv = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *active = lv_tileview_get_tile_active(tv);
    for (int i = 0; i < 4; i++) {
        if (tiles[i] == active) {
            _active_index = i;
            status_bar_set_active_dot(i);
            // Force immediate redraw of the newly active view
            lv_obj_invalidate(tiles[i]);
            break;
        }
    }
}

static void touch_pause_cb(lv_event_t *e) {
    _last_touch_time = millis();
    _last_cycle_time = _last_touch_time; // reset dwell timer so full 10s after unpause
    if (!_cycle_paused) {
        _cycle_paused = true;
        status_bar_set_auto_indicator(false);
    }
}

static void cycle_timer_cb(lv_timer_t *t) {
    uint32_t now = millis();

    // Check if pause has expired
    if (_cycle_paused) {
        if (now - _last_touch_time >= CYCLE_PAUSE_MS) {
            _cycle_paused = false;
            _last_cycle_time = now;
            status_bar_set_auto_indicator(true);
        }
        return;
    }

    // Advance to next view
    if (now - _last_cycle_time >= CYCLE_DWELL_MS) {
        _last_cycle_time = now;
        int next = (_active_index + 1) % 4;
        lv_tileview_set_tile_by_index(tileview, next, 0, LV_ANIM_ON);
    }
}

void views_init(lv_obj_t *parent, AircraftList *list) {
    // Tileview fills screen below status bar
    tileview = lv_tileview_create(parent);
    lv_obj_set_pos(tileview, 0, CONTENT_Y);
    lv_obj_set_size(tileview, LCD_H_RES, CONTENT_H);
    lv_obj_set_style_bg_color(tileview, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_COVER, 0);

    // Create 4 horizontal tiles
    tiles[VIEW_MAP] = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_RIGHT);
    tiles[VIEW_RADAR] = lv_tileview_add_tile(tileview, 1, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
    tiles[VIEW_ARRIVALS] = lv_tileview_add_tile(tileview, 2, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
    tiles[VIEW_STATS] = lv_tileview_add_tile(tileview, 3, 0, LV_DIR_LEFT);

    lv_obj_add_event_cb(tileview, tileview_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Touch anywhere on tileview pauses auto-cycle
    lv_obj_add_event_cb(tileview, touch_pause_cb, LV_EVENT_PRESSED, nullptr);

    // Init all views
    map_view_init(tiles[VIEW_MAP], list);
    radar_view_init(tiles[VIEW_RADAR], list);
    arrivals_view_init(tiles[VIEW_ARRIVALS], list);

    // Start auto-cycle timer
    _last_cycle_time = millis();
    status_bar_set_auto_indicator(true);
    lv_timer_create(cycle_timer_cb, 1000, nullptr);
}

lv_obj_t *views_get_tile(int view_index) {
    return tiles[view_index];
}

int views_get_active_index() {
    return _active_index;
}

lv_obj_t *views_get_tileview() {
    return tileview;
}
