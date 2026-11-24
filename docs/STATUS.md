# SP-1 FX — status, test results, known issues

Evidence key: **[HOST]** verified by automated tests on a development Mac. **[SRC]** from upstream source/docs. **[TODO]** not started. Nothing here has been run on an SP-1.

## What exists
| Area | State |
|---|---|
| DSP core (`firmware/dsp`) | 10 effects + None, 4-slot engine, admission control, tails, limiter, diagnostics. **[HOST]** |
| Controls (`firmware/controls`) | debounce, gesture state machine, soft takeover, controller, monochrome LED renderer. **[HOST]** |
| Sync out scheduler (`firmware/app/sync_out.c`) | 24 PPQN + PO pulses, drift-free. **[HOST]** |
| Simulator (`simulator/`) | WAV in/out through the real engine, `--list`, `--bench`. **[HOST]** |
| Config sanitising (`engine_config_sanitize`) | repairs corrupt configs. **[HOST]** Binary storage/journal: **[TODO]** |
| USB audio in/out, I2S, codec drivers, ladder decode, LED PWM | **[TODO]** (reuse Tape Looper drivers, MIT) |
| Persistence (`firmware/storage`) | versioned record + two-page power-loss-safe journal, write debounce. **[HOST]** |
| Protocol spec (`docs/PROTOCOL.md`) | written; firmware side **[TODO]**. Companion app and guide graphic: in progress (subagents) |
| Presets (factory) | **[TODO]** |

## Test results (`make test`, last run: all pass)
- `test_dsp`: 2,382 checks. Parameter map/unmap, silence, 16 parameter corners x every effect with loud noise, automation stability, stereo isolation, filter frequency response (LP/HP), delay timing exact to the sample, feedback ratio, reverb decay, chorus delay, tremolo depth, compressor level, fast-math accuracy, recovery after NaN.
- `test_engine`: 36,924 checks. Wet/dry, bypass, global bypass, assignment preserves wet/bypass and per-effect params, memory + CPU admission with rollback, 2,000 random reassignments never overlap memory, multiple instances, NaN/Inf input, hot-signal limiter, no pops when switching effects, tails survive bypass, config repair, chain order.
- `test_controls`: 119 checks. Debounce, tap vs hold, rocker only on held channel (and only if pressed after it), wrap-around, repeat, chord, PLAY, volume steps/repeat, edit mode, soft takeover (approach, cross-over, jitter, NaN, resync), FN-hold power-off, admission error LED, LED encodings, sync scheduling over a simulated 10 min.

### Bugs the tests found (fixed)
1. `fx_log2` was off by +1 -> compressor read every level 6 dB hot.
2. Delay time glide stalled up to ~4 samples short of target (float precision) -> fixed with a minimum glide step.
3. Filter "zero resonance" was Q=0.5 (-6 dB at cutoff); now Butterworth.

## Known issues / limitations
- Replacing an effect drops the old effect's tail abruptly (new effect fades in, old one is cut). Acceptable for now; revisit with cross-fade.
- Saturation and bitcrusher are not oversampled; aliasing at high settings is expected.
- Delay max 400 ms (RAM); reverb ~43 KB; delay ~77 KB. With a 150 KB pool, "delay + two reverbs" is rejected by design.
- CPU numbers in effect descriptors are **estimates**. Host speed is not a proxy. Real cycle counts need the Cortex-M4F (DWT cycle counter).
- 8 LEDs, each a single dimmable LED (4 track + 4 beside PLAY). "Length" encodings are impossible; patterns use brightness/blink only. PWM gamma and visibility floor need tuning on a device.
- Rocker is two momentary buttons; button ladder decode thresholds still to be taken from upstream (calibrated raw ADC values).
- Soft-takeover pickup window (2.5%) and deadband (0.4%) are guesses pending real fader noise measurements.
- Default config starts with all slots bypassed and master at 75%.

## Next steps (suggested order)
1. Persistence: versioned binary config + A/B journal on a mock flash, power-loss tests (host).
2. Protocol: COBS-framed, CRC16, versioned; parser + fuzz tests (host); then Web Serial companion.
3. Presets (JSON schema + factory presets validated against the admission limits).
4. Firmware build environment (Zephyr 4.1 / SDK 0.17.4, pinned modules) producing an app-only image at 0x20000. **No flashing.**
5. **Ask for approval** before any flash: back up current firmware, flash one device, measure USB->headphone latency, CPU cycles, underruns.
