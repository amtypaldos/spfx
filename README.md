# SP-1 FX

custom firmware for the SP-1 stem player. turns it into a four-slot stereo effects box.

audio in over USB from a computer or phone. effects. out to headphones or the speaker.

unofficial hobby project. not affiliated with Teenage Engineering or Kano.

![SP-1 FX controls guide](docs/guide/sp1fx-guide.png)

[guide as PDF](docs/guide/sp1fx-guide.pdf)

## status

development firmware.

- **current:** [`v2026.11.25`](https://github.com/amtypaldos/spfx/releases). tested by the owner: works decently well, some minor bugs.
- **newest:** `v2026.11.25.2` (pre-release, not run on hardware). adds the jack-level switch.

details and test history: [docs/STATUS.md](docs/STATUS.md).

## effects

four slots in series. each slot holds one effect, with its own wet/dry and bypass.

filter, chorus, delay, reverb, saturation, flanger, bitcrusher, tremolo, compressor, tape wobble.

each effect has four parameters, set with the faders in edit mode.

## controls

| do this | to |
|---|---|
| hold T1-T4 + FWD/REV | change that slot's effect |
| fader 1-4 | wet/dry of slot 1-4 |
| tap T1-T4 | bypass that slot |
| FN + T1-T4 | edit that slot's four parameters. tap FN to go back |
| FN + FWD/REV | next / previous preset |
| tap PLAY | bypass everything |
| VOL- / VOL+ | volume, 2 dB steps |
| hold VOL- + VOL+ 3 s | headphone-jack level: headphones › loud › line out (plug in first) |
| hold FN 4 s | power on / off |

every start is at headphones level.

**LEDs:**

- **track LEDs:** wet level of each slot. off = bypassed.
- **LEDs beside PLAY:** output level. after VOL: the volume bar.
- **a slow blink:** the slot was switched off to keep the audio running.
- **off with USB plugged in:** battery gauge.

when a fader changes role its LED blinks until you move it to the stored value. then it picks up. nothing jumps.

## companion app

presets, tempo, sync and the jack level from the browser. connects over USB in desktop Chrome or Edge.

open <https://amtypaldos.github.io/spfx/>, or run it locally: [companion/README.md](companion/README.md).

## jacks

- **headphone:** audio out.
- **sync:** MIDI clock and pocket operator sync out. not audio.
- **USB-C:** audio in, charging, companion app.

there is no analog input on this hardware.

## install

1. download the `.bin` and `SHA256SUMS` from [releases](https://github.com/amtypaldos/spfx/releases). run `shasum -a 256 -c SHA256SUMS`.
2. open <https://solderless.engineering> in desktop Chrome.
3. turn the SP-1 off. hold **T1 + T4** and plug in USB. let go when T1 lights.
4. flash the `.bin`.

keep a recovery image: Tape Looper's own release. turn the volume down and keep headphones off your ears for the first start.

safety notes and every image: [docs/IMAGES.md](docs/IMAGES.md).

## build

```sh
make test                                          # host tests, no hardware
firmware/scripts/build_firmware.sh setup           # toolchain, once
firmware/scripts/build_firmware.sh --app sp1fx     # firmware image
```

needs an Apple silicon Mac. details: [docs/BUILD.md](docs/BUILD.md). protocol: [docs/PROTOCOL.md](docs/PROTOCOL.md). contributors: [AGENTS.md](AGENTS.md).

## known limits

- windows is not supported.
- the computer can't set the volume over USB. use VOL- / VOL+.
- changing an effect cuts the old effect's tail.
- saturation and bitcrusher alias at high settings.

more: [docs/STATUS.md](docs/STATUS.md).

## credits

- **[Tape Looper](https://github.com/chattock/sp1-tape-looper)** by chattock (MIT): board definition, codec, controls, sync and USB code this builds on. also the model for the companion's look.
- **SP-1-dev** by Tim Knapen (MIT): pin map and hardware docs.
- **sp1-midi** by Eric Lewis (MIT): board devicetree reference.
- **Zephyr RTOS**, CMSIS, nrfx and picolibc: linked into the images. licences in [docs/BINARY_NOTICES.md](docs/BINARY_NOTICES.md), which ships with every image.
- **the Marisko firmware guide:** the layout idea for the controls guide. no artwork or text reused.
- **solderless.engineering:** the browser flasher.

the SP-1 hardware, name and branding belong to Teenage Engineering and Kano. Bluetooth firmware is not part of this project.

full list: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## licence

MIT. see [LICENSE](LICENSE). copied or adapted files name their source and keep their licence header.
