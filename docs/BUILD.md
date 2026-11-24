# Firmware build (Zephyr) — SP-1 FX

Status labels follow AGENTS.md. Everything on this page is **build-only**.
**Nothing here has been run on an SP-1. The image has not been flashed or tested on hardware.**

## What gets built

`firmware/app/zephyr_min` is a minimal app-only image whose only job is to prove the
toolchain, the link address and the flash-safety checks:

- feeds the watchdog the TE bootloader arms (register-level reload of all 8 RR channels,
  every 10 ms; the bootloader resets the device if it is not fed within ~5 s),
- blinks track LED 1 (P0.29, active-high) at 1 Hz as a heartbeat,
- FUNCTION (P0.27, active-low, pull-up) held 1.5 s → LED off, wait for release, arm
  FUNCTION as the wake source, clear `RESETREAS`, `SYSTEM_OFF` (next press → bootloader).

No USB, audio, I2S, I2C, ADC, codecs, eMMC, storage or SP-1 FX DSP. `app.overlay` disables
the board's USB/UAC2, I2S, I2C and SAADC nodes so no driver touches bootloader-owned or
external hardware.

## Pinned versions (source: chattock/sp1-tape-looper `release612-dependencies.json`)

Machine-readable copy: `firmware/west-lock.json`; manifest: `firmware/west.yml`.

| Component | Version / commit |
|---|---|
| zephyr | `75f67d766726351b30199f9a2bf55803d717a3be` = tag **v4.3.1** |
| cmsis | `512cc7e895e8491696b61f7ba8066b4a182569b8` |
| cmsis_6 | `30a859f44ef8ab4dc8f84b03ed586fd16ccf9d74` |
| hal_nordic | `7858281d843468fe53c829995fb63f45a227387a` |
| Zephyr SDK | 0.17.4, host `macos-aarch64`, `arm-zephyr-eabi` toolchain only (GCC 12.2.0, ld 2.38) |
| CMake | 3.26.4 (PyPI wheel in venv) |
| Ninja | 1.11.1.1 (PyPI wheel in venv) |
| west | 1.5.0 (PyPI) |
| pyelftools | 0.33 (PyPI) |
| DTC | 1.8.1 (Homebrew; optional for Zephyr, adds devicetree checks) |
| Python | 3.13.7 (Homebrew) in a venv; full list `firmware/scripts/requirements-build.txt` |

**Version note (source finding):** the brief and Tape Looper's dts comments say "Zephyr v4.1.0",
but the commit Tape Looper's lock file pins is the **v4.3.1** release tag (v4.1.0 is
`7823374e…`). Tape Looper's own board Kconfig also says "our pinned v4.3.1", and the three
module commits equal those in zephyr v4.3.1's `west.yml`. This environment follows the exact
lock-file commits, i.e. **v4.3.1**. Its `SDK_VERSION` file asks for SDK 0.17.4, which matches.

Tape Looper's `patches/zephyr-uac2.patch` (UAC2 Windows feedback backport) is **not** applied:
it only touches `subsys/usb/device_next/class/*uac2*`, which this image does not build. The
zephyr checkout is pristine upstream v4.3.1 (the build script refuses a modified checkout).
Apply it later, recording that, when USB audio is added.

SDK archives (`sha256.sum` from the same GitHub release; sdk-ng publishes no signature, so this
is an integrity check only):

| File | Bytes | SHA-256 |
|---|---|---|
| `zephyr-sdk-0.17.4_macos-aarch64_minimal.tar.xz` | 5,152 | `cc3d26aee46b4264e49f5ef85f72aca8622c30d62b6233ef495837d4303d7c2b` |
| `toolchain_macos-aarch64_arm-zephyr-eabi.tar.xz` | 99,923,692 | `c0fbf111d2232dd533d813f49ddfbcc45014c4e2da7f2a17bf80599ac5bfbdeb` |

The SDK's `setup.sh` is not run (it registers a CMake package under `~/.cmake`);
`ZEPHYR_SDK_INSTALL_DIR` is set explicitly instead. Archives were listed before extraction (no
absolute or `../` paths) and each was extracted into its own new empty directory.

## Prerequisites

- Apple Silicon Mac, Homebrew `python3` (3.10+), `curl`, `git` (used only inside the toolchain
  workspace), `tar`, `shasum`.
- Optional: `brew install dtc` (1.8.1).
- Disk: venv 137 MB, west workspace 788 MB, SDK 1.1 GB, downloads cache 96 MB.

