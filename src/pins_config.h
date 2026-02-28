#pragma once

// Display
#define LCD_H_RES 1024
#define LCD_V_RES 600
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
