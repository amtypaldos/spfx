# Controls bring-up image (`sp1fx-controls.bin`)

Status labels follow AGENTS.md. **This image has been built but has never run on an SP-1.**
Every observation below is **EXPECTED** behaviour, derived from the source code and the host tests.
None of it has been observed on hardware. Tick the boxes only after you have seen the behaviour yourself.

## What the image does

`firmware/app/zephyr_controls` runs the real SP-1 FX control logic on the real buttons, faders and LEDs. It has **no audio**: I2S, I2C and the codecs stay off, and `engine_process()` is never called.

Every 8 ms it:

1. powers the ladder and fader rail BTN_COM (P1.10). The rail stays on for as long as the image runs.
2. scans the SAADC: ladder 1 (AIN0), ladder 2 (AIN1) and faders 1-4 (AIN3/AIN6/AIN2/AIN7). Each channel is converted twice and averaged.
3. reads FUNCTION on P0.27.
4. decodes the ladders into the button mask (`firmware/drivers/ladder.c`) and normalises the faders to 0..1.
5. calls `controller_tick()` (`firmware/controls/controller.c`, the same code the host tests exercise). The controller owns a real `engine_t` with a 150 KB pool. The pool is never written because no audio runs.
6. renders `controller_leds()` to the 8 LEDs with hardware PWM (PWM0 drives the track row, PWM1 the play row, 1 kHz, brightness 0..255 squared).
7. feeds the watchdog.

Configuration at boot is the factory default: slot 1 Filter, slot 2 Chorus, slot 3 Delay, slot 4 Reverb. **All four slots are bypassed**, wet is 50 % and master is 75 %. The 6 factory presets are installed for FN + rocker. FN held for 1.5 s powers off (SYSTEM_OFF) through the controller's own `GE_POWER_OFF`. The app has no separate FN timer.

The play-row level meter shows the output peak. With no audio that peak is always 0, so **in performance mode the four play LEDs are dark** unless one of the patterns below is showing.

## Build (never flashes)

```sh
firmware/scripts/build_controls.sh            # clean build + check_image.py (must PASS)
firmware/scripts/build_controls.sh repro      # two clean builds, compares SHA-256
firmware/scripts/build_controls.sh release    # repro + copy to ../flash-ready/sp1fx-controls.bin + SHA256SUMS.controls
firmware/scripts/build_controls.sh --no-usb build   # variant without the USB console (Kconfig SP1FX_USB_CONSOLE=n)
```

The script uses the toolchain from `firmware/scripts/build_firmware.sh setup`.

Build of 2026-11-24 (with USB):

- 81,908 B flash (8.97 % of slot0), 174,288 B static RAM (66.5 %; the 150 KB engine pool accounts for most of it)
- `check_image.py` result: PASS
- SHA-256 `b67410dbce966b0fb0b32302d5f31a7a2d4f3dcc1682820af467c11b612d510d`
- two clean builds were bit-identical

## Flashing (manual, by the owner)

1. Check the hash: `cd flash-ready && shasum -a 256 -c SHA256SUMS.controls`.
2. Hold **T1 + T4** while you connect USB. Keep holding until **T1 lights**: the TE bootloader is now in update mode.
3. Open https://solderless.engineering and flash `sp1fx-controls.bin`.
4. Recovery uses the same procedure with one of these images:
   - `flash-ready/sp1fx-min-heartbeat.bin` is known to boot (LED 1 blinks).
   - `RECOVERY-tape-looper-from-repo.bin` is Tape Looper's release.

## Console (diagnostics without a debugger)

Open the console with `screen /dev/cu.usbmodem* 115200` (quit with Ctrl-A then K). The device enumerates as "SP-1 FX controls test" (VID 0x2fe3, PID 0x0002).

Output never blocks the 8 ms loop. If the port is not open or the host stops reading, lines are dropped (counter `dr=`). Nothing is queued until `screen` opens the port.

| Line | Meaning |
|---|---|
| `I ...` (every 10 s) | `resetreas`, `hw`/`adc` (0 = ADC OK), `pwm_released` (bit 2/3 = PWM2/PWM3 were routed to an LED pin and were stopped), `rejected` (default slots that did not fit, expected 0), slot effect names |
| `S <ms> L1= L2= F= f= btn= ...` (10 Hz) | **raw** 12-bit ladder 1, ladder 2 and the 4 raw fader values; `f=` faders in %; `btn=0x...` and names of the decoded mask; `PERF`/`EDITn`; `wet=` (or `par=` in edit) in %; `need=` soft-takeover direction per fader (1 = move up, -1 = move down, 0 = picked up); `byp=` per slot; `gb=` global bypass; `m=` master %; `pre=` last preset (0 = none); `lt=` worst loop time in µs; `dr=` dropped lines |
| `G <ms> ...` | each gesture as it happens: `T2 down`/`up`, `TAP T2 -> toggle bypass`, `T1 + FWD -> cycle effect`, `FN + T3 -> EDIT`, `FN tap`, `PLAY tap`, `VOL+`, `FN + FWD -> preset +1`, `FN held 1.5 s -> POWER OFF` |
| `C <ms> ...` | what the controller actually changed: bypass, effect assigned, **REJECTED** (admission error), global bypass, master, preset loaded |

