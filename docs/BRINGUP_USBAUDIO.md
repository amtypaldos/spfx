# USB audio bring-up images: computer audio through the FX engine

Status: **built, never run on hardware.** Everything below about behaviour is
*expected*, not observed. Only the tone image (`zephyr_tone`) has run on an SP-1.

| Image | File in `~/spfx/flash-ready/` | App dir | USB product / PID |
|---|---|---|---|
| A | `sp1fx-usbaudio-in.bin` | `firmware/app/zephyr_usbaudio` | `SP-1 FX`, 0x2fe3:0x0101 |
| B | `sp1fx-usbaudio-io.bin` | `firmware/app/zephyr_usbaudio_io` (same sources) | `SP-1 FX IO`, 0x2fe3:0x0102 |

SHA-256 in `~/spfx/flash-ready/SHA256SUMS.usbaudio`.
(VID 0x2fe3 is Zephyr's development VID; for bring-up only.)

## HEARING SAFETY: read first

- **First run: computer volume LOW (about 10-20 %), headphones NOT on your ears**
  (hold them a few cm away, or use the speaker). Raise slowly.
- Analog gains are the tone image's quiet ones (CS42L42 mixer -30 dB; TAS2505
  speaker +6 dB driver gain with -6 dB attenuation; `zephyr_tone/src/sp1_codecs.c`,
  shared unmodified).
- Digital ceiling: the engine's soft limiter (|x| <= 1.0), then a fixed
  **-12 dB trim** (`CONFIG_SP1FX_UA_TRIM_DB`, range -40..0). Full-scale computer audio
  therefore leaves the SP-1 at most at -12 dBFS (|sample| <= 8231), before the -30 dB
  analog mixer. The engine master is 0.75 (-15 dB) in every preset, so typical music
  is around -27 dBFS digital: in the same range as the tone image's -30 dBFS tone.
- **200 ms fade-in** whenever the USB stream (re)starts (`CONFIG_SP1FX_UA_FADE_IN_MS`),
  5 ms fade-out when the host stops the stream, a 1.3 ms decay to zero on an
  underrun, 30 ms fade-out / 200 ms fade-in around every headphone/speaker switch,
  and a 25 ms engine duck around every preset change.
- To make it louder later: raise the trim in Kconfig (e.g. `-6`) and rebuild; do not
  touch the codec gains before the levels have been measured.

## What the images do (expected)

**A (`sp1fx-usbaudio-in.bin`)**: the computer sees a USB sound card "SP-1 FX"
(48 kHz, 16-bit, stereo **output only**) plus a serial port (status console).
Audio path per 64-frame block (1.333 ms):
USB ring -> int16->float -> fade-in -> `engine_process()` (real engine, 150 KiB pool)
-> output ramp -> -12 dB trim -> float->int16 (saturating) -> I2S -> headphones
(if plugged in) or speaker.

**B (`sp1fx-usbaudio-io.bin`)**: A, plus an **input** device "SP-1 FX IO" on the
computer carrying the processed signal (exactly the samples sent to I2S, so
including the -12 dB trim). Record it in any app.

Both: **FUNCTION tap** (press < 1.5 s) steps through 7 presets:
`0 Clean` (all slots bypassed: passthrough) -> `1 Dub Station` -> `2 Boom Bap` ->
`3 House` -> 4..6 (the other factory presets, see `firmware/app/factory_presets.h`)
-> back to Clean. Each change: duck (fade to silence), 25 ms, `engine_apply_config()`,
unduck. **FUNCTION held 1.5 s**: fade out, codecs muted, `SYSTEM_OFF` (as tone/min).

LEDs: **LED1** heartbeat (1 Hz). **LED2** USB stream playing. **LED3** an
underrun/overrun/dup/drop event happened in the last second (steady = trouble).
**LED4** preset index as a blink count every 2 s (Clean = off, Dub Station = 1 blink,
... preset 6 = 6 blinks).

## USB topology and endpoint budget

