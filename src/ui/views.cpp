#include <cstdlib> // abs() -- only Arduino-flavored symbol this file used
#include "views.h"
#include "status_bar.h"
#include "map_view.h"
#include "radar_view.h"
#include "arrivals_view.h"
#include "stats_view.h"
#include "stats.h"
#include "../pins_config.h"
#include "../data/storage.h"
#include "detail_card.h"
#include "alerts.h"
#include "range.h"

static lv_obj_t *tileview;
static lv_obj_t *tiles[NUM_VIEWS];
static int _active_index = VIEW_MAP;

#define CONTENT_Y STATUS_BAR_HEIGHT
#define CONTENT_H (LCD_V_RES - STATUS_BAR_HEIGHT)

static void tileview_changed_cb(lv_event_t *e) {
    lv_obj_t *tv = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *active = lv_tileview_get_tile_active(tv);
    for (int i = 0; i < NUM_VIEWS; i++) {
        if (tiles[i] == active) {
            _active_index = i;
            status_bar_set_active_dot(i);
            // Force immediate redraw of the newly active view
            lv_obj_invalidate(tiles[i]);
            // LIST rebuilds from aircraft data on a 2s timer — force a pass
            // when landing here via native tileview scroll (nav uses
            // views_switch_to → arrivals_view_on_show already).
            if (i == VIEW_ARRIVALS) arrivals_view_on_show();
            break;
        }
    }
    // Dismiss overlays when switching views
    detail_card_hide();
    alerts_dismiss();
}

