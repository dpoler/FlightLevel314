#include "display.h"

// Real-hardware backend -- NOT yet build/run-tested (Phase 6, see
// project_pi_port memory). lv_libinput_find_dev() auto-locates the
// Waveshare touch device by capability rather than hardcoding an
// /dev/input/eventN index, since that index isn't stable across other
// input devices the Pi might enumerate. Exact API surface should be
// double-checked against the LVGL version actually vendored once this is
// tested on real hardware -- it hasn't been compiled against a real
// libinput yet.

void pi_input_init(lv_display_t *disp) {
    char *dev = lv_libinput_find_dev(LV_LIBINPUT_CAPABILITY_TOUCH, false);
    lv_indev_t *indev = lv_libinput_create(LV_INDEV_TYPE_POINTER,
                                            dev ? dev : "/dev/input/event0");
    lv_indev_set_display(indev, disp);
    if (dev) lv_free(dev);
}
