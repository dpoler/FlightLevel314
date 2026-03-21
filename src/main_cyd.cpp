// CYD basic display test — calibrated touch
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>

#define XPT2046_IRQ   36
#define XPT2046_MOSI  32
#define XPT2046_MISO  39
#define XPT2046_CLK   25
#define XPT2046_CS    33

// Calibrated touch range (landscape rotation 1)
#define TOUCH_X_MIN 620
#define TOUCH_X_MAX 3538
#define TOUCH_Y_MIN 520
#define TOUCH_Y_MAX 3509

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI = SPIClass(VSPI);

uint16_t touchRead16(uint8_t cmd) {
    digitalWrite(XPT2046_CS, LOW);
    touchSPI.beginTransaction(SPISettings(2500000, MSBFIRST, SPI_MODE0));
    touchSPI.transfer(cmd);
    uint16_t val = touchSPI.transfer16(0);
    touchSPI.endTransaction();
    digitalWrite(XPT2046_CS, HIGH);
    return val >> 3;
}

void setup() {
    Serial.begin(115200);
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("ADS-B Display", 160, 60, 4);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("CYD Ready", 160, 100, 4);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("Draw to test", 160, 140, 2);

    touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    pinMode(XPT2046_IRQ, INPUT);
    pinMode(XPT2046_CS, OUTPUT);
    digitalWrite(XPT2046_CS, HIGH);
}

void loop() {
    if (digitalRead(XPT2046_IRQ) == LOW) {
        uint32_t sumX = 0, sumY = 0;
        for (int i = 0; i < 4; i++) {
            sumX += touchRead16(0xD0);
            sumY += touchRead16(0x90);
        }
        uint16_t rawX = sumX / 4;
        uint16_t rawY = sumY / 4;

        int sx = map(rawX, TOUCH_X_MAX, TOUCH_X_MIN, 0, 319);
        int sy = map(rawY, TOUCH_Y_MAX, TOUCH_Y_MIN, 0, 239);
        sx = constrain(sx, 0, 319);
        sy = constrain(sy, 0, 239);

        tft.fillCircle(sx, sy, 3, TFT_YELLOW);
    }
    delay(20);
}
