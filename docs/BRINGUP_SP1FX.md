# SP-1 FX v1-rc1: integrated firmware bring-up

**Status: built, host-tested, NOT RUN ON HARDWARE.** Nothing in this image has been on an
SP-1 yet. It combines parts that were hardware-verified separately (USB audio + engine v3,
controls, codecs) with parts that have only been host-tested (protocol over USB, settings
journal on real flash, volume buttons, windowed overload guard) or never tested with gear
(sync jack). Test it in the stages below, in order, and stop at the first failure.

| | |
|---|---|
| File | `~/spfx/flash-ready/sp1fx-v1-rc1.bin` |
| SHA-256 | `cf09092a75e575f643c869b8b7a8155f9b28ece42a70d8151b789c67bd20a9a8` (also in `SHA256SUMS`) |
| Size / load address | 144,864 B at 0x20000 (`check_image.py` PASS, two clean builds byte-identical) |
| Source | `firmware/app/zephyr_sp1fx` (`firmware/scripts/build_firmware.sh --app sp1fx`) |
| USB | composite "SP-1 FX", VID 0x2fe3 PID 0x0110: UAC2 speaker (48 kHz, 16-bit stereo) + CDC ACM serial port |
| Recovery | `RECOVERY-tape-looper-from-repo.bin` or `sp1fx-usbaudio-in-v3.bin` (same folder) |

## Safety first

- Check the file: `cd ~/spfx/flash-ready && shasum -a 256 -c SHA256SUMS`.
- Flash with Solderless as before (T1 + T4 while connecting USB, release when T1 lights).
- **Start with headphones off your ears and the Mac's app volume low.** This image is louder
  than the usbaudio images: the fixed output trim is now -6 dB (was -12 dB), and VOL+ goes to
  0 dB master. See the volume table at the end.
- The image writes internal flash, but only the two settings pages 0xFD000-0xFEFFF (see
  "Settings storage"). It never writes the bootloader (0x0-0x1FFFF), the board's `storage`
  page (0xFF000), the Bluetooth module or the eMMC.
- **Flashing may or may not wipe saved settings** (unknown whether the TE bootloader erases
  the whole app slot). If settings survive a reflash, that is fine too: corrupt or foreign
  data is detected (CRC) and replaced by defaults.

## Console (text) and companion (binary): one serial port

The CDC ACM port carries two things. The scheme:

1. When a program opens the port (DTR high, e.g. `screen`), the device prints **text**:
   a banner line `I SP-1 FX sp1fx-v1-rc1 ...`, then every 2 s a short diagnostic block, plus
   one line per event. Event lines are limited to 10/s (bursts of 20); extra lines are
   counted (`ev_drop=`) and dropped.
2. The first **valid protocol frame** from the host (the companion's HELLO) switches the port
   to **binary**: the device sends one `0x00` byte (so the companion's decoder discards any
   half-received text line), then only protocol frames. No text at all from then on.
3. When the host closes the port (DTR low), the next opener gets text again.

So: use either `screen` or the companion at a time (macOS lets only one program open the
port anyway). To see the console:

```sh
ls /dev/cu.usbmodem*
screen /dev/cu.usbmodemXXXX 115200     # quit: Ctrl-A, then K, then y
```

Line types (copy whole lines into your report):

| Prefix | Meaning |
|---|---|
| `I ...` | banner when the port opens: version, reset reason, `store_load=0` (saved settings found) or `-1` (defaults), sync state |
| `D t=...` | every 2 s: preset, engine `cpu%_avg/max`, whole-block `blk%_avg/max` (engine + conversions + anything that pre-empted the audio thread), `ovh_max` (block minus engine, cycles), `spikes>85%` (blocks over the guard limit in the last second), guard window `ovl win=` (last 42.7 ms mean) and `max=`, `shed=`, `vol= dB` |
| `  usb ...` | USB ring fill, under/overruns, input/output peak (x1e-3), `in_clip`, `out_clip`, I2S blocks/s and errors, output (`spk`/`hp`), `quiet=1` when silent for 1.5 s |
| `  threads% ...` | CPU share per thread: `audio`, `main` (controls/protocol/storage), `sync`, `other` (USB stack, workqueue), `idle` = headroom. Interrupt time counts to the thread it interrupted. |
| `  info ...` / `  store ...` | every 10 s: stacks, crash/boot counters, codec status, settings journal counters, sync counters, console drop counters |
| `C ...` | a change: `PRESET n/6 Name`, `slot n effect X`, `bypass`, `VOLUME master=.. = -14 dB, codec input max -20 dBFS`, `mode EDIT slot n`, `output -> hp`, `sync clock ON` |
| `K t fader n ...` | soft-takeover state of a fader changed (`pickup: move UP/DOWN`, `picked up`) |
| `OVERLOAD guard: ...` | the guard switched a slot off (see "Overload guard") |
| `SAVE (...) rc=0 seq= page= slot= took N ms` | a settings record was written |
| `FLASH erase page 0x... took N ms (ducked) i2s_err+.. usb_under+..` | a flash page erase ran |
| `CRASH: ...` | the previous run crashed (printed every 10 s for a minute) |
| `POWER OFF: ...` | FN held 1.5 s |

## LEDs (monochrome, no numbers)

- **Track LEDs (above T1-T4)**, performance mode: brightness = that slot's wet level (dim at 0 %,
  bright at 100 %); **dark when the slot is bypassed or empty**. While a fader needs pickup
  (soft takeover): short bright flash on a dim base = move it up; bright with a short dark gap
  = move it down. Global bypass (PLAY): one LED walks across the four.
