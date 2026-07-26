#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch_gt911.h"
#include "gt911_touch.h"

#ifndef CONFIG_LCD_HRES
#define CONFIG_LCD_HRES 1024
#endif
#ifndef CONFIG_LCD_VRES
#define CONFIG_LCD_VRES 600
#endif

static const char *TAG = "example";

esp_lcd_touch_handle_t tp;
esp_lcd_panel_io_handle_t tp_io_handle;

uint16_t touch_strength[1];
uint8_t touch_cnt = 0;

gt911_touch::gt911_touch(int8_t sda_pin, int8_t scl_pin, int8_t rst_pin, int8_t int_pin)
{
    _sda = sda_pin;
    _scl = scl_pin;
    _rst = rst_pin;
    _int = int_pin;
}

void gt911_touch::begin()
{
    if (!begin_safe()) {
        ESP_LOGE(TAG, "GT911 init failed — aborting");
        abort();
    }
}

bool gt911_touch::begin_safe()
{
    i2c_master_bus_handle_t i2c_handle = NULL;
    i2c_master_get_bus_handle(1,&i2c_handle);

    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_config.scl_speed_hz = 400000;
    ESP_LOGI(TAG, "Initialize touch IO (I2C)");
    esp_err_t ret = esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel IO: %s", esp_err_to_name(ret));
        return false;
    }

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = CONFIG_LCD_HRES,
        .y_max = CONFIG_LCD_VRES,
        .rst_gpio_num = (gpio_num_t)_rst,
        .int_gpio_num = (gpio_num_t)_int,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ESP_LOGI(TAG, "Initialize touch controller gt911");
    ret = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GT911 init failed: %s", esp_err_to_name(ret));
        return false;
    }

    _ready = true;
    return true;
}

bool gt911_touch::getTouch(uint16_t *x, uint16_t *y)
{
    if (!_ready) return false;
    esp_lcd_touch_read_data(tp);
    uint16_t rx, ry;
    bool raw_pressed = esp_lcd_touch_get_coordinates(tp, &rx, &ry, touch_strength, &touch_cnt, 1);

    // Debounce both edges. On the CrowPanel board this touch controller
    // exhibits real contact chatter/bounce: a single, continuous physical
    // tap gets reported as pressed / briefly released / pressed again for a
    // sample or two, not just coordinate noise. Debouncing only the press
    // side (as a first pass here did) doesn't fix that -- each mid-tap
    // bounce was read as a genuine release, so LVGL saw a full press-
    // release-press cycle and fired multiple clicks for one physical tap
    // ("one tap, multiple hits", confirmed on real hardware, no particular
    // screen-location pattern -- consistent with bounce, not e.g. edge
    // coupling). Fix: once a press is confirmed, a raw "released" sample
    // has to persist for RELEASE_DEBOUNCE_N consecutive reads before it's
    // reported as a real release -- a bounce blip within that window is
    // bridged over using the last known coordinates instead.
    constexpr uint8_t PRESS_DEBOUNCE_N = 2;
    constexpr uint8_t RELEASE_DEBOUNCE_N = 3;

    if (_is_pressed) {
        if (raw_pressed) {
            _release_streak = 0;
            _cand_x = rx;
            _cand_y = ry;
            *x = rx;
            *y = ry;
            return true;
        }
        // Raw release while we think we're pressed -- could be a real lift,
        // could be bounce. Bridge over it with the last known coordinates
        // until it's persisted long enough to trust.
        _release_streak++;
        if (_release_streak < RELEASE_DEBOUNCE_N) {
            *x = _cand_x;
            *y = _cand_y;
            return true;
        }
        _is_pressed = false;
        _press_streak = 0;
        _release_streak = 0;
        return false;
    }

    // Not currently pressed -- require PRESS_DEBOUNCE_N consecutive reads at
    // consistent coordinates (within ~20px) before confirming a new press.
    // A jump past the tolerance restarts the debounce window (rather than
    // rejecting outright) so a genuine fast drag just costs one dropped
    // sample, not a stuck touch.
    if (!raw_pressed) {
        _press_streak = 0;
        return false;
    }

    int dx = (int)rx - (int)_cand_x, dy = (int)ry - (int)_cand_y;
    if (_press_streak == 0 || dx * dx + dy * dy > 400) {
        _press_streak = 1;
        _cand_x = rx;
        _cand_y = ry;
        return false;
    }

    _press_streak++;
    _cand_x = rx;
    _cand_y = ry;
    if (_press_streak < PRESS_DEBOUNCE_N) return false;

    _is_pressed = true;
    _release_streak = 0;
    *x = rx;
    *y = ry;
    return true;
}

void gt911_touch::set_rotation(uint8_t r){
switch(r){
    case 0:
        esp_lcd_touch_set_swap_xy(tp, false);
        esp_lcd_touch_set_mirror_x(tp, false);
        esp_lcd_touch_set_mirror_y(tp, false);
        break;
    case 1:
        esp_lcd_touch_set_swap_xy(tp, false);
        esp_lcd_touch_set_mirror_x(tp, true);
        esp_lcd_touch_set_mirror_y(tp, true);
        break;
    case 2:
        esp_lcd_touch_set_swap_xy(tp, false);
        esp_lcd_touch_set_mirror_x(tp, false);
        esp_lcd_touch_set_mirror_y(tp, false);
        break;
    case 3:
        esp_lcd_touch_set_swap_xy(tp, false);
        esp_lcd_touch_set_mirror_x(tp, true);
        esp_lcd_touch_set_mirror_y(tp, true);
        break;
    }

}
