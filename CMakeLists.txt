cmake_minimum_required(VERSION 3.16)
project(adsb_pi_port LANGUAGES C CXX)

# platformio.ini drives the ESP32 (jc1060) build; this CMake project only
# builds the Raspberry Pi / Linux target. Everything Pi-specific lives
# under pi/, outside src/ -- PlatformIO's default src_dir is src/ and
# auto-compiles everything under it, so Linux-only code (libcurl, <mutex>,
# DRM headers) has to stay out of there or it'd break the jc1060 build.
add_subdirectory(pi)
