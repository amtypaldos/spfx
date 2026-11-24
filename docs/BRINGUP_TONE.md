# Audio bring-up image: quiet test tone (`zephyr_tone`)

Status: **built, not run.** This image has **never run on an SP-1**. Every behaviour below
is *expected* from reading the code and the sources it was ported from; none of it has been
observed. Nothing in this repository flashes anything; flashing is a manual step by the owner.

> **HEARING SAFETY — FIRST RUN: keep headphones OFF your ears.** Plug them in and hold them a
> few centimetres from your ear, or lay them on the desk. The tone is designed to be quiet
> (-30 dBFS digital, codec headphone volume -30 dB, slow fade-in), but the codec register
> values were never checked against a running device. A wrong guess about a volume register
> could make it much louder than intended. Do the same with the speaker: don't hold the SP-1
> against your ear.

## What it does

| Part | Behaviour |
|---|---|
| Watchdog | Feeds all 8 WDT reload channels every 10 ms (bootloader WDT, < 5 s). Same code as `zephyr_min`. |
| LED 1 (P0.29) | Heartbeat, 0.5 s on / 0.5 s off. |
| LED 2 (P0.26) | Steady on if the CS42L42 answered at I2C 0x48 with DEVID_AB = 0x42. |
| LED 3 (P1.15) | Steady on if the TAS2505 ACKed at I2C 0x18. |
| LED 4 (P1.14) | On while the I2S stream ran without errors/timeouts for the last second (>= 700 of the nominal 750 blocks/s). Updated once per second. |
| FUNCTION (P0.27) | Held 1.5 s: fade out, mute both outputs, soft-reset TAS2505, stop I2S, hold both codecs in reset, oscillator off, all LEDs off, wait for release, clear `RESETREAS`, `SYSTEM_OFF`. The next FUNCTION press wakes into the TE bootloader. |
| Clocks | P0.13 high turns on the 3.072 MHz oscillator (I2S SCK). The CS42L42 is the LRCK master (48 kHz = 3.072 MHz / 64). The nRF I2S and the TAS2505 are clock slaves. |
| Audio | 48 kHz, stereo, 16-bit I2S words, 64-frame blocks (1.33 ms) from a static 8-block `k_mem_slab` (2 KiB), Zephyr `i2s_nrfx` driver with a 4-block TX queue. No dynamic allocation. |
| Tone | **Left: 440 Hz. Right: 660 Hz.** (660 Hz is a fifth above 440 Hz: the right channel sounds clearly higher.) Each is a pure sine at -30 dBFS peak (\|sample\| <= 1036). Generated with a quadrature (rotation) oscillator in float. The repo's `fx_sin01()` was not used: it is an LFO approximation with ~3 % shape error, which would add harmonics. |
| Fade | Fade-in 0 -> -30 dBFS over **1.5 s** every time an output turns on (also at boot). Before any mute change the level ramps to exactly 0 in 30 ms, and the code waits for the queued blocks (~8 ms) to play out. |
| Headphone detect | CS42L42 DET_STATUS1 (reg 0x1B77) bit 7, as in Tape Looper / Tim's wiki. Read 3x at start (majority), then every 250 ms; 2 equal reads in a row are needed to switch; failed reads keep the last state. **Headphones in: speaker muted, headphones on. Headphones out: headphones muted, speaker on.** |
| Speaker | TAS2505 is mono and (per Tape Looper's DAC routing 0x3F = 0x90) plays the **left** channel: expect **440 Hz only** from the speaker. Class-D gain +6 dB (smallest non-mute step; Tape Looper uses +18 dB), speaker attenuation -6 dB. |
| Headphones | CS42L42 mixer attenuation -30 dB (register max loudness is 0 dB, which Tape Looper uses). |
| USB console | USB CDC ACM (device_next stack, like Tape Looper). VID:PID `2fe3:0001` (VID hard-coded to the Zephyr Project's development VID by Zephyr v4.3.1's sample helper), manufacturer "sp1-fx", product "SP-1 FX tone test", 100 mA. Prints one status line per second while a terminal has the port open (DTR). Non-blocking: if the host is not reading, lines are dropped and counted. |

Boot order: LEDs/FN set up; codec resets asserted; USB started; oscillator on; resets
released; I2C probe (LEDs 2/3); I2S thread starts (prime 4 silent blocks, START; it waits
for clocks); TAS2505 configured (speaker muted); CS42L42 configured (headphones muted; LRCK
starts); wait up to 0.5 s for the stream; headphone detect; unmute the chosen output; fade in.

## Build

```sh
firmware/scripts/build_firmware.sh --app zephyr_tone           # build + check_image.py
firmware/scripts/build_firmware.sh --app zephyr_tone repro     # two clean builds, compare SHA-256
firmware/scripts/build_firmware.sh --app zephyr_tone --no-usb  # variant without USB/console
```

Result of the build on 2026-11-24 (this Mac, pinned toolchain from `docs/BUILD.md`):

| Item | Value |
|---|---|
| Image | `build/firmware/zephyr_tone/zephyr/zephyr.bin`, copied to `~/spfx/flash-ready/sp1fx-tone.bin` |
| Flash (bin) size | **60,636 B** (0xECDC), 6.64 % of the 0xDF000 slot |
| Load range | `0x00020000`–`0x0002ECDB` |
| Static RAM | **19,336 B**, 7.38 % of 256 KiB |
| Initial SP / reset vector | `0x20002c50` / `0x00021a19` |
| SHA-256 | `87886d142ba92ddcd681f9805c39e996eec71451b18310890da455a8ce7433f9` |
| check_image.py | **PASS** |
| Reproducible | yes: two clean builds in different directories gave the identical `.bin` |
| Compiler warnings | 0 (dtc prints the same 2 upstream nRF52840 SoC warnings as `zephyr_min`) |
| `--no-usb` variant | 37,028 B flash, 9,312 B RAM, PASS, SHA-256 `6f89034ffdc7abce78e029615756ec88875765c34d85a3d92338139985a8eb4d` (not copied to flash-ready) |

`zephyr_min` still builds bit-identical to its documented SHA-256 (`18afab48…2667`).

## Flashing (manual, by the owner)

> The SP-1 has no reset pin and no easy power cycle. Flashing replaces the current app.
> Back up anything you need first.

1. Check the file: `shasum -a 256 -c ~/spfx/flash-ready/SHA256SUMS`
   (`sp1fx-tone.bin` must be `87886d14…33f9`).
2. Desktop Chrome (WebUSB), open <https://solderless.engineering>.
3. SP-1 off. Hold **T1 + T4** while connecting USB; release when **T1 lights up**. The
   device is now in the TE bootloader.
4. Flash `sp1fx-tone.bin`.
5. **Headphones unplugged** for the first boot, or plugged in but **not on your ears**.

## Expected observations (not yet observed)

1. Within ~1 s of boot: LED 1 blinks at 1 Hz. LED 2 and LED 3 light steadily (both codecs
   answered; Tape Looper reports both ACK on real hardware). About 1–2 s after boot LED 4
   lights steadily (stream running).
2. No headphones: a **quiet 440 Hz** tone from the speaker, fading in over ~1.5 s. It
   should be soft; hold the SP-1 near (not against) your ear in a quiet room.
3. Plug headphones in (held off-ear): within ~0.5 s the speaker goes silent, and the
   headphones fade in over ~1.5 s: **440 Hz in the left ear, 660 Hz (higher) in the right
   ear**. A swapped result means the I2S slot / channel mapping guess is wrong. Same pitch in
   both ears means a mono routing.
4. Unplug: the headphones go quiet, the speaker fades back in.
5. No clicks or pops on switching (some may still happen: see guesses).
6. USB serial (Mac): `screen /dev/cu.usbmodem* 115200` (quit: Ctrl-A then K, then y).
   One line per second, for example (illustrative, not captured from a device):

   ```
   t=12 cs42=42a400+ clk=02 tas=ack+ hp=0 out=spk i2s=run cfg=0/0 blk/s=750 err=0 tmo=0 rec=0 gen_us=40 per_us=1500 drop=0 rr=4
   ```

   | Field | Meaning | Healthy |
   |---|---|---|
   | `t` | seconds since boot | counting |
   | `cs42=AABBCC` + flag | CS42L42 DEVID_AB, DEVID_CD, DEVID_E; `+` configured, `!` found but a configure write NAKed, `-` not found | `42a4xx+` |
   | `clk` | CS42L42 reg 0x1109 after the "oscillator switch" (Tim's wiki waits for `02` = SCLK selected) | `02` |
   | `tas=ack/nak` + flag | TAS2505 ACK; `+` all configure writes ACKed | `ack+` |
   | `hp` | last raw headphone-detect read (1 in, 0 out, -1 read failed) | 0/1 |
   | `out` | active output: `spk`, `hp` or `none` | as plugged |
   | `i2s` | `run` = >= 700 blocks/s and no new errors/timeouts this second, else `STALL` | `run` |
   | `cfg=a/b` | `i2s_configure()` / START trigger return codes | `0/0` |
   | `blk/s` | blocks written in the last second (48000/64 = 750 expected) | ~750 |
   | `err` | total `i2s_write()` errors (driver ERROR state, e.g. underrun) | 0 |
   | `tmo` | total write/slab timeouts (100 ms each; no LRCK/clock) | stops increasing after boot |
   | `rec` | total stream restarts (drop + re-prime + start) after errors | 0 |
   | `gen_us` | max time to render one 64-frame block this second (us) | far below 1333 |
   | `per_us` | max time between two accepted blocks this second (us) | near 1333; a few ms at most |
   | `drop` | console lines dropped because the host was not reading | 0 while `screen` is open |
   | `rr` | `RESETREAS` captured at boot (hex) | informational |

   Lines are only produced while the port is open (DTR set), so nothing piles up when no
   terminal is attached. A few `tmo` counts during the first second are expected (the stream
   waits for LRCK); the counter should then stop rising.
7. FUNCTION held 1.5 s: sound fades out, all LEDs go off; after release the SP-1 is off.
   The next FUNCTION press should enter the TE bootloader (as with `zephyr_min`). With USB
   connected it may stay dark until FUNCTION is pressed or USB is replugged.

## Pass / fail checklist (owner)

| # | Check | Pass | Fail → what it suggests |
|---|---|---|---|
| 1 | LED 1 blinks 1 Hz and keeps blinking > 10 s | ☐ | No blink / reboot loop: watchdog or early crash. Recover (below). |
| 2 | LED 2 on | ☐ | CS42L42 not answering: reset pin/oscillator/I2C guess wrong |
| 3 | LED 3 on | ☐ | TAS2505 not answering: P0.09 reset (NFC pad) not driven? |
| 4 | LED 4 on within ~3 s and stays on | ☐ | No LRCK (CS42L42 not master) or I2S errors; see `tmo`/`err` |
| 5 | Speaker: quiet 440 Hz, fades in, no headphones plugged | ☐ | Silent with LED 4 on: TAS2505 setup/mute guess wrong |
| 6 | Headphones (off-ear): 440 Hz left, 660 Hz right, quiet | ☐ | Swapped/mono/loud: note exactly what you hear |
| 7 | Plug/unplug switches output within ~0.5 s, smooth | ☐ | Never switches: `hp` field stuck → detect register guess wrong |
| 8 | No loud pop at boot, switch or power-off | ☐ | Note when it happens |
| 9 | `screen /dev/cu.usbmodem* 115200` shows one line/s, `blk/s` ~750, `err=0` | ☐ | No port: USB guess wrong → rebuild with `--no-usb` and compare |
| 10 | Heartbeat and console keep running for 5 minutes | ☐ | Freezes/reboots: report `rr=` and the last line |
| 11 | FUNCTION 1.5 s → everything off; next FUNCTION press → bootloader | ☐ | Not off: recover via T1+T4 |

If the tone is ever uncomfortably loud: unplug the headphones, hold FUNCTION 1.5 s, and
report what you heard.

## Recovery

1. Re-enter the bootloader: SP-1 off (FUNCTION hold, or let it reach `SYSTEM_OFF`), hold
   **T1 + T4** while connecting USB until **T1 lights**. If that fails, use the
   solderless.engineering recovery procedure.
2. Flash a known-good image from `~/spfx/flash-ready/` (verify with
   `shasum -a 256 -c SHA256SUMS` first):
   - `sp1fx-min-heartbeat.bin` (`18afab48…2667`): known to boot (LED 1 blinks), or
   - `RECOVERY-tape-looper-from-repo.bin` (`e73e3786…badbf`), or the stock firmware from
     solderless.engineering.

## Guesses about hardware behaviour (unverified)

- **CS42L42 init** is Tim Knapen's wiki sequence as used by Tape Looper (reported to play
  headphone audio on hardware there). Changed: HP muted (0x2001 = 0x0D) and mixer 0x3F
  written *before* power-up; mixer -30 dB instead of 0 dB; HP left muted until the app
  unmutes it. Not checked against the datasheet here: that 0x2301/0x2303 are 1 dB/step
  attenuation with 0x00 = 0 dB and 0x3F = mute (from memory of the Linux driver), the
  meaning of HP_CTL 0x2001 bits beyond Tim's "0x0D = mute all, 0x01 = unmute", and that
  the tip-sense detect (0x1B77 bit 7) keeps working while the HP output is muted.
- **Headphone level estimate**: -30 dBFS tone and -30 dB mixer ≈ 60 dB below the
  CS42L42's full-scale output (assumed ~1 Vrms class) ≈ 1 mVrms. The real full-scale
  level and the HP_CTL full-scale bit were not verified.
- **TAS2505 init** is Tape Looper's `tas2505_configure()` (TI SLAU472C 5.1, PLL from BCLK).
  Changed: DAC muted (0x40 = 0x0C) before it is powered, driver gain written as mute before
  the driver is powered, +6 dB instead of +18 dB, speaker attenuation 0x0C (assumed -6 dB at
  ~0.5 dB/step) instead of 0x00. "Speaker = left channel" follows from Tape Looper's 0x3F =
  0x90 comment. Tape Looper also powers the TAS2505's own headphone driver; kept as-is.
- **Mute ordering**: off = class-D gain mute then DAC mute (TAS), analog mute then mixer
  mute (CS42L42); on = the reverse. All switching happens with the digital level at
  exactly 0. Whether this is pop-free is unknown.
- **I2S format/alignment**: nRF as clock slave with 16-bit words in a 64-SCK frame (32 SCK
  per channel), Philips I2S, left in the low half-word. The CS42L42 slots are set to 24-bit
  (Tim's values) and the TAS2505 to 16-bit (Tape Looper's), both MSB-first; Tape Looper
  says this works on hardware. Left = LRCK low is assumed for both codecs.
- **Reset and oscillator pins**: CS42L42 reset P0.15 and TAS2505 reset P0.09 driven as
  active-low GPIO outputs; oscillator enable P0.13 active-high. Order (reset low,
  oscillator on, reset high, ≥ 2.5 ms) from Tim's wiki / CS42L42 datasheet 5.1. The
  bootloader's state of these pins at hand-off is unknown; the app forces them.
- **P0.09 (NFC1 pad)**: see "Problems found" below. It is assumed that the device's UICR
  already releases the NFC pads for GPIO (the stock firmware and Tape Looper use P0.09).
- **USB**: device_next CDC ACM, `sample_usbd_init_device()` as in Tape Looper, DTR-gated,
  non-blocking writes through our own ring buffer from the CDC work queue. USB starting
  HFCLK/USBD after the bootloader is assumed harmless because Tape Looper does it.
  VID:PID 2fe3:0001 (Zephyr development VID); bcdDevice left at the Zephyr default.
- **LED pins 2–4** (P0.26, P1.15, P1.14) from Tape Looper's `track_leds[]`, active-high.
  If a bootloader PWM instance is routed to any of them it is stopped (same guarded code
  as `zephyr_min`).
- **Watchdog / clocks / SYSTEM_OFF with USB plugged in**: as in `zephyr_min`.
- **Thread timing**: audio thread priority 0 (preemptive), main priority 7. Render cost
  was not measured; `gen_us`/`per_us` on the console are there to measure it.

## Problems found with earlier assumptions

- **`nfct-pins-as-gpios` in the board dts has no effect.** It sits on the root node; in
  Zephyr v4.3.1 the property is read only from the `uicr` node
  (`modules/hal_nordic/nrfx/CMakeLists.txt`). The build output has no
  `NRF_CONFIG_NFCT_PINS_AS_GPIOS`. This is the *safer* state: if it were set,
  `SystemInit()` would **write UICR.NFCPINS through the NVMC and reset** when the pads are
  still in NFC mode, i.e. a flash/UICR write outside our image. Do **not** "fix" it by
  moving the property to `&uicr` without a deliberate decision. Consequence: P0.09 works as
  GPIO only if the device's UICR already says so (likely, since the TAS2505 ACKs under
  Tape Looper, which has the same board file).
- Tape Looper's `prj.conf` sets `CONFIG_SAMPLE_USBD_VID`, which does not exist in pristine
  Zephyr v4.3.1 (the VID is hard-coded to 0x2fe3 there). Not used here.
- Tape Looper's comments disagree with each other about the clocking ("nRF master" in some
  comments, slave in the code that ships). This image follows the shipped code and the
  wiki: nRF slave, oscillator on, CS42L42 LRCK master.
