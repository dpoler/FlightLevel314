#pragma once

// ESP32-S3 YD-ESP32-23 + 3.5" Display-F (480x320 SPI)
// Pin mapping — verify against actual board silkscreen before first boot

#define LCD_H_RES 480
#define LCD_V_RES 320

// TFT SPI pins (directly connected to Display-F header)
#define TFT_MOSI  11
#define TFT_MISO  13
#define TFT_SCLK  12
#define TFT_CS    10
#define TFT_DC     9
#define TFT_RST    8
#define TFT_BL    -1   // backlight — check if Display-F has a BL pin; -1 = always on

// Touch (XPT2046 on same SPI bus, separate CS)
#define TOUCH_CS   7
#define TOUCH_IRQ  6   // active low when touch detected
