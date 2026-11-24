# AGENTS.md

Project: SP-1 FX, custom effects firmware for the Teenage Engineering SP-1.

Rules:
- Never flash, erase, or modify bootloader/BT firmware/eMMC without explicit user approval. Keep build/simulation separate from deployment.
- LEDs are monochrome. No colour-dependent UI.
- Label every claim: verified on hardware / source finding / simulated / untested assumption. Never fabricate hardware results.
- Pin every upstream to a commit; record license in THIRD_PARTY_NOTICES.md. Do not copy softmodded/marisko (no license).
- No dynamic allocation, logging, or storage writes in the audio path.
- Current state: docs/STATUS.md. Analog input is impossible on this hardware (docs/PHASE0_REPORT.md); the decided routing is USB audio in/out + headphone out + sync-jack out (docs/ADR-001, ADR-002).
- Companion and firmware must follow docs/PROTOCOL.md; change the spec first if they disagree.