Downloads incurred by `setup` (first run, 2026-11-24): SDK ~100 MB (99.9 MB toolchain + 5 KB
bundle + 21 KB checksum list); git shallow fetches ~146 MB packed (zephyr 121 MB, hal_nordic
16 MB, cmsis_6 7.8 MB, cmsis 0.9 MB); PyPI ~50 MB (cmake wheel 45.6 MB, the rest small);
dtc bottle 146 KB. Total ≈ 300 MB.

## Commands

```sh
# from the repo root; toolchain goes to ../toolchain (override: SP1FX_TOOLCHAIN=/path)
firmware/scripts/build_firmware.sh setup    # venv + pinned west workspace + SDK (verifies SHA-256)
firmware/scripts/build_firmware.sh          # clean build -> build/firmware/zephyr_min + check_image.py
firmware/scripts/build_firmware.sh repro    # two clean builds in different dirs, compares SHA-256
firmware/scripts/build_firmware.sh verify   # only checks commit pins and SDK version

# other apps (default is zephyr_min); options go before the command
firmware/scripts/build_firmware.sh --app zephyr_tone           # -> build/firmware/zephyr_tone
firmware/scripts/build_firmware.sh --app zephyr_tone repro     # -> build/firmware/repro_zephyr_tone_{a,b}
firmware/scripts/build_firmware.sh --app zephyr_tone --no-usb  # adds the app's no_usb.conf
```

`zephyr_tone` (audio bring-up, quiet test tone) is described in `docs/BRINGUP_TONE.md`.

**Integrated firmware:** `firmware/scripts/build_firmware.sh --app sp1fx` (and `--app sp1fx repro`)
builds `firmware/app/zephyr_sp1fx` (docs/BRINGUP_SP1FX.md). Its overlay shortens `slot0` to
0x20000-0xFCFFF and adds the settings partition 0xFD000-0xFEFFF (ADR-003); the app's one-line
`check_image.args` (`--reserve 0xFD000:0xFF000`) is passed to `check_image.py` by the build
script, which then also fails if any loadable byte lands in the settings pages.
`repro` directories are now named `repro_<app>_{a,b}`.

Equivalent manual build:

```sh
T=../toolchain
export PATH=$T/venv/bin:$PATH ZEPHYR_BASE=$T/zephyr-ws/zephyr \
       ZEPHYR_SDK_INSTALL_DIR=$T/zephyr-sdk-0.17.4 ZEPHYR_TOOLCHAIN_VARIANT=zephyr
cmake -GNinja -S firmware/app/zephyr_min -B build/firmware/zephyr_min \
      -DBOARD=stem_player -DBOARD_ROOT=$PWD/firmware -DBUILD_VERSION=v4.3.1
cmake --build build/firmware/zephyr_min
python3 -I firmware/scripts/check_image.py \
      --elf build/firmware/zephyr_min/zephyr/zephyr.elf --bin build/firmware/zephyr_min/zephyr/zephyr.bin
```

`BOARD_ROOT` is `firmware/` (Zephyr looks for `<BOARD_ROOT>/boards/<vendor>/<board>`), so the
board files live in `firmware/boards/teenageengineering/stem_player/`. `-DBUILD_VERSION` stops
Zephyr from running `git describe` on the zephyr checkout.

The scripts have no flash, runner, debug or serial step, and the venv has no flashing packages.

## Board definition (source finding)

`firmware/boards/teenageengineering/stem_player/` is a verbatim copy of Tape Looper's board
(commit `0359372e`; MIT, see `firmware/boards/LICENSES/` and `THIRD_PARTY_NOTICES.md`).
Its devicetree flash map, which `check_image.py` enforces:

| Region | Range | Owner |
|---|---|---|
| `boot_partition` | `0x00000`–`0x1FFFF` | TE bootloader (never written by us) |
| `slot0_partition` | `0x20000`–`0xFEFFF` (size `0xDF000`) | app image |
| `storage_partition` | `0xFF000`–`0xFFFFF` | storage page (never written by the image) |

## Result of the first build (2026-11-24, this Mac)

| Item | Value |
|---|---|
| Image | `build/firmware/zephyr_min/zephyr/zephyr.bin` (also `.elf`, `.hex`) |
| Flash (bin) size | **17,316 B** (0x43A4), 1.90 % of the 0xDF000 slot |
| Load range | `0x00020000`–`0x000243A3` |
| Static RAM | **4,296 B** (data + bss + noinit + stacks), 1.64 % of 256 KiB |
| Initial SP / reset vector | `0x200010C8` / `0x00020BED` |
| SHA-256 of `zephyr.bin` | `18afab484422abd760637561940706eefbb78dba07734b84ab0c173fbd592667` |
| Compiler warnings | 0 (dtc prints 2 upstream nRF52840 SoC "duplicate unit-address" warnings) |