The nRF52840 USBD has EP0, 7 bulk/interrupt IN + 7 OUT endpoints, and **exactly one
isochronous IN (EP8 IN) and one isochronous OUT (EP8 OUT)** endpoint, sharing a
1 KiB ISO buffer (Zephyr's driver splits it 512/512, `ISOSPLIT=HalfIN`). This is
the hard limit that shapes image B. (The brief's "8 isochronous-capable endpoint
pairs" does not match the nRF52840 USBD; 8 is the endpoint *number* of the ISO pair.)

| Image | Endpoint (descriptor -> expected after nRF fix-up) | Use | Bytes/ms |
|---|---|---|---|
| A | 0x01 -> **0x08 OUT**, iso, asynchronous, wMaxPacketSize 196 | host -> SP-1 audio (47..49 frames) | <= 196 |
| A | 0x81 -> **0x88 IN**, iso feedback (bmAttributes 0x11), wMaxPacketSize 3 | explicit feedback, Q10.14 | 3 |
| B | 0x01 -> **0x08 OUT**, iso, asynchronous, wMaxPacketSize 196, no feedback EP | host -> SP-1 audio | <= 196 |
| B | 0x81 -> **0x88 IN**, iso, asynchronous, *implicit feedback data* (0x25), 196 | SP-1 -> host processed audio | <= 196 |
| both | interrupt IN + bulk IN/OUT (assigned by the stack) | CDC ACM console | small |

(Descriptor bytes read from the built ELFs: A `07050105c40001` + `07058111030001`;
B `07050105c40001` + `07058125c40001`. The renumbering to 0x08/0x88 happens at run
time in Zephyr's endpoint fix-up; expected, per the comment in Zephyr's
`usbd_uac2_macros.h`, not observed.) Full-Speed bandwidth: at most ~400 B of
isochronous data per 1 ms frame of the ~1500 B available: far below the 90 %
periodic limit.

### Why image B uses implicit feedback (and what that costs)

Image A's OUT stream needs the explicit feedback endpoint, which **is** the only ISO
IN endpoint. A return stream needs another ISO IN endpoint: there is none. Zephyr
v4.3.1's UAC2 class can describe "speaker with explicit feedback + microphone", but
the device cannot provide a second ISO IN endpoint, so that combination cannot run
on this chip. Image B therefore follows Zephyr's `uac2_implicit_feedback` sample
(written for exactly this nRF52/nRF53 limit): the OUT stream has no feedback
endpoint; the device sizes each IN packet (47/48/49 frames) from the OUT ring fill
(same law as A's feedback), and the host matches its OUT packet sizes to the IN
packet sizes. Consequences:

- The host must run the IN stream for its OUT rate to follow the SP-1. If an app plays
  to "SP-1 FX IO" while nothing records from it, the host *may* not open the IN
  stream (macOS behaviour here: **unknown**). Then the OUT rate is the host's own
  48 kHz and the ring's drop/duplicate-one-frame safeguard absorbs the clock
  difference (counted in `dup`/`drop`; for a 10 ppm difference about one event every
  2 s, ideally inaudible). If that matters, use image A for playback-only.
- **Windows' built-in USB Audio 2.0 driver does not support implicit feedback**
  (Zephyr sample README). Image B is for macOS/Linux.

## Clock domains

The I2S clock (3.072 MHz oscillator; CS42L42 makes LRCK = 48 kHz) and the host's USB
SOF clock drift apart. Numbers (all Kconfig, `firmware/app/zephyr_usbaudio/Kconfig`):

| Item | Value | Meaning |
|---|---|---|
| OUT ring | 512 frames (2 KiB) | static, power of two, lock-free SPSC |
| target fill | **192 frames** (3 blocks, 4.0 ms) | prebuffer level and feedback set-point |
| low limit | 96 frames | below: duplicate one frame per block (counted `dup`) |
| high limit | 320 frames | above: drop one frame per block (counted `drop`) |
| feedback | Q10.14 frames/SOF, nominal 786432 (48.0000) | EMA tau 64 SOFs, Kp 8 LSB/frame, clamp +-1 frame |
| return ring (B) | 512 frames, target 160, low 64, high 352 | same safeguards, counted separately |

Host simulation (`tests/test_usb_audio_core.c`, **simulated**, 10 minutes each, host
reacting 3 ms late): device 48000.5 / 47999.5 / +-500 ppm against a 48000.0 host:
fill before each block read stays within 146..292 frames (limits 96..320), zero
dup/drop/underrun/overrun, and the mean feedback over the last minute equals the
true device rate within 0.6 LSB (< 1 ppm). Image B's simulation also keeps the
return ring within 104..225 frames with no events. A host that ignores feedback
(48000.0 vs 48000.5) gives ~224 duplicated frames in 10 minutes and no underrun.

## Build

```sh
firmware/scripts/build_firmware.sh --app zephyr_usbaudio        # image A
firmware/scripts/build_firmware.sh --app zephyr_usbaudio_io     # image B
firmware/scripts/build_firmware.sh --app zephyr_usbaudio repro  # two clean builds, SHA compare
make test                                                       # host tests incl. test_usb_audio_core
```

Image B's directory has only `CMakeLists.txt`, `prj_io.conf` (sets
`CONFIG_SP1FX_USB_AUDIO_IN_STREAM=y`, PID, product) and `app.overlay`; it compiles
A's sources/Kconfig/prj.conf via CMake (the build script has no option for extra
config files, so a second app directory is the switch).

Sizes (Zephyr v4.3.1, SDK 0.17.4, `-O2`): A flash 99,932 B, RAM 185,704 B of 256 KiB;
B flash 101,208 B, RAM 188,800 B. The engine pool is **150 KiB (153,600 B)** of
that, so everything else (USB, rings, I2S slab, stacks, kernel) is ~32-35 KiB and
~73-75 KiB of RAM stays free. 150 KiB fits every factory preset; it rejects (by
design, `docs/STATUS.md`) combinations such as "delay + two reverbs".

### Zephyr UAC2 patch (Tape Looper's `zephyr-uac2.patch`): NOT applied

Decision: **not applied**, nothing in the west workspace was changed. Reasons:
(1) `build_firmware.sh` refuses to build when the pinned Zephyr checkout has local
modifications (`verify_pins`), and it must not be edited; (2) the patch only
changes the Full-Speed explicit-feedback format for Windows (and moves the feedback
write off SOF, which we keep on). A dry run (`patch -p1 --dry-run`) shows it would
apply cleanly to v4.3.1. Effect: macOS/Linux get the USB-spec 3-byte Q10.14 value
(what Apple wants, per Tape Looper); **Windows is expected to fail to start image A
("Code 10")**, see below. Next step if Windows matters: carry the patch as a
reproducible out-of-tree copy (e.g. a Zephyr module in the repo that replaces
`usbd_uac2.c`), or extend the build script's pin check to accept a recorded patch
hash; both need the build-script owner's decision.

## Flashing (manual, by the owner)

1. Check the files: `cd ~/spfx/flash-ready && shasum -a 256 -c SHA256SUMS.usbaudio`.
2. Desktop Chrome (WebUSB), open <https://solderless.engineering>.
3. SP-1 off. Hold **T1 + T4** while connecting USB; release when **T1 lights up**
   (TE bootloader).
4. Flash `sp1fx-usbaudio-in.bin` (A) first. Try B only after A passes.
5. Computer volume low, headphones off your ears (see safety above).

## macOS

1. After boot (~1-2 s) the Mac should list a new device. **System Settings > Sound >
   Output**: choose **SP-1 FX** (image B: **SP-1 FX IO**). Set the output slider to
   about 20 %.
2. **Audio MIDI Setup** (Applications > Utilities): select SP-1 FX; the Output tab
   should show **2 ch 16-bit Integer 48.0 kHz**. If the format is 44.1 kHz the device
   was not enumerated as expected (it offers only 48 kHz); note what is shown.
3. Play music or a test tone (e.g. a 1 kHz sine from a tone generator site or
   Audacity's Generate > Tone) and listen. Pitch must be right (no chipmunk/slow).
4. Buffer size: macOS sets it per app. In a DAW (Logic: Settings > Audio > I/O
   Buffer Size; Ableton: Preferences > Audio > Buffer Size; Audacity: Preferences >
   Devices > Latency/Buffer length) start at **256 samples**, then try 128 and 64.
   Note the smallest that stays click-free.
5. Status console: `ls /dev/cu.usbmodem*`, then `screen /dev/cu.usbmodemXXXX 115200`
   (quit: Ctrl-A, K, y). One block of lines per second (fields below).
6. Image B return stream: **QuickTime Player > File > New Audio Recording**, click the
   arrow next to the record button, choose **SP-1 FX IO** as the microphone, record
   while music plays. Or **Audacity**: Audio Setup > Recording Device = SP-1 FX IO,
   2 channels, project rate 48000 Hz, record. To play and record through the SP-1
   at the same time in Audacity, set Playback Device = SP-1 FX IO too.

## Windows (expected: not working yet)

- Image A: Windows' `usbaudio2.sys` wants a 4-byte Q16.16 Full-Speed feedback value
  (Tape Looper's patch note); stock Zephyr sends 3 bytes. Expected: device shows a
  warning in Device Manager (Code 10). The CDC console should still appear (COM
  port). Report what Device Manager says.
- Image B: Windows' driver does not support implicit feedback: expected not to work.
- Both use the same VID with different PIDs, so Windows will not mix their cached
  descriptors.

## iPhone / iPad / Android (untested)

- iOS/iPadOS: needs Apple's Lightning-to-USB 3 Camera Adapter (Lightning) or a USB-C
  port/adapter, and the SP-1 is battery powered, so it should not need power from
  the phone (it requests 100 mA). Class-compliant UAC2 is supported by iOS in
  general; this device: **untested**. Look in Control Center > AirPlay output list.
- Android: USB-C OTG; Android 10+ supports UAC2 output for most phones;
  implicit-feedback (image B) support varies. **Untested.**

## Console fields (once per second; copy these lines into the report)

```
t=12 preset=1:Dub Station rej=0 cyc_blk_min/avg/max=41234/43120/52011 cyc_frame_avg/max=673.7/812.6 cpu%_avg/max=50.5/60.9 blk_total_max=56012
  usb out=on play=1 starts=1 fill_min/avg/max=150/193/240 fb=786440 (48.0005 f/ms) rx_f/s=48000 sof/s=1000 under=0 ovr=0 dup=0 drop=0 zpkt=0 nobuf=0
  ret in=on tx_f/s=48000 fill_min/avg/max=110/150/190 under=0 dup=0 drop=0 ovr=0 silent_pkts=12 send_fail=0     (image B only)
  eng in_pk=251 out_pk=180 (x1e-3) in_clip=0 out_clip=0 nan=0 sat16=0 | i2s blk/s=750 err=0 tmo=0 rec=0 cfg=0/0 | cs42=42+ tas=ack+ hp=1 out=hp usb=0 con_drop=0 rr=4
```
(Numbers above are illustrative placeholders, **not measurements**.)

| Field | Meaning | Expected |
|---|---|---|
| `cyc_blk_min/avg/max` | DWT cycles of one `engine_process()` call on 64 frames, over the last 750 blocks | the raw numbers for STATUS budgets |
| `cyc_frame_avg/max` | the same / 64 | budget is **1333.3 cycles/frame** (64 MHz / 48 kHz) |
| `cpu%_avg/max` | share of the 1333 cycles/frame budget | must stay < 100 (keep < ~70 for USB/headroom) |
| `blk_total_max` | whole block incl. ring read, conversions, ramps | slightly above `cyc_blk_max` |
| `rej` | slots rejected by `engine_apply_config()` (memory/CPU declaration) | 0 for factory presets |
| `fill_min/avg/max` | OUT ring fill (frames) seen at each block read | around 192, inside 96..320 |
| `fb` | feedback Q10.14 (raw) and frames/ms | 48.0000 +- ~0.01 |
| `rx_f/s`, `sof/s` | frames received / SOFs per second | ~48000 / 1000 |
| `under/ovr/dup/drop` | cumulative ring events | 0 (a few at stream start/stop are OK) |
| `zpkt`, `nobuf` | empty OUT packets / no receive buffer | 0 |
| `ret ...` (B) | return ring and IN stream | `tx_f/s` ~48000, events 0 |
| `in_pk/out_pk` | engine peak meters x1000 (decaying) | out <= ~1000 (limiter) |
| `in_clip/out_clip/nan` | engine diag counters | `nan=0` |
| `sat16` | int16 saturations after the trim | 0 (trim leaves 12 dB headroom) |
| `i2s blk/s err tmo rec` | as the tone image | 750 / 0 / 0 / 0 |

CPU-per-preset measurement: play steady music, tap FUNCTION once every ~10 s,
and copy 3-5 seconds of lines per preset (the `preset N:name rejected=... mem=... cpu_decl=...`
line printed at each change has the declared budget for comparison).

## Pass / fail checklist (owner)

| # | Check | Pass | If it fails |
|---|---|---|---|
| 1 | LED1 blinks 1 Hz within ~2 s of boot | ☐ | not booting: recover (below) |
| 2 | Mac lists "SP-1 FX" (B: "SP-1 FX IO") in Sound settings; Audio MIDI Setup shows 2 ch 16-bit 48 kHz | ☐ | note what is listed; console still there? |
| 3 | Music plays at the right pitch and speed, quiet; starts with a soft fade (no thump) | ☐ | wrong pitch = clock/format problem: send console lines |
| 4 | No clicks or dropouts over **5 minutes** of music (LED3 stays dark; `under/ovr/dup/drop` stay 0 after start) | ☐ | send the counters and the buffer size used |
| 5 | Headphones in/out switches output with a short fade | ☐ | as tone image |
| 6 | FUNCTION tap cycles presets audibly (Dub Station = delay/reverb, Boom Bap = crunch...), LED4 blink count follows, no loud click on change | ☐ | send `preset` lines (`rejected`?) |
| 7 | Console numbers sane: `blk/s` 750, `rx_f/s` ~48000, `fb` ~48.000x, `cpu%_max` < 100 for every preset | ☐ | send 5 lines per preset |
| 8 | Pausing/stopping playback: fades out, no pop; resuming fades in | ☐ | |
| 9 | (B) Recording "SP-1 FX IO" in QuickTime/Audacity gives the processed audio, clean, at the right speed, for 5 minutes | ☐ | send `ret` lines |
| 10 | FUNCTION held 1.5 s: everything off; next FUNCTION press -> bootloader / restart | ☐ | recover via T1+T4 |

## Round-trip latency measurement (image B, loopback)

1. Audacity: project rate 48000 Hz; Playback Device and Recording Device both
   **SP-1 FX IO**; Preset **Clean**; Audacity Preferences > Recording: *uncheck*
   latency compensation (set "Latency correction" to 0 ms) so the raw offset is visible.
2. Generate a click track: Generate > Rhythm Track (or a single 1-sample impulse made
   with Generate > Tone 1 ms), 120 BPM, 10 s, on track 1.
3. Record (Shift+R plays track 1 while recording a new track).
4. Zoom in on one click in both tracks; select from the click in track 1 to the same
   click in the recorded track; read the length in samples (set the selection
   toolbar to "samples"). That is the round trip: host output buffer + USB +
   OUT ring (~192 frames = 4 ms target) + engine (0 extra) + return ring (~160 frames
   = 3.3 ms) + USB + host input buffer. Note: it does not include the I2S queue
   (the return stream taps the same block before it goes to I2S); the headphone path
   has a further 4-6 blocks (5-8 ms) of I2S queue/DMA.
5. Repeat at buffer sizes 512, 256, 128, 64 (if they run clean).

**Record for me:** host OS + version, app, buffer size, the offset in samples for 5
clicks each (min/max), any click or dropout, and 10 console lines taken during the
recording. For the headphone path (latency to your ears) the only way is acoustic:
record the headphone output with a microphone next to the computer speaker click;
optional.

## Recovery

1. Re-enter the bootloader: SP-1 off (FUNCTION hold 1.5 s, or let it reach
   `SYSTEM_OFF`), hold **T1 + T4** while connecting USB until **T1 lights**. If that
   fails, use the solderless.engineering recovery procedure.
2. Flash a known-good image from `~/spfx/flash-ready/` (verify with
   `shasum -a 256 -c SHA256SUMS` first): `sp1fx-tone.bin` (confirmed working by the
   owner), `sp1fx-min-heartbeat.bin` (known to boot), or
   `RECOVERY-tape-looper-from-repo.bin` (Tape Looper's release).

## Guesses and assumptions (unverified)

- The nRF USB driver renumbers the UAC2 ISO endpoints to 0x08/0x88 and the composite
  device (UAC2 + CDC with IAD) enumerates on macOS with these descriptors.
- macOS accepts 3-byte Q10.14 explicit feedback on Full Speed (Tape Looper's finding).
- macOS opens image B's IN stream whenever the device is used for output (needed for
  implicit feedback to steer the host); if not, the drop/dup safeguard runs.
- The audio thread (preemptive priority 0) running the engine does not starve the USB
  stack: Zephyr's USB threads are cooperative priority 8 and pre-empt it. A heavy
  preset delays only the main loop (LEDs/console/watchdog feed, 4 s window).
- The CS42L42 3.072 MHz clocking gives exactly 48 kHz LRCK (tone pitch confirmed by
  the owner; the exact ppm is unknown: `fb` will show it).
- FUNCTION tap detection uses no debounce beyond the 10 ms loop and a 30 ms minimum
  press; a bouncy switch could double-step a preset.
- Per-second statistics are gathered with a seqlock; a line may occasionally show
  `?` after `cyc_blk` when it could not get a consistent snapshot.
- Not done: volume/mute controls (no UAC2 feature unit: the host volume slider is
  software volume), sample rates other than 48 kHz, 24-bit, High Speed (nRF52840 is
  Full Speed only), the Windows feedback patch.