void views_init(lv_obj_t *parent, AircraftList *list) {
    stats_init();

    // Tileview fills screen below status bar
    tileview = lv_tileview_create(parent);
    lv_obj_set_pos(tileview, 0, CONTENT_Y);
    lv_obj_set_size(tileview, LCD_H_RES, CONTENT_H);
    lv_obj_set_style_bg_color(tileview, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_COVER, 0);
    // Keep the horizontal "swipe bar" (tileview scrollbar) visible as the
    // view-switch affordance. MODE_ON so it stays put instead of only
    // flashing mid-swipe (AUTO). The solid blue strip that used to sit
    // under it was not this scrollbar -- it was undrawn canvas BG_COLOR
    // below a CANVAS_H-tall basemap (see map_view.cpp map_basemap_sync);
    // that strip is gone now that the basemap covers full screen height.
    lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_ON);

    // Create 4 horizontal tiles — all get opaque backgrounds to prevent bleed-through during scroll animation.
    // Runway diagrams live inside Map view now (see map_view.cpp draw_saved_airports) —
    // there's no separate APRT tile; which airport's data these show is
    // controlled by the location picker (see locations.h), not by swiping.
    tiles[VIEW_MAP]     = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_RIGHT);
    tiles[VIEW_RADAR]   = lv_tileview_add_tile(tileview, 1, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
    tiles[VIEW_ARRIVALS]= lv_tileview_add_tile(tileview, 2, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
    tiles[VIEW_STATS]   = lv_tileview_add_tile(tileview, 3, 0, LV_DIR_LEFT);
    for (int i = 0; i < NUM_VIEWS; i++) {
        lv_obj_set_style_bg_color(tiles[i], lv_color_hex(0x0a0a1a), 0);
        lv_obj_set_style_bg_opa(tiles[i], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(tiles[i], 0, 0);
    }

    lv_obj_add_event_cb(tileview, tileview_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Init all views
    map_view_init(tiles[VIEW_MAP], list);
    radar_view_init(tiles[VIEW_RADAR], list);
    arrivals_view_init(tiles[VIEW_ARRIVALS], list);
    stats_view_init(tiles[VIEW_STATS], list);
}

lv_obj_t *views_get_tile(int view_index) {
    return tiles[view_index];
}

int views_get_active_index() {
    return _active_index;
}

int views_filterable_index() {
    return (_active_index == VIEW_MAP || _active_index == VIEW_RADAR ||
            _active_index == VIEW_ARRIVALS) ? _active_index : VIEW_MAP;
}

lv_obj_t *views_get_tileview() {
    return tileview;
}

void views_resume_last_view() {
    // Resume the view active at last shutdown/reboot instead of always
    // booting into Map -- last_view_idx is only ever written from a
    // deliberate nav tap/swipe (views_switch_to), so this reflects where
    // the user actually left off.
    //
    // Deliberately called by main.cpp only after detail_card_init()/
    // alerts_init() have run: switching tiles here can synchronously fire
    // tileview_changed_cb, which calls detail_card_hide()/alerts_dismiss() --
    // those touch widgets that don't exist yet if this runs any earlier
    // (views_init() itself runs before either of those two).
    int start_idx = g_config.last_view_idx;
    if (start_idx < 0 || start_idx >= NUM_VIEWS) start_idx = VIEW_MAP;
    // _active_index is still its static-init default (VIEW_MAP) at this
    // point -- nothing to do if that's also where we're resuming to.
    if (start_idx == _active_index) return;
    _active_index = start_idx;
    status_bar_set_active_dot(start_idx);
    lv_tileview_set_tile_by_index(tileview, start_idx, 0, LV_ANIM_OFF);
    if (start_idx == VIEW_ARRIVALS) arrivals_view_on_show();
}

void views_switch_to(int idx) {
    if (idx < 0 || idx >= NUM_VIEWS) return;
    _active_index = idx;
    status_bar_set_active_dot(idx);
    if (idx != VIEW_ARRIVALS) lv_obj_invalidate(tiles[idx]);
    lv_tileview_set_tile_by_index(tileview, idx, 0, LV_ANIM_OFF);
    if (idx == VIEW_MAP)      map_view_on_show();
    if (idx == VIEW_ARRIVALS) arrivals_view_on_show();
    detail_card_hide();
    alerts_dismiss();

    // Persist for resume-on-boot -- deliberately only from this
    // deliberate-switch path (nav tap / swipe gesture), a discrete human
    // action, not something on a timer.
    if (g_config.last_view_idx != idx) {
        g_config.last_view_idx = idx;
        storage_save_config(g_config);
    }
}

static bool _swipe_active = false;
static lv_point_t _swipe_start = {0, 0};
#define SWIPE_THRESHOLD 50  // px horizontal travel to trigger view switch

bool views_swipe_active() { return _swipe_active; }
void views_clear_swipe()   { _swipe_active = false; }

void views_attach_swipe(lv_obj_t *obj) {
    lv_obj_add_event_cb(obj, [](lv_event_t *e) {
        lv_indev_get_point(lv_indev_active(), &_swipe_start);
        _swipe_active = false;
    }, LV_EVENT_PRESSED, nullptr);

    lv_obj_add_event_cb(obj, [](lv_event_t *e) {
        if (_swipe_active) return;
        lv_point_t cur;
        lv_indev_get_point(lv_indev_active(), &cur);
        int dx = cur.x - _swipe_start.x;
        int dy = cur.y - _swipe_start.y;
        if (abs(dx) >= SWIPE_THRESHOLD && abs(dx) > abs(dy) * 2) {
            _swipe_active = true;
            int v = views_get_active_index();
            // Clamp at the ends rather than wrapping -- the tileview is a
            // straight line (Map/Radar/Arrivals/Stats, in that order), not
            // a loop, so wrapping (the old (v +/- 1) % NUM_VIEWS) jumped
            // straight from one end to the other with no animation
            // (LV_ANIM_OFF in views_switch_to()) since there's no sensible
            // transition to animate between two tiles that aren't actually
            // adjacent. Swiping past the first/last view now does nothing,
            // matching the layout instead of fighting it.
            int target = (dx < 0) ? v + 1 : v - 1;
            if (target >= 0 && target < NUM_VIEWS) views_switch_to(target);
        }
    }, LV_EVENT_PRESSING, nullptr);
}
