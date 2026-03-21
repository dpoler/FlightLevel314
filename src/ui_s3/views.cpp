#include <Arduino.h>
#include "views.h"
#include "status_bar.h"
#include "map_view.h"
#include "radar_view.h"
#include "arrivals_view.h"
#include "stats_view.h"
#include "../ui/stats.h"
#include "../pins_s3.h"
#include "../data/storage.h"

static lv_obj_t *tileview;
static lv_obj_t *tiles[4];
static int _active_index = VIEW_MAP;

static const uint32_t CYCLE_DWELL_MS[] = {
    60000, 60000, 60000, 5000,
};
#define CYCLE_PAUSE_MS 60000
static uint32_t _last_touch_time = 0;
static uint32_t _last_cycle_time = 0;
static bool _cycle_paused = false;

#define STATUS_BAR_HEIGHT 24
#define CONTENT_Y STATUS_BAR_HEIGHT
#define CONTENT_H (LCD_V_RES - STATUS_BAR_HEIGHT)

static void tileview_changed_cb(lv_event_t *e) {
    lv_obj_t *tv = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *active = lv_tileview_get_tile_active(tv);
    for (int i = 0; i < 4; i++) {
        if (tiles[i] == active) {
            _active_index = i;
            status_bar_set_active_dot(i);
            lv_obj_invalidate(tiles[i]);
            break;
        }
    }
}

static void touch_pause_cb(lv_event_t *e) {
    _last_touch_time = millis();
    _last_cycle_time = _last_touch_time;
    if (!_cycle_paused) {
        _cycle_paused = true;
        status_bar_set_auto_indicator(false);
    }
}

static void cycle_timer_cb(lv_timer_t *t) {
    if (!g_config.cycle_enabled) {
        if (!_cycle_paused) {
            _cycle_paused = true;
            status_bar_set_auto_indicator(false);
        }
        return;
    }

    uint32_t now = millis();
    uint32_t pause_ms = (uint32_t)g_config.cycle_inactivity_s * 1000;

    if (_cycle_paused) {
        if (now - _last_touch_time >= pause_ms) {
            _cycle_paused = false;
            _last_cycle_time = now;
            status_bar_set_auto_indicator(true);
        }
        return;
    }

    uint32_t dwell = (_active_index == VIEW_STATS) ? 5000
                     : (uint32_t)g_config.cycle_interval_s * 1000;

    if (now - _last_cycle_time >= dwell) {
        _last_cycle_time = now;
        int next = (_active_index + 1) % 4;
        if (next == VIEW_ARRIVALS) next = (next + 1) % 4;
        lv_tileview_set_tile_by_index(tileview, next, 0, LV_ANIM_OFF);
    }
}

void views_init(lv_obj_t *parent, AircraftList *list) {
    stats_init();

    tileview = lv_tileview_create(parent);
    lv_obj_set_pos(tileview, 0, CONTENT_Y);
    lv_obj_set_size(tileview, LCD_H_RES, CONTENT_H);
    lv_obj_set_style_bg_color(tileview, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_COVER, 0);

    tiles[VIEW_MAP] = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_RIGHT);
    tiles[VIEW_RADAR] = lv_tileview_add_tile(tileview, 1, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
    tiles[VIEW_ARRIVALS] = lv_tileview_add_tile(tileview, 2, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
    tiles[VIEW_STATS] = lv_tileview_add_tile(tileview, 3, 0, LV_DIR_LEFT);
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(tiles[i], lv_color_hex(0x0a0a1a), 0);
        lv_obj_set_style_bg_opa(tiles[i], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(tiles[i], 0, 0);
    }

    lv_obj_add_event_cb(tileview, tileview_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(tileview, touch_pause_cb, LV_EVENT_PRESSED, nullptr);

    map_view_init(tiles[VIEW_MAP], list);
    radar_view_init(tiles[VIEW_RADAR], list);
    arrivals_view_init(tiles[VIEW_ARRIVALS], list);
    stats_view_init(tiles[VIEW_STATS], list);

    _last_cycle_time = millis();
    status_bar_set_auto_indicator(g_config.cycle_enabled);
    if (!g_config.cycle_enabled) _cycle_paused = true;
    lv_timer_create(cycle_timer_cb, 1000, nullptr);
}

lv_obj_t *views_get_tile(int view_index) { return tiles[view_index]; }
int views_get_active_index() { return _active_index; }
lv_obj_t *views_get_tileview() { return tileview; }

void views_pause_cycle() {
    _last_touch_time = millis();
    _last_cycle_time = _last_touch_time;
    if (!_cycle_paused) {
        _cycle_paused = true;
        status_bar_set_auto_indicator(false);
    }
}
