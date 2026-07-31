#include "display.h"

// Real-hardware backend -- NOT yet build/run-tested (that's Phase 6 of the
// Pi port; see project_pi_port memory). /dev/dri/card0 assumes the
// Waveshare DSI panel enumerates as the Pi's primary DRM card, which is
// the common case but not guaranteed -- if the panel comes up on a
// different card node, adjust the path here or make it configurable.

lv_display_t *pi_display_init() {
    lv_display_t *disp = lv_linux_drm_create();
    lv_linux_drm_set_file(disp, "/dev/dri/card0", -1);
    return disp;
}
