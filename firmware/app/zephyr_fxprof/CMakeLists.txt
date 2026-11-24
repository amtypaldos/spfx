# SP-1 FX effect CPU profiling image: every effect alone (default / worst-case / automated
# params) and every factory preset through the real engine on a synthetic signal, timed with
# the DWT cycle counter, one console line per case. No audio input, no audio output.
# Build: firmware/scripts/build_firmware.sh --app fxprof (never flashes). docs/BRINGUP_FXPROF.md
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sp1fx_zephyr_fxprof LANGUAGES C)

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

get_filename_component(SP1FX_FW ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)

# Same explicit DSP source list and flags as the usbaudio images (../zephyr_usbaudio/sources.cmake).
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
  ${SP1FX_FW}/audio/fxprof_core.c
)
set_source_files_properties(${SP1FX_DSP_SRC} PROPERTIES COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns")

target_sources(app PRIVATE
  src/main.c
  ${SP1FX_FW}/storage/config_store.c
  ${SP1FX_DSP_SRC}
)
target_include_directories(app PRIVATE ${SP1FX_FW})
