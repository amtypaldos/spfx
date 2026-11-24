# SP-1 FX integrated firmware (v1): USB audio in -> effects engine -> headphones/speaker,
# physical controls + LEDs, companion protocol on the CDC ACM port, settings journal in
# internal flash, sync-jack clock (default off).
# Build: firmware/scripts/build_firmware.sh --app sp1fx (never flashes). See docs/BRINGUP_SP1FX.md.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sp1fx_zephyr_sp1fx LANGUAGES C)

get_filename_component(SP1FX_FW ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
set(SP1FX_APP_DIR ${CMAKE_CURRENT_LIST_DIR})

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

# Explicit, sorted lists (no GLOB) so the link order never depends on the host.
set(SP1FX_DSP_SRC
  ${SP1FX_FW}/dsp/engine.c
  ${SP1FX_FW}/dsp/registry.c
  ${SP1FX_FW}/dsp/effects/bitcrusher.c
  ${SP1FX_FW}/dsp/effects/compressor.c
  ${SP1FX_FW}/dsp/effects/delay.c
  ${SP1FX_FW}/dsp/effects/filter.c
  ${SP1FX_FW}/dsp/effects/mod_delay.c
  ${SP1FX_FW}/dsp/effects/none.c
  ${SP1FX_FW}/dsp/effects/reverb.c
  ${SP1FX_FW}/dsp/effects/saturation.c
  ${SP1FX_FW}/dsp/effects/tapewobble.c
  ${SP1FX_FW}/dsp/effects/tremolo.c
)

# The firmware links the toolchain's size-optimised picolibc, whose memcpy/memset copy one
# byte per iteration. Keep GCC from turning copy/clear loops in the audio path into calls
# to them (dsp/util.h fx_copy32 / fx_zero32), as zephyr_usbaudio does.
set_source_files_properties(${SP1FX_DSP_SRC} ${SP1FX_APP_DIR}/src/main.c
  ${SP1FX_FW}/audio/usb_audio_core.c
  PROPERTIES COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns")

target_sources(app PRIVATE
  ${SP1FX_APP_DIR}/src/main.c
  # hardware-verified codec code of the tone image, shared, not copied
  ${SP1FX_FW}/app/zephyr_tone/src/sp1_codecs.c
  ${SP1FX_FW}/app/app_logic.c
  ${SP1FX_FW}/app/sync_out.c
  ${SP1FX_FW}/audio/usb_audio_core.c
  ${SP1FX_FW}/controls/controller.c
  ${SP1FX_FW}/controls/controls.c
  ${SP1FX_FW}/controls/volume.c
  ${SP1FX_FW}/drivers/controls_hw.c
  ${SP1FX_FW}/drivers/crashdump.c
  ${SP1FX_FW}/drivers/ladder.c
  ${SP1FX_FW}/drivers/midi_out.c
  ${SP1FX_FW}/drivers/sync_hw.c
  ${SP1FX_FW}/protocol/protocol.c
  ${SP1FX_FW}/storage/config_store.c
  ${SP1FX_DSP_SRC}
)
# drivers/crashdump.c snapshots the fault status registers before Zephyr's fault decoder
# clears them (see firmware/app/zephyr_usbaudio/sources.cmake).
zephyr_ld_options(-Wl,--wrap=z_arm_fault)
target_include_directories(app PRIVATE ${SP1FX_FW}/app/zephyr_tone/src ${SP1FX_FW}
  ${SP1FX_FW}/drivers ${SP1FX_FW}/app)
