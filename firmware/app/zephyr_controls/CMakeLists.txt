# SP-1 FX controls bring-up image: real controller logic on the SP-1's
# buttons, faders and LEDs; no audio. Build: firmware/scripts/build_controls.sh
# (see docs/BRINGUP_CONTROLS.md). Never flashes.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sp1fx_zephyr_controls LANGUAGES C)

# Zephyr's sample USB device_next helper (sample_usbd_init_device), as Tape
# Looper and zephyr_tone use it. Only compiled with the USB stack enabled.
include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# The SP-1 FX control logic and the engine's configuration side, exactly the
# sources the host tests build (engine_process is linked but never called).
file(GLOB SP1FX_DSP_SRC ${FW}/dsp/*.c ${FW}/dsp/effects/*.c)
file(GLOB SP1FX_CTL_SRC ${FW}/controls/*.c)
list(SORT SP1FX_DSP_SRC)
list(SORT SP1FX_CTL_SRC)

target_sources(app PRIVATE
	src/main.c
	${FW}/drivers/controls_hw.c
	${FW}/drivers/ladder.c
	${FW}/storage/config_store.c
	${SP1FX_DSP_SRC}
	${SP1FX_CTL_SRC}
)
