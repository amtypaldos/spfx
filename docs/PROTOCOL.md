# SP-1 FX device protocol, version 1

Status: **specification, not yet implemented in firmware.** The browser companion is built against this and a built-in mock device. The firmware side (C) will be written to match; if they disagree, change this document first.

## Transport
- Web Serial on the SP-1's USB CDC-ACM port (composite device with USB audio). Baud rate is irrelevant.
- The device works with no host connected. The companion is optional.

## Framing
Each message is `COBS(body) 0x00`. Body (all multi-byte integers **little-endian**):

| bytes | field |
|---|---|
| 1 | `ver` = 1 |
| 1 | `type` |
| 1 | `seq` (host requests: host picks; responses echo it; device events use 0) |
| n | payload (0..500 bytes) |
| 2 | CRC-16/CCITT-FALSE over `ver..payload` (poly 0x1021, init 0xFFFF) |

Max body 512 bytes (payload 500 + 5 bytes of overhead + COBS expansion is handled by the framer). The STATE message is ~383 bytes for 11 effects, which is why the limit is above 256. A frame that fails COBS or CRC is dropped silently; the host retries after 500 ms (max 3). Normalised values (`u16n`) are `round(x*65535)`, x in 0..1.

## Host to device
| type | name | payload | response |
|---|---|---|---|
| 0x01 | HELLO | - | 0x81 |
| 0x02 | GET_FX | `idx u8` (0..fx_count-1) | 0x82 |
| 0x03 | GET_STATE | - | 0x83 |
| 0x10 | SET_SLOT_FX | `slot u8, fx u8` | 0x90 |
| 0x11 | SET_PARAM | `slot u8, idx u8, v u16n` | 0x90 |
| 0x12 | SET_WET | `slot u8, v u16n` | 0x90 |
| 0x13 | SET_BYPASS | `slot u8, on u8` | 0x90 |
| 0x14 | SET_GLOBAL_BYPASS | `on u8` | 0x90 |
| 0x15 | SET_MASTER | `v u16n` | 0x90 |
| 0x16 | SET_INPUT_GAIN | `cdB i16` (-1200..1200) | 0x90 |
| 0x17 | SET_ORDER | `o0..o3 u8` (permutation of 0..3) | 0x90 |
| 0x18 | SET_BPM | `bpm10 u16` (300..3000) | 0x90 |
| 0x19 | SET_SYNC | `on u8` (0 or 1): start/stop the sync-jack clock (MIDI clock + PO pulses) | 0x90 |
| 0x1A | SET_OUTPUT_MODE | `mode u8`: headphone jack 0 = headphones (default), 1 = line out (+36 dB; firmware v1-rc4+), 2 = loud (+15 dB; v1-rc5+). A mode whose `caps` bit (OUT_LINE / OUT_LOUD) is clear is answered with code 1 | 0x90 |
| 0x20 | SAVE_NOW | - | 0x90 |
| 0x21 | FACTORY_RESET | `0xA5` (confirmation byte) | 0x90 |
| 0x30 | SUBSCRIBE | `hz u8` (0 = off, 1..20) | 0x90 |

## Device to host
**0x81 HELLO_RSP**: `proto u8, fw_len u8, fw[fw_len] ascii, fx_count u8, sample_rate u32, block u16, pool_bytes u32, cpu_budget u16`, then (firmware 2026.11.25+) `caps u32`.

`caps` lists the features this firmware supports. A host disables (grays out) controls whose bit is clear. If the field is absent (firmware v1-rc6 and earlier), the host assumes `0x3F` (the rc6 set). Hosts ignore bits they do not know. Bits:

| Bit | Name | Meaning |
|---|---|---|
| 0 | SYNC | Sync-jack clock: SET_SYNC starts/stops it, SET_BPM sets its tempo |
| 1 | OUT_LINE | SET_OUTPUT_MODE 1 (line out) |
| 2 | OUT_LOUD | SET_OUTPUT_MODE 2 (loud) |
| 3 | BATTERY | METERS carries a meaningful `batt_pct` |
| 4 | SAVE | SAVE_NOW has a storage backend |
| 5 | POWER_FLAGS | METERS flags bits 1-2 (USB power, charging) are reported |
| 6-31 | reserved | 0; each new optional feature takes the next bit in the same spec change |

The `fw` string is for display only. From 2026.11.25 it is date-based, `sp1fx-YYYY.MM.DD` (a second release on the same day appends `.2`, `.3`); earlier images used `sp1fx-v1-rcN`. Hosts must not parse it to decide features: use `caps`.

