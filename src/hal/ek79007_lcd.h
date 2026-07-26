#ifndef _EK79007_LCD_H
#define _EK79007_LCD_H
#include <stdio.h>
#include "esp_lcd_types.h"
#include "esp_lcd_mipi_dsi.h"

// EK79007 MIPI-DSI panel driver wrapper for the CrowPanel ESP32-P4 Advance
// 10.1" -- structured identically to jd9165_lcd (jc1060's own panel), just
// swapped to the EK79007 vendor driver (esp_lcd_ek79007.c/h) and reading
// its DPI timing from pins_config_crowpanel.h instead of jd9165_lcd.cpp's
// pattern of hardcoding everything internally -- necessary here since a
// second board needs those values to actually differ, where jc1060 never
// did. UNBUILT, UNTESTED.

typedef struct {
    esp_lcd_dsi_bus_handle_t    mipi_dsi_bus;  /*!< MIPI DSI bus handle */
    esp_lcd_panel_io_handle_t   io;            /*!< ESP LCD IO handle */
    esp_lcd_panel_handle_t      panel;         /*!< ESP LCD panel (color) handle */
    esp_lcd_panel_handle_t      control;       /*!< ESP LCD panel (control) handle */
} bsp_lcd_handles_ek79007_t;

class ek79007_lcd
{
public:
    ek79007_lcd(int8_t lcd_rst);

    void begin();
    void example_bsp_enable_dsi_phy_power();
    void example_bsp_init_lcd_backlight();
    void example_bsp_set_lcd_backlight(uint32_t brightness_percent);
    void lcd_draw_bitmap(uint16_t x_start, uint16_t y_start,
                         uint16_t x_end, uint16_t y_end, uint16_t *color_data);
    void draw16bitbergbbitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *color_data);
    void fillScreen(uint16_t color);
    void te_on();
    void te_off();
    uint16_t width();
    uint16_t height();
    void get_handle(bsp_lcd_handles_ek79007_t *ret_handles);

private:
    int8_t _lcd_rst;
};
#endif
