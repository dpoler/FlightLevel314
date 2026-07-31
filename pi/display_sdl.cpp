#include "display.h"
#include "../src/pins_config.h"

// Dev-simulator backend: runs natively on macOS via SDL2, no VM or
// hardware required. See project_pi_port memory for why this exists
// alongside the DRM backend. LCD_H_RES/LCD_V_RES come from
// pins_config.h, overridden for the Pi build in pi/CMakeLists.txt to
// match the Waveshare panel's 1280x800 -- shared/ported ui/*.cpp code
// (views.cpp's CONTENT_H etc.) reads the same constants.

lv_display_t *pi_display_init() {
    return lv_sdl_window_create(LCD_H_RES, LCD_V_RES);
}