- **PLAY-side LEDs** (between PLAY and FUNCTION), normally: output level meter (more LEDs =
  louder; all four bright = near full scale).
  - After FN + FWD/REV: all four flash (0.25 s), then a **position bar** for 1.4 s:
    Dub Station 1 LED, Boom Bap 1, House 2, Lo-Fi Tape 2, Ambient 3, Clean 4.
  - After VOL+/VOL-: a **volume bar** for 1.2 s: each LED is a quarter of the range, fully lit,
    partly lit (dimmer) or dark. At mute only LED 1 blinks dim.
  - Refused change (effect does not fit) or overload guard action: alternating pairs flash
    for 0.7 s.
  - Edit mode: one LED pulses slowly = the slot being edited.

## First boot

With no saved settings the device starts with the defaults: four slots Filter, Chorus,
Delay, Reverb, **all bypassed** (clean pass-through), master 75 % (-15 dB), sync clock off,
120 BPM. So on the first boot the track LEDs are dark; that is expected.

The device preset list (FN + rocker) is the six factory presets in this order: 1 Dub Station,
2 Boom Bap, 3 House, 4 Lo-Fi Tape, 5 Ambient, 6 Clean. **There is one Clean now** (the
usbaudio images had an extra "Clean" in front of the list). Loading a preset **keeps the
current volume** and the sync on/off state; it sets the preset's tempo.

---

## Stage 0: boot, watchdog, power off (2 min)

1. Flash `sp1fx-v1-rc1.bin`. Unplug/replug USB (or press FUNCTION if dark).
2. Expect: the Mac sees an audio output "SP-1 FX" and a serial port. Track LEDs dark,
   PLAY-side LEDs dark (no audio yet).
3. Open the console. Report the `I` line and the first `D`/`usb`/`threads` block. Expect
   `store_load=-1` on the very first boot, `cs42=ok tas=ok` in the `info` line, `i2s blk/s=750`.
4. Wait 30 s: no reset (the `D t=` counter keeps rising, `boots=` in `info` stays the same).
5. Hold FUNCTION 1.5 s: `POWER OFF` line, LEDs off. Press FUNCTION: the bootloader starts the
   app again (as with the earlier images; with USB plugged in it may stay dark until FUNCTION
   is pressed or USB is replugged, a known issue).

**Pass:** boots, no reset for 30 s, powers off and back on.

## Stage 1: audio + FN presets + volume (10 min)

1. On the Mac select "SP-1 FX" as output; play music (Mac app volume low). Speaker first, then
   plug headphones: the console prints `C ... output -> hp`.
2. Expect clean audio; PLAY-side LEDs move with the level. `usb ... under=0 ovr=0`,
   `i2s ... err=0` should stay 0 (a few at stream start are fine).
3. **VOL+ / VOL-**: one press = 2 dB. Each press prints `C ... VOLUME master=... = N dB, codec
   input max M dBFS` and shows the volume bar. Hold VOL+: repeats every 90 ms after 0.4 s;
   from 75 % to the top takes about 1 s. **Go up slowly, headphones off your ears first.**
   Report the setting you find comfortable (the `N dB` value).
4. **FN + FWD**: Dub Station (flash, then 1-LED bar). Keep pressing FN + FWD through Boom
   Bap, House, Lo-Fi Tape, Ambient, Clean, and FN + REV back. Each load prints
   `C ... PRESET n/6 Name: fx fx fx fx mem= cpu_decl= bpm=`. The volume must not change on a
   preset load.
5. Stay on **Ambient for 2 minutes** with music playing, then on each other preset for 30 s.
   Report for each preset one `D` line and its `threads%` line. Expect no `OVERLOAD` line;
   `spikes>85%` may be above 0 (that is the case the old guard reacted to); `ovl max=` should
   stay well below 85 %.
6. Report from the `usb` line: `in_clip` (see "Input clipping" below) and `out_clip`.

