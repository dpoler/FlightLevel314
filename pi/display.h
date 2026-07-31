#pragma once
#include "lvgl.h"

// Backend-specific display/input init. Exactly one of display_sdl.cpp /
// display_drm.cpp and input_sdl.cpp / input_libinput.cpp is compiled in,
// chosen by pi/CMakeLists.txt's PI_DISPLAY_BACKEND option.

// Creates the display (SDL window on macOS/dev, or the real DRM/KMS
// framebuffer on Pi hardware).
lv_display_t *pi_display_init();

// Creates the matching input device (SDL mouse-as-touch, or real
// libinput/evdev touch) and attaches it to the display created above.
void pi_input_init(lv_display_t *disp);
