#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SP-1 FX controls bring-up image build (BUILD ONLY - this script never
# flashes, erases or talks to a device; it has no flash/runner step).
#
#   firmware/scripts/build_controls.sh [--no-usb] [build [DIR]]  # clean build + check_image.py (must PASS)
#   firmware/scripts/build_controls.sh [--no-usb] repro          # two clean builds, compare SHA-256
#   firmware/scripts/build_controls.sh [--no-usb] release        # repro, then copy the image to
#                                                                # ../flash-ready/sp1fx-controls[-nousb].bin
#                                                                # and write SHA256SUMS.controls
#
# Uses the same pinned toolchain/venv/west workspace as build_firmware.sh
# (setup: firmware/scripts/build_firmware.sh setup). See docs/BRINGUP_CONTROLS.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO="$(cd "$FW/.." && pwd)"
TC="${SP1FX_TOOLCHAIN:-$(cd "$REPO/.." && pwd)/toolchain}"
FLASH_READY="${SP1FX_FLASH_READY:-$(cd "$REPO/.." && pwd)/flash-ready}"
APP="$FW/app/zephyr_controls"
LOCK="$FW/west-lock.json"
WS="$TC/zephyr-ws"
SDK="$TC/zephyr-sdk-0.17.4"
VENV="$TC/venv"
EXTRA_CONF=""
SUFFIX=""

die() { echo "build_controls.sh: $*" >&2; exit 1; }

lock_get() {
  "$VENV/bin/python3" -I -c "import json,sys; d=json.load(open(sys.argv[1])); print($1)" "$LOCK"
}

verify_pins() {
  [ -x "$VENV/bin/python3" ] || die "no toolchain venv at $VENV (run: firmware/scripts/build_firmware.sh setup)"
  local n i name path want got
  n=$(lock_get "len(d['west_projects'])")
  for ((i = 0; i < n; i++)); do
    name=$(lock_get "d['west_projects'][$i]['name']")
    path=$(lock_get "d['west_projects'][$i]['path']")
    want=$(lock_get "d['west_projects'][$i]['commit']")
    # read-only query of the toolchain checkout (not the sp1-fx repo)
    got=$(git -C "$WS/$path" rev-parse HEAD 2>/dev/null || echo missing)
    [ "$want" = "$got" ] || die "$name at $got, lock wants $want"
    [ -z "$(git -C "$WS/$path" status --porcelain --untracked-files=no)" ] \
      || die "$name checkout has local modifications"
  done
  "$SDK/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" --version | head -1 | grep -q "Zephyr SDK 0.17.4" \
    || die "Zephyr SDK 0.17.4 arm-zephyr-eabi toolchain not found in $SDK"
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
  local out="${1:-$REPO/build/firmware/zephyr_controls$SUFFIX}"
  local extra=()
  [ -n "$EXTRA_CONF" ] && extra=(-DEXTRA_CONF_FILE="$EXTRA_CONF")
  verify_pins
  env_setup
  case "$out" in /*) ;; *) out="$PWD/$out" ;; esac
  rm -rf "$out"
  cmake -GNinja -S "$APP" -B "$out" \
    -DBOARD=stem_player \
    ${extra[@]+"${extra[@]}"} \
    -DBOARD_ROOT="$FW" \
    -DBUILD_VERSION="$(lock_get "d['build_defines']['BUILD_VERSION']")" \
    > "$out.configure.log" 2>&1 || { tail -40 "$out.configure.log"; die "configure failed"; }
  cmake --build "$out" > "$out.build.log" 2>&1 || { tail -60 "$out.build.log"; die "build failed"; }
  grep -E "warning:" "$out.build.log" && echo "(compiler warnings above)" >&2 || true
  sed -n '/Memory region/,/IDT_LIST/p' "$out.build.log"
  # flash-safety check; must PASS (non-zero exit aborts under set -e)
  "$VENV/bin/python3" -I "$SCRIPT_DIR/check_image.py" \
    --elf "$out/zephyr/zephyr.elf" --bin "$out/zephyr/zephyr.bin" \
    --json "$out/check_image.json" || die "check_image.py FAILED"
  echo "image: $out/zephyr/zephyr.bin"
  shasum -a 256 "$out/zephyr/zephyr.bin"
}

repro() {
  local a="$REPO/build/firmware/repro_controls${SUFFIX}_a" b="$REPO/build/firmware/repro_controls${SUFFIX}_b"
  build "$a" > /dev/null
  build "$b" > /dev/null
  ha=$(shasum -a 256 "$a/zephyr/zephyr.bin" | cut -d' ' -f1)
  hb=$(shasum -a 256 "$b/zephyr/zephyr.bin" | cut -d' ' -f1)
  echo "build A: $ha"
  echo "build B: $hb"
  [ "$ha" = "$hb" ] || die "NOT reproducible"
  echo "REPRODUCIBLE: .bin identical"
  REPRO_BIN="$a/zephyr/zephyr.bin"
}

release() {
  repro
  mkdir -p "$FLASH_READY"
  local name="sp1fx-controls$SUFFIX.bin"
  cp "$REPRO_BIN" "$FLASH_READY/$name"
  # SHA256SUMS.controls only (never the shared SHA256SUMS); one line per variant
  local sums="$FLASH_READY/SHA256SUMS.controls" line
  line="$(cd "$FLASH_READY" && shasum -a 256 "$name")"
  touch "$sums"
  { grep -v "  $name\$" "$sums" || true; echo "$line"; } | sort -k2 > "$sums.tmp"
  mv "$sums.tmp" "$sums"
  echo "copied: $FLASH_READY/$name"
  cat "$sums"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --no-usb) EXTRA_CONF="no_usb.conf"; SUFFIX="-nousb"; shift ;;
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
