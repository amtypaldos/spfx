# SP-1 FX — Phase 0 Report

Status: **FEASIBILITY GATE NOT PASSED for analog input.** No firmware has been written and no device has been touched.

Evidence labels used throughout:
- **[SRC]** read in upstream source/docs (not independently verified on hardware by us)
- **[CALC]** our own arithmetic/estimate
- **[UNVERIFIED]** assumption; needs a physical test or datasheet

Upstream clones live in `../upstream/` (outside this repo), pinned in `THIRD_PARTY_NOTICES.md`.

---

## 1. Hardware capability matrix

| Item | Finding | Evidence |
|---|---|---|
| MCU | nRF52840 (QIAA), Cortex-M4F 64 MHz, 256 KB RAM, 1 MB flash | [SRC] SP-1-dev wiki Hardware-overview |
| Headphone codec | Cirrus **CS42L42**, I2C 0x48, reset P0.15; genuine chip ID 0x42 read by Tape Looper | [SRC] tape-looper main.c ("verified on hardware 2026-06-05" is the upstream author's claim) |
| Speaker amp/DAC | TI **TAS2505**, I2C 0x18, reset P0.09 | [SRC] |
| Audio bus | I2S 48 kHz, 24-bit stereo. A 3.072 MHz oscillator (enable P0.13) drives SCK; **CS42L42 is the LRCK master**; nRF and TAS2505 are slaves | [SRC] wiki + tape-looper board pinctrl |
| nRF I2S pins | SCK P0.12 (in), LRCK P0.11 (in), **SDOUT P1.09 only. No SDIN pin exists in any upstream pin map** | [SRC] stemplayer_pins.h, stem_player-pinctrl.dtsi |
| **CS42L42 → MCU data path** | **"There is no audio data path from CS42L42 back to the MCU (ASP_SDOUT is unconnected)"** even though HSIN± look set up for mic use | [SRC] SP-1-dev wiki I2C.md |
| Bluetooth | Infineon CYBT-353027-02 (Classic+BLE) on UART P1.01–P1.04 + SPI CSN P1.05 + reset P0.10; also receives audio via I2S. nRF has no antenna | [SRC] |
| BT audio input | Tape Looper 4.0 receives A2DP; SBC decode runs on the nRF; radio firmware uses Infineon SDK payloads that are **not in the public source** | [SRC] BLUETOOTH.md, BUILDING.md |
| USB | nRF52840 USB, **device mode only** (hardware limit of this MCU). CDC-ACM and UAC2 speaker (48 kHz/16-bit stereo, FS, explicit feedback) both implemented in Tape Looper | [SRC] stem_player.dts; device-only is [CALC] from nRF52840 datasheet knowledge |
| 2nd 3.5 mm jack ("SYNC") | **Digital sync OUTPUT, not audio.** TIP = PO sync pulses driven from P0.20 (+P0.17 paralleled); RING = MIDI via P0.23 → BC807 PNP transistor (inverting). Tape Looper only drives these pins as outputs (MIDI clock/Start/Stop, PO 2-PPQN pulse). No receive circuit (opto/UART RX) is documented. Neither pin is an nRF52840 analog input (AIN0–7 are P0.02–05, P0.28–31), so it cannot be sampled as audio. | [SRC] tape-looper main.c ~L12269 (cites TimK's sync-jack schematic); wiki Peripherals. Upstream notes "untested on real gear" for MIDI out |
| Storage | 4 GB Kioxia eMMC, 1-bit mode (CLK P0.06, DAT0 P0.07, CMD P0.08, RST P1.08, VCCQ P0.14); Tape Looper drives it over SPIM3 at 32 MHz | [SRC] |
| Controls | 4 faders on AIN3/6/2/7 (P0.05/30/04/31). Play + T1–T4 on resistor ladder 1 (AIN0). Vol+/−, FWD/RWD (rocker) on ladder 2 (AIN1). Function = direct GPIO P0.27. Ladder/fader supply switched by P1.10. Ladder decode thresholds are calibrated raw ADC values | [SRC] |
| LEDs | 8 total, **monochrome**, plain GPIO (not PWM in Tape Looper). Playback LEDs P1.13, P0.00, P1.12, P0.01. Track LEDs P0.29, P0.26, P1.15, P1.14. Each is on/off GPIO; brightness needs software PWM/time-multiplexing or the hardware PWM peripheral (marisko uses PWM) | [SRC] |
| Rocker | Two buttons (FWD/RWD) on a ladder. Treated as two momentary buttons. Rocker repeat must be done in software | [SRC] |
| Charger | BQ24232, 500 mA max from USB; battery sense AIN4/P0.28 | [SRC] |
| Flash layout | Bootloader 0x00000–0x1FFFF (TE, do not touch). App slot 0x20000, max 0xDF000. Storage page 0xFF000 (4 KB) | [SRC] Tape Looper dts |
| Watchdog | Bootloader arms WDT; app **must feed it (<5 s)** or the device boot-loops. SYSTEM_OFF returns to bootloader. No hardware reset pin | [SRC] |
| Free GPIO | Unmapped in all pin maps: P0.16–P0.20, P0.23, P0.25, P1.06 (some may be SWD/reset/unrouted) | [CALC] from pin list |
| Debug (SWD) | Not confirmed accessible. Wiki warns to test on a device with a debugger first | [UNVERIFIED] |

Not yet verified by us on hardware: everything in the table. We have not touched a device.

---

## 2. Upstream firmware comparison

| Project | License | What it is | Fit |
|---|---|---|---|
| **chattock/sp1-tape-looper** @ `0359372e` | MIT (+Zephyr Apache-2.0, nrfx/Nordic, CMSIS, FatFs, dr_libs, SBC decoder, Wakes-derived storage) | Most capable: Zephyr 4.1 board def, bare-metal CS42L42/TAS2505/BQ24232/eMMC drivers, **UAC2 speaker input**, BT audio input, CDC console, I2S output with real-time mixing/effects (reverb, chorus, flanger, filter, comp, etc.), pickup faders, bootloader-safe lifecycle, USB-live monitor path, button/gesture handling, recovery gestures | **Best foundation for hardware drivers.** main.c is ~14k lines monolith; extract, don't fork wholesale. BT radio payloads not redistributable |
| **AWD-tech/bonsai-8** @ `ef794f10` | MIT (© chattock; derived from tape-looper pinned `44ba1ecb`) | Dual-deck stem mixer on the same base. Its own hardware log reports USB audio + mirroring caused deck underruns; later candidate removed all effects | Useful as evidence about real underrun behaviour and as a second consumer of the same drivers. Not a better base |
| **jackiscool123123121/marisko** @ `315d5046` | MIT | Small, readable Zephyr app split into `codec.c audio.c leds.c pwm.c saadc.c emmc.c usb.c battery.c` — easiest code to read for each driver. README says WIP, "hold off flashing production hardware" | Good **reference** for clean driver separation; not the base (WIP, less hardware-proven) |
| **softmodded/marisko** @ `b787d906` | **No license file** (GitHub reports none → all rights reserved) | Original marisko, very early (LED animation, WDT feed) | **Do not copy code.** Reference only |
| **jackiscool123123121/rome** @ `40023f90` | MIT | Rust CLI: flash via bootloader UART, eMMC format, stem upload, codec diagnostics | Possible flashing helper; not needed for FX |
| **timknapen/SP-1-dev** (+wiki `d661d3f1`) | MIT | The pin map and hardware docs everything else cites | Primary hardware reference |
| **ARM-software/CMSIS-DSP** @ `9f64ba31` | Apache-2.0 | M4F-optimised biquads, FIR, FFT, vector math | Use selectively (biquad, vector ops) |
| Solderless (solderless.engineering) | Not inspected (web tool) | Browser Web Serial flasher for the TE bootloader | Documented flash path; license/terms **not reviewed** |
| ericlewis/sp1-midi, other forks | Referenced by Tape Looper's board file (MIT) but **not inspected** | USB-MIDI related | Phase 0 follow-up |

Recommendation: **Zephyr 4.1 + Tape Looper's board definition and driver code (MIT) as the foundation.** Pin exact commits (done below). The Bluetooth radio is a black box we should not depend on.

---

## 3. Analog audio routing feasibility report

**Question:** can the SP-1 take stereo analog in from an EP-series device while outputting processed stereo?

**Answer: No, not without hardware modification.** Three independent findings agree:

1. **No data path from codec to MCU.** The only chip on the headphone jack that has an ADC (CS42L42) has its serial-audio-out (ASP_SDOUT) unconnected on the PCB. [SRC, SP-1-dev wiki]
2. **The nRF side has no SDIN configured** in any upstream pin map, and every nRF I2S example sets `sdin_pin = NOT_USED`. [SRC]
3. **No alternative analog capture exists.** SAADC inputs AIN0–AIN4, AIN6, AIN7 are all consumed by buttons/faders/battery; AIN5 is the track-1 LED pin. 48 kHz stereo audio through SAADC is also not credible with those pins. [SRC/CALC]

4. **The second jack is not an audio input.** It is the SYNC jack (PO sync tip, MIDI ring), driven by nRF GPIOs as outputs through a transistor. It cannot carry audio into the MCU. A bare GPIO could at most read a 1-bit digital signal (e.g. incoming MIDI/clock, if the board's input stage allows it, which is unknown), and that is relevant only to tempo sync. [SRC; the schematic itself was not seen]

Also: even in mic mode the CS42L42 headset path is mono mic input; stereo line-in would not be supported by a TRRS jack. [UNVERIFIED against datasheet]

Physical rework (soldering a wire to a CS42L42 ball and a spare nRF pin) is **not recommended**: the CS42L42 is a wafer-scale package, the board has not been verified for a free routed pin, and the work would be irreversible on devices the user owns. It also would not have been tested. Not pursued.

### Alternatives that exist today

| # | Route | Latency (estimate) | Works with EP gear? | Verdict |
|---|---|---|---|---|
| A | **USB Audio in (UAC2)** from a host (computer/phone/tablet). SP-1 is a USB *device*. Output on SP-1 headphone/speaker. Tape Looper already implements this input and a live monitor | I2S block can be 1–2 ms; USB FS = 1 ms frames; **host OS audio buffering usually dominates (5–20+ ms)**. <10 ms end-to-end plausible only with a good host/driver; <5 ms unlikely [CALC] | EP → host (USB audio or analog interface) → SP-1. Needs a computer in the loop. EP-series USB audio behaviour [UNVERIFIED] | **Most practical way to get real-time FX.** Not standalone |
| B | **Bluetooth A2DP in** | Typically 100–250 ms | Phone/computer sources only | Unsuitable for live monitoring (as the brief predicted). Also depends on non-redistributable radio payloads |
| C | EP USB straight into SP-1 USB | n/a | **Impossible.** Both are USB devices; nRF52840 cannot act as USB host | Rejected |
| D | External bridge board (USB host + audio interface) feeding SP-1 USB | Adds a hop | Needs a host-capable board in the rig | Pointless versus A unless the bridge is the product |
| E | Hardware mod (ASP_SDOUT → nRF SDIN) | Best (~2–3 ms) | Analog in works | Irreversible, unverified, risky. Not recommended |
| F | **Use the SP-1 as a controller only** (4 faders, buttons, LEDs → USB CDC/MIDI to a host app running the FX) | Host-defined | Any | Changes the product: no on-device audio |
| G | **Standalone effects on audio the SP-1 already has**: processing of internal/eMMC playback or USB-fed stems | 1–3 ms | Not EP input | Different product (stem player + FX); explicitly outside the brief |

---

## 4. Recommended firmware architecture (conditional on the decision in §8)

If route **A (USB audio in)** is chosen:

- **Base:** Zephyr 4.1, Tape Looper `stem_player` board + extracted drivers (codecs, I2S, ADC ladders, LEDs, WDT/bootloader lifecycle, UAC2 + CDC).
- **Do not use:** eMMC streaming, BT radio, looper engine.
- **Audio path:** UAC2 OUT endpoint → ring buffer (with feedback-based clock matching, as upstream) → block processor (32–64 frames) → I2S DMA double buffer → CS42L42/TAS2505. All DSP runs in the audio thread at highest priority; control thread (~8 ms) posts parameter changes via lock-free queue; smoothing in the audio thread.
- **Host-native core:** `dsp/` is plain C99 with no Zephyr headers, built into both the firmware and a CLI simulator.
- **Control:** gesture state machine and soft-takeover in pure C modules, unit-tested on the host.
- **Companion protocol:** Web Serial over the existing CDC-ACM (already working upstream), framed binary or CBOR with version byte, COBS framing, rate-limited meters (~20 Hz). WebUSB is unnecessary.
- **Persistence:** reserved 4 KB internal flash page `0xFF000` is already claimed by upstream storage; presets are small (<2 KB each). Use a 2-page A/B journal with CRC, written from the control thread only. **Do not touch eMMC** (user's existing data and Tape Looper library live there).
- **Latency measurement:** loopback with a tone/click through the actual USB → I2S → codec path measured with an external recorder. Not done.

## 5. Initial DSP CPU and memory budget **[CALC — to be replaced by benchmarks]**

CPU: 64 MHz / 48 kHz = **1333 cycles per stereo frame**. Reserve ~35% for USB, I2S DMA, Zephyr, ADC scanning, LEDs, serial → **~850 cycles/frame for 4 effects, ~210 each average**. Single-precision FPU helps, but transcendental functions per sample are out; use LUT/recursive oscillators and block-rate coefficient updates.

| Effect | Rough cost (cyc/frame) | RAM | Note |
|---|---|---|---|
| Filter (SVF/biquad) | 40–70 | <0.1 KB | cheap |
| Saturation | 30–60 | <0.1 KB | polynomial/tanh approx |
| Tremolo | 20–40 | <0.1 KB | |
| Bitcrusher | 20–40 | <0.1 KB | |
| Compressor | 60–100 | <0.1 KB | |
| Chorus / Flanger | 90–150 | 6–16 KB | interpolated short delay, 16-bit or float |
| Tape wobble | 90–150 | 8–16 KB | |
| Delay | 40–80 | 24–96 KB | 16-bit stereo, 0.25–1 s at 48 kHz = 48–192 KB; **realistically ≤0.5 s** |
| Reverb (FDN/Dattorro-lite) | 250–450 | 24–64 KB | the dominant cost; two reverbs likely won't fit |

RAM: 256 KB total. Zephyr + USB (CDC+UAC2) + stacks + buffers likely **50–80 KB** [UNVERIFIED], leaving **~150–190 KB** for DSP. A delay + reverb + chorus fits; two delays + reverb is tight; two reverbs is unlikely. Admission control must therefore use declared (and later measured) cycles and bytes.

Block size: 32–64 frames (0.7–1.3 ms) is the design target; upstream uses 256 (5.3 ms), which is too long for this brief.

## 6. Firmware recovery strategy

Facts [SRC]: TE bootloader at 0x0–0x1FFFF resides below our app slot and is not rewritten by app-only images. Entering the bootloader: Tape Looper docs say turn off, hold **T1+T4** while connecting USB, release when T1 lights. Marisko docs: SYSTEM_OFF returns to the bootloader; press any button or plug USB. Flashing via Solderless (Web Serial, Chrome/Edge) or `rome flash`. No hardware reset pin; WDT must be fed.

Plan:
1. **Never** write below 0x20000, never write the BT module, never touch eMMC or `0xFF000` during early bring-up.
2. Build app-only images linked at 0x20000 with the exact upstream board file; verify link addresses and size ≤0xDF000 in CI.
3. First build ships the WDT-feed and a guaranteed **FN-hold → SYSTEM_OFF → bootloader** path before anything else, copied from the upstream pattern.
4. Recovery = re-enter bootloader (T1+T4 + USB) and flash the last known-good image (keep Tape Looper `sp1_looper.bin` and Marisko `marisko.bin` copies, with SHA-256).
5. **Open item:** no backup of the current device state has been taken. Before any flash, the user should save their current firmware images, and we should decide whether the bootloader's flash-read is supported (not verified). eMMC contents are not at risk from app-only flashing, but this is unverified.
6. Test first on one device only; keep the second as the control.

Irreversible operations: none planned. Anything that writes outside 0x20000–0xFEFFF, or modifies BT firmware, requires explicit approval.

## 7. Detailed implementation plan (assuming route A)

| Phase | Deliverables | Gate |
|---|---|---|
| 0b | Inspect sp1-midi + Solderless terms; confirm EP-133/EP-40 USB audio class behaviour; back up current firmware | User approves route |
| 1a (host only) | `dsp/` core + CLI simulator (WAV in/out, 4-slot chain), regression tests (silence, impulse, NaN, bounds, stereo isolation) | Tests pass on host |
| 1b (host only) | Control logic: gesture state machine, soft takeover, LED encoder, preset schema + serializer, protocol parser, resource admission | Unit tests pass |
| 1c | Zephyr build environment reproducible (SDK 0.17.4, pinned modules from upstream lock file), builds an app-only passthrough image. **No flashing** | Reproducible build + size/linking checks |
| 1d | **Ask approval** → flash one device: passthrough via USB audio, measure latency, CPU, underruns, 1 h soak | User-approved hardware session |
| 2–4 | Filter → 4-slot engine → remaining effects with measured benchmarks | per brief |
| 5 | Web Serial companion | |
| 6 | Presets, docs, quick-reference card, release | |

Phases 1a/1b are independent of the transport decision and could proceed now.

## 8. Decision needed

The preferred analog-input architecture is not supported by this hardware. Choose how to proceed (see the question asked in the session). Until then no firmware work that depends on the input path will be started.

## Known gaps in this report

- No hardware was touched; all hardware facts are from upstream docs/code.
- The CS42L42 and TAS2505 datasheets were not read directly. The WLCSP claim for CS42L42 is from memory and **unverified**.
- EP-40 Riddim / EP-133 KO II USB-audio and MIDI behaviour not checked.
- Solderless and sp1-midi licenses/terms not reviewed.
- RAM/CPU numbers are estimates until measured.