If USB itself causes trouble (for example the watchdog fires or the LEDs freeze when the cable is plugged in), flash the `--no-usb` variant instead. It builds and passes `check_image.py`, but it was **not** copied to flash-ready; build it with `build_controls.sh --no-usb release`.

## Reading raw values and recalibrating

The decode thresholds come from Tape Looper (chattock/sp1-tape-looper @0359372e, `decode_tracks`/`decode_vol`). They are raw codes under the board's ADC setup: gain 1/6, 0.6 V internal reference, 20 µs acquisition, 12-bit. **They have never run in this firmware.**

1. Open the console. Press and hold one button at a time and note `L1=` or `L2=`. Expected values per Tape Looper:

   | Button | Ladder | Expected raw value |
   |---|---|---|
   | idle | L1 | < 110 |
   | T1 | L1 | ~213 |
   | T2 | L1 | ~403 |
   | T3 | L1 | ~733 |
   | T4 | L1 | ~1220 |
   | PLAY | L1 | ~1807-1823 |
   | idle | L2 | < 200 |
   | REV | L2 | ~404 |
   | VOL- | L2 | ~729 |
   | FWD | L2 | ~1220 |
   | VOL+ | L2 | ~1820 |
   | VOL+ and VOL- together | L2 | ~1998 |

2. If a reading sits within about 30 counts of a threshold, or `btn=` names the wrong button, edit the `LADDER1_*_MAX` / `LADDER2_*_MAX` macros in `firmware/drivers/ladder.h`. Put each threshold midway between neighbouring measured levels. Then run `make test`: `tests/test_ladder.c` checks Tape Looper's nominals, so update it if you deliberately move them. Rebuild with `build_controls.sh release`.
3. If FWD and REV turn out to be swapped, swap the masks of the `LADDER2_REV_MAX` and `LADDER2_FWD_MAX` bands in `ladder_cfg_ladder2()`. The FWD/REV direction is inferred from Tape Looper's tempo up/down and is an **untested assumption**.
4. To calibrate the faders, note the raw `F=` value at each end stop and set `FADER_RAW_LO_DEFAULT` and `FADER_RAW_HI_DEFAULT`. The Tape Looper values are 0 and 3700. A 1 % snap at each end makes 0 and 1 reachable.
5. Hysteresis (`LADDER_HYST_DEFAULT` 20 counts) and stability (`LADDER_STABLE_DEFAULT` 2 reads) are also set in `ladder.h`.

**What the hardware can and cannot tell apart** (source finding, see `ladder.h`):

- FN is on its own GPIO, so FN + anything works.
- A channel + rocker or a channel + volume button are on different ladders, so both are read.
- Two track buttons at once cannot be decoded: they read as some other single button.
- PLAY + a track reads as PLAY by default. `LADDER1_DECODE_PLAY_CHORDS 1` splits these chords, but a unit that reads 2 % high would then see a bare PLAY as PLAY+T1.
- A rocker and a volume button at once cannot be decoded.

## Pass/fail checklist (owner)

All entries are EXPECTED results. Write P or F, and note what you actually saw.

**Boot**

- [ ] After flashing, the console shows `I SP-1 FX controls bring-up ... adc=0 ... rejected=0` and slots Filter/Chorus/Delay/Reverb.
- [ ] (Observed on hardware: the slots boot ENABLED with wet 0% (`byp=0000`), so the track LEDs are dim, not dark. This item was wrong.) There is no audio, so the play-side LEDs are dark.
- [ ] `lt=` stays well below 8000 µs. Expect roughly 1000 µs or less.
- [ ] The device keeps running for more than 1 minute without a reset. A watchdog reset would show as a new `I` line with a fresh low timestamp.

**Buttons (console `btn=` while held, and a `G` line)**

- [ ] T1, T2, T3 and T4 each show `T1`…`T4`, with `L1` near the expected value.
- [ ] PLAY shows `PLAY`.
- [ ] FUNCTION shows `FN`. L1 and L2 do not change.
- [ ] VOL+ and VOL- show `VOL+` and `VOL-`.
- [ ] FWD and REV on the rocker show `FWD` and `REV`. If they are swapped, see step 3 of the calibration section.
- [ ] Nothing pressed shows `btn=0x000 -`. There are no phantom presses while you move the faders.

**Faders (each of 1-4, end to end)**

- [ ] `F=` runs from about 0 to about 3700 or more, and `f=` from 0 to 100. Note the raw end values.
- [ ] Tap T*n* (shorter than 0.6 s): `C slot n bypass=0`. Track LED *n* lights.
- [ ] **Soft takeover direction cue:** until the fader passes the stored wet value of 50 %, the LED blinks:
  - fader below 50 % (`need=1`, move up): a short **bright flash** every 0.5 s on a dim base
  - fader above 50 % (`need=-1`, move down): **bright with a short dark gap** every 0.5 s
