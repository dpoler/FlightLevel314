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

/* OS + multi-core software draw.
 * Pi 3B+ has 4 cores. LV_USE_OS defaulted to LV_OS_NONE and DRAW_UNIT_CNT
 * was 1, so the SW rasterizer ran entirely on the main thread -- matching
 * the ~4.5-6ms/draw-call cost measured on real DRM. pthread + CNT>1 fans
 * draw tasks across worker cores (leave headroom for the UI main thread
 * and the detached fetch thread). Currently trying CNT=3 after CNT=2
 * looked smooth with text_local fixes; don't jump straight to 4.
 * Stack bumped to 32KB because LVGL's internal ThorVG path is enabled
 * when CNT > 1 (lv_draw_sw.c) -- LVGL's own note recommends >=32KB then. */
#define LV_USE_OS LV_OS_PTHREAD
#define LV_DRAW_THREAD_STACK_SIZE (32 * 1024)
#define LV_DRAW_SW_DRAW_UNIT_CNT 3

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
