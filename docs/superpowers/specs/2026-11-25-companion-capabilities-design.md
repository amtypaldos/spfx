# Companion feature detection (capabilities in HELLO_RSP)

Status: design, approved approach A (2026-11-25). Short design (small change).

## Intent

The companion will be hosted once, on GitHub Pages, from the latest release. Going forward it must work
with whatever firmware is on the device: show what that firmware supports, and gray out the rest instead of
failing. Only future releases matter. Everything so far is an RC, so no per-RC support for rc1-rc5.

## Goals

- Firmware declares its features in HELLO_RSP; the companion grays out controls whose feature is missing.
- Companions ignore unknown trailing bytes in device messages, so an older companion keeps working with
  newer firmware (it just doesn't show the new things).
- New features in future releases only need a new capability bit.

## Non-goals / anti-goals

- No release-name table in the companion, and no parsing of `sp1fx-v1-rcN`. The version string stays
  display-only.
- No support for firmware older than rc6. A HELLO_RSP without the caps field is treated as the rc6 feature set.
- Firmware request parsing stays strict (host -> device). Only the host side becomes tolerant.
- Anti-goal: hiding controls so the page layout jumps between devices, or failing to connect because of a
  missing feature.

## Approaches considered

- **A (chosen): capability bitmask in HELLO_RSP.** The firmware is the authority, custom builds work, and nothing
  needs updating per release. Cost: spec + firmware change, new image (rc7).
- B: companion table keyed by the version string. No firmware change, but it has to be updated every release and
  has to guess for unknown names.
- C (dropped): infer features from message lengths. Can't tell Line out from Loud, and gets more fragile with every
  release.

## Design

**Spec first (`docs/PROTOCOL.md`):**
- HELLO_RSP gains a trailing `caps u32` (little-endian, firmware v1-rc7+).
- Absent caps = the rc6 set (all bits defined below set).
- Hosts must ignore unknown caps bits and unknown trailing bytes in any device -> host message.
- Bits (initial set; reserved bits are 0):
  - bit0 `SYNC`: sync-jack clock, SET_SYNC / SET_BPM drive it.
  - bit1 `OUT_LINE`: SET_OUTPUT_MODE 1.
  - bit2 `OUT_LOUD`: SET_OUTPUT_MODE 2.
  - bit3 `BATTERY`: METERS batt_pct.
  - bit4 `SAVE`: SAVE_NOW has a storage backend.
  - bit5 `POWER_FLAGS`: METERS flags bits 1-2.
  - Every later feature adds the next bit, in the same spec change.

**Firmware (`firmware/protocol/`):**
- `build_hello` appends caps.
- Bits that follow from `proto_config_t` are derived from it: sync != NULL, save != NULL, batt_pct != NULL,
  power_flags != NULL.
- The output-mode bits come from a new config field set by the app, with zephyr_sp1fx setting both.
- `CONFIG_SP1FX_FW_VERSION` -> `sp1fx-v1-rc7`.
- No change to the audio path.

**Companion:**
- `protocol.js` parses the optional caps; the device -> host decoders stop rejecting trailing bytes.
- The model stores `caps`, which the UI uses to disable unsupported controls:
  - Sync toggle.
  - The Loud and Line out options in the level picker.
  - Battery text: show "battery: n/a".
  - Save-to-device.
- A disabled control gets a short "Needs newer firmware" note (title + visible hint), and the control stays in place.
- The demo device advertises all caps.

**Tests:**
- Host C test that HELLO_RSP carries the expected caps for given configs.
- Companion tests:
  - parse with and without caps;
  - trailing bytes tolerated on every device message;
  - the UI model disables the right controls for a reduced caps mask.
- `make test` 0 warnings; companion tests pass.

**Release:**
- rc7 is built with `--app sp1fx repro`.
- STATUS.md / BRINGUP_SP1FX.md updated, labelled NOT RUN ON HARDWARE.
- The owner flashes. No flashing by agents.

## Left to the builder

The exact config field name for output modes, the wording and styling of the "needs newer firmware" hint (match
existing `.stale`/disabled styles), and test file placement.
