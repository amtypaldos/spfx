#!/usr/bin/env bash
# Build the emulation harness with the firmware's compiler flags (-O2, hard float).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW="$HERE/../../firmware"
TC="${SP1FX_TOOLCHAIN:-$(cd "$HERE/../../.." && pwd)/toolchain}"
GCC="$TC/zephyr-sdk-0.17.4/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc"
OUT="${1:-$HERE/../../build/fxprof_emu}"
mkdir -p "$OUT"
F=(-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -O2 -std=c99
   -ffunction-sections -fdata-sections -fno-common -fno-strict-aliasing -fno-tree-loop-distribute-patterns -Wall -Wno-unused-parameter)
SRC=("$HERE/harness.c" "$FW/audio/fxprof_core.c" "$FW/storage/config_store.c"
     "$FW/dsp/engine.c" "$FW/dsp/registry.c" "$FW"/dsp/effects/*.c)
"$GCC" "${F[@]}" -I"$FW" --specs=picolibc.specs -nostartfiles -T "$HERE/link.ld" \
  -Wl,--gc-sections -Wl,-u,h_ncases,-u,h_setup,-u,h_prep,-u,h_block -o "$OUT/harness.elf" "${SRC[@]}" -lm
echo "$OUT/harness.elf"
