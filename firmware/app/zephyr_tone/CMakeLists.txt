# SP-1 FX audio bring-up image: quiet test tone on headphones / speaker.
# Build: firmware/scripts/build_firmware.sh --app zephyr_tone (see docs/BRINGUP_TONE.md).
# Never flashes.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sp1fx_zephyr_tone LANGUAGES C)

# Zephyr's sample USB device_next helper (sample_usbd_init_device), the same
# helper Tape Looper uses for its composite device. Only compiled when the USB
# stack is enabled (it is unless no_usb.conf is added).
include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

target_sources(app PRIVATE src/main.c src/sp1_codecs.c)
