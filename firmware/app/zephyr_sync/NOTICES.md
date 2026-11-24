# Third-party notices: zephyr_sync (sync-jack bring-up image)

To be merged into the repo's THIRD_PARTY_NOTICES.md by the owner. (It is kept
separate because other work was editing that file at the same time.)

## chattock/sp1-tape-looper: MIT, (c) chattock

- Upstream: chattock/sp1-tape-looper, commit `0359372e11f61d74a85e2e4449178a5c62010664`
- File: `tools/capacity422/runtime/src/main.c`, the "MIDI clock + Pocket-Operator sync out
  over the SYNC jack" section (around lines 12260-12385: `MIDI_PIN`, `POSYNC_PIN`,
  `POSYNC_PIN_B`, `MIDI_INVERT`, `MIDI_BIT_US`, `PO_PULSE_MS`, `PO_DIV`, `midi_pins_init()`,
  `midi_line()`, `midi_timer_isr()`, `midi_timer_init()`, `midi_send()`).
- License text: `firmware/boards/LICENSES/Tape-Looper-MIT.txt`.

What was reused, and how it was changed:

| SP-1 FX file | Taken from Tape Looper | Adaptation |
|---|---|---|
| `firmware/drivers/midi_out.c` | 10-bit frame layout `(b << 1) \| (1 << 9)` shifted out LSB first, one bit per 32 us timer event; the line idles at mark | Rewritten as a hardware-independent module: a `tick()` function returns pin levels, two lock-free queues (real-time bytes first, full queue drops the new byte), continuous back-to-back bytes, PO pulse generator with configurable width and polarity and a minimum gap, statistics. No Zephyr or nRF code. |
| `firmware/drivers/sync_hw.c/.h` | Pin assignment P0.23 (BC807 base, ring, inverting PNP), P0.20 PO_A + P0.17 PO_B (tip, paralleled); push-pull S0S1 output config; NRF_TIMER2, 16-bit, prescaler 4 (1 us), CC[0] = 32 with COMPARE0_CLEAR short, the event clear + read-back in the ISR; `MIDI_INVERT = 1` default; 5 ms PO pulse | The timer runs continuously instead of per byte; the ISR calls `midi_out_tick()` and writes all three pins with one OUTSET/OUTCLR pair; IRQ priority 1 instead of 2; idle levels are set before the pins become outputs; an ISR-latency measurement through CAPTURE[1]; a swap-tip/ring option and PO polarity option; `sync_hw_shutdown()` returns the pins to their reset state before SYSTEM_OFF. |
| `firmware/app/zephyr_sync/src/main.c` | Watchdog feed of all 8 RR channels, RESETREAS handling, SYSTEM_OFF sequence (through zephyr_min, which follows Tape Looper's `feed_wdt()` / `power_off()`), the USB CDC ACM / `sample_usbd_init_device` setup pattern (through zephyr_tone) | Written for SP-1 FX; no code copied verbatim. |

Tape Looper's own comments say the MIDI / PO sync output is "untested on real gear". This
image is untested on hardware as well.

## Zephyr RTOS: Apache-2.0

The image links Zephyr v4.3.1 (`75f67d76…`), hal_nordic and CMSIS as pinned in
`firmware/west-lock.json`, plus Zephyr's `samples/subsys/usb/common/sample_usbd_init.c`
(Apache-2.0) for the USB device setup, as the other bring-up images do. These are already
covered by the existing Zephyr notice entries.

Nothing from softmodded/marisko, and no Infineon/Bluetooth files, is used.
