# ADR-001: Audio routing — USB audio in/out, headphone jack out, sync jack for tempo

Status: accepted (user decision, 2026-11-23/24). Supersedes the "analog stereo in" goal; see PHASE0_REPORT.md.

## Context
The SP-1 has no analog audio input path (CS42L42 ASP_SDOUT unconnected, no nRF I2S data-in pin; source findings, not yet confirmed on hardware). The second 3.5 mm jack is a digital sync jack (PO pulse on TIP, MIDI on RING), not audio.

## Decision
- **Input:** USB Audio Class 2 from a host (computer/phone). SP-1 is a USB device.
- **Output:** processed stereo to the **headphone jack** (CS42L42) and the internal speaker (TAS2505) as the upstream firmware does. Processed audio is also returned to the host over USB (UAC2 microphone/IN endpoint) so it can be recorded digitally. *(USB IN endpoint is new work: Tape Looper only implements the OUT/speaker direction.)*
- **Sync jack:** digital **output** only: MIDI clock/Start/Stop (24 PPQN) on RING, PO sync pulses (2 PPQN) on TIP. Scheduling is counted in audio frames (`firmware/app/sync_out.c`) so it cannot drift against the audio clock. Not audio.
- **Control channel:** CDC-ACM serial on the same USB connection (composite device), for the Web Serial companion.

## Consequences
- A host must sit between the EP instrument and the SP-1. Not standalone. Latency is dominated by host audio buffering; the <10 ms / <5 ms targets are **unverified** and may not be reachable on all hosts.
- Composite USB (UAC2 OUT + UAC2 IN + CDC) on Full Speed (12 Mbit/s) must fit the endpoint/bandwidth budget and nRF52840 USBD endpoint count: **untested**.
- Upstream's own note says MIDI sync out is "untested on real gear"; we inherit that uncertainty until measured.
- Windows needs upstream's UAC2 full-speed workaround patch (`patches/zephyr-uac2.patch` in Tape Looper).

## Rejected
Bluetooth input (latency), hardware mod for ASP_SDOUT (irreversible, unverified), USB host bridge, controller-only pivot.
