#pragma once

// Waveshare ESP32-S3-Touch-LCD-7B pin definitions
// 1024x600 RGB565 parallel display with GT911 capacitive touch
// I2C IO expander (CH422G / CH32V003) at address 0x24

// Display resolution
#define LCD_H_RES 1024
#define LCD_V_RES 600

// RGB LCD control pins
#define LCD_HSYNC  46
#define LCD_VSYNC  3
#define LCD_DE     5
#define LCD_PCLK   7

// RGB565 data bus — ordered B[4:0] G[5:0] R[4:0] for esp_lcd_rgb_panel
// Blue (5 bits): B3, B4, B5, B6, B7
#define LCD_D0   14   // B3
#define LCD_D1   38   // B4
#define LCD_D2   18   // B5
#define LCD_D3   17   // B6
#define LCD_D4   10   // B7
// Green (6 bits): G2, G3, G4, G5, G6, G7
#define LCD_D5   39   // G2
#define LCD_D6   0    // G3
#define LCD_D7   45   // G4
#define LCD_D8   48   // G5
#define LCD_D9   47   // G6
#define LCD_D10  21   // G7
// Red (5 bits): R3, R4, R5, R6, R7
#define LCD_D11  1    // R3
#define LCD_D12  2    // R4
#define LCD_D13  42   // R5
#define LCD_D14  41   // R6
#define LCD_D15  40   // R7

// I2C bus (shared by IO expander and GT911 touch)
#define I2C_SDA  8
#define I2C_SCL  9

// IO Expander (CH422G / CH32V003) I2C address
#define IO_EXP_ADDR  0x24

// IO Expander pin assignments
#define EXIO_TP_RST   1   // Touch reset
#define EXIO_DISP     2   // Backlight enable
#define EXIO_LCD_RST  3   // LCD reset

// Touch (GT911 via shared I2C bus)
#define TP_INT   4

// Rotary encoder + back button (active low, internal pull-up)
#define ENC_CLK   15   // Encoder A (CLK) — RS485 TX header
#define ENC_DT    16   // Encoder B (DT)  — RS485 RX header
#define ENC_SW    19   // Encoder push button (SELECT) — CAN RX header
#define BTN_BACK  20   // Back/nav button — CAN TX header
