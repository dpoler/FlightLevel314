#include "views.h"
#include "status_bar.h"
#include "map_view.h"
#include "../pins_config.h"

static lv_obj_t *tileview;
static lv_obj_t *tiles[3];

#define STATUS_BAR_HEIGHT 30
#define CONTENT_Y STATUS_BAR_HEIGHT
#define CONTENT_H (LCD_V_RES - STATUS_BAR_HEIGHT)

static void tileview_changed_cb(lv_event_t *e) {
    lv_obj_t *tv = lv_event_get_target(e);
    lv_obj_t *active = lv_tileview_get_tile_active(tv);
    for (int i = 0; i < 3; i++) {
        if (tiles[i] == active) {
            status_bar_set_active_dot(i);
            break;
        }
    }
}

void views_init(lv_obj_t *parent, AircraftList *list) {
    // Tileview fills screen below status bar
    tileview = lv_tileview_create(parent);
    lv_obj_set_pos(tileview, 0, CONTENT_Y);
    lv_obj_set_size(tileview, LCD_H_RES, CONTENT_H);
    lv_obj_set_style_bg_color(tileview, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_COVER, 0);

    // Create 3 horizontal tiles
    tiles[VIEW_MAP] = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_RIGHT);
    tiles[VIEW_RADAR] = lv_tileview_add_tile(tileview, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    tiles[VIEW_ARRIVALS] = lv_tileview_add_tile(tileview, 2, 0, LV_DIR_LEFT);

    lv_obj_add_event_cb(tileview, tileview_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Init map view (replaces placeholder)
    map_view_init(tiles[VIEW_MAP], list);

    // Placeholder labels for remaining views
    const char *names[] = {nullptr, "Radar View", "Arrivals Board"};
    for (int i = 1; i < 3; i++) {
        lv_obj_t *label = lv_label_create(tiles[i]);
        lv_label_set_text(label, names[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_36, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x334455), 0);
        lv_obj_center(label);
    }
}

lv_obj_t *views_get_tile(int view_index) {
    return tiles[view_index];
}
