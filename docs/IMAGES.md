# SP-1 FX: images and flashing

## Safety

- **Nothing in this repository flashes anything, and the firmware is never flashed automatically.** Flashing is a manual step that the owner does.
- Flashing replaces the current app. Check the image name and SHA-256 before you flash.
- Never write to the bootloader (0x00000-0x1FFFF), the Bluetooth module, eMMC, or the storage page (0xFF000). The images are linked to the app slot at 0x20000 and `check_image.py` enforces that. Do not erase storage. The integrated firmware (`sp1fx-*.bin`, from v1-rc1 on) itself writes only its two settings pages 0xFD000-0xFEFFF, inside the app slot and outside its image (`check_image.py --reserve`, `docs/ADR-003-storage.md`).
- The SP-1 has no reset pin. An app that does not feed the watchdog (every 5 s) boot-loops until the bootloader is re-entered. An app that cannot reach SYSTEM_OFF can only be recovered through the bootloader key combination.
- **Keep a recovery image ready.** Tape Looper's own release image, downloaded from Tape Looper's GitHub releases (the owner's copy is `RECOVERY-tape-looper-from-repo.bin`, SHA-256 `e73e3786...badbf`), is the known-good fallback. It is not redistributed here. The tone and minimal heartbeat images have also booted on the owner's unit.
- Start with the computer volume low and the headphones off your ears. USB audio images have a fade-in and a digital trim, but the analog gains are not yet measured.

## Flash

1. Open desktop Chrome (WebUSB) at <https://solderless.engineering>.
2. Turn the SP-1 off. Hold **T1 + T4** while you connect USB, and release when **T1 lights**. The device is now in the TE bootloader.
3. Check the image against its `SHA256SUMS`, then flash it from the Solderless page.
4. To recover from a bad image, repeat step 2 and flash a known-good image.

## Where to get images

Flash images are not in git. Released images are on the [GitHub releases page](https://github.com/amtypaldos/spfx/releases): each release has the `.bin`, the `.elf` (for `addr2line`), the matching companion zip, `BINARY_NOTICES.md` and `SHA256SUMS`. Download them into one folder and run `shasum -a 256 -c SHA256SUMS` there. Releases not yet run on hardware are marked as pre-releases. Published so far: `v1-rc6`, `v2026.11.25` and `v2026.11.25.2`. All other images in the table below are local builds: rebuild them from source (`docs/BUILD.md`). The owner's workflow stages images in a local `flash-ready/` folder outside the repository (copies from `build/firmware/...` with SHA-256 checked, verified the same way).

## All images

| Image | Purpose |
|---|---|
| `sp1fx-min-heartbeat.bin` | Minimal heartbeat and power-off image. Boots and blinks LED 1. |
| `sp1fx-tone.bin` | Audio bring-up: quiet test tone (left low, right high). Confirmed on the owner's unit. |
| `sp1fx-controls.bin` | Buttons, faders, LEDs, effect and preset cycling, with the engine in the loop. Confirmed on the owner's unit. |
| `sp1fx-sync.bin` | MIDI clock and PO sync output. Serial log confirmed. Real gear not separately confirmed. |
| `sp1fx-usbaudio-in.bin` (v1) | USB audio in, A. Crashes on the first preset change. Superseded. |
| `sp1fx-usbaudio-in-v2.bin` | USB audio in, A, v2 (crash mitigations and crash dump). Ran on the owner's unit: no crash, but every effect preset overloaded (170-200 % CPU). Superseded by v3. |
| `sp1fx-usbaudio-io.bin` (v1) | USB audio in and return to host, B. Not run on hardware. Superseded. |
| `sp1fx-usbaudio-io-v2.bin` | USB audio in and return to host, B, v2. Not run on hardware (no result reported). |
| `sp1fx-usbaudio-in-v3.bin` | USB audio in, A, v3 (optimised DSP, overload guard). Runs on the owner's unit (presets ~50 % CPU). |
| `sp1fx-v1-rc1.bin` | Integrated firmware rc1. **Resets by watchdog on hardware.** Superseded. |
| `sp1fx-v1-rc2.bin` | Integrated firmware rc2 (reset fixes + stall recorder). Ran on the owner's unit; FN-hold power off too easy to hit. Superseded. |
| `sp1fx-v1-rc3.bin` | Integrated firmware rc3: controls fix, 4 s power on/off, charge standby. Ran on the owner's unit; control-loop starvation near-misses under heavy load. Superseded. |
| `sp1fx-v1-rc4.bin` | Integrated firmware rc4. Superseded by rc5 (not run). |
| `sp1fx-v1-rc5.bin` | Integrated firmware rc5 (three jack levels, feedback saturation). Worked on the owner's unit. Superseded by rc6. |
| `sp1fx-v1-rc6.bin` | Integrated firmware rc6 = rc5 + battery fuel gauge in the charge standby (off + USB), battery % in console/companion, no charging LED while on. GitHub release `v1-rc6`. No hardware result reported for rc6 itself. Needs the companion from the same commit or later. Superseded by 2026.11.25. |
| `sp1fx-2026.11.25.2.bin` | **Integrated firmware 2026.11.25.2 (newest, pre-release)** = 2026.11.25 + VOL- and VOL+ held 3 s change the headphone-jack level; the companion follows level changes made on the device. GitHub release `v2026.11.25.2`. NOT RUN ON HARDWARE. |
| `sp1fx-2026.11.25.bin` | **Integrated firmware 2026.11.25 (current release)** = rc6 + feature list for the companion (it grays out what the firmware lacks), date-based version, no build paths in the ELF. GitHub release `v2026.11.25`. Owner report: works decently well, some minor bugs not yet itemised. Works with this and later companions. Test plan: `docs/BRINGUP_SP1FX.md`. |
| `RECOVERY-tape-looper-from-repo.bin` | Tape Looper release (from Tape Looper's GitHub releases), recovery only. Not built from or redistributed by this repository. |

Bring-up procedures and pass/fail checklists are in `docs/BRINGUP_SP1FX.md` (integrated firmware), `docs/BRINGUP_TONE.md`, `docs/BRINGUP_CONTROLS.md`, `docs/BRINGUP_SYNC.md` and `docs/BRINGUP_USBAUDIO.md`.
