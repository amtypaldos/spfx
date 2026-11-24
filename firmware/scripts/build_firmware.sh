#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SP-1 FX firmware build (BUILD ONLY - this script never flashes, erases or
# talks to a device; it has no flash/runner step on purpose).
#
#   firmware/scripts/build_firmware.sh setup        # create pinned toolchain (downloads)
#   firmware/scripts/build_firmware.sh [build] [DIR] # clean build + check_image.py
#   firmware/scripts/build_firmware.sh repro        # two clean builds, compare SHA-256
#
# Options (before the command):
#   --app NAME   app under firmware/app/ to build (default: zephyr_min;
#                also: zephyr_tone, zephyr_usbaudio, zephyr_fxprof, ...; the
#                zephyr_ prefix may be left out: --app fxprof; --app sp1fx is
#                the integrated firmware). An app may add checker arguments in
#                firmware/app/NAME/check_image.args (one line).
#                Default output dir: build/firmware/NAME.
#   --no-usb     add the app's no_usb.conf (zephyr_tone only: USB console off)
#
# Toolchain location: $SP1FX_TOOLCHAIN (default: <repo>/../toolchain).
# See docs/BUILD.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO="$(cd "$FW/.." && pwd)"
TC="${SP1FX_TOOLCHAIN:-$(cd "$REPO/.." && pwd)/toolchain}"
APP_NAME="zephyr_min"
EXTRA_CONF=""
LOCK="$FW/west-lock.json"
WS="$TC/zephyr-ws"
SDK="$TC/zephyr-sdk-0.17.4"
VENV="$TC/venv"
SDK_URL="https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.4"

die() { echo "build_firmware.sh: $*" >&2; exit 1; }

lock_get() {  # lock_get <python expression on dict d>
  "$VENV/bin/python3" -I -c "import json,sys; d=json.load(open(sys.argv[1])); print($1)" "$LOCK"
}

setup() {
  mkdir -p "$TC"
  if [ ! -x "$VENV/bin/python3" ]; then
    python3 -m venv "$VENV"
  fi
  "$VENV/bin/pip" install --quiet -r "$SCRIPT_DIR/requirements-build.txt"

  # West workspace pinned by firmware/west.yml (shallow fetch of exact commits).
  if [ ! -d "$WS/.west" ]; then
    mkdir -p "$WS/sp1fx-manifest"
    cp "$FW/west.yml" "$WS/sp1fx-manifest/west.yml"
    (cd "$WS" && "$VENV/bin/west" init -l sp1fx-manifest)
  else
    cp "$FW/west.yml" "$WS/sp1fx-manifest/west.yml"
  fi
  (cd "$WS" && "$VENV/bin/west" update --narrow -o=--depth=1)

  # Zephyr SDK 0.17.4: minimal bundle + arm-zephyr-eabi only, sha256-verified,
  # each archive extracted into its own new empty directory.
  if [ ! -x "$SDK/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" ]; then
    local dl="$TC/downloads/sdk-0.17.4"
    mkdir -p "$dl"
    local files
    files=$(lock_get "' '.join(f['name'] for f in d['zephyr_sdk']['files'])")
    for f in $files; do
      [ -f "$dl/$f" ] || curl -sSfL -o "$dl/$f" "$SDK_URL/$f"
      want=$(lock_get "[f['sha256'] for f in d['zephyr_sdk']['files'] if f['name']=='$f'][0]")
      got=$(shasum -a 256 "$dl/$f" | cut -d' ' -f1)
      [ "$want" = "$got" ] || die "sha256 mismatch for $f: $got (want $want)"
    done
    local xm xa
    xm=$(mktemp -d "$TC/x-minimal.XXXX"); xa=$(mktemp -d "$TC/x-arm.XXXX")
    tar -xJf "$dl/zephyr-sdk-0.17.4_macos-aarch64_minimal.tar.xz" -C "$xm"
    tar -xJf "$dl/toolchain_macos-aarch64_arm-zephyr-eabi.tar.xz" -C "$xa"
    rm -rf "$SDK"
    mv "$xm/zephyr-sdk-0.17.4" "$SDK"
    mv "$xa/arm-zephyr-eabi" "$SDK/arm-zephyr-eabi"
    rmdir "$xm" "$xa"
    # setup.sh is NOT run (it would register a CMake package in ~/.cmake);
    # ZEPHYR_SDK_INSTALL_DIR is set explicitly instead.
  fi
  command -v dtc >/dev/null || echo "note: dtc not found (optional; 'brew install dtc' gives 1.8.1)" >&2
  verify_pins
}