`check_image.py` output:

```
first_load_addr          0x00020000
last_load_end            0x000243a4
flash_image_bytes        17316
flash_limit_bytes        913408
_vector_table            0x00020000
ram_static_bytes         4296
ram_sections_sum_bytes   4291
ram_limit_bytes          262144
initial_sp               0x200010c8
reset_vector             0x00020bed
bin_bytes                17316
bin_sha256               18afab484422abd760637561940706eefbb78dba07734b84ab0c173fbd592667
result                   PASS
flash_used               1.90% of slot0
ram_used                 1.64% of 256 KiB
```

The checker was also shown to FAIL on a one-byte-tampered `.bin` and on the same app linked at
0x0 (`CONFIG_USE_DT_CODE_PARTITION=n`).

**Reproducibility:** three clean builds in three different directories (two via `repro`, one by
hand with a different environment) produced the identical `.bin` SHA-256 above. The `.elf`
files differ between build directories (DWARF debug info records the build path); the `.elf`
is not what gets flashed, and `CONFIG_BUILD_OUTPUT_STRIP_PATHS=y` already maps source paths
in `__FILE__`. No timestamps end up in the image.

## Guesses about hardware behaviour (untested assumptions)

- **Track LED 1 = P0.29, active-high; FUNCTION = P0.27 active-low with pull-up.** Source:
  Tape Looper `main.c` pin tables and its `LED_PWM_POL` comment. The SP-1-dev
  `stemplayer_pins.h` was not available locally (that clone is empty).
- **PWM ownership of the LED pin.** The bootloader starts PWM2/PWM3 (wiki). If an enabled PWM
  instance has its PSEL on P0.29, the image stops and disables that instance so GPIO can drive
  the pin; otherwise it touches no PWM. Whether the bootloader actually routes PWM to P0.29 at
  hand-off is unknown.
- **Watchdog.** Feeding RR[0..7] directly (as Tape Looper does) is assumed to satisfy whatever
  RREN channels the bootloader enabled. Zephyr's WDT driver init/`wdt_setup` on an
  already-running WDT is assumed harmless because Tape Looper does the same calls on hardware.
- **Clocks.** Zephyr's nRF clock driver still starts LFCLK (synthesised, as in the board
  defconfig and Tape Looper). Assumed bootloader-compatible because Tape Looper ships with it.
- **SYSTEM_OFF while USB is plugged in.** Tape Looper does a soft reset instead when VBUS is
  present; this image always enters SYSTEM_OFF and relies on the FUNCTION press to wake. With
  USB connected the device may stay dark until FUNCTION is pressed or USB is replugged.
- Charger (BQ24232 nCE on P0.21) is not touched; the bootloader's pin state is assumed fine.

## FLASHING (NOT DONE)

> **WARNING.** Nothing in this repository flashes anything, and nothing has been flashed.
> This image has **never run on an SP-1**. The SP-1 has **no reset pin and no easy power
> cycle**; an app that fails to feed the watchdog boot-loops, and an app without a working
> path to `SYSTEM_OFF` can only be escaped by the bootloader key combination or by draining
> the battery. Flashing replaces the current app (for example Tape Looper or the stock
> firmware) and may affect data an app keeps in the `storage` page. Back up first, and
> flash only with the owner's explicit approval (AGENTS.md rule).

If the owner decides to try it, it is a **manual step done by the owner**:

1. Use desktop Chrome (WebUSB) and open <https://solderless.engineering>.
2. Turn the SP-1 off. Hold **T1 + T4** while connecting USB, and release when **T1 lights up**
   (procedure from Tape Looper's README). The device is now in the TE bootloader.
3. Load `build/firmware/zephyr_min/zephyr/zephyr.bin` only after re-running
   `check_image.py` and confirming the SHA-256 above.
4. Expected (untested): track LED 1 blinks at 1 Hz; holding FUNCTION 1.5 s turns it off.

**Recovery:** re-enter the bootloader (T1 + T4 while connecting USB until T1 lights, or the
solderless.engineering recovery procedure if that fails) and flash a known-good image (for
example the Tape Looper release BIN, SHA-256 `e73e3786…badbf`, or the stock firmware from
solderless.engineering).

## Why this is not Tape Looper's build

Tape Looper's `BUILDING.md` builds three images (runtime, maintenance, radio) and packages
them; the radio image needs two excluded vendor inputs (`admission.h`, `generated/ram_image.h`)
and Infineon SDK dependencies. We use none of that and never built Tape Looper. We reuse only
its pins (lock file) and its MIT board definition.
