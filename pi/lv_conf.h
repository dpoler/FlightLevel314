#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* Memory -- plain libc malloc. Unlike the ESP32-P4 side's lv_conf.h (repo
 * root), there's no PSRAM-vs-internal-DRAM split to route around here --
 * the Pi has plenty of regular RAM. */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

/* Tick -- provided via lv_tick_set_cb() in pi/main.cpp, same pattern as
 * the ESP32 side. */

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN

/* Fonts -- kept in sync with the ESP32 side's lv_conf.h so shared
 * src/ui code has the same widgets/fonts available on both targets. */
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1

/* Widgets */
#define LV_USE_LABEL 1
#define LV_USE_BTN 1
#define LV_USE_IMG 1
#define LV_USE_LINE 1
#define LV_USE_ARC 1
#define LV_USE_TABLE 1
#define LV_USE_CANVAS 1
#define LV_USE_TILEVIEW 1
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1
#define LV_USE_TEXTAREA 1
#define LV_USE_KEYBOARD 1
#define LV_USE_LIST 1
#define LV_USE_MSGBOX 1
#define LV_USE_ROLLER 1
#define LV_USE_DROPDOWN 1
#define LV_USE_CHECKBOX 1
#define LV_USE_SPINNER 1

/* Animations */
#define LV_USE_ANIM 1

/* Drawing */
#define LV_DRAW_SW_DRAW_UNIT_CNT 1

/* Image decoders */
#define LV_USE_LODEPNG 1

/* Display/input backend -- selected by pi/CMakeLists.txt via the
 * PI_BACKEND_SDL / PI_BACKEND_DRM compile definitions (PI_DISPLAY_BACKEND
 * CMake option). SDL is the macOS/dev-simulator path; DRM+libinput is the
 * real Pi hardware path (untested until Phase 6 of the port -- see
 * project_pi_port memory). */
#ifdef PI_BACKEND_SDL
    #define LV_USE_SDL 1
    #define LV_SDL_INCLUDE_PATH <SDL2/SDL.h>
#endif

#ifdef PI_BACKEND_DRM
    #define LV_USE_LINUX_DRM 1
    #define LV_USE_LIBINPUT 1
    #define LV_LIBINPUT_BSD 0
#endif

#endif /* LV_CONF_H */
