#include "rgb_lcd.h"
#include "pins.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"

static const char *TAG = "rgb_lcd";

void rgb_lcd::begin() {
    // RGB panel configuration
    esp_lcd_rgb_panel_config_t panel_config = {};
    panel_config.clk_src = LCD_CLK_SRC_DEFAULT;
    panel_config.data_width = 16;
    panel_config.bits_per_pixel = 16;
    panel_config.num_fbs = 2;  // Double buffering for reduced tearing
    panel_config.bounce_buffer_size_px = LCD_H_RES * 15;
    panel_config.psram_trans_align = 64;
    panel_config.sram_trans_align = 4;
    panel_config.flags.fb_in_psram = 1;

    // Timing for 1024x600 @ 12MHz pixel clock
    panel_config.timings.h_res = LCD_H_RES;
    panel_config.timings.v_res = LCD_V_RES;
    panel_config.timings.pclk_hz = 12 * 1000 * 1000;
    panel_config.timings.hsync_pulse_width = 10;
    panel_config.timings.hsync_back_porch = 50;
    panel_config.timings.hsync_front_porch = 50;
    panel_config.timings.vsync_pulse_width = 1;
    panel_config.timings.vsync_back_porch = 20;
    panel_config.timings.vsync_front_porch = 10;
    panel_config.timings.flags.pclk_active_neg = 1;

    // Control pins
    panel_config.hsync_gpio_num = LCD_HSYNC;
    panel_config.vsync_gpio_num = LCD_VSYNC;
    panel_config.de_gpio_num = LCD_DE;
    panel_config.pclk_gpio_num = LCD_PCLK;
    panel_config.disp_gpio_num = -1;

    // RGB565 data bus
    panel_config.data_gpio_nums[0]  = LCD_D0;
    panel_config.data_gpio_nums[1]  = LCD_D1;
    panel_config.data_gpio_nums[2]  = LCD_D2;
    panel_config.data_gpio_nums[3]  = LCD_D3;
    panel_config.data_gpio_nums[4]  = LCD_D4;
    panel_config.data_gpio_nums[5]  = LCD_D5;
    panel_config.data_gpio_nums[6]  = LCD_D6;
    panel_config.data_gpio_nums[7]  = LCD_D7;
    panel_config.data_gpio_nums[8]  = LCD_D8;
    panel_config.data_gpio_nums[9]  = LCD_D9;
    panel_config.data_gpio_nums[10] = LCD_D10;
    panel_config.data_gpio_nums[11] = LCD_D11;
    panel_config.data_gpio_nums[12] = LCD_D12;
    panel_config.data_gpio_nums[13] = LCD_D13;
    panel_config.data_gpio_nums[14] = LCD_D14;
    panel_config.data_gpio_nums[15] = LCD_D15;

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(_panel));

    ESP_LOGI(TAG, "RGB LCD initialized %dx%d (double-buffered)", LCD_H_RES, LCD_V_RES);
}

void rgb_lcd::lcd_draw_bitmap(int x_start, int y_start, int x_end, int y_end, uint16_t *color_data) {
    esp_lcd_panel_draw_bitmap(_panel, x_start, y_start, x_end, y_end, color_data);
}

void rgb_lcd::get_frame_buffers(void **fb0, void **fb1) {
    esp_lcd_rgb_panel_get_frame_buffer(_panel, 2, fb0, fb1);
}

void rgb_lcd::register_vsync_cb(esp_lcd_rgb_panel_event_callbacks_t *cbs, void *user_data) {
    esp_lcd_rgb_panel_register_event_callbacks(_panel, cbs, user_data);
}
