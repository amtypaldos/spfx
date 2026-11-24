# SP-1 FX USB audio bring-up, image B: image A + the processed signal returned
# to the host (UAC2 IN stream, implicit feedback). Reuses the sources, Kconfig
# and prj.conf of ../zephyr_usbaudio; only prj_io.conf and app.overlay differ.
# Build: firmware/scripts/build_firmware.sh --app zephyr_usbaudio_io (never flashes).
cmake_minimum_required(VERSION 3.20.0)

get_filename_component(SP1FX_UA_DIR ${CMAKE_CURRENT_LIST_DIR}/../zephyr_usbaudio ABSOLUTE)
set(KCONFIG_ROOT ${SP1FX_UA_DIR}/Kconfig)
set(CONF_FILE "${SP1FX_UA_DIR}/prj.conf;${CMAKE_CURRENT_LIST_DIR}/prj_io.conf")
set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/app.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sp1fx_zephyr_usbaudio_io LANGUAGES C)

include(${SP1FX_UA_DIR}/sources.cmake)
