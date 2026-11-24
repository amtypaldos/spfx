# ADR-002: User-experience decisions (user answers, 2026-11-24)

| Question | Decision | Consequence |
|---|---|---|
| Host | Both computer and phone/tablet | Must work with USB audio class 2 on macOS/Windows and be tolerant of mobile hosts. Web Serial companion works on desktop Chromium only; iOS needs a separate approach later. |
| Processed audio | Returned to the host over USB as well as the headphone jack | Composite USB: audio OUT (host to SP-1), audio IN (SP-1 to host) and CDC serial. New firmware work; bandwidth and endpoint count untested. |
| Effect-selection LEDs | Fixed order, **no numeric display** | While cycling, the held channel's LED pulses; a successful change flashes the four LEDs beside PLAY; None shows a steady dim row; a refused change flashes them alternately. The printed card and the companion list the cycling order. |
| Tempo | Companion only at first | Manual BPM over the serial protocol. No tap-tempo gesture, no MIDI clock input yet. PLAY+FN stays unassigned. |
| Foundation | Reuse Tape Looper's MIT drivers with attribution | Board definition, codec, I2S, ADC ladder, USB audio and bootloader-safe lifecycle code will be copied in with notices in THIRD_PARTY_NOTICES.md. Bluetooth components are not used or redistributed. |

Hardware/test policy (user): the user keeps one SP-1 running Tape Looper that is free for experiments. No backup procedure is required for it. Flashing still needs an explicit go-ahead each time.
