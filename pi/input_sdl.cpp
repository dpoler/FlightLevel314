#include "display.h"

// SDL mouse click stands in for a touch event in the simulator.

void pi_input_init(lv_display_t *disp) {
    lv_indev_t *mouse = lv_sdl_mouse_create();
    lv_indev_set_display(mouse, disp);
}
