#pragma once

// Elecrow CrowPanel ESP32-P4 Advance, 10.1" HMI Display, SKU DHE04310D,
// hardware/software V1.1. Included only via pins_config.h's #ifdef
// BOARD_CROWPANEL dispatch -- never included directly.
//
// Every value in this file was pulled from Elecrow's own GitHub repo
// (Elecrow-RD/CrowPanel-Advanced-10.1inch-ESP32-P4-HMI-AI-Display-
// 1024x600-IPS-Touch-Screen, V1.1 Arduino_Code/Lesson07-Turn_on_the_screen
// and Lesson08-SD_Card_File_Reading's board_config.h, plus Lesson17-
// Wi-Fi_function's ESP32_P4-softAP/sdkconfig.defaults.esp32p4) on
// 2026-07-25 -- not guessed, not carried over from the earlier (wrong)
// ILI9881C-based research pass for a different CrowPanel model. Never
// build or hardware tested.

// Display -- MIPI-DSI, EK79007 panel driver (see hal/ek79007_lcd.h and
// hal/esp_lcd_ek79007.c/h, vendored from Espressif's published
// esp-iot-solution component of the same name -- also the panel used on
// Espressif's own reference ESP32-P4-Function-EV-Board, so a well-trodden
// path, unlike the 5"/7" CrowPanel line's less common ILI9881C).
#ifndef LCD_H_RES
#define LCD_H_RES 1024
#endif
#ifndef LCD_V_RES
#define LCD_V_RES 600
#endif
#define LCD_RST 41      // GPIO41, active-low (LCD_GPIO_RST in Elecrow's board_config.h)
#define LCD_LED 31      // GPIO31, active-high PWM (LCD_GPIO_BLIGHT) -- unlike jc1060's
                        // jd9165_lcd.cpp (which hardcodes GPIO23 internally and ignores
                        // this macro entirely), ek79007_lcd.cpp reads LCD_LED from here,
                        // since a hardcoded pin would be silently wrong on this board.
#define LCD_LED_ON_LEVEL 1     // Active level: 1 = high (BLIGHT_ON_LEVEL)
#define LCD_LED_PWM_HZ 30000    // BLIGHT_PWM_Hz

// MIPI-DSI/DPI panel timing -- Elecrow's own board_config.h values, NOT the
// esp_lcd_ek79007 component's built-in EK79007_1024_600_PANEL_60HZ_CONFIG
// default macro (which differs: HPW 10 vs 70, VPW 1 vs 10, VFP 12 vs 21,
// clock 52 vs 51 MHz) -- Elecrow's own values are what their hardware team
// actually validated against this specific panel, so ek79007_lcd.cpp
// builds its esp_lcd_dpi_panel_config_t from these instead of the macro.
#define LCD_MIPI_DSI_LANE_NUM 2
#define LCD_MIPI_DSI_LANE_RATE_MBPS 1000
#define LCD_DPI_CLK_MHZ 51
#define LCD_DPI_HPW 70
#define LCD_DPI_HBP 160
#define LCD_DPI_HFP 160
#define LCD_DPI_VPW 10
#define LCD_DPI_VBP 23
#define LCD_DPI_VFP 21

// Touch (GT911 via I2C bus 0 on this board, not bus 1 like jc1060 -- see
// gt911_touch.cpp, which takes the I2C peripheral as a runtime Wire
// argument, not a compile-time bus-number macro, so this difference is
// just which physical pins get passed to the same driver, no code change
// needed there).
#define TP_I2C_SDA 45
#define TP_I2C_SCL 46
#define TP_RST 40
#define TP_INT 42

// SD Card (SDMMC 1-bit -- this board only wires CLK/CMD/D0, unlike
// jc1060's 4-bit SD bus. Confirmed from Elecrow's own board_config.h;
// SD_D1-D3 are intentionally left undefined rather than guessed at --
// any code assuming a 4-bit SD bus (grep for SD_D1/SD_D2/SD_D3) needs a
// hardware-in-hand look before this board can use the SD card at all).
#define SD_CMD 44
#define SD_CLK 43
#define SD_D0  39

// No Ethernet on this board (Elecrow's spec sheet lists no PHY) -- see
// platformio.ini's [env:crowpanel] comment for why -DUSE_ETHERNET stays
// undefined there. That alone keeps fetcher.cpp's actual Ethernet code
// path compiled out safely, but settings.cpp's "Ethernet" switch itself
// is unconditional (not gated on USE_ETHERNET) -- confirmed harmless to
// leave as-is for now (toggling it just sets an inert g_config bool with
// zero runtime effect once USE_ETHERNET is undefined, verified by reading
// fetcher.cpp's #if defined(USE_ETHERNET)/#else structure), but it'll
// still show a switch that does nothing on this board's Settings screen,
// worth hiding per-board eventually rather than leaving as a UI wart.

// WiFi C6 (ESP-Hosted SDIO). The actual SDIO CMD/CLK/D0/D1/bus-width pins
// are NOT set here -- unlike jc1060 (where they're the pioarduino board
// variant's own defaults, this app never overrides them), this board
// needs real overrides, done via sdkconfig.defaults.crowpanel + platformio.
// ini's board_build.cmake_extra_args (see platformio.ini's [env:crowpanel]
// comment) since ESP-Hosted's SDIO pins are an ESP-IDF Kconfig setting,
// not something pins_config.h's plain #defines can reach. WIFI_C6_RST
// below is listed for reference only, same as jc1060's -- the app itself
// doesn't read it (reset_wifi_c6() in fetcher.cpp uses a jc1060-specific
// GPIO write path that has not been checked for compatibility with this
// board; CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE in sdkconfig.defaults.
// crowpanel is what actually matters for the ESP-Hosted-managed reset).
#define WIFI_C6_RST 32
