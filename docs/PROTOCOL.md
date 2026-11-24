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
| 0x20 | SAVE_NOW | - | 0x90 |
| 0x21 | FACTORY_RESET | `0xA5` (confirmation byte) | 0x90 |
| 0x30 | SUBSCRIBE | `hz u8` (0 = off, 1..20) | 0x90 |

## Device to host
**0x81 HELLO_RSP**: `proto u8, fw_len u8, fw[fw_len] ascii, fx_count u8, sample_rate u32, block u16, pool_bytes u32, cpu_budget u16`

**0x82 FX_DESC**: `id u8, name_len u8, name, nparams u8, cpu_cycles u16, mem_bytes u32, tail_ms u32, mix_mode u8 (0 crossfade, 1 send)`, then per param: `name_len u8, name, unit_len u8, unit, min f32, max f32, def f32, scale u8 (0 lin, 1 log, 2 step), labels_len u8, labels` (labels is a comma-separated list for step params, else empty). Floats are IEEE-754 little-endian.

**0x83 STATE**: the config payload exactly as `store_encode()` produces it (see `firmware/storage/config_store.c`): `fx_id[4] bypass[4] order[4] global u8, reserved u8, fx_count u8, in_gain_cdB i16, master u16n, bpm10 u16, wet[4] u16n, params[4][fx_count][4] u16n`, followed by `ui_mode u8 (0 perform, 1 edit), edit_slot u8`. `params` holds the remembered values for **every** effect in every slot, not just the active one.

**0x90 RESULT**: `request_type u8, code u8` (matched to its request by `seq`; it does not carry the slot, so a host that sees a non-zero code should re-read GET_STATE). A GET_FX with an out-of-range index is answered with 0x90 code 1. FACTORY_RESET has no event; the host re-reads GET_STATE afterwards. Codes: 0 ok, 1 bad argument, 2 out of DSP memory, 3 CPU budget exceeded, 4 busy, 5 storage error. On 2 or 3 the previous assignment is unchanged.

**0xA0 EVENT** (unsolicited, seq 0): `setter_type u8` followed by that setter's payload, sent when the **device** changed something (a physical fader, button gesture, or another host). Host-originated changes are acknowledged with 0x90 and are **not** echoed back, which prevents feedback loops.

**0xA1 ADMISSION_ERROR** (unsolicited): `slot u8, fx u8, code u8`. Sent when a physical effect-cycle was rejected, so the companion can show why.

**0xA2 METERS** (only while subscribed): `in_peak u16, out_peak u16` (linear, 32768 = full scale, peak since the last report), `cpu_pct u8, cpu_peak_pct u8, mem_used u32, in_clips u32, out_clips u32, underruns u32, overruns u32, nan_events u32, ui_mode u8, edit_slot u8, flags u8` (bit0 = global bypass).

## Definitions (v1)
- `fx_count` counts **None** (id 0), so it is 11 for the current effect set. Effect ids are the firmware enum: none, filter, chorus, delay, reverb, saturation, flanger, bitcrusher, tremolo, compressor, tape_wobble.
- `order[position]` holds the **slot index** processed at that position (a permutation of 0..3).
- Normalised-to-real mapping: `lin`: `min + n*(max-min)`; `log`: `min*(max/min)^n`; `step`: `min + floor(n*(max-min+1))`, capped at `max`.
- Admission (what 0x90 codes 2 and 3 mean): the device checks the **CPU budget first**, then memory, summed over all four slots, with the replaced slot's own effect excluded. The companion may predict this, but the device decides.
- `ui_mode` and `edit_slot` are reported only; there is no message to set them.

## Rules
- Fader changes are rate-limited by the device to 20 events/s per control. The host should coalesce slider drags to about 30 messages/s.
- After any host-originated change the device re-arms soft takeover for the affected controls, so physical faders never make the value jump.
- The device persists changes on its own, debounced (1.5 s quiet, at most one write per 5 s). The host does not have to call SAVE_NOW.
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
