# Third-party notices: zephyr_controls bring-up image

The coordinator merges this into THIRD_PARTY_NOTICES.md. This app does not edit that file.

## chattock/sp1-tape-looper: MIT, (c) chattock

- Upstream: `~/spfx/upstream/chattock_sp1-tape-looper`, commit **0359372e**
- License: MIT (copy in `firmware/boards/LICENSES/Tape-Looper-MIT.txt`)
- Source file: `tools/capacity422/runtime/src/main.c`. Overlay: `tools/capacity422/runtime/app.overlay`

What was used. Everything was adapted and rewritten. Nothing was copied verbatim except constants:

| Our file | From Tape Looper | Adaptation |
|---|---|---|
| `firmware/drivers/ladder.h/.c` | `decode_tracks()` thresholds 110/300/560/950/1500; `decode_vol()` thresholds 200/560/950/1500/1910 and the nominal comments; PLAY + track nominals 1861/1910/2004/2159 from `lad_press()` (W113); VOL-both ~1998 (W115); fader scaling `raw*256/3700` | Re-expressed as a table-driven decoder that outputs the SP-1 FX `BTN_*` bitmask. It adds band hysteresis and an N-read stability commit, which play the role of Tape Looper's sticky 3-sample commit. PLAY-chord split is optional and off by default. The LADREL-937 per-press gain learning is **not** ported. The fader normaliser is new: end-stop constants 0/3700, end snap, low-pass and hysteresis. Mapping VOL_TEMPO_UP→FWD and VOL_TEMPO_DOWN→REV is our inference. |
| `firmware/drivers/controls_hw.c` | `ladder_scan()`: one `adc_read` over all channels, twice, then averaged. `controls_init()`: BTN_COM P1.10 driven high. `led_pwm_one()`: register-level PWM at 16 MHz, 1 kHz frame, Individual load, `LOOPSDONE→SEQSTART0`, polarity bit 0x8000. `status_level()`: squared brightness law. LED pin tables `leds[]` and `track_leds[]` | 6 channels instead of 7 (no battery). Rail through a devicetree GPIO spec. LEDs on **PWM0/PWM1** instead of the bootloader-started PWM2/PWM3. PWM2/PWM3 are stopped only if they are routed to an LED pin. Full-scale duty is 25 %. Brightness input is 0..255. |
| `firmware/app/zephyr_controls/app.overlay` | Fader ADC channels 2-5 (AIN3/AIN6/AIN2/AIN7, gain 1/6, internal ref, 20 us, 12-bit) and `io-channels` on `zephyr,user` | Battery channel 6 omitted. Added `fn-gpios` and `btn-com-gpios`. |
| `firmware/app/zephyr_controls/src/main.c` | Watchdog feed of all 8 RR channels; power-off order (LEDs dark, wait for FN release, 60 ms, sense-low wake, clear RESETREAS, SYSTEM_OFF); USB device_next CDC setup through `sample_usbd_init_device` | Written fresh, by way of `firmware/app/zephyr_min` and `zephyr_tone` (this repo). Also drives BTN_COM low before SYSTEM_OFF, which Tape Looper does not do. |

## Others

- Zephyr RTOS v4.3.1 (Apache-2.0): `samples/subsys/usb/common` (sample_usbd helper, Kconfig.sample_usbd), used unmodified from the pinned west workspace.
- timknapen/SP-1-dev wiki (MIT): pin and peripheral facts only (SAADC, PWM, Bootloader pages). No code was taken.
- softmodded/marisko: **not used** (unlicensed). No Infineon or Bluetooth files are used.
