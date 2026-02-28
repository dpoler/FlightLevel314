#pragma GCC push_options
#pragma GCC optimize("O3")

#include <Arduino.h>
#include "lvgl.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "pins_config.h"
#include "config.h"
#include "hal/jd9165_lcd.h"
#include "hal/gt911_touch.h"
#include "data/aircraft.h"
#include "data/fetcher.h"

// Hardware drivers
static jd9165_lcd lcd(LCD_RST);
static gt911_touch touch(TP_I2C_SDA, TP_I2C_SCL, TP_RST, TP_INT);

// Aircraft data
AircraftList aircraft_list;

// LVGL display
static lv_display_t *disp;
static uint16_t *buf0;
static uint16_t *buf1;

// Display flush callback
static void disp_flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *color_map) {
    lcd.lcd_draw_bitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, (uint16_t *)color_map);
    // For DPI panel: flush_ready called via vsync callback
}

// Vsync callback — signals LVGL that flush is complete
static bool flush_ready_cb(esp_lcd_panel_handle_t panel,
    esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx) {
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

// Touch read callback
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    uint16_t x, y;
    if (touch.getTouch(&x, &y)) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("ADS-B Display starting...");

    // Init I2C bus 1 (MUST be before touch.begin())
    i2c_master_bus_handle_t i2c_handle = NULL;
    i2c_master_bus_config_t i2c_bus_conf = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = (gpio_num_t)TP_I2C_SDA,
        .scl_io_num = (gpio_num_t)TP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    i2c_new_master_bus(&i2c_bus_conf, &i2c_handle);
    Serial.println("I2C bus initialized");

    // Init display hardware
    lcd.begin();
    Serial.println("LCD initialized");

    // Init touch hardware
    touch.begin();
    Serial.println("Touch initialized");

    // Init LVGL
    lv_init();

    // Allocate framebuffers in PSRAM
    uint32_t buf_size = LCD_H_RES * LCD_V_RES;
    buf0 = (uint16_t *)heap_caps_malloc(buf_size * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    buf1 = (uint16_t *)heap_caps_malloc(buf_size * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    assert(buf0 && buf1);

    // Create LVGL display
    disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(disp, disp_flush_cb);
    lv_display_set_buffers(disp, buf0, buf1, buf_size * sizeof(uint16_t),
                           LV_DISPLAY_RENDER_MODE_FULL);

    // Register vsync callback for proper flush synchronization
    bsp_lcd_handles_t lcd_handles;
    lcd.get_handle(&lcd_handles);
    esp_lcd_dpi_panel_event_callbacks_t cbs = {};
    cbs.on_color_trans_done = flush_ready_cb;
    esp_lcd_dpi_panel_register_event_callbacks(lcd_handles.panel, &cbs, disp);

    // Create LVGL touch input device
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, disp);

    // Init aircraft data
    aircraft_list.init();

    // Start data fetcher on core 1
    fetcher_init(&aircraft_list);

    // Temporary: show status label
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);

    lv_obj_t *status_label = lv_label_create(lv_screen_active());
    lv_label_set_text(status_label, "Connecting to WiFi...");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    lv_obj_center(status_label);

    // Timer to update status label
    lv_timer_create([](lv_timer_t *timer) {
        lv_obj_t *label = (lv_obj_t *)lv_timer_get_user_data(timer);
        if (fetcher_wifi_connected()) {
            lv_label_set_text_fmt(label, "WiFi: Connected\nAircraft: %d\nLast update: %lus ago",
                                  aircraft_list.count,
                                  (millis() - fetcher_last_update()) / 1000);
        } else {
            lv_label_set_text(label, "Connecting to WiFi...");
        }
    }, 1000, status_label);

    Serial.println("LVGL initialized - fetcher started");
}

void loop() {
    lv_timer_handler();
    delay(5);
}
