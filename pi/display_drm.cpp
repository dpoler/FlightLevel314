#include "display.h"
#include "../src/platform/platform.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <xf86drmMode.h>

// Real-hardware backend. The panel used to be hardcoded as /dev/dri/card0,
// but card numbering isn't stable: on a Pi 5 / newer Pi OS, card0 can be the
// render-only v3d GPU and the display controller (DSI, HDMI) card1. Pick the
// card from sysfs instead: the one with a connected DSI connector, else any
// connected output. FLIGHTLEVEL314_DRM_DEVICE=/dev/dri/cardN overrides
// (optionally FLIGHTLEVEL314_DRM_CONNECTOR=<id>).

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

namespace {

struct DrmCandidate {
    std::string dev;      // /dev/dri/cardN
    int64_t connector_id; // -1 = LVGL picks the card's first connected one
    int rank;             // lower tries first
    std::string why;      // for the log
};

std::string read_line(const std::string &path) {
    std::string out;
    if (FILE *f = fopen(path.c_str(), "r")) {
        char buf[64];
        if (fgets(buf, sizeof(buf), f)) out = buf;
        fclose(f);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

// sysfs connector dirs are "card<N>-<type>-<idx>", e.g. card1-DSI-1.
std::vector<DrmCandidate> find_candidates() {
    std::vector<DrmCandidate> out;
    if (const char *env = getenv("FLIGHTLEVEL314_DRM_DEVICE")) {
        if (env[0]) {
            int64_t conn = -1;
            if (const char *c = getenv("FLIGHTLEVEL314_DRM_CONNECTOR"))
                if (c[0]) conn = strtoll(c, nullptr, 10);
            out.push_back({env, conn, -1, "FLIGHTLEVEL314_DRM_DEVICE"});
        }
    }
    if (DIR *d = opendir("/sys/class/drm")) {
        while (dirent *ent = readdir(d)) {
            const char *name = ent->d_name;
            if (strncmp(name, "card", 4) != 0) continue;
            const char *dash = strchr(name + 4, '-');
            if (!dash || dash == name + 4) continue; // "card1" itself, not a connector
            const std::string card(name, dash - name);
            const std::string base = std::string("/sys/class/drm/") + name;
            const std::string status = read_line(base + "/status");
            const bool dsi = strncmp(dash + 1, "DSI", 3) == 0;
            int rank;
            if (status == "connected") rank = dsi ? 0 : 1;
            else if (status == "unknown") rank = 2; // some panels never report
            else continue;
            // Newer kernels expose the id -- lets us pick DSI over an HDMI
            // monitor plugged into the same card. Else LVGL's first connected.
            int64_t conn = -1;
            const std::string id = read_line(base + "/connector_id");
            if (!id.empty()) conn = strtoll(id.c_str(), nullptr, 10);
            out.push_back({"/dev/dri/" + card, conn, rank, std::string(name) + " " + status});
        }
        closedir(d);
    }
    // readdir order is arbitrary: stable sort by rank, then name.
    std::stable_sort(out.begin(), out.end(), [](const DrmCandidate &a, const DrmCandidate &b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        return a.why < b.why;
    });
    // Last resort (no sysfs info): the old hardcoded card, then the next.
    out.push_back({"/dev/dri/card0", -1, 9, "fallback"});
    out.push_back({"/dev/dri/card1", -1, 9, "fallback"});
    return out;
}

} // namespace

lv_display_t *pi_display_init() {
    lv_display_t *disp = lv_linux_drm_create();
    if (!disp) {
        platform_log_error("DRM: lv_linux_drm_create failed\n");
        exit(1);
    }

    // A failed set_file closes its fd and leaves the display reusable, so
    // just try the next candidate. Used to ignore the result entirely: a
    // wrong card left LVGL running with no framebuffer.
    bool ok = false;
    std::vector<std::string> tried;
    for (const DrmCandidate &c : find_candidates()) {
        const std::string key = c.dev + "#" + std::to_string((long long)c.connector_id);
        if (std::find(tried.begin(), tried.end(), key) != tried.end()) continue;
        tried.push_back(key);
        if (lv_linux_drm_set_file(disp, c.dev.c_str(), c.connector_id) == LV_RESULT_OK) {
            platform_log_info("DRM: using %s connector %lld (%s)\n", c.dev.c_str(),
                              (long long)c.connector_id, c.why.c_str());
            ok = true;
            break;
        }
        platform_log_warn("DRM: %s connector %lld (%s) failed\n", c.dev.c_str(),
                          (long long)c.connector_id, c.why.c_str());
    }
    if (!ok) {
        // No usable display: exit so systemd's Restart= retries (the panel
        // may still be probing) instead of running with nothing to draw into.
        platform_log_error("DRM: no usable display found -- set "
                           "FLIGHTLEVEL314_DRM_DEVICE=/dev/dri/cardN to force one\n");
        exit(1);
    }

    // VC4/KMS often leaves a hardware cursor plane active at (0,0) after
    // modeset — looks like a stuck pointer in the upper-left. LVGL never
    // enables a software cursor; this is the DRM plane. Blank it using
    // LVGL's DRM-master fd (a second open wouldn't have permission).
    blank_hw_cursor(disp);
    lv_display_add_event_cb(disp, blank_hw_cursor_after_modeset, LV_EVENT_REFR_READY, disp);

    return disp;
}
