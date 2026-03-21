#pragma once

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"

class rgb_lcd {
public:
    void begin();
    void lcd_draw_bitmap(int x_start, int y_start, int x_end, int y_end, uint16_t *color_data);
    esp_lcd_panel_handle_t get_panel() { return _panel; }
    void get_frame_buffers(void **fb0, void **fb1);
    void register_vsync_cb(esp_lcd_rgb_panel_event_callbacks_t *cbs, void *user_data);

private:
    esp_lcd_panel_handle_t _panel = nullptr;
};
