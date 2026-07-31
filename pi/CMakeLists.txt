set(PI_DISPLAY_BACKEND "SDL" CACHE STRING "Display backend: SDL (macOS/dev simulator) or DRM (real Pi hardware)")
set_property(CACHE PI_DISPLAY_BACKEND PROPERTY STRINGS SDL DRM)

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

include(FetchContent)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG v9.2.2
)
FetchContent_MakeAvailable(lvgl)

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
)

add_executable(adsb_pi
    main.cpp
    app_stubs.cpp
    platform_linux/platform_linux.cpp
    ${SHARED_SOURCES}
)
target_include_directories(adsb_pi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(adsb_pi PRIVATE lvgl)

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
