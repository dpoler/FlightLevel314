#include "display.h"
#include "../src/platform/platform.h"

#include <cerrno>
#include <cstring>
#include <xf86drmMode.h>

// Real-hardware backend. /dev/dri/card0 assumes the Waveshare DSI panel
// enumerates as the Pi's primary DRM card (verified on bring-up).

// Minimal prefix of LVGL's private drm_dev_t (lv_linux_drm.c) so we can
// blank the hardware cursor via the same DRM-master fd LVGL already holds.
// Layout must match: int fd; then conn/enc/crtc/plane/crtc_idx uint32s.
struct DrmDevPeek {
    int fd;
    uint32_t conn_id;
    uint32_t enc_id;
    uint32_t crtc_id;
    uint32_t plane_id;
    uint32_t crtc_idx;
};

static void blank_hw_cursor(lv_display_t *disp) {
    auto *dev = static_cast<DrmDevPeek *>(lv_display_get_driver_data(disp));
    if (!dev || dev->fd < 0 || dev->crtc_id == 0) return;
    // handle=0 disables the cursor plane. Ignore EBUSY — next REFR_READY retries.
    if (drmModeSetCursor(dev->fd, dev->crtc_id, 0, 0, 0) != 0 && errno != EBUSY) {
        platform_log_warn("DRM: hide HW cursor failed (crtc=%u): %s\n",
                     dev->crtc_id, strerror(errno));
    }
}

// LVGL's first atomic flush uses DRM_MODE_ATOMIC_ALLOW_MODESET, which can
// resurrect the VC4 cursor plane after we blanked it in pi_display_init().
// Re-blank for a few frames after that modeset, then detach.
static void blank_hw_cursor_after_modeset(lv_event_t *e) {
    static int remaining = 8;
    lv_display_t *disp = static_cast<lv_display_t *>(lv_event_get_user_data(e));
    blank_hw_cursor(disp);
    if (--remaining <= 0) {
        lv_display_remove_event_cb_with_user_data(disp, blank_hw_cursor_after_modeset, disp);
    }
}

lv_display_t *pi_display_init() {
    lv_display_t *disp = lv_linux_drm_create();
    lv_linux_drm_set_file(disp, "/dev/dri/card0", -1);

    // VC4/KMS often leaves a hardware cursor plane active at (0,0) after
    // modeset — looks like a stuck pointer in the upper-left. LVGL never
    // enables a software cursor; this is the DRM plane. Blank it using
    // LVGL's DRM-master fd (a second open wouldn't have permission).
    blank_hw_cursor(disp);
    lv_display_add_event_cb(disp, blank_hw_cursor_after_modeset, LV_EVENT_REFR_READY, disp);

    return disp;
}
