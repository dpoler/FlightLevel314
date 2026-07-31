set(PI_DISPLAY_BACKEND "SDL" CACHE STRING "Display backend: SDL (macOS/dev simulator) or DRM (real Pi hardware)")
set_property(CACHE PI_DISPLAY_BACKEND PROPERTY STRINGS SDL DRM)

# Waveshare 10.1" DSI panel resolution -- overrides pins_config.h's
# jc1060 default (1024x600). Shared ui/*.cpp code (views.cpp's CONTENT_H
# etc.) reads these same constants, so this one override reaches
# everything ported so far.
add_compile_definitions(LCD_H_RES=1280 LCD_V_RES=800)

if(PI_DISPLAY_BACKEND STREQUAL "SDL")
    add_compile_definitions(PI_BACKEND_SDL)
elseif(PI_DISPLAY_BACKEND STREQUAL "DRM")
    add_compile_definitions(PI_BACKEND_DRM)
else()
    message(FATAL_ERROR "PI_DISPLAY_BACKEND must be SDL or DRM, got: ${PI_DISPLAY_BACKEND}")
endif()

# This app doesn't use LVGL's bundled examples/demos -- skip building them
# to keep the CMake dev-loop build fast.
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)

# Must match whatever PlatformIO actually resolves platformio.ini's
# `lvgl/lvgl@^9.2.2` to for jc1060, NOT that literal version string --
# ^9.2.2 floats to the latest compatible 9.x release (currently 9.5.0,
# confirmed via .pio/libdeps/jc1060/lvgl/library.json), and shared code
# like src/ui/aircraft_icons.h uses APIs (lv_draw_triangle_dsc_t's
# bg_color/bg_opa vs. an older color/opa naming) that genuinely differ
# between LVGL 9.x point releases. If a fresh jc1060 build ever resolves a
# newer 9.x and something here stops compiling, re-check that file and
# bump this tag to match, don't just patch around the API diff locally.
include(FetchContent)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG v9.5.0
)
FetchContent_MakeAvailable(lvgl)

FetchContent_Declare(
    ArduinoJson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG v7.4.3
)
FetchContent_MakeAvailable(ArduinoJson)

# lv_conf.h lives here (pi/), not at the repo root -- the ESP32 side's root
# lv_conf.h uses a custom PSRAM allocator that doesn't apply on Linux. See
# pi/lv_conf.h's own comment.
target_include_directories(lvgl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)

# Shared with jc1060 (compiled here for Linux, via PlatformIO for ESP32) --
# see src/data/aircraft.h and each file's own comments for the #if
# defined(ARDUINO) forks where platform behavior actually differs.
set(SHARED_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ui/views.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ui/stats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ui/stats_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ui/map_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ui/radar_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ui/arrivals_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ui/detail_card.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ui/filters.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ui/display_prefs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ui/range.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/data/http_mutex.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/data/error_log.cpp
)

find_package(CURL REQUIRED)

add_executable(adsb_pi
    main.cpp
    app_stubs.cpp
    platform_linux/platform_linux.cpp
    platform_linux/storage_linux.cpp
    platform_linux/http_linux.cpp
    platform_linux/datasource_remote.cpp
    platform_linux/fetcher_stats_linux.cpp
    ${SHARED_SOURCES}
)
target_include_directories(adsb_pi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(adsb_pi PRIVATE lvgl ArduinoJson CURL::libcurl)

if(PI_DISPLAY_BACKEND STREQUAL "SDL")
    find_package(SDL2 REQUIRED)
    target_sources(adsb_pi PRIVATE display_sdl.cpp input_sdl.cpp)
    # lvgl's own SDL driver .c files (src/drivers/sdl/*.c) need SDL2/SDL.h
    # too, not just our app code -- link SDL2 to the lvgl target itself
    # (PUBLIC so adsb_pi inherits it transitively).
    target_link_libraries(lvgl PUBLIC SDL2::SDL2)
elseif(PI_DISPLAY_BACKEND STREQUAL "DRM")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DRM REQUIRED libdrm)
    pkg_check_modules(LIBINPUT REQUIRED libinput)
    target_sources(adsb_pi PRIVATE display_drm.cpp input_libinput.cpp)
    target_include_directories(adsb_pi PRIVATE ${DRM_INCLUDE_DIRS} ${LIBINPUT_INCLUDE_DIRS})
    target_link_libraries(adsb_pi PRIVATE ${DRM_LIBRARIES} ${LIBINPUT_LIBRARIES})
endif()
