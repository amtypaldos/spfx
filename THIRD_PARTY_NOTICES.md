# Third-party notices and pinned upstreams

## Copied into this repository

| Path | Source | Commit | License | Notes |
|---|---|---|---|---|
| `firmware/app/zephyr_tone/src/sp1_codecs.c` (adapted; licence + copyright header kept in the file) | chattock/sp1-tape-looper `tools/capacity422/runtime/src/main.c`: `tas_wr()`/`tas_page()`/`tas_set_speaker()`, `tas2505_configure()`, `tpw()`/`tpr()`, `hp_detect_connected()`, `HP_TIM_TEST` branch of `hp_codec_init()` | `0359372e11f61d74a85e2e4449178a5c62010664` | MIT, Copyright (c) 2026 chattock (`firmware/boards/LICENSES/Tape-Looper-MIT.txt`). The CS42L42 sequence in it is Tim Knapen's SP-1-dev wiki `I2C.md` (MIT, Copyright (c) 2026 Tim Knapen, `firmware/boards/LICENSES/Tim-Knapen-SP1-dev-MIT.txt`) | Register addresses/values kept; changed for hearing safety and pops: both outputs configured muted and unmuted only by the app, CS42L42 mixer -30 dB (was 0 dB), TAS2505 class-D gain +6 dB (was +18 dB), speaker attenuation -6 dB (was 0 dB), DAC muted before power-up, new mute/unmute helpers; restructured into functions without Tape Looper globals |
| `firmware/boards/teenageengineering/stem_player/*` (8 files, unmodified; byte-identical to the upstream blobs at that commit) | chattock/sp1-tape-looper `boards/teenageengineering/stem_player` | `0359372e11f61d74a85e2e4449178a5c62010664` | MIT, Copyright (c) 2026 chattock (`firmware/boards/LICENSES/Tape-Looper-MIT.txt`). The Kconfig and defconfig files carry `SPDX-License-Identifier: Apache-2.0` headers, kept as-is (`firmware/boards/LICENSES/Apache-2.0.txt`). | dts header: reconstructed from ericlewis/sp1-midi's board (MIT, Copyright (c) 2026 Eric Lewis, commit `43f5eae90d999a7defd9a20d06cd1bf4612fa235`, `firmware/boards/LICENSES/Eric-Lewis-sp1-midi-MIT.txt`) and timknapen/SP-1-dev pin map + wiki (MIT, Copyright (c) 2026 Tim Knapen, `firmware/boards/LICENSES/Tim-Knapen-SP1-dev-MIT.txt`) |

`firmware/app/zephyr_tone/src/main.c` is new code; its I2S configuration (clock-slave, 16-bit, I2S format, prime-then-START,
drop/re-prime failsafe), its init order (I2S, TAS2505, CS42L42) and its prj.conf USB device_next/CDC ACM settings follow Tape Looper's
`audio_thread()`/`audio_init()`/`usb_audio_start()` and `prj.conf` (same commit, MIT); the track-LED pin table (P0.29, P0.26, P1.15, P1.14)
comes from Tape Looper's `track_leds[]`. It links Zephyr's `samples/subsys/usb/common/sample_usbd_init.c` (Apache-2.0, part of the pinned
Zephyr checkout, not copied). Nothing from softmodded/marisko or jackiscool123123121/marisko was used.

`firmware/app/zephyr_min/src/main.c` is new code; its watchdog-feed and SYSTEM_OFF sequence follow
the approach in sp1-tape-looper `tools/capacity422/runtime/src/main.c` (MIT) but no code was copied.

Build-time dependencies (downloaded into `../toolchain`, not redistributed in source form; pins in `firmware/west-lock.json`):
Zephyr v4.3.1 `75f67d76…` (Apache-2.0), cmsis `512cc7e8…` (Apache-2.0), cmsis_6 `30a859f4…` (Apache-2.0),
hal_nordic `7858281d…` (nrfx 3.14.0: BSD-3-Clause; four MDK `system_*` files Apache-2.0; the module's Zephyr glue and other
parts are under other licences, including `LicenseRef-Nordic-5-Clause`, none of which is linked into zephyr_sp1fx),
Zephyr SDK 0.17.4 arm-zephyr-eabi (GCC 12.2.0 and binutils, GPL; picolibc 1.8.10 and libgcc are linked into images, see below).

