# SP-1 FX — status, test results, known issues

Evidence key: **[HW]** observed on the owner's SP-1 (owner-run; see the hardware log). **[HOST]** verified by automated tests on a development Mac. **[SRC]** from upstream source/docs or our own build output. **[UNTESTED]** built but never run. **[TODO]** not started. The integrated image v1-rc1 has **not** been run on an SP-1.

## v1-rc6 (2026-11-24) -- NOT RUN ON HARDWARE

`sp1fx-v1-rc6.bin`, SHA-256 `daabe9b3961c0b30d38230891b71ae7db819ff83cb3924093f0fa181b2a51597`,
153,592 B, check PASS, reproducible, 0 warnings; host suites pass, companion 56/56.
rc5 on hardware [HW, owner]: everything worked, including the charge standby pulse/steady display.
rc6 = rc5 plus:
- Battery fuel gauge in the charge standby: 1-8 LEDs, top one pulsing while charging, steady
  when done.
- No charging LED while running (rc3-rc5's PLAY-LED-4 blip removed).
- Battery % in the console and in the companion: METERS trailing byte; older companions reject
  it, the companion in this commit accepts it with or without.
- Conversion is CALCULATED/UNCALIBRATED [HOST tests]:
  - divider 2.0, assumed from Tape Looper's 2380 = 4.21 V anchor;
  - Li-ion table 3.30-4.20 V;
  - -0.1 V and a 95 % cap while charging;
  - ~2 s smoothing, 3 % hysteresis, 2 s updates;
  - load sag not corrected.

## v1-rc5 (2026-11-24) -- superseded by rc6 (worked on hardware)

`sp1fx-v1-rc5.bin`, SHA-256 `bfaa973eb2369d935b79d19ca02750296f270988166d5e83c4052dc3954810db`,
152,584 B, check PASS, reproducible, 0 warnings; host suites pass, companion 56/56.

= rc4 plus (owner feedback on line out):
- Three jack levels: Headphones / Loud (+15 dB) / Line out (+36 dB). SET_OUTPUT_MODE value 2
  = Loud; 0 and 1 keep their meaning. Levels are CALCULATED, not measured [HOST policy].
- Every start is at Headphones (`CONFIG_SP1FX_LINEOUT_RESTORE=n`).
- Going louder lowers the master by the difference (tested for every pair of levels over the
  whole VOL grid).
- A jack plug-in at Loud/Line out returns to Headphones [UNTESTED; uses the tip-sense read
  already polled].
- Soft feedback saturation (`fx_fbsat`) in delay and chorus/flanger: linear to full scale,
  smooth knee to 1.9 [HOST: identity below 1, monotonic, bounded; hot input at max feedback
  never pins the +-2 rail].

## v1-rc4 (2026-11-24) -- superseded by rc5 (not run)

`sp1fx-v1-rc4.bin`, SHA-256 `ae95d5caef17b36613dce3104ee03047a6ac381dcb0b4d5d79fd44511365ffbe`,
151,504 B, check PASS, reproducible, 0 warnings; host: 12 suites pass, companion 56/56.

**rc3 on hardware [HW, owner-run, console read live by the lead]:**
- The register-level SAADC scanner works (ticks=38676 scans=38675, adc_to=0); all buttons,
  faders, PLAY and VOL work; FN + FWD presets work; no resets.
- Two **control-loop starvation near-misses**:
  - 2.1 s with stage flash_w;
  - 4.9 s with stage diag, i.e. formatting the diag block under starvation.
  - Saves took up to 1.8 s.
- CPU [HW]: total ~99 % (audio thread 77 %, USB threads usbd + udc_nrfx ~21 %, main 0.4 %,
  idle < 1 %). Engine 66-81 % vs 46 % for Boom Bap on the profiling image. The engine figure
  includes interrupt and USB-thread pre-emption during engine_process, so it is not a clean
  DSP number.
- The guard (engine cycles only) did not act.

**rc4 changes:**
- Guard on total block time; starvation shed from the audio thread (control loop 0.5 s
  late and < 5 % idle) [HOST engine part].
- Saves stepped at 32 B/tick, never under high load [HOST].
- Event-based saving (owner decision) [HOST]: preset switch, edit/effect changes and the
  companion's settings save 1.5 s after the last event; tweaks only at the next event, at
  power off, SAVE_NOW, or after 120 s; nothing at boot. Trade-off documented in
  BRINGUP_SP1FX.md.
- Diagnostics off by default ('d'/'i' keys), one line per tick, effect names and per-thread
  shares [UNTESTED].
- Headphone-jack **Line out** mode [HOST policy/protocol/storage; levels CALCULATED, not
  measured]: mixer 0 dB + trim 0 dB = +36 dB over headphones, ~1.0 Vrms at 0 dBFS (from
  Cirrus' 35 mW / 30 ohm). Master drops 36 dB on the switch. New SET_OUTPUT_MODE (0x1A),
  STATE trailing out_mode byte (companion updated; older companions reject it), journal
  extension byte (older records read as headphones).
- Preset LEDs: 7 rapid flashes then the bar [HOST].

**Load analysis, open:**
- The USB threads' 21 % with only audio streaming is large. Zephyr device_next handles every
  1 ms frame (SOF, ISO OUT, explicit feedback IN) through two threads plus buffer
  allocations; the size-optimised libc memcpy inside the stack is a suspect. Not measured
  per event.
- The integrated image's per-block overhead (whole block minus engine) is visible as blk% vs
  cpu%.
- Next: profile the USB path, and if the heavy presets stay above ~70 % total, a "lite"
  reverb/delay mode (owner accepts grit). Not done in rc4.

## v1-rc3 (2026-11-24) -- superseded by rc4 (ran on hardware, starvation near-misses)

`sp1fx-v1-rc3.bin`, SHA-256 `ec36e3befc937ab229822cc036d2e7788865134261af7d61a5631c06f3fa7199`,
149,612 B, check PASS, reproducible, 0 warnings, all host tests pass. (An interim rc3 build with
a 3 s hold, 57cbe11b..., was never handed over and is replaced.) Details/test: docs/BRINGUP_SP1FX.md.

**rc2 on hardware [HW, owner-run, console read live by the lead]:**
- Ran steadily: `stalls=0`, no CRASH/STALL; threads: audio 60 %, USB/other 21 %, main 1.4 %,
  idle 17 %.
- The owner's "crash" was the 1.5 s FN-hold power off firing during FN + rocker.
- **Controls dead for whole boots**: no control events after `adc_to=1`.
  - Cause in our code [SRC]: rc2's async SAADC read never restarted a scan that did not
    complete.
  - Why the SAADC lost a conversion is not known. It is consistent with rc1's blocking
    `adc_read` hangs.
- Overload guard shed slots 0, 1, 3 after one 114 % window and stayed latched (effects
  silent).
- Heavy saved state: engine 48 % average, whole block 73 % average, window max 94-114 %.

**rc3 changes:**
- Register-level, interrupt-free, self-restarting SAADC scanner that cleans up the
  bootloader's SAADC/PPI state [HOST state machine; driver UNTESTED]. Plus `B` button lines
  and a 2 s `ctl` line (alive counters, scan age, raw ladders/faders, FN/gesture state)
  [UNTESTED].
- FN power on/off = 4 s hold with an 8-LED fill (0.5 s per LED, PLAY-side then T1-T4, nothing
  before 0.5 s). Any other button in the same FN press cancels it (rc2: VOL did not;
  FN-then-rocker only from the rocker press) [HOST].
- Power on is implemented in the app after the bootloader hands over (Tape Looper's
  charge-standby design, MIT, shipped on this hardware). FN or USB wake goes to standby,
  watchdog/crash recovery and power-on start at once [HOST decision logic; UNTESTED on
  hardware, in particular whether the TE bootloader preserves RESETREAS OFF/VBUS (it preserved
  DOG in rc1/rc2)].
- Charger [SRC]: /CE P0.21 now driven low (charging enabled; earlier images never touched it),
  /CHG P0.22 and /PGOOD P0.24 read; battery divider AIN4 P0.28 scanned (`batt_raw=`,
  uncalibrated). USB into an off unit: no start, charge pulse on one LED. Power off while
  plugged: soft reset into the charge display. Charging while running: dim blip on PLAY LED 4,
  `info`, companion METERS flags [UNTESTED].
- Overload guard [HOST]:
  - judges engine cycles, not whole-block time (USB/other overhead averaged ~25 % on the
    device);
  - the safety net needs 2 windows over 9/8 in a row;
  - blocks during flash writes are skipped;
  - auto-recovery after ~10 s calm, one slot at a time, back-off x2 per repeat (max x16);
  - shed slots blink at 1 Hz;
  - METERS flags carry the shed mask / forced bypass / USB power / charging (spec updated,
    companion shows them);
  - `D` line shows `win_max_period=`.
- First boot without saved settings is already pass-through (all slots bypassed, not
  processed), so no change there; the heavy state was the owner's saved configuration [SRC].

## v1-rc2 (2026-11-24): rc1 watchdog-reset fixes

`sp1fx-v1-rc2.bin`, SHA-256 `3ccf3c8ed9fee5de1276e1f26034cd33dd52f893120aa0c6eee27654a2a8f250`,
152,636 B, `check_image.py` PASS, reproducible, 0 warnings. Test plan: top of `docs/BRINGUP_SP1FX.md`.

**rc1 hardware result [HW, owner-run, console read by the lead]:** watchdog resets (rr=2), dozens
in a row, every ~6-20 s, with and without the companion, often after effect/preset changes.
Crash records at the watchdog moment: thread `idle` (pc 0x31700 = WFI in `cpu_idle`) in most,
`usbd` (`k_event_wait`) and `udc_nrfx` (`z_pend_curr`) in others [SRC: addr2line on a
byte-identical rebuild of rc1]. Settings were written on nearly every boot (seq 41..48),
127-195 ms per record; one boot showed an erase of 87 ms that completed.

**Analysis [SRC, hypotheses ranked; none proven]:**
1. All threads blocked at the watchdog moment => the control thread (only watchdog feeder) was
   waiting for something that never came, not starved (starve=0, idle running). Its only
   unbounded wait in rc1 was the SAADC `adc_read` (Zephyr `adc_context`: semaphore,
   `K_FOREVER`), 2x every 8 ms. Every other wait there is bounded (I2C 500 ms timeout, flash,
   sleeps, duck waits); there is no wait on the audio thread (`engine_config_settled` is only
   polled). => rc2 reads the SAADC asynchronously with a 100 ms timeout. **Leading hypothesis.**
   Why the SAADC interrupt would go missing with USB audio + flash writes running is not
   known.
2. Flash activity coinciding with the hangs: rc1 wrote a record on every boot (fader pickup
   counted as a change; bug, fixed in `controller.c`, host-tested) and could erase with USB
   running (87 ms CPU stall). => rc2: no boot write, dedup, >= 30 s between saves, no erase
   with VBUS present.
3. USB stack as a whole (owner's suspicion): no livelock found in Zephyr's CDC ACM / our
   callbacks by inspection; the CDC TX path never waits (now the host-tested queue). The
   crash records do not show a USB thread hogging the CPU. Not excluded: the rc2 stall
   watcher records per-thread CPU shares, SOF rate and USB frame rate.
4. A loaded config replayed at every boot: config is sanitised and admission-checked by the
   engine at load (unchanged); rc2 adds a crash-loop breaker (2 early crashes in a row ->
   factory defaults, journal neither loaded nor written).
Host simulation [HOST]: preset changes + effect cycling + VOL + debounced saves for 10 min with
the audio thread stopped: every control step returns, no erase is chosen, 16 records then
saves wait. Stalled/slow USB host [HOST]: the TX queue never waits, drops whole frames only,
the companion's connect burst (HELLO, 11 FX_DESC, STATE, SUBSCRIBE) fits the 4 KB queue with
no host reading at all; reconnect starts in text mode and switches again with a separator.

## v1-rc1: integrated firmware (2026-11-24) -- superseded by rc2 (resets on hardware)

`firmware/app/zephyr_sp1fx` -> `flash-ready/sp1fx-v1-rc1.bin`, SHA-256
`cf09092a75e575f643c869b8b7a8155f9b28ece42a70d8151b789c67bd20a9a8`, 144,864 B at 0x20000,
RAM 205,240 B of 256 KiB, `check_image.py` PASS (incl. `--reserve 0xFD000:0xFF000`), two clean
builds byte-identical, 0 compiler warnings. [SRC] Owner test plan: `docs/BRINGUP_SP1FX.md`.

| Part | Where it comes from | Evidence |
|---|---|---|
| USB audio in (UAC2 explicit feedback) -> engine -> I2S, codecs, headphone switch, crash dump, starvation guard | zephyr_usbaudio image A v3, same code | the v3 image [HW]; this image [UNTESTED] |
| Buttons, faders, gestures, soft takeover, LED renderer, FN + rocker presets | zephyr_controls + controller.c | the controls image [HW]; in this image (with audio running) [UNTESTED] |
| VOL+/VOL-: 2 dB per press on the -60..0 dB master taper, volume bar on the PLAY-side LEDs, saved; output trim now -6 dB (was -12) | new `controls/volume.c`, controller | [HOST] (step grid, dB mapping vs the engine's actual gain, repeat, LED bar, protocol event bytes); loudness [UNTESTED] |
| Device preset list = the six factory presets, **one Clean**; a preset load keeps the volume | controller (`preset_keep_master`) | [HOST] |
| Overload guard: mean over 32-block windows, 3 windows over 85 % (~128 ms) or one over ~96 % | `engine_overload_feed` | [HOST] (the hardware Ambient pattern never acts in 20,000 blocks); [UNTESTED] on the device |
| Companion protocol on the CDC ACM port; text diag until the first valid frame, then binary only (one 0x00 separator), back to text when the port closes | protocol.c + `app/app_logic.c` | protocol [HOST] (unchanged, golden frames vs the JS); mode switch [HOST]; over real USB [UNTESTED] |
| Settings journal in 0xFD000-0xFEFFF; chunked writes; erases only while quiet (pre-erase of the spare page), at power off or SAVE_NOW, always ducked | config_store.c (+ spare pre-erase API), app_logic.c | [HOST] (power loss, pre-erase, 2 h simulated session: 0 erases while audible); Zephyr erase is one blocking NVMC op [SRC]; timing on the device [UNTESTED] |
| Sync jack (MIDI clock + PO), clocked by rendered audio frames, **default off**, pins/timer untouched until switched on, on/off saved | sync_hw.c (+ run/stop), sync_out.c | scheduler [HOST]; driver serial log [HW] in the sync image; with gear and in this image [UNTESTED] |
| Diagnostics: engine vs whole-block cycles, pre-emption overhead, per-thread CPU share (runtime stats), store/sync/console counters | main.c | [UNTESTED] |

CPU estimate with the controls running [estimate, unmeasured]: v3 presets ~50 % [HW], plus
~2-4 % for the control thread (SAADC scan, controller, LEDs, protocol), the config copies while
faders move (now word copies) and the runtime statistics; sync on adds ~4-6 % (31,250 bit-timer
interrupts/s). The new `threads%` diag line measures it.

Fixes in this round:
1. Duplicate Clean: the usbaudio images had "Clean" at index 0 plus `clean.json` in the
   factory table. v1-rc1 uses the factory table only. [HOST] (the old images are unchanged.)
2. Overload guard false trips (Ambient, reverb shed 14 times on hardware): windowed mean as
   above. Ambient was not changed: the reverb's cost is parameter-independent, so there is no
   cheaper Ambient setting; spike sources are reduced instead (no text on the port while the
   companion is connected; diag lines every 2 s instead of 4 lines per second), and the new
   diag line separates engine spikes from pre-emption. [HOST] / [UNTESTED]
3. Input clip: by design, no change. `in_clip` counts host samples at full scale before the
   input gain; the clipping (if any) happened on the Mac. Internally float with headroom and
   an output soft limiter. Remedy: lower the source, or the companion's input gain. [SRC]
4. T3/T4 blink on first fader touch: no cause found in the code (takeover cue, renderer, PWM
   update and PWM release checked). This image prints every soft-takeover state change (`K`
   lines) so the next occurrence can be pinned down. [SRC]
5. Engine `post()` copied the ~900 B config with the byte-wise libc memcpy (struct
   assignment) on every fader move; now word copies. [HOST]

Flash safety [SRC]: ADR-003 addresses re-verified against the board dts; slot0 shortened to
0x20000-0xFCFFF in the app overlay, settings partition 0xFD000-0xFEFFF, board `storage`
0xFF000 untouched; compile-time asserts + a runtime range check on every flash access +
`check_image.py --reserve`. The brief's "0xFF000 area" was not used: ADR-003 explicitly keeps
settings out of the board's `storage` page.

Not done / open: device-side user presets (owner decision: companion only); the guide PNG/PDF
still say "16 steps" (HTML updated); Windows; latency measurement.


## What exists
| Area | State |
|---|---|
| DSP core (`firmware/dsp`) | 10 effects + None, 4-slot engine, admission control, tails, limiter, diagnostics. **[HOST]** |
| Controls (`firmware/controls`) | debounce, gesture state machine, soft takeover, controller, monochrome LED renderer. **[HOST]** |
| Sync out scheduler (`firmware/app/sync_out.c`) | 24 PPQN + PO pulses, drift-free. **[HOST]** |
| Simulator (`simulator/`) | WAV in/out through the real engine, `--list`, `--bench`. **[HOST]** |
| Config sanitising (`engine_config_sanitize`) | repairs corrupt configs. **[HOST]** Binary storage/journal: **[TODO]** |
| USB audio in/out, I2S, codec drivers, ladder decode, LED PWM | **[TODO]** (reuse Tape Looper drivers, MIT) |
| Persistence (`firmware/storage`) | versioned record + two-page power-loss-safe journal, write debounce, spare-page pre-erase. **[HOST]** On flash at 0xFD000-0xFEFFF in v1-rc1 **[UNTESTED]** |
| Protocol (`docs/PROTOCOL.md`, `firmware/protocol`) | spec + C device side, byte-compatible with the companion's JS on golden frames, 100k-stream fuzz. **[HOST]** USB CDC transport: built in v1-rc1 **[UNTESTED]** |
| Companion (`companion/`) | Web Serial + demo device, Tape Looper-style UI, presets. 51 Node tests + headless browser smoke test. Never run against a real SP-1. |
| Factory presets (`presets/`, `firmware/app/factory_presets.h`) | six presets, each runs through the real engine within budget. **[HOST]** |
| Build (`docs/BUILD.md`, `firmware/scripts`, `firmware/app/zephyr_min`) | Zephyr **v4.3.1** (Tape Looper's lock file pins this, not 4.1) + SDK 0.17.4 in `../toolchain`. Minimal heartbeat/watchdog/FN-power-off image builds: 17,316 B at 0x20000, bit-identical across 3 clean builds, `check_image.py` passes. **Verified on hardware (owner report, 2026-11-24, spare SP-1, flashed via Solderless): the image boots and track LED 1 blinks.** Not yet confirmed: steady blinking beyond the 5 s watchdog window, FN-hold power-off, behavior with USB plugged in. |
| Preset cycling (ADR-004) | FN + rocker, fade, flash + position bar, `PRESET_LOADED` event, C + companion. **[HOST]** Device-side user presets: **[TODO]** |
| Guide (`docs/guide/`) | printable quick-reference (schematic device drawing). |

## CPU: why effects were ~4x the declared cost (2026-11-24, v3)

Evidence key here: **[HW]** measured on the device by the owner, **[ELF]** read from the built
image (objdump/nm), **[SIM]** instruction-count emulation (`tools/fxprof_emu`, Unicorn +
Cortex-M4 cost table, zero-wait-state memory, no interrupts; not a measurement),
**[HOST]** host tests. Nothing below except the [HW] line was run on an SP-1.

- [HW] v2 image: Clean ~138 cycles/frame; every effect preset 2,270-2,430 (170-200 %) -> static.
- **Not the cause [ELF]:** the FPU is used: `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`,
  `CONFIG_FP_HARDABI=y`; no `__aeabi_f*`/`__addsf3`/`__mulsf3` in the image; `-O2`
  (`CONFIG_SPEED_OPTIMIZATIONS`) is applied to the DSP sources; the nRF52 I-cache is enabled
  (`CONFIG_NRF_ENABLE_ICACHE`, `soc.c`). libm in per-sample loops: only `floorf` (compressor every
  sample via `fx_exp2`, bitcrusher per held sample); `powf`/`expf`/`tanf`/`logf` were per block
  or per parameter change. Denormals: effects already flush their state; the M4 FPU has no
  denormal slow path as far as I know (source finding, unverified); a branch-free flush is now
  used in the reverb loop anyway.
- **Cause 1 [SIM]: the code really costs that much; the declared values were guesses ~3x low
  and left out the engine's own cost.** Model of the v2 code: Clean 132, Dub Station 1710 cycles/
  frame (reverb alone 580, delay 156, filter 111, saturation 98, engine glue 350). [HW]/[SIM]
  ratio for Dub Station ~1.36 (interrupts ~60-80/frame + flash wait states), consistent.
- **Cause 2 [ELF]: byte-wise `memcpy`/`memset`.** The toolchain's size-optimised picolibc
  `memcpy` is `ldrb/strb` one byte per loop (~5 cycles/byte). GCC turned the engine's per-slot
  dry-copy loops into `memcpy` calls: ~48 cycles/frame **per active slot** (190 of Dub Station's
  1710); every parameter change copied the ~900 B config twice byte-wise (+~190/frame while a
  fader moves); effect init/reset cleared up to 77 KB byte-wise (~6 ms in one 1.33 ms block);
  the USB ring copies were byte-wise too.
- **Cause 3 [SIM]: per-sample overhead from code shape:** state accessed through pointers
  (Zephyr builds with `-fno-strict-aliasing`, so every store reloads every field), per-sample
  `switch`, per-sample clamps/LFOs, per-sample one-pole smoothing that never settles in float.

What changed (v3), all [HOST]-tested, cost [SIM] (before -> after, cycles/frame):
| Change | Effect |
|---|---|
| Engine: no dry copy (the chain buffer IS the dry signal), in-place processing for 100 %-wet insert effects, smoothers snap to target (5e-4) so steady state runs constant-gain loops, gain targets (`powf`) recomputed only on change, NaN screen once per block, peaks as integer max, word copies (`fx_copy32`) for the mailbox | Clean 132 -> 97; per-slot glue ~60 -> ~25 |
| `fx_zero32`/`fx_copy32`, `fx_floori` (no `floorf`), `-fno-tree-loop-distribute-patterns` on the DSP, usbaudio `main.c` and `usb_audio_core.c` (no libc calls in the audio path: checked in the ELF) | |
| **Reverb at 12 kHz** (4:1 average-decimate, network, linear 1:4 upsample), block-rate LFOs, state in locals. **Intentional lo-fi** (owner decision): tail band-limited to ~5 kHz, some aliasing/imaging, 4-sample latency | 580 -> ~180 (incl. glue) |
| Delay: steady time = whole-sample delay (no interpolation), state in locals; glide path unchanged | 156 -> ~110 |
| Chorus/flanger/tape wobble: LFO every 16 samples, delay ramped linearly in between (< 0.02 sample deviation); tape drift low-pass at the 16-sample rate (variance-matched noise; its random sequence changed) | ~200/290 -> ~150/195 |
| Saturation and filter drive: **cubic soft clipper** `fx_softclip` (no divide) instead of the rational tanh: harder knee (within 0.1 of `fx_tanh`), **intentional sound change** | |
| Compressor: detector/gain computer once per 4 frames, gain ramped linearly | ~110 -> ~80 |
| Filter: per-sample `switch` -> mode coefficients; tremolo: one loop per waveform; bitcrusher: `fx_floori` | |
| **Overload guard** (`engine_overload_feed`, usbaudio v3): block work > 85 % of the period for 8 blocks -> fade out + switch off the most expensive live slot, then forced global bypass; `OVERLOAD` console line, `diag` fields; cleared by a preset change / new effect in the slot | [HOST] `tests/test_fxprof_core.c` |
| **Declared `cpu_cycles` now (UNMEASURED)**: [SIM] worst-case cost of the effect alone minus Clean, x1.3: Filter 130, Chorus 200, Delay 210, Reverb 240, Saturation 115, Flanger 200, Bitcrusher 125, Tremolo 115, Compressor 110, Tape wobble 255 (was 70/120/80/320/90/120/40/45/110/120). Budget stays 850. Companion table + golden frames updated. | presets: Dub 695, Boom Bap 590, House 780, Lo-Fi 625, Ambient 765 declared |

[SIM] presets now: Dub Station 673, Boom Bap 607, House 713, Lo-Fi Tape 607, Ambient 771 cycles/
frame (were 1710/1615/1800/1256/1849). Expected on the device x1.3-1.4 plus interrupts:
**~900-1100 = 68-83 %, untested**; House and Ambient are closest to the guard's 85 %.
Output vs the v2 code on the fxprof signal [HOST, scratch comparison, 2 s per case]: Clean,
filter (no drive), delay, bitcrusher, tremolo differ by < 3e-4 (difference 68-80 dB below the
signal: the smoother snap); filter drive, saturation, compressor, chorus/flanger, tape wobble,
reverb differ by design (waveform SNR 2-29 dB; for modulated delays that is mostly a sub-sample
time shift, not noise). Not listened to. Tests: no existing accuracy test was loosened; only the
admission-arithmetic tests (C and companion JS) were updated for the new declared values.
Reverb memory declaration kept at 43,116 B (uses 10,868) to keep memory admission and the
protocol unchanged.

Images (built, check_image PASS, reproducible, **not run on hardware**): `sp1fx-fxprof.bin`
(SHA-256 81c9b1de...0443; profiling, silent, docs/BRINGUP_FXPROF.md) and
`sp1fx-usbaudio-in-v3.bin` (315d7eef...e00f; docs/BRINGUP_USBAUDIO.md "v3").
Next: owner runs fxprof -> replace the declared values with measured ones.

## Test results (`make test`: 12 C test binaries; `node --test companion/tests/*.test.mjs`: 55; all pass, 0 warnings)
- v1-rc1 additions: `test_app_logic` 75,359 checks (save/erase scheduling incl. a 2 h simulated session with the real journal, console mode switch, 0x00 separator through the C parser, quiet detector, rate limiter); `test_controls` +volume grid/dB/LED bar, one Clean, volume kept on preset load (318 total); `test_storage` +spare pre-erase and power cut during it (2,725); `test_fxprof_core` windowed guard (271,539); `test_protocol` VOL+ event golden frame regenerated from the JS for the 2 dB grid (903).
- Also: `test_storage` 1,426 checks, `test_protocol` 903, `test_presets` 557, `test_controls` 172.
- `test_dsp`: 2,382 checks. Parameter map/unmap, silence, 16 parameter corners x every effect with loud noise, automation stability, stereo isolation, filter frequency response (LP/HP), delay timing exact to the sample, feedback ratio, reverb decay, chorus delay, tremolo depth, compressor level, fast-math accuracy, recovery after NaN.
- `test_fxprof_core`: 250,922 checks. Every profiling case admitted, finite, bounded, not silent; signal deterministic and looping; overload guard (spikes ignored, sheds most expensive slot first, fade without click, forced bypass, clear, init blocks ignored, disabled); `fx_floori` exact vs floorf, `fx_exp2`, `fx_softclip` (odd, monotonic, bounded, within 0.1 of fx_tanh), `fx_flush_fast` (exact above 2e-11, never denormal), word copy/clear, `fx_dl_q` identical to the old rounding.
- `test_engine`: 36,926 checks. Wet/dry, bypass, global bypass, assignment preserves wet/bypass and per-effect params, memory + CPU admission with rollback, 2,000 random reassignments never overlap memory, multiple instances, NaN/Inf input, hot-signal limiter, no pops when switching effects, tails survive bypass, config repair, chain order.
- `test_controls`: 119 checks. Debounce, tap vs hold, rocker only on held channel (and only if pressed after it), wrap-around, repeat, chord, PLAY, volume steps/repeat, edit mode, soft takeover (approach, cross-over, jitter, NaN, resync), FN-hold power-off, admission error LED, LED encodings, sync scheduling over a simulated 10 min.

### Bugs the tests found (fixed)
1. `fx_log2` was off by +1 -> compressor read every level 6 dB hot.
2. Delay time glide stalled up to ~4 samples short of target (float precision) -> fixed with a minimum glide step.
3. Filter "zero resonance" was Q=0.5 (-6 dB at cutoff); now Butterworth.

## Known issues / limitations
- Replacing an effect drops the old effect's tail abruptly (new effect fades in, old one is cut). Acceptable for now; revisit with cross-fade.
- Saturation and bitcrusher are not oversampled; aliasing at high settings is expected.
- Delay max 400 ms (RAM); reverb ~43 KB; delay ~77 KB. With a 150 KB pool, "delay + two reverbs" is rejected by design.
- CPU numbers in effect descriptors are **UNMEASURED estimates** (instruction-count emulation x1.3, see "CPU" above). Host speed is not a proxy. Real numbers: run `sp1fx-fxprof.bin` (docs/BRINGUP_FXPROF.md).
- 8 LEDs, each a single dimmable LED (4 track + 4 beside PLAY). "Length" encodings are impossible; patterns use brightness/blink only. PWM gamma and visibility floor need tuning on a device.
- Rocker is two momentary buttons; button ladder decode thresholds still to be taken from upstream (calibrated raw ADC values).
- Soft-takeover pickup window (2.5%) and deadband (0.4%) are guesses pending real fader noise measurements.
- Default config (no saved settings) starts with all slots bypassed and master at 75 % (-15 dB, -21 dBFS ceiling with the -6 dB trim).

## Next steps (suggested order)
1. Persistence: versioned binary config + A/B journal on a mock flash, power-loss tests (host).
2. Protocol: COBS-framed, CRC16, versioned; parser + fuzz tests (host); then Web Serial companion.
3. Presets (JSON schema + factory presets validated against the admission limits).
4. Firmware build environment (Zephyr 4.3.1 / SDK 0.17.4, pinned modules) producing an app-only image at 0x20000. **No flashing.**
5. **Ask for approval** before any flash: back up current firmware, flash one device, measure USB->headphone latency, CPU cycles, underruns.

## Known discrepancies / open items (from review)
- Reverb memory: firmware declares 43,116 B, the companion's table says 43,120 (aligned). Cosmetic; the device is authoritative for admission.
- Companion admission prediction sums memBytes only; the engine also counts state bytes and fragmentation.
- Sync jack: BPM and an on/off switch (companion only, **saved and restored at power-up**, not part of presets) are stored and scheduled. Still missing: the hardware driver for the jack pins (timer-driven MIDI/PO output, untested upstream) and any tempo-synced effect.
- METERS resets held peaks, which also affects the output-level LEDs while a host is subscribed.
- Event ring drops the oldest events silently if the host stops polling.
- Bug found in review: companion step-parameter mapping (round vs floor) disagreed with firmware; fixed and tested.
- Minimal image: when USB is plugged in, FN-hold enters SYSTEM_OFF and the device may stay dark until FN is pressed or USB is replugged (Tape Looper soft-resets instead). Pins, PWM ownership of the LED pin, and watchdog-channel assumptions are unverified guesses from Tape Looper's source.

## Hardware log (owner-reported; no automated evidence)
- 2026-11-24 (owner-run): `sp1fx-v1-rc5.bin`: everything works incl. the charge standby display -> rc6 adds the fuel gauge.
- 2026-11-24 (owner-run, console read live by the lead): `sp1fx-v1-rc3.bin`: controls fixed (scanner works), power on/off and presets fine, no resets; control-loop starvation near-misses at ~99 % CPU -> rc4.
- 2026-11-24 (owner-run, console read live by the lead): `sp1fx-v1-rc2.bin` runs steadily (stalls=0, no crash); FN-hold power off (1.5 s) fired by accident while cycling presets; controls dead in some boots after a SAADC scan timeout; overload guard shed and stayed latched -> rc3.
- 2026-11-24 (owner-run, console read by the lead): `sp1fx-v1-rc1.bin` resets by watchdog every ~6-20 s (see "v1-rc2" at the top). Superseded by rc2 (not yet run).
- 2026-11-24: `sp1fx-min-heartbeat.bin` (SHA-256 18afab48...592667) flashed with Solderless to the spare unit; LED 1 blinks. This confirms: bootloader accepts an app linked at 0x20000, our reset/vector layout is valid, LED pin P0.29 is right.
- 2026-11-24: `sp1fx-tone.bin` (SHA-256 87886d14...433f9): owner reports left channel low tone, right channel high tone, "sounds great". This confirms (owner-observed) the CS42L42/TAS2505 init, I2S slave clocking from the 3.072 MHz oscillator, and channel order. Not yet reported: which output was heard (speaker vs headphones), LEDs 2-4, serial status line, speaker mute on headphone insert, pop-free switching.
- Finding from the tone build: `nfct-pins-as-gpios` in the board dts has no effect in Zephyr 4.3.1 (it is only read from `&uicr`). Making it effective would write UICR.NFCPINS (outside our image) and reset the chip: do not change without an explicit decision. P0.09 (TAS2505 reset) works as a GPIO because this unit's UICR already allows it.
- 2026-11-24: `sp1fx-sync.bin` (SHA-256 12255e2c...3073d) flashed by the owner; serial log pasted (observed): modes cycle with FN taps, MIDI clock count rises 48/s at 120 BPM, PO pulses scheduled 4/s, 0 dropped bytes, HFXO on. Anomalies (from reading sync_hw.c, cause not proven): bit-timer ISR rate about 31,050/s vs 31,250 nominal (about 0.6% merged interrupts) and the `late` counter rising about 550/s; the reported max latency of 31 us is the measurement ceiling (timer clears at 32 us), so the true worst case is hidden. Possible fix if gear shows errors: hardware-generated edges (SPIM/PWM DMA) instead of ISR-written bits. **Real-gear test (EP-133 / EP-40 / PO / Volca) pending: owner will test later.**
- Pending owner tests: `sp1fx-controls.bin` (checklist in docs/BRINGUP_CONTROLS.md), sync with real gear. Pending build: USB audio images (agent running).
- 2026-11-24: USB audio images built (not yet flashed): `sp1fx-usbaudio-in.bin` (A) and `sp1fx-usbaudio-io.bin` (B). **Architecture finding:** the nRF52840 USB controller has exactly ONE isochronous IN and ONE isochronous OUT endpoint. Image A uses the IN endpoint for explicit feedback, so image B (return stream to the host) must use implicit feedback instead (no feedback endpoint): expected to work on macOS/Linux, **not on Windows' built-in driver**. Tape Looper's UAC2 patch (Windows feedback fix) is NOT applied because build_firmware.sh refuses a modified Zephyr checkout; Windows is expected to fail on image A (Code 10). Needs an owner decision. Engine pool 150 KiB; RAM 185-189 KB of 262 KB used.
- Pending owner tests: USB audio A then B (docs/BRINGUP_USBAUDIO.md). First real CPU-per-preset numbers will come from the A console.
- 2026-11-24 (owner-run, console log captured by me): `sp1fx-controls.bin` OBSERVED working on the spare SP-1. All 10 buttons decode correctly (raw ladder values: T1-T4 L1 = 214/405/732/1216, PLAY 908; VOL+/VOL- L2 = 1810/729; FWD/REV L2 = 1209/405). Faders 1-4 span raw 0..~3680 (0..100%), smooth, no reversals. Soft-takeover cue and edit mode (FN+T2) work (owner-observed LED behaviour). Effect cycle direction correct (FWD = next, REV = previous, wraps through None); admission refusal (`REJECTED Delay status=2`, old effect kept); FN+rocker presets step and wrap (Boom Bap, Dub Station, Clean, Ambient, Lo-Fi Tape, House); bypass tap; volume 75% <-> 81%; PLAY global bypass (walking LED); FN-hold 1.5 s power-off. Main-loop time peaked at 3.2 ms (limit 8 ms) at preset loads. Corrections to the docs: slots boot ENABLED with wet 0% (byp=0000), not bypassed. Still unobserved: the checklist items for LED brightness steps and the "blink on T3/T4 on first fader touch" the owner mentioned (console reported no pickup-needed state at boot, so that blink was not the takeover cue; cause unknown, possibly leftover bootloader PWM on the LED pins).
- 2026-11-24 (owner-run, console-captured): `sp1fx-usbaudio-in.bin` (v1) OBSERVED: USB audio from a Mac plays through the real engine on the SP-1 (speaker), 48 kHz, drift feedback converged (~48.000 frames/ms), 60+ s with 0 underruns/overruns/I2S errors; first real CPU numbers: Clean preset (all bypassed) about 130-215 cycles/frame average, peaks 400-490; the Mac cannot control the volume (no UAC2 volume control); output peak only -15 dBFS because master 75% and the analog stages were set quiet on purpose. **CRASH OBSERVED:** the first FN tap (Clean -> Dub Station) hung the firmware: static, then silence, watchdog reset (`rr=2`) after about 5 s, device recovered by itself and the Mac stream resumed.
- Crash analysis (subagent, not reproduced on hardware): two real engine bugs found and fixed (non-atomic `engine_apply_config` race with the audio thread; unchanged slots kept running at a stale layout after a config change), neither proven to be THE cause. Leading unproven hypothesis: Dub Station costs more CPU than the budget on the real M4F, so the priority-0 audio thread starves the main thread and the watchdog feed. Mitigations in v2: staged-config mailbox, init budget per block, starvation guard, audio stack 6 KB, crash dump (`CRASH:` line on next boot) and diagnostics (`diag` line). v2 images: `sp1fx-usbaudio-in-v2.bin` (SHA-256 9da4341d...5cd3), `sp1fx-usbaudio-io-v2.bin` (fb8b3efa...2ab7). NOT yet flashed. Retest steps: docs/BRINGUP_USBAUDIO.md.
- Declared effect CPU costs in the effect descriptors are estimates written before any measurement; the Clean passthrough already costs more than expected, so treat all declared numbers as unreliable until measured per preset on the device.
- 2026-11-24 (owner-run, relayed by the lead; console not seen by me): `sp1fx-usbaudio-in-v3.bin`: the six presets run at about 50 % CPU, no crashes. Ambient: average ~49 %, occasional blocks ~1,190 cycles/frame (89 %), the overload guard shed the reverb slot 14 times. `in_clip` counted (hot Mac source). Master 0.75 is quiet and the Mac cannot change the volume. These led to the v1-rc1 changes above.
- 2026-11-24 (owner-run, console): `sp1fx-usbaudio-in-v2.bin` OBSERVED: no crash on preset changes, but Clean ~138 cycles/frame and every effect preset 2,270-2,430 cycles/frame (170-200 % CPU), audio static. Analysis and fix (not yet run on hardware): see "CPU" section; new images `sp1fx-fxprof.bin`, `sp1fx-usbaudio-in-v3.bin`.
