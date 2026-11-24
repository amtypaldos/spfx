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
| Protocol (`docs/PROTOCOL.md`, `firmware/protocol`) | spec + C device side, byte-compatible with the companion's JS on golden frames, 100k-stream fuzz. **[HOST]** USB CDC transport: **[TODO]** |
| Companion (`companion/`) | Web Serial + demo device, Tape Looper-style UI, presets. 51 Node tests + headless browser smoke test. Never run against a real SP-1. |
| Factory presets (`presets/`, `firmware/app/factory_presets.h`) | six presets, each runs through the real engine within budget. **[HOST]** |
| Build (`docs/BUILD.md`, `firmware/scripts`, `firmware/app/zephyr_min`) | Zephyr **v4.3.1** (Tape Looper's lock file pins this, not 4.1) + SDK 0.17.4 in `../toolchain`. Minimal heartbeat/watchdog/FN-power-off image builds: 17,316 B at 0x20000, bit-identical across 3 clean builds, `check_image.py` passes. **Verified on hardware (owner report, 2026-11-24, spare SP-1, flashed via Solderless): the image boots and track LED 1 blinks.** Not yet confirmed: steady blinking beyond the 5 s watchdog window, FN-hold power-off, behavior with USB plugged in. |
| Preset cycling (ADR-004) | FN + rocker, fade, flash + position bar, `PRESET_LOADED` event, C + companion. **[HOST]** Device-side user presets: **[TODO]** |
| Guide (`docs/guide/`) | printable quick-reference (schematic device drawing). |

## Test results (`make test`: 6 C suites; `node --test companion/tests/*.test.mjs`: 55; all pass)
- Also: `test_storage` 1,426 checks, `test_protocol` 903, `test_presets` 557, `test_controls` 172.
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
- 2026-11-24: `sp1fx-min-heartbeat.bin` (SHA-256 18afab48...592667) flashed with Solderless to the spare unit; LED 1 blinks. This confirms: bootloader accepts an app linked at 0x20000, our reset/vector layout is valid, LED pin P0.29 is right.
- 2026-11-24: `sp1fx-tone.bin` (SHA-256 87886d14...433f9): owner reports left channel low tone, right channel high tone, "sounds great". This confirms (owner-observed) the CS42L42/TAS2505 init, I2S slave clocking from the 3.072 MHz oscillator, and channel order. Not yet reported: which output was heard (speaker vs headphones), LEDs 2-4, serial status line, speaker mute on headphone insert, pop-free switching.
- Finding from the tone build: `nfct-pins-as-gpios` in the board dts has no effect in Zephyr 4.3.1 (it is only read from `&uicr`). Making it effective would write UICR.NFCPINS (outside our image) and reset the chip: do not change without an explicit decision. P0.09 (TAS2505 reset) works as a GPIO because this unit's UICR already allows it.
- 2026-11-24: `sp1fx-sync.bin` (SHA-256 12255e2c...3073d) flashed by the owner; serial log pasted (observed): modes cycle with FN taps, MIDI clock count rises 48/s at 120 BPM, PO pulses scheduled 4/s, 0 dropped bytes, HFXO on. Anomalies (from reading sync_hw.c, cause not proven): bit-timer ISR rate about 31,050/s vs 31,250 nominal (about 0.6% merged interrupts) and the `late` counter rising about 550/s; the reported max latency of 31 us is the measurement ceiling (timer clears at 32 us), so the true worst case is hidden. Possible fix if gear shows errors: hardware-generated edges (SPIM/PWM DMA) instead of ISR-written bits. **Real-gear test (EP-133 / EP-40 / PO / Volca) pending: owner will test later.**
- Pending owner tests: `sp1fx-controls.bin` (checklist in docs/BRINGUP_CONTROLS.md), sync with real gear. Pending build: USB audio images (agent running).
- 2026-11-24: USB audio images built (not yet flashed): `sp1fx-usbaudio-in.bin` (A) and `sp1fx-usbaudio-io.bin` (B). **Architecture finding:** the nRF52840 USB controller has exactly ONE isochronous IN and ONE isochronous OUT endpoint. Image A uses the IN endpoint for explicit feedback, so image B (return stream to the host) must use implicit feedback instead (no feedback endpoint): expected to work on macOS/Linux, **not on Windows' built-in driver**. Tape Looper's UAC2 patch (Windows feedback fix) is NOT applied because build_firmware.sh refuses a modified Zephyr checkout; Windows is expected to fail on image A (Code 10). Needs an owner decision. Engine pool 150 KiB; RAM 185-189 KB of 262 KB used.
- Pending owner tests: USB audio A then B (docs/BRINGUP_USBAUDIO.md). First real CPU-per-preset numbers will come from the A console.