**Pass:** no dropouts, no `OVERLOAD`, no reset; volume steps audible; presets cycle with
LED flash + bar; one Clean.

## Stage 2: faders, buttons, edit mode (10 min)

1. Load Dub Station (all four slots active). Move each fader: that slot's track LED and
   wet level follow. On the first touch after a preset load a fader may need pickup: its LED
   blinks (direction cue) until you pass the stored value, and a `K` line shows it.
   **T3/T4 blink:** if you see T3/T4 blink on the first fader touch again, note which fader
   you touched and copy the `K` lines around it (see "T3/T4 blink" below).
2. Tap T1-T4: bypass toggles (`C ... bypass=`), LED goes dark/lit.
3. Hold T2 + FWD/REV: slot 2 effect cycles (held LED pulses, PLAY LEDs flash). Try to put a
   Delay into a second slot: expect the refused pattern and the old effect kept.
4. FN + T3: edit mode slot 3 (`C ... mode EDIT slot 3`, one PLAY-side LED pulses); faders now
   set slot 3's four parameters; FN tap returns to performance.
5. PLAY tap: global bypass (walking LED), tap again to return.
6. While moving all four faders quickly with music playing, report a `D` + `threads%` line
   (CPU with the controls busy).

**Pass:** all gestures as in the controls image, no audio glitch while moving faders.

## Stage 3: companion, saving, reload (15 min)

1. Close `screen`. Start the companion (`cd companion && npm run serve`, open
   http://localhost:8137 in desktop Chrome), connect, pick the SP-1 FX port.
2. Expect it to connect (HELLO, effect table, state, meters). Meters move with the music.
3. Change things from the companion (an effect, wet, a parameter, BPM 100) and on the device
   (fader, VOL, FN + rocker): both sides stay in sync (device changes appear in the companion).
4. **Persistence:** make a distinctive change (e.g. slot 1 = Bitcrusher, wet 70 %, volume
   2 presses down), wait 5 s, then FN hold 1.5 s (power off) and FUNCTION to power on.
   Reconnect: the state must be restored. Also try: change something, wait 5 s, unplug USB
   (hard power loss if the battery is flat; otherwise just replug) and check again.
5. Open `screen` again (companion closed). In the `store` line report `saves= erases=
   errors= write_ms_max= erase_ms_max= deferred=`, and any `SAVE` / `FLASH erase` lines.
   Expect `errors=0`; `write_ms_max` around 30-50 ms; erases only while quiet (see below).
6. Companion "factory reset" (if offered) returns to the first-boot defaults.

**Pass:** companion works against the real device; settings survive power off/on; no audio
glitch at any save.

## Stage 4: sync jack (last; default OFF)

The sync driver is **untested with real gear**. It stays completely off (jack pins untouched,
no 31 kHz timer interrupt) until switched on from the companion, and the on/off state is
saved, so an "on" survives a power cycle.

1. Connect the gear (EP-133 / PO / Volca) to the sync jack. Companion: set BPM, switch the
   sync clock on.
2. Console afterwards (companion closed): `C ... sync clock ON bpm=...`, `store ... sync on=1
   hw=1 ticks= pulses=` rising, `threads% sync=` (expect ~1 %), and the `D` line CPU (the
   bit timer adds ~31,250 interrupts/s, estimated +4-6 % CPU).
3. Report what the gear does (follows tempo? start/stop?). Switch it off again from the
   companion; power off/on: it must stay off.

**If anything misbehaves: switch sync off from the companion; that fully stops the driver.**

---

## Reference

### Volume (VOL+ / VOL-), dB mapping

Master `m` (0..1, saved, protocol `SET_MASTER`) maps to the engine gain `-60 dB + 60 dB x m`
(mute below 0.005). One button press = 1/30 = **2 dB**. After the engine's soft limiter
(|x| <= 1.0) the fixed trim `CONFIG_SP1FX_OUT_TRIM_DB = -6 dB` sets the digital ceiling.

| master | engine gain | max level into the codec | note |
|---|---|---|---|
| 100 % | 0 dB | -6 dBFS | loudest setting |
| 90 % | -6 dB | -12 dBFS | = the old images' absolute ceiling (master 100 %, trim -12) |
| 75 % | -15 dB | -21 dBFS | default (old images: -27 dBFS here) |
| 50 % | -30 dB | -36 dBFS | |
| 0 % | mute | - | |

The analog stages are unchanged (CS42L42 headphone mixer -30 dB, i.e. 30 dB below its
loudest register setting; TAS2505 speaker +6 dB gain, -6 dB attenuation). Hearing safety
reasoning: the loudest setting is 6 dB above the ceiling the usbaudio images could reach and
still 36 dB below the codec's full digital level at its loudest analog setting. **The actual
SPL is unmeasured**; headphones off the ears for the first test.

### Settings storage (ADR-003)

- Two 4 KB pages 0xFD000-0xFEFFF, carved from the top of the app slot in the devicetree
  (`slot0` is 0x20000-0xFCFFF in this image; the linker and `check_image.py --reserve
  0xFD000:0xFF000` refuse a bigger image). Every flash access goes through one function that
  refuses anything outside the two pages; compile-time asserts pin the addresses.
- What is saved: the whole working config (effects, wet, bypass, remembered parameters of
  every effect, order, input gain, master volume), BPM and the sync on/off state.
- When: 1.5 s after the last change, at most every 5 s; on FN-hold power off; on the
  companion's SAVE_NOW.
- **Flash stalls, confirmed from the Zephyr source (not measured):** this build has no radio,
  so Zephyr's nRF flash driver erases a page in one blocking NVMC operation; the nRF52840
  halts the CPU for that time (datasheet: page erase up to ~85 ms) and every interrupt (USB,
  I2S) waits. A word write halts it up to 41 us.
  - Writes: a 512 B record is written 8 bytes at a time with a 0.4 ms sleep in between
    (stalls of at most ~82 us; about 35 ms per record in total). Safe while playing.
  - Erases: needed once per 8 saves. The firmware erases the spare page **ahead of time**
    whenever the audio has been quiet (input and output below -66 dBFS) for 1.5 s, so normal
    saves never erase. A save that would need an erase while music plays is **deferred**
    until a quiet moment (or power off). Every erase runs under a duck (output faded to
    silence first). Trade-off: if music plays without any pause for a long time, the last
    changes can wait in RAM; they are saved at the next pause or at FN-hold power off, but
    lost if the battery dies first.

### Overload guard (changed)

Old (v3): any 8 consecutive 64-frame blocks over 85 % of the block time shed the most
expensive slot. On hardware Ambient averaged ~49 % but had occasional ~89 % blocks, and the
reverb was shed 14 times.

New: the block cost is averaged over 32-block windows (42.7 ms). A slot is shed only after
**3 windows in a row whose mean is over 85 %** (~128 ms of sustained overload). Safety net:
a single window whose mean is over 85 % x 9/8 (~96 %; the audio thread cannot keep up)
acts at once. Blocks that initialise an effect never count. The starvation guard (audio
thread sleeps 1 ms after 32 blocks with no idle time) is unchanged. Host test: the measured
Ambient pattern (49 % mean, 89 % spikes, even 8 in a row) never acts over 20,000 blocks.

Ambient itself was not changed: the reverb's cost does not depend on its parameters (fixed
4-line network at 12 kHz), and the spikes are not reverb work. The new `D` line separates
them: `cpu%` (engine only) vs `blk%` (whole block) and `ovh_max` (block minus engine) show
whether a spike is DSP or pre-emption; `threads%` shows the USB stack share.

