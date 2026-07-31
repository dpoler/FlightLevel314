#include "display.h"

// Dev-simulator backend: runs natively on macOS via SDL2, no VM or
// hardware required. See project_pi_port memory for why this exists
// alongside the DRM backend.

lv_display_t *pi_display_init() {
    return lv_sdl_window_create(1280, 800);
}
