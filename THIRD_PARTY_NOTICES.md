# Third-party notices and pinned upstreams

## Copied into this repository

| Path | Source | Commit | License | Notes |
|---|---|---|---|---|
| `firmware/app/zephyr_tone/src/sp1_codecs.c` (adapted; licence + copyright header kept in the file) | chattock/sp1-tape-looper `tools/capacity422/runtime/src/main.c`: `tas_wr()`/`tas_page()`/`tas_set_speaker()`, `tas2505_configure()`, `tpw()`/`tpr()`, `hp_detect_connected()`, `HP_TIM_TEST` branch of `hp_codec_init()` | `0359372e11f61d74a85e2e4449178a5c62010664` | MIT, Copyright (c) 2026 chattock (`firmware/boards/LICENSES/Tape-Looper-MIT.txt`). The CS42L42 sequence in it is Tim Knapen's SP-1-dev wiki `I2C.md` (MIT, Copyright (c) 2026 Tim Knapen, `firmware/boards/LICENSES/Tim-Knapen-SP1-dev-MIT.txt`) | Register addresses/values kept; changed for hearing safety and pops: both outputs configured muted and unmuted only by the app, CS42L42 mixer -30 dB (was 0 dB), TAS2505 class-D gain +6 dB (was +18 dB), speaker attenuation -6 dB (was 0 dB), DAC muted before power-up, new mute/unmute helpers; restructured into functions without Tape Looper globals |
| `firmware/boards/teenageengineering/stem_player/*` (8 files, unmodified) | chattock/sp1-tape-looper `boards/teenageengineering/stem_player` | `0359372e11f61d74a85e2e4449178a5c62010664` | MIT, Copyright (c) 2026 chattock (`firmware/boards/LICENSES/Tape-Looper-MIT.txt`). Kconfig/defconfig/yaml files carry `SPDX-License-Identifier: Apache-2.0` headers, kept as-is (`firmware/boards/LICENSES/Apache-2.0.txt`). | dts header: reconstructed from ericlewis/sp1-midi's board (MIT, licence text not reviewed/copied yet) and timknapen/SP-1-dev pin map + wiki (MIT, Copyright (c) 2026 Tim Knapen, `firmware/boards/LICENSES/Tim-Knapen-SP1-dev-MIT.txt`) |

`firmware/app/zephyr_tone/src/main.c` is new code; its I2S configuration (clock-slave, 16-bit, I2S format, prime-then-START,
drop/re-prime failsafe), its init order (I2S, TAS2505, CS42L42) and its prj.conf USB device_next/CDC ACM settings follow Tape Looper's
`audio_thread()`/`audio_init()`/`usb_audio_start()` and `prj.conf` (same commit, MIT); the track-LED pin table (P0.29, P0.26, P1.15, P1.14)
comes from Tape Looper's `track_leds[]`. It links Zephyr's `samples/subsys/usb/common/sample_usbd_init.c` (Apache-2.0, part of the pinned
Zephyr checkout, not copied). Nothing from softmodded/marisko or jackiscool123123121/marisko was used.

`firmware/app/zephyr_min/src/main.c` is new code; its watchdog-feed and SYSTEM_OFF sequence follow
the approach in sp1-tape-looper `tools/capacity422/runtime/src/main.c` (MIT) but no code was copied.

Build-time dependencies (downloaded into `../toolchain`, not redistributed; pins in `firmware/west-lock.json`):
Zephyr v4.3.1 `75f67d76…` (Apache-2.0), cmsis `512cc7e8…` (Apache-2.0), cmsis_6 `30a859f4…` (Apache-2.0),
hal_nordic `7858281d…` (BSD-3-Clause / mixed, see module), Zephyr SDK 0.17.4 arm-zephyr-eabi (GCC/binutils/newlib, GPL/LGPL/BSD tools; nothing copied).
The built `zephyr.bin` links Zephyr (Apache-2.0), nrfx/MDK (BSD-3-Clause) and CMSIS (Apache-2.0) code; their notices must accompany any distributed image.
No Infineon/Cypress Bluetooth SDK files or radio payloads are used.

## Research references

Entries below are research references, pinned to the commit inspected.

