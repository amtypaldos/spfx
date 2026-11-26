# SP-1 FX companion (browser app)

Static, dependency-free web app (plain HTML/CSS/ES modules, no build step) for the SP-1 FX firmware project. No cloud, no accounts, no analytics, no external requests at runtime.

## Status, honestly
- **Works with a real SP-1** (owner report, 2026-11-25, over USB with Web Serial). The firmware side of `docs/PROTOCOL.md` is `firmware/protocol/` (in the integrated image since v1-rc1), byte-identical to this app on the shared golden frames. Not recorded in detail: which firmware image and which controls were exercised.
- **Browsers:** connecting needs Web Serial, i.e. desktop Chrome or Edge. iPhone and iPad browsers (all WebKit), Safari and Firefox can't connect; they show a notice, and the demo device and preset library still work. Android browsers are untested: the page detects Web Serial and only enables Connect where it exists.
- Everything else has been checked against the **built-in demo device** (`js/mock-device.js`), a simulation written from the spec. It speaks the real COBS+CRC wire format through the same client code path, but it is not evidence that the firmware behaves the same way.
- Protocol gaps and assumptions: see `PROTOCOL_NOTES.md` (notably: the STATE message is 383 bytes, over the spec's 250-byte payload limit).

## Run
Two ways, same app:

- **Hosted (GitHub Pages):** open <https://amtypaldos.github.io/spfx/>. It is deployed by `.github/workflows/pages.yml` from each release tag (`v*`), after the tests pass, so it always matches the latest firmware release.
- **Locally:** use this for unreleased changes, or for a companion that matches an older firmware image (check out that image's tag first). Each release also attaches `sp1fx-companion-<tag>.zip`, which is this folder at that tag.

```
cd companion
python3 -m http.server 8137      # then open http://localhost:8137/
```

A newer firmware's STATE/METERS format can be rejected by an older companion (and the other way round), so use the companion from the same release as the image on the device.

Locally, any static server works (a module script and a `fetch` of `data/fx_table.json` are used, so `file://` will not). Desktop Chrome or Edge is needed for **Connect SP-1** (Web Serial). Other browsers (Safari, Firefox, iOS) show a notice; the demo device and the preset manager still work there.

## Tests
```
cd companion
node --test tests/*.test.mjs        # Node 22: node
```
Covers COBS (incl. zero bytes, 254/255-byte runs, random), CRC known vector (0x29B1), framing and corrupt-frame rejection (every single-bit flip), every message type round trip, STATE decode, preset validation (valid/malformed/hostile/wrong version), library behaviour (persistence, missing/throwing storage), scale mapping, admission arithmetic, and the mock device through the real client and controller (admission errors code 2 and 3, no echo of host changes, device events, retries, rate limit, preset apply).

`tests/browser-smoke.html` drives the real UI in headless Chrome (demo device): slot assignment, parameter drag coalescing, fader events, admission error message, preset load/save/export/import (incl. a hostile name), disconnect. It was run with `--dump-dom` ; it needs a static server on the folder, e.g. `chrome --headless=new --virtual-time-budget=30000 --dump-dom http://localhost:8765/tests/browser-smoke.html` and read the `RESULT{...}END` text. Add `?w=390` for phone width, `?theme=light|dark` to force a theme.

## Layout
- `index.html`, `css/style.css`: UI. Look and feel modelled on the SP-1 Tape Looper companion site (MIT, (c) 2026 chattock): palette, glass top bar, rounded cards, pill tabs, eyebrow labels. The CSS was re-written, not copied; the MIT notice is kept in `LICENSES/Tape-Looper-MIT.txt` anyway. No three.js, Bluetooth, or firmware-install code was used. This app is not Tape Looper and uses none of its branding.
- `js/protocol.js` COBS, CRC16, framing, all message codecs (no DOM). `js/fx.js` effect table, scale mapping, admission. `js/client.js` request/response with retries and a coalescing rate limiter. `js/controller.js` store <-> device glue, preset apply. `js/state.js` model. `js/presets.js` schema validation, factory presets, library. `js/mock-device.js` demo device + loopback transport. `js/device.js` Web Serial. `js/ui.js`, `js/app.js` DOM and wiring.
- `data/fx_table.json` fallback effect table (a connected device sends the same data via GET_FX).

## Behaviour notes
- Host edits send setters, coalesced to about 29 messages/s while dragging; device events (0xA0) update the UI and are never echoed back. A rejected change (result 2 or 3) re-syncs from the device and shows e.g. "Not enough DSP memory for Delay in slot 3; slot unchanged."
- Presets live in `localStorage` (try/catch; falls back to memory with a warning). Import validates schema `sp1fx.preset` version 1 only, clamps numbers, ignores unknown keys, rejects unknown effects, and renders names with `textContent`.
- All six factory presets fit the admission limits using the table's numbers (largest: House and Ambient at 126,064 B of 153,600 B, Dub Station 119,920 B; CPU 560-590 of 850 cycles). "Dub Station" uses plain Delay as the stand-in for a dub delay.
- Loading a preset sends slot assignments first, ordered so memory is freed before it is claimed, then the individual setters.
