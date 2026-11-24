# Notices: zephyr_usbaudio / zephyr_usbaudio_io (USB audio bring-up images)

These two apps (and `firmware/audio/usb_audio_core.[ch]`) are new SP-1 FX code
(MIT) that reuse or follow third-party work as listed below. This file exists
so the attribution travels with the app; fold it into the repo-wide
`THIRD_PARTY_NOTICES.md` when convenient.

## chattock/sp1-tape-looper (MIT, Copyright (c) 2026 chattock), commit 0359372e

- **Feedback law** (`ua_fb_update()` in `firmware/audio/usb_audio_core.c`):
  follows `feedback_update()` in `tools/capacity422/runtime/src/main.c`
  (EMA low-pass of the ring fill, proportional-only correction around the
  nominal Q10.14 value, clamp of +-1 frame per SOF; "the ring level is itself
  the integral of the rate mismatch"). Re-implemented, not copied; gain and
  set-point differ (Kp 8, 192-frame set-point; Tape Looper: Kp 3, 512).
- **UAC2 board node** `uac2_speaker` (used unmodified by image A) lives in
  `firmware/boards/teenageengineering/stem_player/stem_player.dts`, which
  already carries its Tape Looper attribution (`firmware/boards/LICENSES/`).
- **Codec bring-up** is the tone image's `firmware/app/zephyr_tone/src/sp1_codecs.c`
  (adapted from Tape Looper, attribution in that file), compiled in, not copied.
- **Patch `patches/zephyr-uac2.patch`** (Windows Full-Speed feedback format):
  NOT used and NOT copied; see docs/BRINGUP_USBAUDIO.md, "Windows".

## Zephyr RTOS (Apache-2.0), v4.3.1

- The UAC2 callback structure (`get_recv_buf` / `data_recv_cb` /
  `sof_cb` / `feedback_cb`, IN packets sent from the SOF callback) follows
  `samples/subsys/usb/uac2_explicit_feedback` and
  `samples/subsys/usb/uac2_implicit_feedback` (Copyright (c) 2023-2024 Nordic
  Semiconductor ASA). Image B's devicetree node follows the implicit-feedback
  sample's node layout.
- `samples/subsys/usb/common` (sample_usbd_init_device) is compiled in from the
  Zephyr tree, as in the tone image.

Nothing from softmodded / marisko (no license) was used.
