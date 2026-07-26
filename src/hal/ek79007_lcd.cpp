#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_ldo_regulator.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "Arduino.h"

#include "esp_lcd_ek79007.h"
#include "ek79007_lcd.h"
#include "../pins_config.h"

// Structured identically to jd9165_lcd.cpp (jc1060's own panel driver
// wrapper) -- see that file for the pattern this mirrors. The one
// deliberate difference: jd9165_lcd.cpp hardcodes its resolution/backlight
// GPIO/DSI timing internally (fine when there was only ever one board);
// this file reads all of that from pins_config.h (pins_config_crowpanel.h)
// instead, since a second board needs those values to actually differ.
// UNBUILT, UNTESTED.

#define MIPI_DPI_PX_FORMAT (LCD_COLOR_PIXEL_FORMAT_RGB565)
#define LCD_BIT_PER_PIXEL (16)

// "VDD_MIPI_DPHY" should be powered at 2.5V, can be sourced from internal LDO regulator or external LDO chip
#define EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN 3 // LDO_VO3 connected to VDD_MIPI_DPHY
#define EXAMPLE_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL LCD_LED_ON_LEVEL
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL
// Bare integer macro, not a (gpio_num_t) cast -- this needs to work both as
// a plain #if-evaluable token below (a cast expression is not valid
// preprocessor syntax) and, cast at the point of use instead, as the
// ledc_channel_config_t.gpio_num field's enum type.
#define EXAMPLE_PIN_NUM_BK_LIGHT LCD_LED

#define LCD_LEDC_CH           LEDC_CHANNEL_0

static const char *TAG = "ek79007_lcd";
static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;

ek79007_lcd::ek79007_lcd(int8_t lcd_rst)
{
    _lcd_rst = lcd_rst;
}

void ek79007_lcd::example_bsp_enable_dsi_phy_power()
{
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
#ifdef EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = EXAMPLE_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
    // Settle delay -- confirmed on real hardware: the DSI video link
    // reliably locks/produces an image on a fresh cold power-on, but after
    // many rapid ESP.restart()-driven resets in quick succession (from the
    // WiFi crash-loop -- see fetcher.cpp's known SDIO fragility), the panel
    // started coming up blank/black (backlight on, no image, everything
    // else running normally) on most subsequent boots until a slower manual
    // reset or full power-cycle. Nothing here ever explicitly releases this
    // LDO channel, and a warm software reset reaches this line far sooner
    // after the previous boot than a cold power-on ever would, with the
    // same acquire call each time regardless of whatever state the analog
    // PHY rail is actually in. This delay is a mechanism-agnostic
    // mitigation (covers both "needs more settle time" and "needs a genuine
    // off-then-on transition to fully reset" without knowing which) --
    // cheap on a cold boot where it likely wasn't needed at all. Unverified
    // whether this actually fixes the rapid-reset case; needs testing
    // specifically by deliberately reset-cycling the board a few times in a
    // row, not just a single cold boot.
    vTaskDelay(pdMS_TO_TICKS(50));
#endif
}

void ek79007_lcd::example_bsp_init_lcd_backlight()
{
#if EXAMPLE_PIN_NUM_BK_LIGHT >= 0
   const ledc_channel_config_t LCD_backlight_channel = {
        .gpio_num = (gpio_num_t)EXAMPLE_PIN_NUM_BK_LIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0
    };
    const ledc_timer_config_t LCD_backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = LCD_LED_PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK
    };

    ledc_timer_config(&LCD_backlight_timer);
    ledc_channel_config(&LCD_backlight_channel);
#endif
}

void ek79007_lcd::example_bsp_set_lcd_backlight(uint32_t brightness_percent)
{
#if EXAMPLE_PIN_NUM_BK_LIGHT >= 0
    if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    if (brightness_percent < 0) {
        brightness_percent = 0;
    }

    ESP_LOGI(TAG, "Setting LCD backlight: %d%%", brightness_percent);
    uint32_t duty_cycle = (1023 * brightness_percent) / 100; // LEDC resolution set to 10bits, thus: 100% = 1023
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH, duty_cycle);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH);
#endif
}

void ek79007_lcd::begin()
{
    example_bsp_enable_dsi_phy_power();
    example_bsp_init_lcd_backlight();
    example_bsp_set_lcd_backlight(EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL);

    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = LCD_MIPI_DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = LCD_MIPI_DSI_LANE_RATE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

    ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
    esp_lcd_dbi_io_config_t dbi_config = EK79007_PANEL_IO_DBI_CONFIG();

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io_handle));

    // Elecrow's own board_config.h timing values (see pins_config_crowpanel.h),
    // not the esp_lcd_ek79007 component's built-in default macro -- those
    // differ (see pins_config_crowpanel.h's comment on the DPI constants).
    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = LCD_DPI_CLK_MHZ,
        .pixel_format = MIPI_DPI_PX_FORMAT,
        .num_fbs = 1,
        .video_timing = {
            .h_size = LCD_H_RES,
            .v_size = LCD_V_RES,
            .hsync_pulse_width = LCD_DPI_HPW,
            .hsync_back_porch = LCD_DPI_HBP,
            .hsync_front_porch = LCD_DPI_HFP,
            .vsync_pulse_width = LCD_DPI_VPW,
            .vsync_back_porch = LCD_DPI_VBP,
            .vsync_front_porch = LCD_DPI_VFP,
        },
        .flags = {
            .use_dma2d = true,
        },
    };

    ek79007_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num = LCD_MIPI_DSI_LANE_NUM,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = _lcd_rst,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    example_bsp_set_lcd_backlight(EXAMPLE_LCD_BK_LIGHT_ON_LEVEL);
}

void ek79007_lcd::lcd_draw_bitmap(uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end, uint16_t *color_data)
{
    esp_lcd_panel_draw_bitmap(panel_handle, x_start, y_start, x_end, y_end, color_data);
}

void ek79007_lcd::draw16bitbergbbitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t *color_data)
{
    uint16_t x_start = x;
    uint16_t y_start = y;
    uint16_t x_end = w + x;
    uint16_t y_end = h + y;

    esp_lcd_panel_draw_bitmap(panel_handle, x_start, y_start, x_end, y_end, color_data);
}

void ek79007_lcd::fillScreen(uint16_t color)
{
    uint16_t *color_data = (uint16_t *)heap_caps_malloc(480 * 272 * 2, MALLOC_CAP_INTERNAL);
    memset(color_data, color, 480 * 272 * 2);
    draw16bitbergbbitmap(0, 0, 480, 272, color_data);
    free(color_data);
}

void ek79007_lcd::te_on()
{
    esp_lcd_panel_io_tx_param(io_handle, 0x35, new (uint8_t[]){0x00}, 1);
}

void ek79007_lcd::te_off()
{
    esp_lcd_panel_io_tx_param(io_handle, 0x34, new (uint8_t[]){0x00}, 0);
}

uint16_t ek79007_lcd::width()
{
    return LCD_H_RES;
}

uint16_t ek79007_lcd::height()
{
    return LCD_V_RES;
}

void ek79007_lcd::get_handle(bsp_lcd_handles_ek79007_t *ret_handles)
{
    ret_handles->io = io_handle;
    ret_handles->mipi_dsi_bus = NULL;
    ret_handles->panel = panel_handle;
    ret_handles->control = NULL;
}