**Binary images.** What the integrated firmware image actually links was read from its linker map (zephyr_sp1fx, v1-rc6; release
2026.11.25 links the same libraries, see `docs/BINARY_NOTICES.md`):
Zephyr (Apache-2.0), CMSIS_6 Core headers (Apache-2.0; the older `cmsis` module is not used), nrfx/MDK (BSD-3-Clause, four
Apache-2.0 MDK files), picolibc 1.8.10 from the SDK (BSD-style licences, a Sun/SunPro notice, a Cygnus notice, Ryu under
Apache-2.0 OR BSL-1.0), libgcc (GPL-3.0+ with the GCC Runtime Library Exception, no notice required) and the Tape Looper-derived
code (MIT). The full licence texts and copyright lines are in **`docs/BINARY_NOTICES.md`**, which must be shipped with any
published image. No Infineon/Cypress Bluetooth SDK files or radio payloads are used.

## Research references

Entries below are research references, pinned to the commit inspected.

| Project | Commit | License | Use |
|---|---|---|---|
| chattock/sp1-tape-looper | `0359372e11f61d74a85e2e4449178a5c62010664` | MIT; bundles Zephyr (Apache-2.0), Nordic HAL/nrfx (BSD-3), CMSIS (Apache-2.0), FatFs, dr_libs, SBC decoder, Wakes-derived storage code. Bluetooth radio/Infineon payloads are **not** MIT and not in the source | Candidate foundation for board def and drivers |
| AWD-tech/bonsai-8 | `ef794f10f620c45be22cb38a46f9c7dda6f0007b` | MIT (derived from tape-looper `44ba1ecb`) | Reference |
| jackiscool123123121/marisko | `315d50469ae0f1dd24f78bcb9b6a082879cf824d` | MIT | Driver structure reference |
| softmodded/marisko | `b787d906c96db3f03b11860ec20f0b53441c266d` | **No license file. Do not copy** | Reference only |
| jackiscool123123121/rome | `40023f9d03603c2db29d8adacd1281f3fd5ff71d` | MIT | Possible flashing tool |
| timknapen/SP-1-dev (wiki) | wiki `d661d3f176c869c467e63a66f3fb89e24bb124d6`; repo licence checked at `af2c50315996dfe5c78725995dbe498790ad763c` | MIT, Copyright (c) 2026 Tim Knapen (text identical to `firmware/boards/LICENSES/Tim-Knapen-SP1-dev-MIT.txt`) | Hardware docs and pin map |
| ericlewis/sp1-midi | `43f5eae90d999a7defd9a20d06cd1bf4612fa235` (the only commit) | MIT, Copyright (c) 2026 Eric Lewis (`firmware/boards/LICENSES/Eric-Lewis-sp1-midi-MIT.txt`) | Credited by the board devicetree (part of its source); nothing else used |
| ARM-software/CMSIS-DSP | `9f64ba31591e527c505951943b4b95b0eb2f834b` | Apache-2.0 | Possible DSP primitives |
| bnjreece/awesome-te | `02c9fb3c95b7fa4bae954be4d7ae795396d10e93` | see repo | Link index |

Not reviewed: solderless.engineering terms, reverbose.com page (neither is used in or shipped with anything here).

## Bring-up apps added later (details in each app's NOTICES.md)

- `firmware/app/zephyr_controls/NOTICES.md`: ladder/fader thresholds and LED/rail handling adapted from chattock/sp1-tape-looper `0359372e` (MIT).
- `firmware/app/zephyr_sync/NOTICES.md`: MIDI/PO sync bit-banging approach adapted from chattock/sp1-tape-looper `0359372e` (MIT).
- `firmware/app/zephyr_usbaudio/NOTICES.md`: UAC2/feedback approach adapted from chattock/sp1-tape-looper `0359372e` (MIT) and Zephyr samples (Apache-2.0).
- `firmware/app/zephyr_sp1fx/NOTICES.md`: the integrated firmware; collects the credits above for the parts it is built from.

