# SP-1 FX minimal bring-up image (toolchain / linking / flash-safety proof).
# Build: firmware/scripts/build_firmware.sh (see docs/BUILD.md). Never flashes.
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sp1fx_zephyr_min LANGUAGES C)

target_sources(app PRIVATE src/main.c)
