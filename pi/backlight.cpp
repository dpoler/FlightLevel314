// Panel backlight via Linux sysfs (/sys/class/backlight/*/brightness).
// Enumerate and probe-write so we pick a real device across Pi 4/5 DSI
// bus numbers (4-0045, 6-0045, 10-0045, rpi_backlight, …) instead of
// hardcoding a path. HDMI-only / SDL builds typically have nothing here.

#include "backlight.h"

#include "../src/platform/platform.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <string>

namespace {

constexpr const char *k_backlight_root = "/sys/class/backlight";

std::string g_bright_path; // cached successful device brightness path
int g_max = 0;
bool g_probed = false;
bool g_ok = false;

bool read_int_file(const std::string &path, int *out) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return false;
    int v = 0;
    const bool ok = (fscanf(f, "%d", &v) == 1);
    fclose(f);
    if (!ok) return false;
    *out = v;
    return true;
}

bool write_int_file(const std::string &path, int v) {
    // O_WRONLY — some kernels reject append-style fopen("w") on sysfs.
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d", v);
    ssize_t w = write(fd, buf, (size_t)n);
    close(fd);
    return w == (ssize_t)n;
}

bool probe_device(const std::string &dir) {
    std::string max_path = dir + "/max_brightness";
    std::string bright_path = dir + "/brightness";
    int max_b = 0, cur = 0;
    if (!read_int_file(max_path, &max_b) || max_b <= 0) return false;
    if (!read_int_file(bright_path, &cur)) return false;
    // Capability probe: write current value back. Ghost KMS stubs fail here.
    if (!write_int_file(bright_path, cur)) return false;
    g_bright_path = bright_path;
    g_max = max_b;
    return true;
}

void ensure_probed() {
    if (g_probed) return;
    g_probed = true;
    g_ok = false;
    g_bright_path.clear();
    g_max = 0;

    DIR *d = opendir(k_backlight_root);
    if (!d) {
        platform_log("Backlight: no %s (HDMI/SDL?)\n", k_backlight_root);
        return;
    }
    while (dirent *ent = readdir(d)) {
        if (ent->d_name[0] == '.') continue;
        std::string dir = std::string(k_backlight_root) + "/" + ent->d_name;
        if (probe_device(dir)) {
            g_ok = true;
            platform_log("Backlight: using %s (max=%d)\n", g_bright_path.c_str(), g_max);
            break;
        }
    }
    closedir(d);
    if (!g_ok)
        platform_log("Backlight: no writable device under %s\n", k_backlight_root);
}

} // namespace

bool backlight_available(void) {
    ensure_probed();
    return g_ok;
}

bool backlight_set_percent(int percent) {
    ensure_probed();
    if (!g_ok || g_max <= 0) return false;
    if (percent < 10) percent = 10;
    if (percent > 100) percent = 100;
    // Map 10–100% → at least 1 raw unit so the panel never fully blacks out
    // from the slider (use blank/screensaver for off if revisited later).
    int raw = (percent * g_max) / 100;
    if (raw < 1) raw = 1;
    if (raw > g_max) raw = g_max;
    if (!write_int_file(g_bright_path, raw)) {
        platform_log("Backlight: write %s failed (permissions? add udev rule)\n",
                     g_bright_path.c_str());
        g_ok = false; // force re-probe next time
        g_probed = false;
        return false;
    }
    return true;
}
