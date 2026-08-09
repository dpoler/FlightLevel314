#include "display.h"
#include "../src/platform/platform.h"

// Real-hardware touch via libinput. lv_libinput_find_dev() locates the
// Waveshare (Goodix) touch device by capability — /dev/input/eventN is not
// stable across keyboards/mice the Pi might enumerate.

void pi_input_init(lv_display_t *disp) {
    // Pointer into LVGL's static device-scan cache — do NOT free it.
    char *dev = lv_libinput_find_dev(LV_LIBINPUT_CAPABILITY_TOUCH, true);
    if (!dev) {
        platform_log("Input: no libinput TOUCH device found — taps will not work\n");
        return;
    }

    platform_log("Input: using touch device %s\n", dev);
    lv_indev_t *indev = lv_libinput_create(LV_INDEV_TYPE_POINTER, dev);
    if (!indev) {
        platform_log("Input: lv_libinput_create failed for %s\n", dev);
        return;
    }
    lv_indev_set_display(indev, disp);
}
