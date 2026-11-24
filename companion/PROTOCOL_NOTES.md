# Protocol gaps and assumptions found while implementing the companion

Written against `docs/PROTOCOL.md` (v1). Nothing here has been checked against firmware (none exists yet). Each item says what the companion does, so the firmware author can either match it or change the spec.

1. **STATE does not fit the frame limits.** The 0x83 payload is 4+4+4+1+1+1+2+2+2+8 (fixed part, 29 bytes) + 4*11*4*2 (352 bytes of params) + 2 (ui_mode, edit_slot) = **383 bytes** for 11 effects, but the spec caps payload at 250 and body at 256. Companion: the frame receiver accepts bodies up to 1024 bytes (`MAX_BODY_RX`); everything else is still limited to 250/256. Options for the spec: raise the max body for 0x83, or split STATE (e.g. GET_STATE with a slot index), or send only the active effect's params.
2. **Max body vs max payload are off by one.** Payload 0..250 + 5 bytes (ver, type, seq, crc16) = 255, not 256. Companion enforces payload <= 250.
3. **`fx_count` includes None.** HELLO_RSP `fx_count` is treated as the number of effect descriptors including id 0 (None), and STATE `params[4][fx_count][4]` includes a (meaningless) None row. The spec does not say so explicitly.
4. **`order[]` meaning.** Treated as `order[position] = slot index` (signal flows position 0 to 3). Not stated.
5. **Normalised mapping of real units.** The spec says params are 0..1 but not how they map to min/max for log and step scales. Companion assumes: lin `min + n*(max-min)`; log `min*(max/min)^n`; step `round(min + n*(max-min))`, inverse `(v-min)/(max-min)`. This is display-only; the device remains authoritative.
6. **Admission arithmetic.** The mock sums `mem_bytes` and `cpu_cycles` of all four assigned effects (bypassed ones included) against the pool (153600 B) and budget (850 cycles); memory is checked before CPU. The real engine may add overhead or skip bypassed slots; the companion only displays what RESULT/0xA1 say.
7. **Error replies to GET_*.** A bad `GET_FX idx` has no defined reply (0x82 only). The mock answers 0x90 RESULT with `request_type = 0x02, code 1`; the client accepts a 0x90 naming the request type as a reply to any request. Suggest adding this to the spec.
8. **FACTORY_RESET has no event.** After a successful reset the host does not know the new state; the companion re-sends GET_STATE.
9. **Failed setters and the UI.** 0x90 carries only `request_type` and `code`, not the slot. The companion correlates by `seq`, and on any non-zero code re-reads GET_STATE to revert the UI to the device's truth.
10. **Retries.** "Retry after 500 ms (max 3)" is implemented as 1 try + 3 retries with the same seq. Setters are idempotent so duplicates are harmless; the spec does not say whether the device must tolerate duplicate seqs.
11. **Seq 0 is reserved** for device events, so the host cycles seq 1..255.
12. **Meters units.** `in_peak/out_peak` use 32768 = full scale and can exceed it; the companion shows dBFS = 20*log10(p/32768) over a -60..0 range. `cpu_pct` is assumed to be percent of the 850-cycle budget.
13. **SUBSCRIBE rate.** The companion subscribes at 10 Hz (allowed 1..20).
14. **Preset `fx` ids vs table names.** Preset ids are `fxKey(name)` = lower-cased table name with non-alphanumerics replaced by `_` (`Tape wobble` -> `tape_wobble`), which matches the list in PROTOCOL.md. A device with different effect names would not round-trip presets.
15. **Preset `params` for a slot with `none`.** Stored as zeros, ignored on apply. For every other effect, params the preset omits are filled with the effect's default.
16. **No message sets `ui_mode`/`edit_slot`.** They are read-only in the companion (shown only in the model).
