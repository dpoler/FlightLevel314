#include "display.h"
#include <SDL2/SDL.h>

// Deliberately NOT lv_sdl_mouse_create() (LVGL's bundled SDL mouse driver).
// That driver runs the indev in LV_INDEV_MODE_EVENT, reading once per
// queued SDL event -- when SDL delivers several motion events within the
// same lv_tick_get() millisecond (routine for a fast mouse/trackpad),
// LVGL's scroll-throw math sees ~0 elapsed time between several real
// samples and computes a huge (near-infinite) velocity, flinging the
// tileview straight to its edge no matter how slowly you actually drag.
// This is LVGL issue #6832 -- a real fix landed in core (PR #7794,
// already in the v9.5.0 pinned here), but it only works if the input
// driver supplies precise per-event timestamps, and lv_sdl_mouse.c's
// bundled driver never sets data->timestamp -- so the bug still shows up
// through that specific driver even on a version that "has the fix".
//
// Polling instead of per-event reads sidesteps the whole problem: each
// read is naturally spaced out by the poll period below, so consecutive
// samples always have real, distinguishable elapsed time between them.

static void sdl_mouse_poll_cb(lv_indev_t *, lv_indev_data_t *data) {
    int x, y;
    Uint32 buttons = SDL_GetMouseState(&x, &y);
    data->point.x = (int32_t)x;
    data->point.y = (int32_t)y;
    data->state = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void pi_input_init(lv_display_t *disp) {
    lv_indev_t *mouse = lv_indev_create();
    lv_indev_set_type(mouse, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(mouse, sdl_mouse_poll_cb);
    lv_indev_set_display(mouse, disp);
    lv_timer_set_period(lv_indev_get_read_timer(mouse), 10);
}
