# Shared by zephyr_usbaudio (image A) and zephyr_usbaudio_io (image B).
# SP1FX_UA_DIR = firmware/app/zephyr_usbaudio
get_filename_component(SP1FX_FW ${SP1FX_UA_DIR}/../.. ABSOLUTE)

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

# Explicit, sorted list (no GLOB) so the link order never depends on the host.
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

target_sources(app PRIVATE
  ${SP1FX_UA_DIR}/src/main.c
  # hardware-verified codec code of the tone image, shared, not copied
  ${SP1FX_FW}/app/zephyr_tone/src/sp1_codecs.c
  ${SP1FX_FW}/audio/usb_audio_core.c
  ${SP1FX_FW}/storage/config_store.c
  ${SP1FX_DSP_SRC}
)
target_include_directories(app PRIVATE ${SP1FX_FW}/app/zephyr_tone/src ${SP1FX_FW})
