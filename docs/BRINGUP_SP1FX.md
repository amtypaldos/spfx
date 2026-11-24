# SP-1 FX v1: integrated firmware bring-up (rc5 current)

## rc5 (2026-11-24) = rc4 + safer jack levels + feedback saturation -- NOT RUN ON HARDWARE

| | |
|---|---|
| File | `~/spfx/flash-ready/sp1fx-v1-rc5.bin` (ELF: `sp1fx-v1-rc5.elf`) |
| SHA-256 | `bfaa973eb2369d935b79d19ca02750296f270988166d5e83c4052dc3954810db` (in `SHA256SUMS`) |
| Size | 152,584 B at 0x20000, `check_image.py` PASS, two clean builds identical; companion from the same commit |

- **Headphone jack level, three steps** (companion: "Headphone jack level"):
  | Level | mixer | trim | over Headphones | jack at 0 dBFS, master 100 % (CALCULATED) |
  |---|---|---|---|---|
  | Headphones (normal) | -30 dB | -6 dB | 0 dB | ~16 mVrms |
  | Loud | -15 dB | -6 dB | +15 dB | ~91 mVrms |
  | Line out (speakers only) | 0 dB | 0 dB | +36 dB | ~1.0 Vrms (+2 dBu) |
  (From Cirrus' 35 mW / 30 ohm = ~1.02 Vrms full scale; not measured.)
- **Every power-up starts at Headphones**, whatever was used before.
- Going louder asks for confirmation in the companion and lowers the master by the
  difference (15 or 36 dB) so nothing jumps. Going quieter is always allowed and keeps the
  master.
- **Plugging something into the jack while at Loud or Line out returns to Headphones** (the
  refused LED pattern flashes, console `C ... jack plugged`). It uses the codec's tip-sense
  detection, already read every 250 ms. A speaker cable counts too: plug it in first, then
  choose Line out.
- **Delay, chorus and flanger feedback saturate softly** above full scale (linear below,
  rounding off towards 1.9, under the delay lines' +-2.0 limit) instead of hard clipping with
  hot input and high feedback. The reverb has no clamp in its loop (it always decays) and tape
  wobble has no feedback; both unchanged.
- rc5 test: rc4 steps 1-4. Then: Loud, then Line out with a powered speaker, starting low (VOL+
  from where it lands); unplug/replug the jack: it must come back at Headphones. Turn delay
  feedback to max with a loud input: it should get thick and saturated, not crackle.

---

# rc4 (superseded by rc5)

## rc4 (2026-11-24) -- NOT RUN ON HARDWARE

| | |
|---|---|
| File | `~/spfx/flash-ready/sp1fx-v1-rc4.bin` (ELF: `sp1fx-v1-rc4.elf`) |
| SHA-256 | `ae95d5caef17b36613dce3104ee03047a6ac381dcb0b4d5d79fd44511365ffbe` (in `SHA256SUMS`) |
| Size | 151,504 B at 0x20000, `check_image.py` PASS (incl. `--reserve`), two clean builds identical |
| Companion | use the companion from this commit (STATE has a new trailing byte, see PROTOCOL.md) |

### 1. Stability under load (rc3 near-misses: control loop starved 2-4.9 s at ~99 % CPU)
- The overload guard judges the **whole block time again** (engine + every interrupt and USB
  thread that pre-empted it = total load), with rc3's two-window safety net and ~10 s recovery.
  rc3 judged engine cycles only (81 %) while the total was 97-99 %.
- **Starvation net:** if the control loop has not run for 0.5 s and the CPU has < 5 % idle, the
  audio thread switches one effect off at once (`OVERLOAD`/blinking LED; it comes back after
  calm).
- **Saves never hold the control loop:** a record is programmed 32 bytes per 8 ms tick (about
  13 ticks); no save starts while the total load is high (`load_defer=`).
- **Diagnostics are off by default** (the 2 s block itself took > 1 s of the starved control
  thread). In `screen`: press **`d`** to switch the periodic block on/off, **`i`** for one block
  now. It prints one line per tick. Event lines (`B`, `C`, `K`, `SAVE`, `OVERLOAD`, `STALL`,
  `CRASH`) stay on. The `D` line now names the effects (`fx=Sat,Bitc,Comp,Rev`, `(b)` =
  bypassed); `thr%o` gives every thread's share (per mille) over the last 0.5 s.
- About "control loop stopped for 4865 ms (watchdog at ~4000)": the 4000 ms is only what our
  code requests; the nRF52 watchdog cannot be reconfigured once the TE bootloader has started
  it, so the real timeout is the bootloader's (at least ~4.9 s, observed). Source finding.

### 2. Saving is event-based (owner decision)
Saved: at FN-hold power off (at once), ~1.5 s after a preset switch / leaving edit mode / an
effect change (a burst of rocker presses is saved once), on the companion's SAVE_NOW and its
setting changes (effect, order, BPM, sync, output mode, factory reset). **Fader moves, VOL,
bypass taps** and the companion's wet/param/master/gain only go to RAM and are saved with the next
event, at power off, or after 120 s with unsaved changes and low load. Nothing at boot.
**Trade-off: those tweaks are lost if the battery dies or the unit resets before the next save.**
Volume is saved at power off like the other tweaks. `store` line: `events= tweaks= load_defer=
job_ms_max=`.

### 3. Headphone jack: Headphones / Line out (companion: "Line out (headphone jack)")
| Mode | CS42L42 mixer | digital trim | max into the codec | jack level at 0 dBFS, master 100 % (CALCULATED) |
|---|---|---|---|---|
| Headphones (default) | -30 dB | -6 dB | -6 dBFS | about 16 mVrms (-36 dB re ~1 Vrms) |
| Line out | 0 dB | 0 dB | 0 dBFS | about 1.0 Vrms (+2 dBu, consumer line level) |
Basis (source, not measured): mixer 0..-63 dB in 1 dB steps (Linux cs42l42 driver TLV); 0 dB is
Tape Looper's shipping setting; Cirrus product page "2 x 35 mW into 30 ohm" = sqrt(0.035 x 30)
= 1.02 Vrms at full scale. The datasheet's full-scale table could not be read here; the meaning
of HP_CTL bit 1 ("full scale volume") is not used (left as before). Into a 10 kohm line input the
level should be about the same (voltage output); not measured. The engine's limiter keeps the
signal at or below full scale, so line out cannot drive the codec past 0 dBFS.
- Switching to line out first **lowers the master by 36 dB** (so the level does not jump), then
  fades out, changes the codec, fades in. Raise it with VOL+ (2 dB per press). Back to headphones
  keeps the master (36 dB quieter).
- Saved; restored at power-up (`CONFIG_SP1FX_LINEOUT_RESTORE=y`). First start, factory reset
  and SAFE BOOT are always headphones. The speaker is never affected.
- **Owner measurement (do this before using line out live):** headphones OFF; connect the
  powered speaker (or a meter / audio interface input) to the headphone jack with the speaker's
  volume low. Companion: switch to Line out (confirm the warning). Play a sine or music at full
  scale from the Mac; press VOL+ step by step from where it starts and note the master dB
  (`C ... VOLUME ... = N dB` line) at which the speaker is loud enough, and whether anything
  distorts at master 0 dB. If you have a meter: report the Vrms at master 0 dB with a 0 dBFS
  1 kHz sine (expected ~1 Vrms, calculated).

### 4. Preset switch LEDs (owner request)
The four PLAY-side LEDs flash rapidly together (7 flashes at 10 Hz, 0.7 s), then the position
bar for 1.4 s. The refused pattern (alternating pairs) is unchanged and different.

### rc4 test (in this order)
1. As rc3 steps 1-5 (power on/off, controls, charging).
2. Heavy preset or saved state, music on, companion connected: no STALL lines; if the CPU is
   too high you should see an OVERLOAD line and a blinking slot LED instead of a stall.
   Press `d` (companion closed) and report `D` + `thr%o` lines per preset.
3. Presets: FN + FWD several times quickly: rapid flashing, bar; one `SAVE (event)` ~1.5 s
   after the last press.
4. Move faders, wait 10 s: no SAVE line. Power off (FN 4 s) and on: the fader state is back.
5. Line out: the measurement above.

---

# rc3 (superseded by rc4)

## rc3 (2026-11-24) -- NOT RUN ON HARDWARE

| | |
|---|---|
| File | `~/spfx/flash-ready/sp1fx-v1-rc3.bin` (ELF: `sp1fx-v1-rc3.elf`) |
| SHA-256 | `ec36e3befc937ab229822cc036d2e7788865134261af7d61a5631c06f3fa7199` (in `SHA256SUMS`) |
| Size | 149,612 B at 0x20000, `check_image.py` PASS (incl. `--reserve`), two clean builds identical |

(An interim rc3 with a 3 s hold, 57cbe11b..., was built but never handed over; it is replaced.)

### 1. Controls no longer die (rc2 bug)
rc2 read the SAADC asynchronously through Zephyr's driver; one scan per boot never finished
(`adc_to=1`) and rc2 then waited for it forever: no control events for the rest of the run.
rc3 drops Zephyr's ADC driver and scans the SAADC itself at register level
(`firmware/drivers/saadc_scan.c`): no interrupts, never waits; a scan is started in one tick and
collected in the next, and a scan that has not ended after 50 ms is stopped and restarted
(host-tested state machine). At start it stops whatever the bootloader left running in the
SAADC, clears its events/interrupts and disconnects any PPI channel aimed at a SAADC task
(`ppi_cleared=` / `saadc_was_on=` in the `store` line tell whether there was anything).
Same channel setup as the verified controls image. **Why a scan was lost is not proven.**

New on the console: a `B <ms> buttons=...` line for every change of the decoded buttons, and
every 2 s an extra `  ctl ...` line: loop `ticks=`, good `scans=`, `scan_age_ms=` (should stay
< 20), raw ladder codes `lad_raw=` (idle < 110 / < 200), raw faders, the button mask, the
debounced mask, and the FN state (`gpio armed down consumed long`), mode, held slot. If the
controls ever die again, copy those lines.

### 2. Power off / power on: FUNCTION held 4 s, 8-LED fill
- **Off:** hold FUNCTION **alone**. Nothing shows for the first 0.5 s (a tap or FN + rocker
  never flashes it). Then one more LED every 0.5 s: the four PLAY-side LEDs left to right, then
  T1, T2, T3, T4; lit LEDs stay lit. At 4.0 s the eighth lights, all eight stay on for 0.3 s and
  the device powers off. Release before that, or press any other button during the same
  FUNCTION press (rocker, channel, PLAY, VOL): no power off, the normal display returns.
- **On:** the same 4 s hold with the same fill. Releasing earlier puts the unit back to sleep
  (on battery). Implemented in the app after the TE bootloader hands over (nothing outside the
  app slot is touched), so the 4 s count starts when the app starts: the bootloader's own
  start-up time comes on top (not measured).
- **USB plugged into a unit that is off:** it does **not** start. It charges and shows the charge
  state on the first PLAY-side LED: a slow gentle pulse while charging, steady dim when the
  charger reports done. Hold FUNCTION 4 s to switch on. Unplugging puts it back to System OFF.
- **Powering off while plugged in** goes to that same charge display (a soft reset into the
  standby, as Tape Looper does; System OFF with USB already present would have no wake edge).
- **Never blocked:** watchdog or crash recovery, a power-on/battery/fresh-flash start with no
  wake reason, a reset-pin or other soft reset start at once. (After a flash the bootloader's
  reset reason decides; if it lands in the charge display, hold FUNCTION 4 s.)
- **While running and charging:** the 4th PLAY-side LED blips dimly every 4 s when nothing else
  uses it. `info` line: `vbus= pgood= charging= boot=`; companion: "USB power"/"charging".

### 3. Charging (BQ24232) -- what the hardware offers (source findings, nothing verified)
- `/CE` P0.21 (drive low = charging enabled), `/CHG` P0.22 (low = charging), `/PGOOD` P0.24 (low
  = USB power good), `ISET` P1.00 (charge-current monitor, not used), battery divider on AIN4
  P0.28 (SP-1-dev wiki "Battery charger"; Tape Looper `charger_init()`, MIT).
- The charger charges by itself while `/CE` is low; rc3 drives it low at every start (earlier
  sp1fx images never touched it). GPIO output levels are retained in System OFF (nRF52840),
  so charging continues while off. The charge status cannot be read in System OFF (CPU off):
  that is why "off with USB" is a standby loop with the CPU idle, not System OFF.
- Battery voltage: `batt_raw=` in the `store` line (raw 12-bit, uncalibrated; Tape Looper's
  interim anchors: ~2380 = full at rest, ~1900 = empty estimate).

### 4. Overload guard (rc2: `shed=0xb` and effects silent until a preset change)
- The guard now judges the **engine's own cycles** (what shedding can reduce), sustained limit
  85 % of the block period as before, and the safety net needs **two** windows over 9/8 in a row.
  rc2 judged whole-block time, which on the owner's unit averaged ~25 % above the engine
  (USB/other threads 21 %), so USB or flash activity bursts (a 114 % window) shed effects.
  Blocks during a flash write are left out.
- **Auto-recovery:** after ~10 s of calm (window means under 80 % of the limit) one shed slot
  comes back (the cheapest first); if it has to be shed again, the next wait doubles (max x16).
- **Visible:** a shed slot's track LED blinks slowly (1 Hz, different from the 0.5 s takeover
  cue); all of them blink when the guard forced bypass. The companion shows "overload guard:
  slot N off" next to the CPU figure (METERS flags, docs/PROTOCOL.md).

### rc3 test (in this order)
1. Off -> hold FN 4 s: fill, unit starts. Off -> tap FN / hold 2 s: stays off.
2. Running: controls (faders, T taps, FN + rocker presets, VOL) for 3 min. Every action must
   produce `B`/`C`/`K` lines; the `  ctl` line must show `scans=` rising and `scan_age_ms` small.
3. Hold FN alone: fill, power off at 4 s. Hold FN 2 s then press the rocker, keep FN 4 s more:
   no power off.
4. Off + plug USB: no start, LED pulse (charging) or steady (full). Unplug: dark.
5. Running + plugged: FN hold 4 s: goes to the charge display, not dark.
6. Load the heavy saved state, play music with the companion connected: report `D` lines
   (`win_max_period=`, `shed=`); if a slot is shed its LED blinks and it comes back ~10 s later.

---

# rc2 (superseded by rc3)

## rc2 (2026-11-24): fixes for the rc1 watchdog resets -- NOT RUN ON HARDWARE

| | |
|---|---|
| File | `~/spfx/flash-ready/sp1fx-v1-rc2.bin` (ELF for addr2line: `sp1fx-v1-rc2.elf`, same folder) |
| SHA-256 | `3ccf3c8ed9fee5de1276e1f26034cd33dd52f893120aa0c6eee27654a2a8f250` (in `SHA256SUMS`) |
| Size | 152,636 B at 0x20000, `check_image.py` PASS (incl. `--reserve`), two clean builds identical |

**rc1 on hardware (owner):** watchdog resets (rr=2) every ~6-20 s, with or without the
companion, often right after trying effects or presets. At the watchdog moment the running
thread was `idle` (pc = WFI in `cpu_idle`) or a USB thread in its normal wait (`usbd`
in `k_event_wait`, `udc_nrfx` in `z_pend_curr`): every thread was blocked, so the control
thread (the only watchdog feeder) was waiting for something that never happened. Settings
were written on almost every boot (seq rising), 127-195 ms per record.

**What rc2 changes (each one small, and each aimed at a specific finding):**
1. **No unbounded wait on the control thread.** rc1's only `K_FOREVER` wait there was the
   SAADC read (`adc_read`, Zephyr `adc_context` waits forever for the conversion-done
   interrupt), twice every 8 ms. rc2 starts a scan and collects it on the next tick
   (`adc_read_async`); a scan that does not finish in 100 ms is counted (`adc_to=`) and the
   controls freeze, but audio, USB and the watchdog keep running. *Leading hypothesis, not
   proven.*
2. **No settings write at boot, at most one per 30 s, and only if something changed.** In rc1
   every fader pickup counted as a change, so each boot saved ~1.5 s after start; fixed in
   the controller (a pickup at the stored value is not a change). Saves now skip when the
   record would be identical (`dedup=`). SAVE_NOW and FN-hold power off still save at once.
3. **No flash page erase while the USB cable is in** (an erase stalls the CPU and every USB/I2S
   interrupt for ~85 ms; rc1 was the first image to do that). Erases happen on battery when
   quiet, or at power off. SAVE_NOW answers "busy" if it would need an erase (the companion
   shows "the device is busy, try again"); the change is saved at power off.
4. **Crash-loop breaker.** If the last two boots both ended in a crash within 60 s of starting,
   the device boots with factory defaults, does not load or write the saved settings, and
   says `SAFE BOOT` in the `I` banner. A run that lasts 60 s resets the counter (next boot
   loads the settings again). The counter lives in RAM: a full power loss resets it.
5. **Pinning the next hang.** A stall watcher in the audio thread (clocked by I2S, so it works
   even if the kernel timer were the problem) writes up to two `STALL[n]` lines into
   retained RAM when the control loop has not run for 1.5 s and 3.5 s; the watchdog capture
   adds `crumbs=` to the CRASH line. Printed after the next boot, in text mode.
6. Console TX queue: the host-tested module (stalled host, separator, connect burst) is now
   the one in the image; RX parsing is bounded to 512 B per tick.

**rc2 test (do this before the stages below):**
1. Flash rc2. Open the console (`screen`), companion closed. Report the `I` banner.
2. Play music; do what crashed rc1: switch presets with FN+FWD/REV and cycle effects
   (hold T2 + FWD) for 2-3 minutes. Then connect the companion and do the same for 2 minutes.
3. If it resets: open `screen` again (companion closed) and copy **every** `CRASH` and
   `STALL[...]` line plus the next `info`/`store` lines. If it does not reset but a
   `STALL near miss` line appears, copy that.
4. Either way report from the `info`/`store` lines: `stalls= stall_max_ms= adc_to=
   adc_max_ms= saves= dedup= save_busy= safe= vbus=`.

How to read the lines (for the lead):
- `CRASH: ... crumbs=stage:S loop_ms:L stage_ms:M adc:0xA`: S = loop stage (1 top, 2 console,
  3 adc, 4 controller, 5 proto_rx, 6 proto_tick, 7 store, 8 leds, 9 hp_i2c, 10 diag, 11 sleep,
  12 flash_w, 13 flash_e, 14 wait_out, 15 poweroff); `up - L` = how long the loop had not run;
  `up - M` = time in that stage; adc bit1 = a scan never finished, bits 8+ = timeouts.
- `STALL[n] prev run: up= loop_age= stage=NAME(ms) main:off st=0x.. pc= lr= bt=... cur=...
  cpu%o: <thread>=permille ... idle= sof/s= blk/s= usbf/s= con= txq= ... since_flash= vbus=`.
  `st` is the control thread's scheduler state (0x02 = pending on a kernel object, 0x04 =
  sleeping, 0x80 = ready but not running). `pc/lr/bt` are code addresses in the control
  thread's stack: `arm-zephyr-eabi-addr2line -f -i -e sp1fx-v1-rc2.elf <addr>`. The thread
  shares tell starvation (a USB thread near 1000) from blocking (idle near 1000); `sof/s`
  ~1000 and `blk/s` ~750 mean USB and audio were still alive.

---

# rc1 (superseded by rc2; kept for reference)

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
| `POWER OFF: ...` | FN held alone 4 s (rc3; 1.5 s in rc1/rc2) |

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
5. Hold FUNCTION 4 s (rc3; 8-LED fill): `POWER OFF` line, LEDs off. Press FUNCTION: the bootloader starts the
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
   2 presses down), wait 5 s, then FN hold 4 s (power off) and FUNCTION to power on.
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
