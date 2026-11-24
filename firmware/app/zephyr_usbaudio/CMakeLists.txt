# SP-1 FX USB audio bring-up, image A: host -> UAC2 -> engine -> headphones/speaker.
# Build: firmware/scripts/build_firmware.sh --app zephyr_usbaudio (never flashes).
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sp1fx_zephyr_usbaudio LANGUAGES C)

set(SP1FX_UA_DIR ${CMAKE_CURRENT_LIST_DIR})
include(${SP1FX_UA_DIR}/sources.cmake)
