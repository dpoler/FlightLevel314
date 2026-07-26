#pragma once

// Board selection -- every src/ file includes this header via a relative
// path pointing at this one physical file (confirmed via repo-wide grep,
// 2026-07-25), so board selection has to dispatch from inside here via a
// build_flags macro (-DBOARD_CROWPANEL, set only by [env:crowpanel] in
// platformio.ini) rather than via -I search-path tricks, which quoted
// relative includes ("../pins_config.h") never fall through to. jc1060
// (this file's original, un-guarded contents) stays the implicit default
// so it needs no macro of its own and nothing changes for that env.
#ifdef BOARD_CROWPANEL
#include "pins_config_crowpanel.h"
#else

// Display (overridable via build flags for multi-board support)
#ifndef LCD_H_RES
#define LCD_H_RES 1024
#endif
#ifndef LCD_V_RES
#define LCD_V_RES 600
#endif
#define LCD_RST 5       // GPIO5 (confirmed from vendor source)
#define LCD_LED 23      // GPIO23 backlight PWM (handled by jd9165_lcd)

// Touch (GT911 via I2C bus 1)
#define TP_I2C_SDA 7
#define TP_I2C_SCL 8
#define TP_RST -1
#define TP_INT -1

// SD Card (SDMMC 4-bit)
#define SD_CMD  44
#define SD_CLK  43
#define SD_D0   39
#define SD_D1   40
#define SD_D2   41
#define SD_D3   42

// WiFi C6 (ESP-Hosted SDIO - handled by framework, listed for reference)
#define WIFI_C6_RST 54

#endif // BOARD_CROWPANEL
