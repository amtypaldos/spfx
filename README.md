# SP-1 FX

Custom multi-effects firmware project for the Teenage Engineering SP-1 Stem Player: four assignable stereo effect slots driven by the SP-1's faders and buttons.

**Status: host-side development only. Nothing has been flashed to an SP-1.** See `docs/STATUS.md`.

Audio path (ADR-001): USB audio in from a host -> 4-slot FX engine -> headphone jack / speaker, and back to the host over USB. The sync jack outputs MIDI clock / PO sync. The SP-1 has no analog audio input (`docs/PHASE0_REPORT.md`).

```
make          # builds build/sp1fx_sim and the tests
make test
build/sp1fx_sim --list
build/sp1fx_sim in.wav out.wav --slot 1=filter --param 1.1=800 --slot 2=delay --wet 2=0.4 --tail 2
```

Layout: `firmware/dsp` (effects + engine, plain C99), `firmware/controls` (gestures, takeover, LEDs), `firmware/app` (sync), `simulator/`, `tests/`, `docs/`. Upstream sources and licences: `THIRD_PARTY_NOTICES.md`. Rules for contributors/agents: `AGENTS.md`.

Licence: not yet chosen (no code from upstream has been copied in).
