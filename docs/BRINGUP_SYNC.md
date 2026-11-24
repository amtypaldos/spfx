# Bring-up image: SYNC jack (MIDI clock + Pocket Operator sync)

Status labels follow AGENTS.md.
**The image has been built and checked, but it has never run on an SP-1 (untested). Nothing on this
page has been observed on hardware.** The transmitter core is tested in simulation
(`tests/test_midi_out.c`). Tape Looper, where the pin map comes from, also calls its MIDI/PO
output "untested on real gear". Do not assume the jack works until you have tried the checklist
below.

- Image: `~/spfx/flash-ready/sp1fx-sync.bin` (SHA-256 in `SHA256SUMS.sync`)
- Source: `firmware/app/zephyr_sync/`, `firmware/drivers/midi_out.{h,c}` (pure C, host-tested),
  `firmware/drivers/sync_hw.{h,c}` (Zephyr/nRF), `firmware/app/sync_out.{h,c}` (the scheduler)
- Build: `firmware/scripts/build_sync.sh` (build only; it has no flash step)

## What it should do (expected; not observed)

| Thing | Expected behaviour |
|---|---|
| Power/watchdog | As `zephyr_min`: watchdog fed every ~1 ms. Hold FUNCTION 1.5 s → LEDs off; release → off (SYSTEM_OFF). The next FUNCTION press wakes into the TE bootloader. |
| Tempo | Fixed **120 BPM** (`CONFIG_SP1FX_SYNC_BPM`). The rocker is not read: it sits on the SAADC button ladder, which this image does not use. |
| MIDI (ring, P0.23 via PNP) | `0xFA` Start, then `0xF8` clock at 24 PPQN = 48 bytes/s at 120 BPM. 31250 baud 8N1, 320 µs per byte. `0xFC` Stop when MIDI turns off. |
| PO sync (tip, P0.20 + P0.17) | One 5 ms pulse (156 × 32 µs = 4.992 ms) per 1/8 note (2 PPQN) = 4 pulses/s at 120 BPM. Idle low, pulse high (default). |
| FUNCTION tap | Cycles modes: **1** MIDI + PO → **2** MIDI only → **3** PO only → **4** stopped → 1 … Boots in mode 1. |
| LEDs (next to the faders) | For ~1 s after a change: LEDs 1..N lit steady for mode N (mode 2 = LEDs 1+2). Otherwise LED 1 flashes (60 ms) on every beat, i.e. twice a second at 120 BPM. In mode 4, LED 1 gives a short blip every 2 s (alive). |
| USB console | `screen /dev/cu.usbmodem* 115200`: one status line per second (see below). |

Mode transitions: 1→2 the PO pulses stop; 2→3 `0xFC` Stop is sent, then MIDI goes quiet;
3→4 PO stops; 4→1 the clock restarts: `0xFA` Start, then clocks from beat 1.

The final firmware saves the sync on/off state (docs/PROTOCOL.md `SET_SYNC`) and restores it at
power-up. This image saves nothing: it always boots into mode 1.

### Time source

There is no audio clock in this image. `sync_out` (the scheduler the final firmware uses) is
advanced every ~1 ms by a 48 kHz frame count derived from the kernel uptime
(`k_uptime_ticks()`, RTC/LFCLK based). In the final firmware the argument is the real audio frame
counter, so the clock is locked to the audio. Consequences here:

- Each clock byte can be up to ~1 ms late (polling), plus up to 32 µs to the next bit slot. A
  follower should average this out; a scope will show it as jitter.
- The tempo accuracy follows the LFCLK source (`CONFIG_CLOCK_CONTROL_NRF_K32SRC_SYNTH`, derived
  from HFCLK).
- Bit timing comes from NRF_TIMER2 on HFCLK. With the crystal (HFXO) it is exact; on the internal
  RC oscillator it is only about 1 % accurate (nRF52840 spec), which a MIDI receiver normally
  tolerates. The console prints `hfxo=1` when the crystal is running (USB should turn it on).

## Status line (USB CDC ACM)

Format (the values are illustrative, not observed output):

```
t=12 mode=1(midi+po) bpm=120 midi_bytes=577 f8=576 ticks=577 pulses=49/49 drop=0/0 isr/s=31250 late=0 maxlat_us=1 hfxo=1 inv=1 swap=0 con_drop=0 rr=4
```