## Added with the integrated firmware (zephyr_sp1fx, rc1 onwards)

This project's own code is MIT, Copyright (c) 2026 Alex Typaldos (see `LICENSE`).

Adapted from chattock/sp1-tape-looper `0359372e11f61d74a85e2e4449178a5c62010664` (MIT, Copyright (c) 2026 chattock; full text in
`firmware/boards/LICENSES/Tape-Looper-MIT.txt`). Each file below names its source in a comment, or (for `zephyr_sp1fx/src/main.c`
and `saadc_scan.h`, which only reuse patterns or settings) in `firmware/app/zephyr_sp1fx/NOTICES.md`:

| Path | What was taken |
|---|---|
| `firmware/drivers/ladder.c/.h` | button-ladder ADC thresholds |
| `firmware/drivers/controls_hw.c/.h` | fader/ladder/LED/rail handling, ported and restructured |
| `firmware/drivers/midi_out.c/.h`, `firmware/drivers/sync_hw.c/.h` | MIDI and PO sync pin handling and timer approach, rewritten |
| `firmware/drivers/saadc_scan.h` | same channel setup as the verified controls image (register-level scanner is new code) |
| `firmware/audio/usb_audio_core.c/.h` | explicit-feedback law after `feedback_update()`, re-implemented with different gains |
| `firmware/app/zephyr_sp1fx/src/main.c` | patterns only (watchdog feed, power-off order, charge standby, charger pins, I2S/codec init order); see its `NOTICES.md` |
| `firmware/app/zephyr_tone/src/sp1_codecs.c/.h` | CS42L42 / TAS2505 initialisation (licence header kept in the file) |
| `firmware/boards/teenageengineering/stem_player/*` | verbatim copy of the board definition |

Techniques only, no code copied (see `docs/research/TAPELOOPER_DSP_NOTES.md`): reduced-rate reverb and echo, cheap LFOs, per-block
coefficient updates, cycle-budget guard. Tape Looper's reverb topology descends from Mutable Instruments Clouds; its licence was not
reviewed and nothing from Clouds is used here.

Hardware facts (pin map, codec/charger signals) come from timknapen/SP-1-dev (MIT; text in `firmware/boards/LICENSES/Tim-Knapen-SP1-dev-MIT.txt`)
and from the Tape Looper source. The charger pin names and the battery divider ratio are taken from those sources and are not verified by measurement.

## Published algorithms (own implementations, credited for their origin)

The DSP, protocol and storage code is written for this project. It implements textbook methods; no third-party code was copied
for them (the files named were reviewed before publication; no copied code was identified, but this is a source review, not a
similarity scan):

- `firmware/dsp/effects/filter.c`: state-variable filter in Vadim Zavalishin's topology-preserving-transform form
  ("The Art of VA Filter Design").
- `firmware/dsp/effects/reverb.c`: Schroeder allpass diffusers and a 4-line feedback delay network with Hadamard mixing (Stautner and
  Puckette, Jot). Line lengths are this project's own primes; Tape Looper's Clouds-derived reverb was studied but not used.
- `firmware/dsp/util.h`: rational (Padé-type) tanh approximation `x(27+x^2)/(27+9x^2)`, Marsaglia's xorshift32 noise generator,
  a truncated Taylor series for exp2, and a quadratic log2 mantissa fit whose constants (`-0.34484843`, `2.02466578`, `-1.67487759`)
  are widely published; their first source was not traced.
- `firmware/protocol/protocol.c`, `companion/js/protocol.js`: COBS framing (Cheshire and Baker) and CRC-16/CCITT-FALSE.
  `firmware/storage/config_store.c`: CRC-32 (IEEE 802.3, reflected polynomial `0xEDB88320`).

## Look-and-feel references (no assets copied)

- `companion/` look and feel is modelled on the Tape Looper companion site (MIT, Copyright (c) 2026 chattock; `companion/LICENSES/Tape-Looper-MIT.txt`).
- `docs/guide/` card-and-leader-line layout is modelled on the Marisko firmware guide image (`download.png` in jackiscool123123121/marisko
  `315d5046`, a repository with an MIT licence file). Only the layout idea is used; no artwork or text was copied.
  Note: two Marisko repositories are listed above and only one has a licence file, so no code, text or artwork from either was reused.

