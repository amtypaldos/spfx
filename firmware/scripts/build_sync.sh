#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SP-1 FX sync-jack bring-up image build (BUILD ONLY - never flashes, erases
# or talks to a device; there is no flash/runner step on purpose).
#
#   firmware/scripts/build_sync.sh [--no-usb] [build [DIR]]  # clean build + check_image.py (must PASS)
#   firmware/scripts/build_sync.sh [--no-usb] repro          # two clean builds, compare SHA-256
#   firmware/scripts/build_sync.sh release                   # repro, then copy to ../flash-ready/
#                                                            #   sp1fx-sync.bin + SHA256SUMS.sync
#
# Uses the same pinned toolchain/venv/workspace as build_firmware.sh (run
# `build_firmware.sh setup` once first) and verifies the pins through its
# read-only `verify` command. Toolchain: $SP1FX_TOOLCHAIN (default <repo>/../toolchain).
# See docs/BRINGUP_SYNC.md and docs/BUILD.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO="$(cd "$FW/.." && pwd)"
TC="${SP1FX_TOOLCHAIN:-$(cd "$REPO/.." && pwd)/toolchain}"
LOCK="$FW/west-lock.json"
WS="$TC/zephyr-ws"
SDK="$TC/zephyr-sdk-0.17.4"
VENV="$TC/venv"
APP="$FW/app/zephyr_sync"
FLASH_READY="$(cd "$REPO/.." && pwd)/flash-ready"
EXTRA_CONF=""

die() { echo "build_sync.sh: $*" >&2; exit 1; }

lock_get() {
  "$VENV/bin/python3" -I -c "import json,sys; d=json.load(open(sys.argv[1])); print($1)" "$LOCK"
}

env_setup() {
  export PATH="$VENV/bin:$PATH"
  export ZEPHYR_BASE="$WS/zephyr"
  export ZEPHYR_SDK_INSTALL_DIR="$SDK"
  export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
  export LC_ALL=C TZ=UTC
  export SOURCE_DATE_EPOCH=0
  unset ZEPHYR_EXTRA_MODULES EXTRA_CONF_FILE EXTRA_DTC_OVERLAY_FILE
}

build() {
  local out="${1:-$REPO/build/firmware/zephyr_sync${EXTRA_CONF:+_nousb}}"
  local extra=()
  [ -n "$EXTRA_CONF" ] && extra=(-DEXTRA_CONF_FILE="$EXTRA_CONF")
  "$SCRIPT_DIR/build_firmware.sh" verify   # read-only pin check
  env_setup
  case "$out" in /*) ;; *) out="$PWD/$out" ;; esac
  rm -rf "$out"
  echo "app: zephyr_sync${EXTRA_CONF:+ (+$EXTRA_CONF)}"
  cmake -GNinja -S "$APP" -B "$out" \
    -DBOARD=stem_player \
    ${extra[@]+"${extra[@]}"} \
    -DBOARD_ROOT="$FW" \
    -DBUILD_VERSION="$(lock_get "d['build_defines']['BUILD_VERSION']")" \
    > "$out.configure.log" 2>&1 || { tail -40 "$out.configure.log"; die "configure failed"; }
  cmake --build "$out" > "$out.build.log" 2>&1 || { tail -40 "$out.build.log"; die "build failed"; }
  grep -E "warning:" "$out.build.log" && echo "(compiler warnings above)" >&2 || true
  sed -n '/Memory region/,/IDT_LIST/p' "$out.build.log"
  "$VENV/bin/python3" -I "$SCRIPT_DIR/check_image.py" \
    --elf "$out/zephyr/zephyr.elf" --bin "$out/zephyr/zephyr.bin" \
    --json "$out/check_image.json" || die "check_image.py FAILED"
  "$VENV/bin/python3" -I -c "import json,sys; sys.exit(0 if json.load(open(sys.argv[1]))['result']=='PASS' else 1)" \
    "$out/check_image.json" || die "check_image.py did not report PASS"
  echo "image: $out/zephyr/zephyr.bin"
  shasum -a 256 "$out/zephyr/zephyr.bin"
}

repro() {
  local a="$REPO/build/firmware/repro_zephyr_sync_a" b="$REPO/build/firmware/repro_zephyr_sync_b"
  build "$a" > /dev/null
  build "$b" > /dev/null
  local ha hb
  ha=$(shasum -a 256 "$a/zephyr/zephyr.bin" | cut -d' ' -f1)
  hb=$(shasum -a 256 "$b/zephyr/zephyr.bin" | cut -d' ' -f1)
  echo "build A: $ha"
  echo "build B: $hb"
  [ "$ha" = "$hb" ] || die "NOT reproducible"
  echo "REPRODUCIBLE: .bin identical"
}

release() {
  [ -z "$EXTRA_CONF" ] || die "release publishes the default (USB console) image only"
  repro
  local out="$REPO/build/firmware/zephyr_sync"
  build "$out"
  mkdir -p "$FLASH_READY"
  cp "$out/zephyr/zephyr.bin" "$FLASH_READY/sp1fx-sync.bin"
  (cd "$FLASH_READY" && shasum -a 256 sp1fx-sync.bin > SHA256SUMS.sync)
  echo "copied: $FLASH_READY/sp1fx-sync.bin"
  cat "$FLASH_READY/SHA256SUMS.sync"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --no-usb) EXTRA_CONF="no_usb.conf"; shift ;;
    *) break ;;
  esac
done

cmd="${1:-build}"
case "$cmd" in
  build) shift || true; build "${1:-}" ;;
  repro) repro ;;
  release) release ;;
  *) die "usage: $0 [--no-usb] [build [DIR]|repro|release]" ;;
esac
