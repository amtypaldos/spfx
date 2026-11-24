# SP-1 FX sync-jack bring-up image: MIDI clock (ring) + PO sync pulses (tip).
# Build: firmware/scripts/build_sync.sh (see docs/BRINGUP_SYNC.md). Never flashes.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sp1fx_zephyr_sync LANGUAGES C)

# Zephyr's sample USB device_next helper (sample_usbd_init_device), as in
# zephyr_tone / Tape Looper. Only compiled when the USB stack is enabled.
include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_include_directories(app PRIVATE ${FW}/drivers ${FW}/app)
target_sources(app PRIVATE
  src/main.c
  ${FW}/drivers/sync_hw.c
  ${FW}/drivers/midi_out.c
  ${FW}/app/sync_out.c
)