## Fonts and external resources

- `companion/index.html` and `companion/css/style.css` use system fonts only (`-apple-system`, `BlinkMacSystemFont`, `Helvetica Neue`,
  Arial, `ui-monospace`/Menlo) and load nothing from the network; the favicon is an inline SVG data URI (a rounded square and a path), not an external file.
- `docs/guide/sp1fx-guide.html` uses system fonts only (`Helvetica Neue`, Helvetica, Arial) and inline SVG (the device drawing, redrawn for this project per `docs/guide/README.md`); no external loads.
- `docs/guide/sp1fx-guide.pdf` was printed from that HTML by headless Chrome on macOS and embeds subsets of the macOS system fonts
  Helvetica Neue and Arial (font subsetting is how PDFs normally carry text). `docs/guide/sp1fx-guide.png` is a raster render with no fonts.

## Developer-only tools (not part of any firmware image, not redistributed)

- `tools/fxprof_emu/` needs Python packages installed by the user: `unicorn` (GPL-2.0), `capstone` (BSD), `pyelftools` (public domain). They are used to estimate
  cycle counts offline and are not linked into or shipped with anything here.
- Host tests are plain C99 with no third-party code. The companion has no npm runtime dependencies; tests run on Node 22.

## Public-release review (the repository is now public)

Resolved (2026-11-25 review, branch `public-ready`):

- [x] `ericlewis/sp1-midi`: licence read at its only commit `43f5eae9` (MIT, Copyright (c) 2026 Eric Lewis); full text added as
  `firmware/boards/LICENSES/Eric-Lewis-sp1-midi-MIT.txt` and referenced from `firmware/boards/LICENSES/NOTICE.txt`.
- [x] Copyright holder: "Alex Typaldos" in `LICENSE`, this file and the binary notices; no other holder name is used for this
  project's own code.
- [x] Upstream licence texts: `Tape-Looper-MIT.txt` and `Tim-Knapen-SP1-dev-MIT.txt` are identical to the upstream files at the pinned
  commits; the 8 board files are byte-identical to Tape Looper `0359372e`; `Apache-2.0.txt` is identical to Zephyr v4.3.1's `LICENSE`.
- [x] Binary notices: `docs/BINARY_NOTICES.md` lists what zephyr_sp1fx actually links (from the linker map) with full licence texts
  and copyright lines (Zephyr, CMSIS_6, nrfx/MDK, picolibc, libgcc, Tape Looper, SP-1-dev, sp1-midi).
- [x] Fonts and external resources: system fonts only, no network loads (see "Fonts and external resources" above).
- [x] Attribution comments: every file listed in this document names its source in a header comment or in its app's `NOTICES.md`;
  the integrated firmware now has `firmware/app/zephyr_sp1fx/NOTICES.md`.
- [x] Personal paths: none in the current tree (absolute home paths were removed in `bf9dd0b`; they remain in older commits, see below).

- [x] **Commit history** (e-mail addresses, home-directory paths in older commits): the history was rewritten before the repository
  was made public (owner, 2026-11-25).
- [x] Release **v2026.11.25** ships `BINARY_NOTICES.md` as a release asset, and its `.elf` has no build-machine paths
  (`-ffile-prefix-map`, see `docs/STATUS.md`).

Remaining (owner decisions or manual steps):

- [ ] **Release v1-rc6** (`sp1fx-v1-rc6.bin`, `.elf`) had no notices attached when this list was written. Upload
  `docs/BINARY_NOTICES.md` as a release asset (or add it to the archive); not confirmed whether this was done. The rc6 `.elf` contains
  the build machine's home-directory paths in its debug information; remove it from the release or leave it as is (owner decision).
- [ ] `docs/guide/sp1fx-guide.pdf` embeds subsets of the macOS system fonts Helvetica Neue and Arial. Embedding is normal for PDFs, but
  the font licences were not checked; re-print with an openly licensed font if that matters.
- [ ] The research notes (`docs/research/`, `docs/PHASE0_REPORT.md`) quote short excerpts and line numbers from upstream sources; review
  them once more if any upstream author objects.
