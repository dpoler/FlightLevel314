#pragma once

#include "lvgl.h"

// Initialize rotary encoder + button GPIOs (call once from setup)
void encoder_input_init(int clk_pin, int dt_pin, int sw_pin, int back_pin);

// Get accumulated rotation delta since last call (positive = CW, negative = CCW)
int encoder_get_delta();

// Edge-detected button presses (returns true once per press, auto-clears)
bool encoder_select_pressed();
bool encoder_back_pressed();

// LVGL encoder indev read callback (for settings panel group navigation)
void encoder_lvgl_read_cb(lv_indev_t *indev, lv_indev_data_t *data);