| Project | Commit | License | Use |
|---|---|---|---|
| chattock/sp1-tape-looper | `0359372e11f61d74a85e2e4449178a5c62010664` | MIT; bundles Zephyr (Apache-2.0), Nordic HAL/nrfx (BSD-3), CMSIS (Apache-2.0), FatFs, dr_libs, SBC decoder, Wakes-derived storage code. Bluetooth radio/Infineon payloads are **not** MIT and not in the source | Candidate foundation for board def and drivers |
| AWD-tech/bonsai-8 | `ef794f10f620c45be22cb38a46f9c7dda6f0007b` | MIT (derived from tape-looper `44ba1ecb`) | Reference |
| jackiscool123123121/marisko | `315d50469ae0f1dd24f78bcb9b6a082879cf824d` | MIT | Driver structure reference |
| softmodded/marisko | `b787d906c96db3f03b11860ec20f0b53441c266d` | **No license file. Do not copy** | Reference only |
| jackiscool123123121/rome | `40023f9d03603c2db29d8adacd1281f3fd5ff71d` | MIT | Possible flashing tool |
| timknapen/SP-1-dev (wiki) | wiki `d661d3f176c869c467e63a66f3fb89e24bb124d6` | MIT (repo) | Hardware docs and pin map |
| ARM-software/CMSIS-DSP | `9f64ba31591e527c505951943b4b95b0eb2f834b` | Apache-2.0 | Possible DSP primitives |
| bnjreece/awesome-te | `02c9fb3c95b7fa4bae954be4d7ae795396d10e93` | see repo | Link index |

Not reviewed: solderless.engineering terms, ericlewis/sp1-midi, reverbose.com page.

## Bring-up apps added later (details in each app's NOTICES.md)

- `firmware/app/zephyr_controls/NOTICES.md`: ladder/fader thresholds and LED/rail handling adapted from chattock/sp1-tape-looper `0359372e` (MIT).
- `firmware/app/zephyr_sync/NOTICES.md`: MIDI/PO sync bit-banging approach adapted from chattock/sp1-tape-looper `0359372e` (MIT).
- `firmware/app/zephyr_usbaudio/NOTICES.md`: UAC2/feedback approach adapted from chattock/sp1-tape-looper `0359372e` (MIT) and Zephyr samples (Apache-2.0).

## Added with the integrated firmware (zephyr_sp1fx, rc1 onwards)

This project's own code is MIT, Copyright (c) 2026 Alex Typaldos (see `LICENSE`).

Adapted from chattock/sp1-tape-looper `0359372e11f61d74a85e2e4449178a5c62010664` (MIT, Copyright (c) 2026 chattock; full text in
`firmware/boards/LICENSES/Tape-Looper-MIT.txt`). Each file below carries a comment naming its source:

| Path | What was taken |
|---|---|
| `firmware/drivers/ladder.c/.h` | button-ladder ADC thresholds |
| `firmware/drivers/controls_hw.c/.h` | fader/ladder/LED/rail handling, ported and restructured |
| `firmware/drivers/midi_out.c/.h`, `firmware/drivers/sync_hw.c/.h` | MIDI and PO sync pin handling and timer approach, rewritten |
| `firmware/drivers/saadc_scan.h` | same channel setup as the verified controls image (register-level scanner is new code) |
| `firmware/app/zephyr_tone/src/sp1_codecs.c/.h` | CS42L42 / TAS2505 initialisation (licence header kept in the file) |
| `firmware/boards/teenageengineering/stem_player/*` | verbatim copy of the board definition |

Techniques only, no code copied (see `docs/research/TAPELOOPER_DSP_NOTES.md`): reduced-rate reverb and echo, cheap LFOs, per-block
coefficient updates, cycle-budget guard. Tape Looper's reverb topology descends from Mutable Instruments Clouds; its licence was not
reviewed and nothing from Clouds is used here.

Hardware facts (pin map, codec/charger signals) come from timknapen/SP-1-dev (MIT; text in `firmware/boards/LICENSES/Tim-Knapen-SP1-dev-MIT.txt`)
and from the Tape Looper source. The charger pin names and the battery divider ratio are taken from those sources and are not verified by measurement.

## Look-and-feel references (no assets copied)

- `companion/` look and feel is modelled on the Tape Looper companion site (MIT, Copyright (c) 2026 chattock; `companion/LICENSES/Tape-Looper-MIT.txt`).
- `docs/guide/` card-and-leader-line layout is modelled on the Marisko firmware guide. Only the layout idea is used; no artwork or text was copied.
  Note: two Marisko repositories are listed above and only one has a licence file, so nothing from either was reused.

## Developer-only tools (not part of any firmware image, not redistributed)

- `tools/fxprof_emu/` needs Python packages installed by the user: `unicorn` (GPL-2.0), `capstone` (BSD), `pyelftools` (public domain). They are used to estimate
  cycle counts offline and are not linked into or shipped with anything here.
- Host tests are plain C99 with no third-party code. The companion has no npm runtime dependencies; tests run on Node 22.

## Items to review before the repository is made public

- `ericlewis/sp1-midi`: credited in the board devicetree header (MIT per Tape Looper's file); its licence text has not been reviewed or copied.
- Confirm the copyright holder name in `LICENSE`, and that no personal paths or device identifiers remain in docs, logs or commit history.
- Flash images are not committed (`/flash-ready` is outside git). If images are ever published they must ship with the Zephyr, nrfx and CMSIS notices.
- Fonts or external resources used by the companion and the guide (check `companion/index.html` and `docs/guide/sp1fx-guide.html`).