**0x82 FX_DESC**: `id u8, name_len u8, name, nparams u8, cpu_cycles u16, mem_bytes u32, tail_ms u32, mix_mode u8 (0 crossfade, 1 send)`, then per param: `name_len u8, name, unit_len u8, unit, min f32, max f32, def f32, scale u8 (0 lin, 1 log, 2 step), labels_len u8, labels` (labels is a comma-separated list for step params, else empty). Floats are IEEE-754 little-endian.

**0x83 STATE**: the config payload exactly as `store_encode()` produces it (see `firmware/storage/config_store.c`): `fx_id[4] bypass[4] order[4] global u8, sync_on u8 (was reserved; 0 or 1), fx_count u8, in_gain_cdB i16, master u16n, bpm10 u16, wet[4] u16n, params[4][fx_count][4] u16n`, followed by `ui_mode u8 (0 perform, 1 edit), edit_slot u8`, then (firmware v1-rc4+) `out_mode u8` (0 headphones, 1 line out, 2 loud (rc5+)). A host must accept STATE with or without the trailing `out_mode` byte (absent = headphones). **Compatibility:** companions older than this change reject the extra byte (they check for trailing bytes); the companion in this repository and firmware rc4+ go together. `params` holds the remembered values for **every** effect in every slot, not just the active one.

**0x90 RESULT**: `request_type u8, code u8` (matched to its request by `seq`; it does not carry the slot, so a host that sees a non-zero code should re-read GET_STATE). A GET_FX with an out-of-range index is answered with 0x90 code 1. FACTORY_RESET has no event; the host re-reads GET_STATE afterwards. Codes: 0 ok, 1 bad argument, 2 out of DSP memory, 3 CPU budget exceeded, 4 busy, 5 storage error. On 2 or 3 the previous assignment is unchanged.

**0xA0 EVENT** (unsolicited, seq 0): `setter_type u8` followed by that setter's payload, sent when the **device** changed something (a physical fader, button gesture, or another host). Host-originated changes are acknowledged with 0x90 and are **not** echoed back, which prevents feedback loops.

**0xA1 ADMISSION_ERROR** (unsolicited): `slot u8, fx u8, code u8`. Sent when a physical effect-cycle was rejected, so the companion can show why.

**0xA3 PRESET_LOADED** (unsolicited, seq 0): `idx u8, bpm10 u16`. The device loaded one of its own presets (FN + rocker), so many values changed at once. The host must re-read GET_STATE. Individual 0xA0 events for the same change are not sent; pending fader events from before the load are discarded.

**0xA2 METERS** (only while subscribed): `in_peak u16, out_peak u16` (linear, 32768 = full scale, peak since the last report), `cpu_pct u8, cpu_peak_pct u8, mem_used u32, in_clips u32, out_clips u32, underruns u32, overruns u32, nan_events u32, ui_mode u8, edit_slot u8, flags u8` (bit0 = global bypass; added in firmware v1-rc3, 0 before: bit1 = USB power present, bit2 = battery charging, bit3 = overload guard forced bypass, bits 4-7 = slots 1-4 switched off by the overload guard), then (firmware v1-rc6+) `batt_pct u8` (battery 0..100 %, 255 unknown; calculated, uncalibrated). Hosts must accept METERS with or without the trailing `batt_pct`; companions older than rc6 reject it.

## Definitions (v1)
- `fx_count` counts **None** (id 0), so it is 11 for the current effect set. Effect ids are the firmware enum: none, filter, chorus, delay, reverb, saturation, flanger, bitcrusher, tremolo, compressor, tape_wobble.
- `order[position]` holds the **slot index** processed at that position (a permutation of 0..3).
- Normalised-to-real mapping: `lin`: `min + n*(max-min)`; `log`: `min*(max/min)^n`; `step`: `min + floor(n*(max-min+1))`, capped at `max`.
- Admission (what 0x90 codes 2 and 3 mean): the device checks the **CPU budget first**, then memory, summed over all four slots, with the replaced slot's own effect excluded. The companion may predict this, but the device decides.
- **Sync clock** (`SET_SYNC`): on = the device sends MIDI Start then 24 PPQN clock on the sync jack ring and 2 PPQN pulses on the tip; off = MIDI Stop. The state is **saved** and restored at power-up (the clock starts itself if it was on). It is a device setting, not part of a preset; loading a preset changes the BPM but never the on/off state.
- `ui_mode` and `edit_slot` are reported only; there is no message to set them.
- **Output mode** (`SET_OUTPUT_MODE`): a device setting, not part of a preset; firmware rc5 starts every power-up at headphones and returns to headphones when something is plugged into the jack at a louder level; FACTORY_RESET returns to headphones. From 2026.11.25.2 the device can also change the level on its own (holding VOL+ and VOL- together for 3 s cycles Headphones -> Loud -> Line out -> Headphones while headphones/a cable are plugged in; plugging something into the jack at a louder level returns to Headphones). It then sends EVENT `SET_MASTER` (the master as lowered for the new level) followed by EVENT `SET_OUTPUT_MODE`. Older firmware sent no event for the jack-plug return. Switching to a louder level lowers the master volume by the difference (15 or 36 dB) so the level never jumps (the host sees the new master in its next GET_STATE / it should re-read state after the RESULT).

