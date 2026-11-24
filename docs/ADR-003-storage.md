# ADR-003: Persistent configuration

- Location: **two 4 KB internal-flash pages inside our own app region** (not the 0xFF000 page that the upstream board file reserves as `storage`), so nothing outside our image is touched. Exact addresses are set by the linker script when the firmware build exists; the addresses must be inside the app slot (0x20000..0xFEFFF) and excluded from the image.
- eMMC is **not used** for settings; the user's Tape Looper library lives there.
- Record: 512 B (magic, layout version, length, sequence, payload, CRC-32), 8 per page, append-only; newest valid sequence wins; a page is erased only when the other page holds the newest record.
- Payload: layout v1 in `firmware/storage/config_store.c` (also reused by the protocol STATE message). Floats are stored as 16-bit fixed point. Remembered parameters for every effect in every slot are kept. Unknown future layouts fall back to defaults.
- Wear: one save per 1.5 s of quiet and at most one per 5 s. 8 saves per page, two pages: about 16 saves per erase pair. nRF52840 flash is rated for 10,000 erase cycles per page (datasheet figure, not verified here), so well over 100,000 saves.
- Never called from the audio thread. Erase stalls the CPU on nRF52840 (the flash controller halts bus access for up to ~85 ms per page erase); the firmware must perform erases while audio is muted or in a thread that tolerates the stall. **Open item: this is unverified, and needs measurement on the device.** Until measured, save only when no audio underrun can result, e.g. on a short fade or when idle.
- Tested on the host with a NOR-semantics mock flash, power loss injected at every 7th byte of a record write, an interrupted erase, bit flips and fully corrupted pages.