| Field | Meaning | Expected |
|---|---|---|
| `mode` | 1..4 | changes on FUNCTION tap |
| `midi_bytes` / `f8` | bytes fully sent on the wire / clock bytes among them | grows by 48/s in modes 1–2 |
| `ticks` | 24 PPQN ticks scheduled since boot (counted in every mode except 4) | grows by 48/s |
| `pulses=a/b` | PO pulses sent / scheduled | `a` grows 4/s in modes 1 and 3 |
| `drop=a/b` | MIDI bytes / PO pulses dropped because a queue was full | 0/0 |
| `isr/s` | timer interrupts in the last second | ≈ 31250 (exactly at 1 MHz/32; less if interrupts are missed) |
| `late` | ISRs that started ≥ 16 µs (half a bit) after their compare (risk of a bad bit) | 0, or rare |
| `maxlat_us` | worst ISR latency in the last second | a few µs |
| `hfxo` | 1 = HFCLK from the crystal | 1 with USB on |
| `inv`, `swap` | the compiled MIDI-invert and tip/ring-swap settings | 1, 0 by default |
| `con_drop` | console lines dropped (host not reading fast enough) | 0 |
| `rr` | RESETREAS at boot (hex) | depends on how it was started |

The console never blocks: lines are dropped if the host is not reading. If USB disturbs anything
(watchdog resets, LED stutter, `late` climbing), build without it: `build_sync.sh --no-usb`
(Kconfig `CONFIG_SP1FX_USB_CONSOLE=n` via `firmware/app/zephyr_sync/no_usb.conf`). That variant
does not go into flash-ready; build it yourself.

## The jack: what is NOT known

**Unverified:** what the SP-1's 3.5 mm SYNC jack carries on which contact, which TRS-MIDI type
(TRS-A or TRS-B) it matches, and the polarity of each signal. Everything here comes from Tape
Looper's comments (TimK's schematic, which we have not seen):

- P0.23 → BC807 PNP base → SYNC_RING. The PNP inverts: P0.23 low = ring high.
- P0.20 (PO_A) and P0.17 (PO_B) → SYNC_TIP, paralleled. Both are always driven to the same level
  (driving them to different levels would short two outputs against each other).
- All three are plain push-pull GPIO outputs (S0S1), as Tape Looper sets them. Whether the tip
  pins drive the jack directly or through a resistor is not known.