### Input clipping (`in_clip`)

By design, no change: `in_clip` counts input samples at or above -0.01 dBFS **as they arrive
from the host**, before the input gain. If the Mac sends full-scale (clipped) audio, the
clipping happened on the Mac and cannot be undone here. Inside the device everything is
float with headroom, and the output has a soft limiter (`out_clip` = samples that needed it).
Fix on the host side (lower the app volume) or lower the device input gain from the
companion (-12..+12 dB). The saturation/compressor effects also react to a hot input.

### T3/T4 blink on first fader touch

Not found in the code. Checked: the soft-takeover cue (needs `need != 0`, which the controls
image's console did not show), the PWM sequence update (cannot blink), the LED renderer
(bypassed/empty slots are dark), the bootloader PWM release (PWM2/PWM3 are stopped if routed
to an LED pin). Remaining candidates, unproven: ADC crosstalk on the fader 3/4 channels when
another fader is touched (would show as `K` lines or wet changes on faders 3/4), or a
bootloader-started PWM still driving those pins before our init. This image prints every
takeover state change as a `K` line; please report them if it happens.

### Known risks (not verifiable without hardware)

- CPU: v3 presets measured ~50 % on hardware; the controls loop, protocol, thread statistics
  and the LED meter add an estimated 2-4 % (main thread at lower priority, plus config copies
  while faders move). Sync on adds an estimated 4-6 % (bit-timer interrupts). Unmeasured.
- The CDC port now also receives (protocol); RX was never exercised on hardware.
- Flash write/erase timing and the bootloader's behaviour towards 0xFD000-0xFEFFF.
- The 0x00 separator relies on the companion's COBS decoder dropping the first bad frame
  (host-tested with the C parser; the JS decoder works the same way).
- Sync jack: timing jitter ~1.3 ms (clock counted in rendered audio blocks, polled every 1 ms).
