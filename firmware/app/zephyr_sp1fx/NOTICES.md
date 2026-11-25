# Notices: zephyr_sp1fx (integrated firmware)

The integrated firmware is SP-1 FX code (MIT, see `LICENSE`). It is assembled from the
bring-up images, so it carries the same third-party credits. This file lists them in one
place; the per-image details are in `firmware/app/zephyr_controls/NOTICES.md`,
`firmware/app/zephyr_sync/NOTICES.md` and `firmware/app/zephyr_usbaudio/NOTICES.md`, and the
repository-wide list is `THIRD_PARTY_NOTICES.md`. The notices that must ship with a built
image are in `docs/BINARY_NOTICES.md`.

## chattock/sp1-tape-looper (MIT, Copyright (c) 2026 chattock), commit 0359372e

Licence text: `firmware/boards/LICENSES/Tape-Looper-MIT.txt`.

| Part of this image | From Tape Looper (`tools/capacity422/runtime/src/main.c` unless noted) |
|---|---|
| `firmware/app/zephyr_tone/src/sp1_codecs.c/.h` (compiled in) | CS42L42 / TAS2505 init and helpers, adapted; the file keeps Tape Looper's MIT header |
| `firmware/drivers/controls_hw.c`, `firmware/drivers/ladder.c/.h` | ladder thresholds, fader scaling, LED PWM approach and pin tables, adapted |
| `firmware/drivers/midi_out.c`, `firmware/drivers/sync_hw.c` | MIDI / PO sync framing, pins and TIMER2 approach, rewritten |
| `firmware/drivers/saadc_scan.c/.h` | the SAADC channel setup the ladder thresholds were calibrated under |
| `firmware/audio/usb_audio_core.c` | the explicit-feedback law (`feedback_update()`), re-implemented |
| `src/main.c` | patterns, no code copied: watchdog feed of all RR channels, power-off / SYSTEM_OFF order, soft reset into a charge standby while USB power is present, BQ24232 charger pins (`charger_init()`), I2S slave configuration and codec init order, USB device_next + CDC ACM setup |
| board definition `firmware/boards/teenageengineering/stem_player/` | verbatim copy (see `firmware/boards/LICENSES/NOTICE.txt`) |

## timknapen/SP-1-dev (MIT, Copyright (c) 2026 Tim Knapen)

Pin map and hardware facts (codec, charger, battery divider, bootloader notes) from the
repository and its wiki. Licence text: `firmware/boards/LICENSES/Tim-Knapen-SP1-dev-MIT.txt`.

## ericlewis/sp1-midi (MIT, Copyright (c) 2026 Eric Lewis), commit 43f5eae9

Credited by the board devicetree header (the board was reconstructed in part from
sp1-midi's `stem_player` board). Licence text: `firmware/boards/LICENSES/Eric-Lewis-sp1-midi-MIT.txt`.

## Zephyr RTOS v4.3.1 (Apache-2.0)

- `samples/subsys/usb/common/sample_usbd_init.c` and `Kconfig.sample_usbd`
  (Copyright (c) 2023 Nordic Semiconductor ASA) are compiled in unmodified from the pinned
  Zephyr checkout (`CMakeLists.txt` includes `common.cmake`).
- The UAC2 callback structure in `src/main.c` (`get_recv_buf`, `data_recv_cb`, `sof_cb`,
  `feedback_cb`) follows `samples/subsys/usb/uac2_explicit_feedback`
  (Copyright (c) 2023-2024 Nordic Semiconductor ASA), by way of `zephyr_usbaudio`.

Nothing from softmodded/marisko (no licence) is used. No Infineon/Cypress or Bluetooth files
are used.