verify_pins() {
  local n i name path want got
  n=$(lock_get "len(d['west_projects'])")
  for ((i = 0; i < n; i++)); do
    name=$(lock_get "d['west_projects'][$i]['name']")
    path=$(lock_get "d['west_projects'][$i]['path']")
    want=$(lock_get "d['west_projects'][$i]['commit']")
    # read-only query of the toolchain checkout (not the sp1-fx repo)
    got=$(git -C "$WS/$path" rev-parse HEAD 2>/dev/null || echo missing)
    [ "$want" = "$got" ] || die "$name at $got, lock wants $want (run: $0 setup)"
    if [ -n "$(git -C "$WS/$path" status --porcelain --untracked-files=no)" ]; then
      die "$name checkout has local modifications"
    fi
  done
  "$SDK/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" --version | head -1 | grep -q "Zephyr SDK 0.17.4" \
    || die "Zephyr SDK 0.17.4 arm-zephyr-eabi toolchain not found in $SDK"
  [ "$(cat "$SDK/sdk_version")" = "0.17.4" ] || die "unexpected SDK version"
  echo "pins OK: zephyr v4.3.1 + cmsis + cmsis_6 + hal_nordic, SDK 0.17.4"
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
  local out="${1:-$REPO/build/firmware/$APP_NAME}"
  local app="$FW/app/$APP_NAME"
  [ -f "$app/CMakeLists.txt" ] || die "no such app: $app"
  local extra=()
  if [ -n "$EXTRA_CONF" ]; then
    [ -f "$app/$EXTRA_CONF" ] || die "$APP_NAME has no $EXTRA_CONF"
    extra=(-DEXTRA_CONF_FILE="$EXTRA_CONF")
  fi
  verify_pins
  env_setup
  case "$out" in /*) ;; *) out="$PWD/$out" ;; esac
  rm -rf "$out"          # always a clean build directory
  echo "app: $APP_NAME${EXTRA_CONF:+ (+$EXTRA_CONF)}"
  cmake -GNinja -S "$app" -B "$out" \
    -DBOARD=stem_player \
    ${extra[@]+"${extra[@]}"} \
    -DBOARD_ROOT="$FW" \
    -DBUILD_VERSION="$(lock_get "d['build_defines']['BUILD_VERSION']")" \
    > "$out.configure.log" 2>&1 || { tail -40 "$out.configure.log"; die "configure failed"; }
  cmake --build "$out" > "$out.build.log" 2>&1 || { tail -40 "$out.build.log"; die "build failed"; }
  grep -E "warning:" "$out.build.log" && echo "(compiler warnings above)" >&2 || true
  sed -n '/Memory region/,/IDT_LIST/p' "$out.build.log"
  # optional per-app checker arguments (e.g. zephyr_sp1fx: --reserve for its settings pages)
  local chk=()
  if [ -f "$app/check_image.args" ]; then
    read -r -a chk < "$app/check_image.args"
  fi
  "$VENV/bin/python3" -I "$SCRIPT_DIR/check_image.py" \
    --elf "$out/zephyr/zephyr.elf" --bin "$out/zephyr/zephyr.bin" \
    ${chk[@]+"${chk[@]}"} \
    --json "$out/check_image.json"
  echo "image: $out/zephyr/zephyr.bin"
  shasum -a 256 "$out/zephyr/zephyr.bin"
}

repro() {
  local a="$REPO/build/firmware/repro_${APP_NAME}_a" b="$REPO/build/firmware/repro_${APP_NAME}_b"
  build "$a" > /dev/null
  build "$b" > /dev/null
  local ha hb
  ha=$(shasum -a 256 "$a/zephyr/zephyr.bin" | cut -d' ' -f1)
  hb=$(shasum -a 256 "$b/zephyr/zephyr.bin" | cut -d' ' -f1)
  echo "build A: $ha"
  echo "build B: $hb"
  if [ "$ha" = "$hb" ]; then echo "REPRODUCIBLE: .bin identical"; else die "NOT reproducible"; fi
}

while [ $# -gt 0 ]; do
  case "$1" in
    --app) [ $# -ge 2 ] || die "--app needs a name"; APP_NAME="$2"; shift 2 ;;
    --app=*) APP_NAME="${1#--app=}"; shift ;;
    --no-usb) EXTRA_CONF="no_usb.conf"; shift ;;
    *) break ;;
  esac
done
case "$APP_NAME" in */*|.*|"") die "bad app name: $APP_NAME" ;; esac
# short names: --app fxprof = --app zephyr_fxprof (likewise usbaudio, tone, ...)
if [ ! -d "$FW/app/$APP_NAME" ] && [ -d "$FW/app/zephyr_$APP_NAME" ]; then
  APP_NAME="zephyr_$APP_NAME"
fi

cmd="${1:-build}"
case "$cmd" in
  setup) setup ;;
  build) shift || true; build "${1:-}" ;;
  repro) repro ;;
  verify) verify_pins ;;
  *) die "usage: $0 [--app NAME] [--no-usb] [setup|build [DIR]|repro|verify]" ;;
esac
