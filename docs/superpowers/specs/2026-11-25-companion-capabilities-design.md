# Companion feature detection (capabilities in HELLO_RSP)

Status: design, approved approach A (2026-11-25), plus date-based version numbers (owner request). Short design (small change).

## Intent

The companion will be hosted once, on GitHub Pages, from the latest release. Going forward it must work
with whatever firmware is on the device: show what that firmware supports, and gray out the rest instead of
failing. Only future releases matter. Everything so far is an RC, so no per-RC support for rc1-rc5.

## Goals

- Firmware declares its features in HELLO_RSP; the companion grays out controls whose feature is missing.
- Companions ignore unknown trailing bytes in device messages, so an older companion keeps working with
  newer firmware (it just doesn't show the new things).
- New features in future releases only need a new capability bit.
- Releases are named by date instead of `v1-rcN` (owner request, 2026-11-25).

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
- The app declares its features in a new `proto_config_t.caps` field (`PROTO_CAP_*` bits); HELLO_RSP sends it as is.
  Not derived from the other config pointers: zephyr_sp1fx drives the sync jack itself and passes `sync = NULL`
  (`main.c`), so a derived SYNC bit would be wrong (found while planning).
- SET_OUTPUT_MODE answers BAD_ARG for a mode whose cap bit is not set (zephyr_sp1fx sets both, so no change there).
- `CONFIG_SP1FX_FW_VERSION` -> `sp1fx-YYYY.MM.DD` of the release (see Version numbers).
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

**Version numbers (date-based, from this release on):**
- Format `YYYY.MM.DD`, the release date. A second or third release on the same day gets `.2`, `.3`
  (rc4-rc6 all shipped on 2026-11-24, so this happens).
- Firmware string `sp1fx-2026.11.26`, git tag `v2026.11.26` (the `v` prefix keeps the Pages workflow trigger `v*`),
  image `sp1fx-2026.11.26.bin` / `.elf`, companion zip `sp1fx-companion-2026.11.26.zip`.
- "Not run on hardware" is shown by the GitHub pre-release flag and the STATUS.md label, no longer by "rc" in the name.
  The release is promoted (pre-release flag off) once the owner reports it working on hardware.
- The protocol version (`proto` in HELLO_RSP, currently 1) stays separate and only changes on breaking wire changes.
- Old `v1-rcN` tags, releases and docs stay as history; the docs say once where the scheme changed.
- Nothing parses the version string (checked: only the Kconfig default and a test fixture name contain it).

**Release:**
- The first date-named image (replaces the planned rc7) is built with `--app sp1fx repro`.
- STATUS.md / BRINGUP_SP1FX.md updated, labelled NOT RUN ON HARDWARE.
- The owner flashes. No flashing by agents.

## Left to the builder

The exact config field name for output modes, the wording and styling of the "needs newer firmware" hint (match
existing `.stale`/disabled styles), and test file placement.
