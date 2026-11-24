# AGENTS.md

Project: **SP-1 FX**, custom real-time multi-effects firmware for the Teenage Engineering / Kano SP-1 Stem Player
(nRF52840, Zephyr 4.3.1). Owner: Alex. Licence: MIT. Start with `README.md`, then `docs/STATUS.md` (the state document; read its top and
"Hardware log"). This file holds the rules and the context that is not in the code.

## Hard rules (from the owner)
- **Never flash, erase, or modify the bootloader / Bluetooth firmware / eMMC / UICR without explicit approval, every time.** The owner flashes
  by hand with https://solderless.engineering in Chrome (hold T1+T4 while connecting USB until T1 lights). Build scripts have no flash step.
  Approval for one flash is not approval for the next.
- **Never fabricate hardware results.** Label every claim: verified on hardware / source finding / simulated / untested assumption.
  Say "not run on hardware" for anything that has not been.
- **Hearing safety:** every boot starts at Headphones level (never apply Loud/Line out at boot). Test new level changes at low volume.
- Respect upstream licences, pin every upstream to a commit, record it in `THIRD_PARTY_NOTICES.md`. Do not copy `softmodded/marisko` (no licence).
  Do not redistribute the Tape Looper release image or any Bluetooth radio payload.
- LEDs are monochrome (8 single LEDs: 4 PLAY-side, 4 track). No colour or number encodings.
- Audio path: no allocation, logging or storage writes. Flash writes are chunked, deferred under load, and never in the audio thread.
- Protocol: `docs/PROTOCOL.md` is the spec; change the spec first if firmware and companion disagree.

## Hardware facts that shape everything
- **No analog input exists** (`docs/PHASE0_REPORT.md`). Routing is USB audio in (UAC2, 48 kHz) -> effects -> speaker/headphone jack. The sync jack is
  digital MIDI/PO output only. nRF52840 USB is device-only with one ISO IN and one ISO OUT endpoint.
- Bootloader 0x0-0x1FFFF is TE's; app at 0x20000; our settings journal at 0xFD000-0xFEFFF; the board's page at 0xFF000 is never touched.
  The bootloader starts the watchdog (timeout not ours to change, observed >= ~4.9 s); the app must keep feeding it.
- CPU budget is tight (64 MHz, ~1333 cycles/frame). With USB audio the heavy presets run ~70-90 % block time. Trade audio quality for CPU
  when needed; the owner explicitly accepts grit (especially dub/tape). Declared effect `cpu_cycles` are estimates, not measurements.

## Where things are
- `firmware/dsp/` engine + 10 effects; `firmware/controls/` gestures, soft takeover, LEDs, volume; `firmware/protocol/`, `firmware/storage/`.
- `firmware/app/zephyr_sp1fx/` is the integrated firmware (current). Older bring-up apps (min, tone, controls, sync, usbaudio, fxprof) stay for isolation tests.
- `companion/` Web Serial app (serve statically, Chrome only, e.g. `python3 -m http.server 8765 --bind 127.0.0.1` in `companion/`).
  A companion must match the firmware's STATE/METERS format (they are released together).
- `tests/` host C tests (`make test`) and `companion/tests/` (Node 22). `tools/fxprof_emu/` offline cycle estimator (needs unicorn/capstone/pyelftools).
- `tools/serial_log.py` read-only serial logger (see below). `docs/BRINGUP_SP1FX.md` owner test steps. `docs/research/` research notes.

## Set up on a new computer
1. Clone the repo. Node 22 for the companion, Python 3.13 for scripts, Chrome for Web Serial and the flasher.
2. `firmware/scripts/build_firmware.sh setup` builds the toolchain in `../toolchain` (venv, pinned west workspace, Zephyr SDK 0.17.4, SHA-256 checked;
   see `docs/BUILD.md`). `firmware/scripts/build_firmware.sh --app sp1fx build` builds and runs `check_image.py`; `... --app sp1fx repro` builds twice and compares.
3. `make clean && make && make test` must print 0 warnings and `ALL TEST BINARIES PASSED`; `node --test companion/tests/*.test.mjs` must pass.
4. **Flash images are not in git.** Rebuild them, or copy the owner's `flash-ready/` folder (it holds `SHA256SUMS`; verify with `shasum -a 256 -c`).
   Keep a recovery image: Tape Looper's own release (from its GitHub releases), flashed the same way.

## How we work with the hardware
- Read the device console with `python3 tools/serial_log.py <port> <logfile> <seconds>` (opens the port, raises DTR, never writes to the device).
  The port is exclusive in practice: **do not read it while the companion is connected** (the device then switches to binary framing and the
  reads steal data). Ask the owner to disconnect the companion first. Console diagnostics are off by default in rc4+ ('d' toggles them; event
  lines B/C/K/SAVE/OVERLOAD/STALL/CRASH are always on).
- Crash/stall evidence lives in retained RAM and is lost on power loss. After a reset, read the `CRASH:` / `STALL` lines first; resolve addresses with
  `arm-zephyr-eabi-addr2line` against the matching ELF (kept next to each image).
- The owner tests one image at a time and reports by chat. Do not call something a crash until the log shows a reset (an earlier "crash" was the FN
  long-hold power-off). Check the log before guessing.
- Power gestures: FN hold 4 s (8-LED fill) powers off and on; plugging in USB while off does not boot, it charges and shows a fuel gauge. FN+FWD/REV
  cycles presets; FN+T1-T4 edit mode; PLAY global bypass; VOL +-2 dB steps. See `docs/guide/` and `docs/BRINGUP_SP1FX.md`.

## Decisions already made (do not re-ask)
USB audio in/out; computer and phone hosts (Mac and iPhone verified); effect selection LEDs show no numbers; tempo and sync on/off from the companion only
(sync default OFF, saved); presets are factory-only on the device, named/user presets live in the companion; saves are event-based (preset switch,
mode exit, effect change, power off, companion SAVE_NOW, 120 s fallback); output levels Headphones / Loud / Line out with Headphones at every boot;
reuse Tape Looper's MIT drivers with attribution; one change at a time, staged images; subagents with small context for large tasks.

## Open items (see docs/STATUS.md)
Sync jack with real gear (PO/EP) untested; image B (USB audio return to host) untested; Windows needs a UAC2 patch not applied; USB threads use ~21 % CPU
for plain audio (unexplained); battery fuel gauge and charge pins are source-derived and uncalibrated; unexplained one-off T3/T4 blink on first fader touch;
history/email/path review before making the repo public (`THIRD_PARTY_NOTICES.md` end).
