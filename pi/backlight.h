#pragma once

// Pi panel backlight via /sys/class/backlight (DSI official touch, etc.).
// No-ops cleanly when no writable backlight exists (SDL/dev, HDMI-only).

// Apply 10–100% to the first writable sysfs backlight. Returns true if a
// write succeeded.
bool backlight_set_percent(int percent);

// True when at least one /sys/class/backlight/*/brightness is writable.
bool backlight_available(void);
