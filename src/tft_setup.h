// TFT_eSPI User_Setup for ESP32-S3 + 3.5" Display-F
// This file is included by TFT_eSPI as User_Setup.h via build flags
#pragma once

// --- Driver ---
// Display-F 3.5" ships with either ILI9488 or ST7796. Try ILI9488 first.
#define ILI9488_DRIVER
// If colors are wrong, comment above and uncomment:
// #define ST7796_DRIVER

// --- Resolution ---
#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// --- SPI pins ---
#define TFT_MOSI  11
#define TFT_MISO  13
#define TFT_SCLK  12
#define TFT_CS    10
#define TFT_DC     9
#define TFT_RST    8

// Backlight — set to -1 if not wired
#define TFT_BL    -1

// --- Touch (XPT2046) ---
#define TOUCH_CS   7
#define TOUCH_IRQ  6

// --- SPI frequency ---
#define SPI_FREQUENCY       40000000   // 40 MHz SPI clock (safe starting point)
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000   // XPT2046 max 2.5 MHz

// --- Color swap ---
// ILI9488 needs this; ST7796 may not. Toggle if colors look wrong.
#define TFT_RGB_ORDER TFT_BGR
