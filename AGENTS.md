# AGENTS.md

Project: SP-1 FX, custom effects firmware for the Teenage Engineering SP-1.

Rules:
- Never flash, erase, or modify bootloader/BT firmware/eMMC without explicit user approval. Keep build/simulation separate from deployment.
- LEDs are monochrome. No colour-dependent UI.
- Label every claim: verified on hardware / source finding / simulated / untested assumption. Never fabricate hardware results.
- Pin every upstream to a commit; record license in THIRD_PARTY_NOTICES.md. Do not copy softmodded/marisko (no license).
- No dynamic allocation, logging, or storage writes in the audio path.
- Current state: see docs/PHASE0_REPORT.md. The analog-input feasibility gate failed; the input transport awaits a user decision.