## Rules
- **Forward compatibility:** a host must ignore unknown trailing bytes at the end of any device-to-host message (types 0x80-0xFF, including the setter inside an EVENT), so newer firmware can append fields. Hosts from before 2026.11.25 reject them (they check for trailing bytes). The device stays strict about host-to-device requests.
- Fader changes are rate-limited by the device to 20 events/s per control. The host should coalesce slider drags to about 30 messages/s.
- After any host-originated change the device re-arms soft takeover for the affected controls, so physical faders never make the value jump.
- The device persists changes on its own (firmware v1-rc4: event-based: setting changes such as effect, order, BPM, sync, output mode, factory reset are saved ~1.5 s after the last one; continuous controls such as wet, params, master, input gain, bypass are kept in RAM and saved with the next event, at power off, by SAVE_NOW, or after 120 s; nothing is written while the device's CPU load is high). The host does not have to call SAVE_NOW, but should if it wants continuous-control changes on flash at once.
- The device is the source of truth. On connect the host sends HELLO, GET_FX for each effect, GET_STATE, then SUBSCRIBE.
- Presets live in the browser (see below). Applying one sends the individual setters; there is no bulk-write message in v1.

## Preset JSON, schema v1
```json
{
  "schema": "sp1fx.preset", "version": 1, "name": "Dub Station",
  "inputGainDb": 0, "master": 0.75, "bpm": 120, "order": [0, 1, 2, 3], "globalBypass": false,
  "slots": [
    { "fx": "saturation", "bypass": false, "wet": 0.6, "params": [0.4, 0.7, 0.5, 0.5] },
    { "fx": "filter", "bypass": false, "wet": 1.0, "params": [0.5, 0.4, 0.0, 0.0] },
    { "fx": "delay", "bypass": false, "wet": 0.4, "params": [0.6, 0.5, 0.4, 0.5] },
    { "fx": "reverb", "bypass": false, "wet": 0.3, "params": [0.5, 0.5, 0.5, 0.15] }
  ]
}
```
`fx` is one of `none, filter, chorus, delay, reverb, saturation, flanger, bitcrusher, tremolo, compressor, tape_wobble`. `params` are normalised 0..1 for the active effect. Importers must validate everything, reject unknown schema or version, clamp numbers, and ignore unknown keys.

## Implementation notes (firmware/protocol, not normative)
Added with the C device implementation. These record choices where v1 is silent; they do not change the spec above.
- A request whose payload length is not exactly the documented length, an unknown request type (< 0x80), an out-of-range argument, or `SET_PARAM idx >= nparams` of the slot's active effect (so any SET_PARAM on a None slot) is answered with 0x90 code 1. Frames with a device-to-host type (>= 0x80) are ignored.
- A valid CRC with `ver != 1` is dropped silently, like a CRC error.
- `cpu_pct` / `cpu_peak_pct` = `round(100 * engine_diag.cpu_{last,peak}_cycles / cpu_budget)`, capped at 255, i.e. the diag cycle counters are taken to be per stereo frame like the budget. `mem_used` is the engine's placed block bytes (state + delay memory, 8-byte aligned), which is slightly more than the sum of the descriptors' `mem_bytes`.
- METERS resets `engine_diag.in_peak/out_peak` after each report. The output-level LEDs read `out_peak` too, so while a host is subscribed they show the peak since the last report.
- After SUBSCRIBE the first METERS goes out on the next tick, then every `1000/hz` ms (integer ms; 3 Hz is 333 ms). After a stall longer than a period the schedule restarts instead of bursting.
- FACTORY_RESET applies `store_data_default()` (all four slots bypassed, BPM 120); if the engine rejected a default slot the answer is code 2. SAVE_NOW without a storage backend, or with a failing write, answers code 5. SAVE_NOW answers code 4 (busy) when the save would need a flash page erase while USB is connected (zephyr_sp1fx v1-rc2: an erase stalls the CPU ~85 ms); the settings stay pending and are saved at FN-hold power off or later.
- Device events: fader events (SET_WET, SET_PARAM) are coalesced per control and sent at most every 50 ms, latest value wins; pending fader values for a slot are flushed before a SET_SLOT_FX event for it, and dropped when the host sets the same control. A fader pickup that does not change the value sends nothing. Button events (bypass, global bypass, master, effect change) are sent immediately.
