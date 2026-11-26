# ADR-004: Cycling presets on the device (user decision, 2026-11-24)

- **Gesture:** FN + rocker. FWD = next preset, REV = previous, wraps around, holding the rocker repeats (450 ms, then every 200 ms). FN is marked "used" by the gesture, so releasing it afterwards is not an FN tap and does not exit anything. If a channel button or PLAY is held, FN + rocker is ignored (channel + rocker stays "cycle that slot's effect").
- **First press** with no preset loaded yet: forward goes to the first preset, back goes to the last.
- **Loading:** the output fades to silence over about 20 ms (`engine_set_duck`), the controller waits 25 ms, then applies the whole config through `engine_apply_config` (so memory/CPU admission applies), then fades back. A press arriving during the fade re-arms it, so quick successive presses coalesce into one load of the final target. Delay/reverb tails of the old preset are cut (under the fade). Edit mode is left; takeover is re-armed on all four faders, so faders never make the sound jump and the fader LEDs show which way to move.
- **LED feedback (no numbers):** the four LEDs beside PLAY flash together for 250 ms (rc4 and later: 7 rapid flashes), then show a 1 to 4 LED bar for about 1.4 s: the place in the list (first preset = 1 LED, last = 4). If any slot of the preset was refused, the usual refused pattern is shown.
- **Tempo:** the preset's BPM is applied to the sync clock and reported to the host.
- **Host notification:** one unsolicited `0xA3 PRESET_LOADED {idx, bpm10}`; the companion re-reads the whole state.
- **List contents:** the six factory presets (compiled in, `firmware/app/factory_presets.h`). User presets on the device are NOT built yet.

## Open items
- User presets on the device: need protocol messages (list/send/delete a device preset slot), a small separate flash area (about 8 x 400 B, written rarely), and a decision about saving from the device (no gesture assigned).
- The working configuration (power-up state) is saved by the existing debounced journal. Loading a preset overwrites the working configuration; there is no "undo".
- Whether a 25 ms fade is enough to avoid an audible dip/click, and the flash-erase stall during a save right after a preset load, need listening/measurement on hardware.

## Amendments (2026-11-24, zephyr_sp1fx v1-rc1)
- **Volume is kept:** a device preset load keeps the current master volume (`controller_t.preset_keep_master`; VOL+/VOL- is a device setting). The factory presets' `master` (0.75) is still used by the companion when it applies a preset. Tempo and everything else still come from the preset; the sync on/off state is never touched.
- **One Clean:** the device list is exactly the six factory presets (Dub Station, Boom Bap, House, Lo-Fi Tape, Ambient, Clean). The usbaudio bring-up images put an extra built-in "Clean" in front of the table, so Clean appeared twice there.
