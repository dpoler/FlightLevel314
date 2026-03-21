#include "encoder_input.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// --- Quadrature decoder (ISR-driven) ---
static volatile int32_t _enc_count = 0;
static int _clk_pin = -1;
static int _dt_pin = -1;
static int _sw_pin = -1;
static int _back_pin = -1;

// Last encoder state for quadrature decoding
static volatile uint8_t _last_ab = 0;

// Quadrature lookup table: [prev_AB << 2 | curr_AB] -> delta
// Valid transitions give +1 or -1, invalid give 0
static const int8_t _quad_table[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
};

static void IRAM_ATTR encoder_isr(void *arg) {
    uint8_t a = gpio_get_level((gpio_num_t)_clk_pin);
    uint8_t b = gpio_get_level((gpio_num_t)_dt_pin);
    uint8_t ab = (a << 1) | b;
    uint8_t idx = (_last_ab << 2) | ab;
    _enc_count += _quad_table[idx];
    _last_ab = ab;
}

// --- Button debounce state ---
static volatile bool _sw_raw = false;
static volatile bool _back_raw = false;
static bool _sw_last = false;
static bool _back_last = false;
static bool _sw_edge = false;
static bool _back_edge = false;
static uint32_t _sw_debounce_ms = 0;
static uint32_t _back_debounce_ms = 0;
#define DEBOUNCE_MS 50

void encoder_input_init(int clk_pin, int dt_pin, int sw_pin, int back_pin) {
    _clk_pin = clk_pin;
    _dt_pin = dt_pin;
    _sw_pin = sw_pin;
    _back_pin = back_pin;

    // Configure all pins as input with pull-up
    gpio_config_t io_conf = {};
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;

    // Encoder CLK + DT (interrupt on any edge)
    io_conf.pin_bit_mask = (1ULL << clk_pin) | (1ULL << dt_pin);
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&io_conf);

    // Buttons (no interrupt, polled)
    io_conf.pin_bit_mask = (1ULL << sw_pin) | (1ULL << back_pin);
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    // Read initial encoder state
    uint8_t a = gpio_get_level((gpio_num_t)clk_pin);
    uint8_t b = gpio_get_level((gpio_num_t)dt_pin);
    _last_ab = (a << 1) | b;

    // Install ISR service and attach handler to both encoder pins
    gpio_install_isr_service(0);
    gpio_isr_handler_add((gpio_num_t)clk_pin, encoder_isr, nullptr);
    gpio_isr_handler_add((gpio_num_t)dt_pin, encoder_isr, nullptr);
}

int encoder_get_delta() {
    int val = _enc_count;
    _enc_count = 0;
    // Divide by 4 for typical detent encoders (4 state changes per detent)
    // Use integer division with rounding toward zero
    return val / 4;
}

static void poll_buttons() {
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    // Select button (active low)
    bool sw_now = !gpio_get_level((gpio_num_t)_sw_pin);
    if (sw_now != _sw_raw) {
        _sw_raw = sw_now;
        _sw_debounce_ms = now;
    }
    if ((now - _sw_debounce_ms) >= DEBOUNCE_MS) {
        if (_sw_raw && !_sw_last) {
            _sw_edge = true;  // Rising edge detected
        }
        _sw_last = _sw_raw;
    }

    // Back button (active low)
    bool back_now = !gpio_get_level((gpio_num_t)_back_pin);
    if (back_now != _back_raw) {
        _back_raw = back_now;
        _back_debounce_ms = now;
    }
    if ((now - _back_debounce_ms) >= DEBOUNCE_MS) {
        if (_back_raw && !_back_last) {
            _back_edge = true;
        }
        _back_last = _back_raw;
    }
}

bool encoder_select_pressed() {
    poll_buttons();
    bool val = _sw_edge;
    _sw_edge = false;
    return val;
}

bool encoder_back_pressed() {
    poll_buttons();
    bool val = _back_edge;
    _back_edge = false;
    return val;
}

// LVGL encoder indev callback — used for settings panel navigation
void encoder_lvgl_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    poll_buttons();

    int delta = encoder_get_delta();
    if (delta > 0) {
        data->enc_diff = delta;
    } else if (delta < 0) {
        data->enc_diff = delta;
    } else {
        data->enc_diff = 0;
    }

    // Map select button to LV_KEY_ENTER
    if (_sw_edge) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = LV_KEY_ENTER;
        _sw_edge = false;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