MIDI over TRS uses two signal contacts: the source (DIN pin 4, current from the sender's supply)
and the sink (DIN pin 5, the sender pulls it low for a 0 bit). TRS-A (the MMA standard, used by
Korg, Arturia, TE and others): **tip = sink (pin 5), ring = source (pin 4)**. TRS-B (some Novation,
Line 6 and others) swaps them. The SP-1 puts MIDI on the ring and PO sync on the tip, which is
neither exactly, so expect to experiment. A guess, not a finding: a TRS-A receiver connected
directly sees current flowing ring → tip whenever ring is above tip, and the PO pin on the tip
sits low at idle. That would make "ring high" read as a 0 bit, which is the OPPOSITE of what
`MIDI_INVERT=1` assumes. So the first thing to try if MIDI fails is to flip the inversion. A PO
pulse on the tip also disturbs a MIDI receiver wired across tip and ring, which is why mode 2
(MIDI only) exists: test MIDI there.

### Compile-time switches (in `firmware/app/zephyr_sync/prj.conf`)

| Kconfig | Default | Effect |
|---|---|---|
| `CONFIG_SP1FX_SYNC_MIDI_INVERT` | `y` | Invert the MIDI bit stream at the GPIO to compensate for the PNP (Tape Looper default). Set to `n` if a receiver sees nothing or garbage. |
| `CONFIG_SP1FX_SYNC_SWAP_TIP_RING` | `n` | `y` sends the MIDI bit stream on P0.20+P0.17 (tip) and the PO pulse on P0.23 (ring). The pins are driven exactly as before (GPIO levels); only which signal goes to which pin changes. |
| `CONFIG_SP1FX_SYNC_PO_ACTIVE_LOW` | `n` | `y` makes PO pulses active-low at the pin. Useful together with SWAP (the ring path is inverted by the PNP). |
| `CONFIG_SP1FX_SYNC_PO_PULSE_MS` | `5` | Pulse width, 1..20 ms. |
| `CONFIG_SP1FX_SYNC_BPM` | `120` | Fixed tempo, 30..300. |
| `CONFIG_SP1FX_USB_CONSOLE` | `y` | Status console (`--no-usb` turns it off). |

Edit `prj.conf`, then rebuild with `firmware/scripts/build_sync.sh` (the image is in
`build/firmware/zephyr_sync/zephyr/zephyr.bin`). Each variant gets a different SHA-256. Only the
default image is copied to flash-ready (`build_sync.sh release`).

## Testing with real gear

Nothing should be connected while flashing. Keep the volume of anything you connect low: the sync
jack is a logic output, and the jack of a connected synth input may make noise.

**MIDI clock (mode 2):** a device that follows MIDI clock and has a TRS MIDI IN or a DIN MIDI IN:
a TE EP-133 K.O. II / EP-40 Riddim (TRS-A MIDI in on the 3.5 mm jack; set its sync/clock source to
MIDI per its manual), a Korg Volca with MIDI in, or a computer with a USB-MIDI interface and a
MIDI monitor (e.g. MIDI Monitor on macOS) through a TRS-A → DIN adapter. Use a TRS-to-TRS cable
(not mono TS), or the adapter that matches the receiver.

**PO sync (mode 3):** a Pocket Operator (SY1 / SY4 / SY5 sync modes, which listen on the left
channel = tip) or a Korg Volca SYNC IN (mono 3.5 mm, tip = pulse), with a stereo cable for the PO
and a mono or stereo cable for the Volca.

### Pass/fail checklist (fill in; every row is currently unobserved)

| # | Step | Pass if |
|---|---|---|
| 1 | Flash, power on | LED 1 lit steady ~1 s (mode 1), then LED 1 blinks twice per second. No reboot loop (the blink does not restart every ~5 s). |
| 2 | `screen /dev/cu.usbmodem* 115200` | One line per second; `isr/s` ≈ 31250, `late` = 0 or barely growing, `drop=0/0`, `f8` +48/s, `pulses` +4/s. |
| 3 | Tap FUNCTION 4 times | LEDs show 1, 1-2, 1-3, 1-4 for ~1 s each; `mode=` follows; in mode 4 counters stop and LED 1 blips every 2 s. |
| 4 | Hold FUNCTION 1.5 s | LEDs off, device off; next press enters the bootloader (T1 behaviour as with the other images). |
| 5 | Mode 2, MIDI monitor | `Start` once, then a steady clock; the monitor/receiver shows ~120 BPM. Tap to mode 3: `Stop` arrives. |
| 6 | Mode 2, EP-133 / Volca following MIDI clock | Press play on the follower: it runs at 120 BPM and stays in time for minutes. |
| 7 | Mode 3, Pocket Operator in SY mode | The PO plays at 120 BPM when the SP-1 is in mode 3 (or 1), and stops in mode 4. |
| 8 | Mode 1 | Both at once, if your cable/receiver allows it (see the tip/ring note). |

If 5–6 fail: rebuild with `CONFIG_SP1FX_SYNC_MIDI_INVERT=n`, then try `SWAP_TIP_RING=y`
(with either inversion), then try another cable type. Write down which combination works:
that is the hardware finding the final firmware needs.
If 7 fails: try `CONFIG_SP1FX_SYNC_PO_ACTIVE_LOW=y`, or a wider pulse (10 ms).

### Verifying timing with a scope or logic analyser

Probe the jack with a TRS breakout (ground = sleeve), or the pads for P0.23/P0.20 if you have
them; do not short anything.

- MIDI (ring, decode as UART 31250 8N1; try both polarities): bits are 32.0 µs wide; a byte is
  320 µs; the idle level and the start-bit direction tell you the polarity. At 120 BPM, clock
  bytes start every 20.83 ms on average (expect up to ~1 ms of jitter from the 1 ms polling in this
  image). Most logic analysers (Saleae, sigrok/PulseView) have a UART/MIDI decoder: the bytes should
  read `FA F8 F8 …`, then `FC` when you tap to mode 3.
- PO (tip): 4.99 ms pulses, 250 ms apart at 120 BPM (2 PPQN), rising edge on every 12th clock
  byte, starting together with the first clock after Start.
- Check rise/fall shape through the PNP stage on the ring (slow edges point to a weak pull-up in
  the stage).

## Flashing (manual, by the owner)

The build never flashes. To flash:

1. Disconnect everything from the sync jack.
2. Hold **T1 + T4** while connecting USB, until T1 lights (TE bootloader / DFU mode).
3. Open https://solderless.engineering and flash `~/spfx/flash-ready/sp1fx-sync.bin`.
   Check its SHA-256 against `~/spfx/flash-ready/SHA256SUMS.sync` first
   (`shasum -a 256 sp1fx-sync.bin`).

### Recovery

- `~/spfx/flash-ready/sp1fx-min-heartbeat.bin` is known to boot (LED blinks, verified
  by the owner on hardware).
- `~/spfx/flash-ready/RECOVERY-tape-looper-from-repo.bin` is Tape Looper's release image.
- If the image misbehaves: hold FUNCTION 1.5 s (if the app runs), or let the battery or watchdog
  reset it, then use T1 + T4 while connecting USB to enter the bootloader and flash one of the
  images above. The bootloader (below 0x20000) is never written by this image:
  `check_image.py` confirms the image lies within 0x20000..0xFEFFF.

## Build and checks (done; build-only)

```sh
firmware/scripts/build_sync.sh            # clean build + check_image.py (must PASS)
firmware/scripts/build_sync.sh --no-usb   # variant without USB console
firmware/scripts/build_sync.sh repro      # two clean builds in different dirs, compare SHA-256
firmware/scripts/build_sync.sh release    # repro + copy to ../flash-ready/sp1fx-sync.bin + SHA256SUMS.sync
make test                                 # host tests incl. tests/test_midi_out.c
```

Results of the 2026-11-24 build (default config, USB console on): FLASH 48,632 B (5.32 % of
slot0), RAM 15,328 B (5.85 %), first load address 0x00020000, last 0x0002bdf8, check_image.py
**PASS**, SHA-256 `12255e2cd61bcd9c7716b0bf97f290758b3b1a658e9b1aabdccf476f73c3073d`, reproducible
(two clean builds in different directories, identical .bin). `--no-usb` variant: 24,108 B flash,
5,512 B RAM, PASS, `56d43490490fdb2445c42cb02283249dcb8be1d191570ddebbc7d95b8f856fad`.

## Design notes

- **Timer:** NRF_TIMER2, direct registers, IRQ priority 1, continuous 32 µs compare/clear.
  This is the instance Tape Looper uses for this job. TIMER0 is avoided (owned by the BLE
  controller / MPSL when one is present, as the final firmware might have). Zephyr's system clock
  on nRF is RTC1, `timer2` is disabled in the devicetree and no counter driver is built, so
  nothing else touches TIMER2. The bootloader is not documented to leave a TIMER running; init stops
  and reprograms TIMER2 anyway. Cost: 31,250 small ISRs/s (estimated, not measured, a few % CPU).
  The ISR writes the pin levels, so edges lag the compare by the interrupt latency (measured on
  the device and reported as `maxlat_us` / `late`).
- **Core:** `midi_out_tick()` is called once per bit. Real-time bytes (≥ 0xF8) have their own FIFO
  that is served before other message bytes, never interrupting a byte already on the wire. A full
  queue drops the new byte (counted). PO pulses have a pending limit of 4 and a minimum idle gap
  equal to the pulse width. At 300 BPM (120 clocks/s) the line is busy 3.84 % of the time and never
  has more than one byte pending (tested).
- **Pins at power-off:** before SYSTEM_OFF the three jack pins go back to their reset state
  (input, disconnected), so the PNP is not held on while the device is off. (Guess: this is the
  state they are in before the app configures them.)
- **Idle levels while running:** ring pin P0.23 low (= ring high through the PNP = MIDI idle, per
  Tape Looper), tip pins low (PO idle). Note that with inversion on, the PNP conducts the whole
  time the clock is idle; this is what Tape Looper does too.
