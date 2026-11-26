# Effect CPU profiling image (`sp1fx-fxprof.bin`)

Status: **built, never run on hardware** (no result recorded as of 2026-11-25). Everything below about behaviour is *expected*.
The declared effect `cpu_cycles` therefore stay unmeasured estimates. Not published as a release; rebuild it from source.

| File in `flash-ready/` | App dir | USB product / PID | SHA-256 |
|---|---|---|---|
| `sp1fx-fxprof.bin` | `firmware/app/zephyr_fxprof` | `SP-1 FX fxprof`, 0x2fe3:0x0103 | in `SHA256SUMS` |

Build (never flashes): `firmware/scripts/build_firmware.sh --app fxprof` (reproducible:
`... --app fxprof repro`).

## What it does

- **No audio input and no audio output.** No I2S, no USB audio, the codec oscillator is off
  and both codecs are held in reset: the speaker and headphones stay silent by construction.
- A bench thread runs **37 cases** through the real engine (`firmware/audio/fxprof_core.c`)
  on a looping synthetic signal (white noise -20 dBFS RMS + 220 Hz + 3.3 kHz sines, peaks
  < 0.6, different on L and R):
  - case 0: **Clean**, the engine with every slot empty (its fixed per-block cost);
  - cases 1-30: each of the 10 effects **alone in slot 0** (wet 100 %, enabled) in three
    variants: `def` (default params), `worst` (params that do the most work per sample, see
    the table in `fxprof_core.c`), `auto` (default params, one parameter changed every block,
    like a moving fader: exercises `set_param` + the per-block coefficient update);
  - cases 31-36: the **6 factory presets** exactly as the device loads them.
- Per case: 1000 untimed blocks (effect init, smoothing and delay-time glide settle), then
  600 timed 64-frame blocks, each `engine_process()` call timed with the DWT cycle counter.
  Interrupts stay enabled (only the USB console and the 10 ms main loop run): `min` is the
  undisturbed cost, `avg` the useful number, `max` includes interrupts.
- One console line per case, ~2.5 s apart (one round ~1.5 min), then the round repeats:

```
FXPROF round=1 cases=37 blocks/case=600 warmup=1000 image=fxprof-1 | cal kernel (64 x 4 MAC): flash=NNN ram=NNN cycles (con_drop=0)
FXPROF case=0 id=0 Clean var=clean avg=..  min=.. max=.. cyc/frame cpu%=.. decl=0 nan=0 blocks=600
FXPROF case=10 id=4 Reverb var=def avg=..  min=.. max=.. cyc/frame cpu%=.. decl=240 nan=0 blocks=600
FXPROF case=31 preset=1 "Dub Station" avg=.. min=.. max=.. cyc/frame cpu%=.. decl=695 rej=0 nan=0 blocks=600
FXPROF round=1 done
```
  `cyc/frame` = cycles per stereo frame (budget 1333 at 64 MHz / 48 kHz); `cpu%` = avg as a
  share of that; `decl` = the declared (UNMEASURED) cost the engine's admission uses
  (sum over the case's effects); `nan` must be 0. The effect cases include the engine's fixed
  cost (compare with case 0).
- The `cal` part of the round line runs the same small FP kernel from **flash** and from
  **RAM** (best of 50, interrupts off for ~16 us): if `flash` is much larger than `ram`,
  flash wait states / cache misses are a real cost on this chip.
- LED1 heartbeat (1 Hz), LED2 on while a case is being timed. FUNCTION held 1.5 s: power off.
  The watchdog (4 s) is fed by the main loop.
- The first round starts when a terminal opens the serial port (DTR), so it begins at case 0.

## Owner test steps

1. `cd flash-ready && shasum -a 256 -c SHA256SUMS` (both new lines must say OK).
2. Flash `sp1fx-fxprof.bin` as usual (desktop Chrome, <https://solderless.engineering>;
   SP-1 off, hold **T1 + T4** while connecting USB, release when T1 lights up, flash).
3. After boot, LED1 blinks. Open the console: `screen /dev/cu.usbmodem* 115200`
   (quit: Ctrl-A then K). No sound is expected at any time.
4. Wait for `FXPROF round=1 done` (~1.5-2 minutes; LED2 flickers while measuring).
5. Send me the whole log from `FXPROF round=1 ...` to `FXPROF round=1 done` (a second round
   is a welcome repeatability check). Nothing else is needed: no music, no button presses.
6. Power off with FUNCTION held 1.5 s (or unplug), then flash back whatever image you want.

What I will do with it: replace the declared `cpu_cycles` in `firmware/dsp/effects/*.c`
(and `companion/data/fx_table.json`, regenerate `tests/golden_frames.h`) with the measured
`worst`/`auto` numbers minus the Clean case, and set `ENGINE_DEFAULT_CPU_BUDGET` from the
measured Clean cost and the USB/I2S interrupt load seen in `sp1fx-usbaudio-in-v3.bin`.

## Expected numbers (SIMULATED, not measured)

`tools/fxprof_emu` runs the same cases (same compiler, flags and libm) in an instruction-
count emulator (Unicorn) with a Cortex-M4 cost table: zero-wait-state memory, no interrupts,
no flash cache effects. Before the optimisation the same model gave Dub Station ~1710
cycles/frame where the hardware showed ~2,300-2,400 (incl. ~60-80 of USB/I2S interrupts), so
expect the device to be roughly **1.3-1.4x** the model. Values below: cycles per frame.

| Case | before (v2 code) | now (v3 code) |
|---|---|---|
| Clean (engine only) | 132 | 97 |
| Filter def / worst / auto | 340 / 389 / 551 | 171 / 195 / 219 |
| Chorus def / auto | 509 / 718 | 249 / 295 |
| Delay def / auto (gliding) | 453 / 664 | 259 / 354 |
| Reverb def / auto | 901 / 1114 | 278 / 327 |
| Saturation def / auto | 368 / 582 | 184 / 233 |
| Flanger def / auto | 509 / 724 | 250 / 301 |
| Bitcrusher def / worst | 334 / 404 | 157 / 193 |
| Tremolo def / auto | 383 / 592 | 178 / 231 |
| Compressor def / auto | 389 / 627 | 180 / 247 |
| Tape wobble def / auto | 603 / 812 | 292 / 336 |
| Dub Station | 1710 | 673 |
| Boom Bap | 1615 | 607 |
| House | 1800 | 713 |
| Lo-Fi Tape | 1256 | 607 |
| Ambient | 1849 | 771 |

Reproduce: `tools/fxprof_emu/build.sh && python3 tools/fxprof_emu/fxprof_emu.py --warmup 1000`
(needs `pip install unicorn capstone pyelftools` in a scratch venv; `--profile --case N`
prints the cost per function).