- [ ] Moving the fader through 50 % picks it up (`need=0`). From then on the LED brightness follows the fader smoothly, dim at the bottom (never fully off) and bright at the top, and `wet=` follows `f=`.
- [ ] Tap T*n* again: the slot is bypassed and the LED goes dark.

**Effect cycle (channel + rocker)**

- [ ] Hold T1 for more than 0.25 s: track LED 1 pulses (about 0.4 s triangle) and the play row is dark.
- [ ] While still holding T1, press FWD. If the new effect fits, all 4 play LEDs flash for 0.25 s and the console shows `C slot 1 effect -> <name>`.
- [ ] If the new effect does not fit (expected for some effects, such as a second Delay or Reverb, because the pool is 150 KB), the play LEDs blink in **alternating pairs** (80 ms) for 0.7 s, the console shows `REJECTED ... status=2`, and the old effect stays.
- [ ] Holding the rocker repeats after 0.45 s, then every 0.2 s. REV steps backwards.
- [ ] Release T1: the slot's bypass does **not** toggle, because the press was consumed.
- [ ] If the slot was set to "None", the play row shows a steady dim row while the channel is held.

**Edit mode (FN + channel)**

- [ ] Hold FN and tap T2: `G FN + T2 -> EDIT slot 2` and the status line shows `EDIT2`.
- [ ] **The play LED beside PLAY for slot 2 (play LED 2) pulses slowly** (1.2 s triangle).
- [ ] The track LEDs show the effect parameters (`par=`), with the same pickup blink. Faders beyond the effect's parameter count are dark.
- [ ] A fader moves its parameter after pickup.
- [ ] FN tap (shorter than 0.6 s, alone) returns to `PERF`.

**Presets (FN + rocker)**

- [ ] Hold FN and press FWD: `C preset 1/6 'Dub Station' loaded`. All 4 play LEDs flash for 0.25 s, then a **bar** shows until 1.4 s.
- [ ] Bar length by preset:

  | Preset | Lit LEDs |
  |---|---|
  | 1 | 1 |
  | 2 | 1 |
  | 3 | 2 |
  | 4 | 2 |
  | 5 | 3 |
  | 6 | 4 |

- [ ] The first REV press after boot goes to preset 6. Holding the rocker repeats.
- [ ] Releasing FN after this does **not** count as an FN tap, and does not power off.
- [ ] The track LEDs show the preset's wet values for every slot that is not bypassed.

**Global bypass**

- [ ] Tap PLAY: `C global bypass=1`. **One dim LED walks** across the 4 track LEDs, one step every 0.25 s.
- [ ] Tap PLAY again: back to normal.

**Volume**

- [ ] VOL+ and VOL- change `m=` in steps of 6.25 % (1/16) and print `C master=`. There is no LED feedback for volume.
- [ ] Holding a volume button repeats after 0.4 s, then every 90 ms.

**Power off**

- [ ] Hold FN **alone** for 1.5 s: `G FN held 1.5 s -> POWER OFF`. All LEDs go dark.
- [ ] After you release FN, USB disconnects (SYSTEM_OFF).
- [ ] FN held together with a channel, PLAY or the rocker does **not** power off.
- [ ] The next FN press wakes the chip through the TE bootloader, as with `zephyr_min`.

## Guesses and open points (untested assumptions)

- **Rail settle time:** 5 ms after BTN_COM goes high. Tape Looper never waits explicitly. The rail stays on while the image runs and is driven low before SYSTEM_OFF, which Tape Looper does not do. This keeps the fader pots from drawing current while the chip is off.
- **ADC acquisition:** the board's 20 µs (Tape Looper). There is no SAADC oversampling. Each value is the mean of 2 scans.
- **LED PWM:** every nRF52840 GPIO can be routed to any PWM channel. The chip has **four** PWM instances with 4 channels each (PWM0-3), not three.
  - The TE bootloader starts PWM2 and PWM3. This image uses PWM0 for the track row and PWM1 for the play row.
  - PWM2 or PWM3 is stopped and disconnected only if it is found routed to an LED pin. The console reports this as `pwm_released`.
  - Hardware PWM was chosen over software PWM because Tape Looper moved from a TIMER ISR soft-PWM to hardware PWM (LEDPWM-710): ISR jitter flickered at low duty, while hardware PWM costs no CPU.
  - Full brightness (255) is 25 % duty (`LED_FULL_TICKS`, `controls_hw.c`). Tape Looper's default runs the LEDs at about 5 %.
- **LED polarity** (`0x8000`, active-high) and the pin map come from Tape Looper.
- If the bootloader leaves anything else on the LED pins, LEDs may stay lit or stay dark. Compare against `pwm_released` in the `I` line.
