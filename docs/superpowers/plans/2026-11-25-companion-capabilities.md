# Companion capabilities + date versions: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The firmware declares its features in HELLO_RSP, and the companion grays out what is missing. The first date-versioned release is `v2026.11.25`.

**Architecture:**
- HELLO_RSP gains a trailing `caps u32`, set by the app in `proto_config_t.caps`.
- The companion's device->host decoders ignore trailing bytes. The model stores `caps`, which the UI uses to disable controls.

**Tech Stack:** C99 (firmware/protocol, host tests via `make test`), Zephyr 4.3.1 app, plain ES modules (companion, `node --test`).

**Spec:** `docs/superpowers/specs/2026-11-25-companion-capabilities-design.md`

## Global Constraints

- The spec is changed first (`docs/PROTOCOL.md`), as AGENTS.md requires.
- Bits:
  - SYNC = 1<<0
  - OUT_LINE = 1<<1
  - OUT_LOUD = 1<<2
  - BATTERY = 1<<3
  - SAVE = 1<<4
  - POWER_FLAGS = 1<<5
  - `CAPS_RC6 = 0x3F` (assumed when caps is absent).
- No change to the audio path. Firmware request parsing stays strict.
- Version string `sp1fx-2026.11.25`, tag `v2026.11.25`, GitHub pre-release, labelled NOT RUN ON HARDWARE.
- `make clean && make && make test`: 0 warnings, ALL TEST BINARIES PASSED. Companion tests all pass. No flashing.

## Review Focus

1. HELLO_RSP from rc6 firmware (no caps): companion shows everything enabled (CAPS_RC6). Pinned by a companion test.
2. Unknown future caps bits set (e.g. 0xFFFFFFFF): no error, and known controls are enabled. Pinned by a companion test.
3. Extra trailing bytes after caps / after METERS batt_pct / after STATE out_mode: parse succeeds. Pinned by a companion test.
4. Device->host EVENT carrying a setter with trailing bytes: accepted. Host->device requests with trailing bytes are still rejected by both the mock and the firmware. Pinned by a companion test.
5. Demo device and disconnected state: demo advertises all caps; when disconnected, controls stay disabled as before (the caps graying must not re-enable them).

---

### Task 1: Spec + firmware caps

**Files:** `docs/PROTOCOL.md`, `firmware/protocol/protocol.h`, `firmware/protocol/protocol.c`, `firmware/app/zephyr_sp1fx/src/main.c`, `tests/test_protocol.c`

**Interfaces:**
- Produces: `PROTO_CAP_SYNC..PROTO_CAP_POWER_FLAGS` and `PROTO_CAPS_RC6` macros in protocol.h, plus `uint32_t caps` in `proto_config_t`.
- The HELLO_RSP payload gains `caps u32 LE` after `cpu_budget`.

- [ ] PROTOCOL.md: document caps (bits, absence = rc6 set, hosts ignore unknown bits and trailing bytes), and SET_OUTPUT_MODE BAD_ARG for an undeclared mode.
- [ ] test_protocol.c: setup sets `c.caps = PROTO_CAP_SYNC|PROTO_CAP_OUT_LINE|PROTO_CAP_OUT_LOUD|PROTO_CAP_SAVE` (0x17). `test_hello_rsp_layout` checks len +4 and `g32(p+13) == G_HELLO_CAPS`. A new test, `test_output_mode_needs_cap`, uses caps without OUT_LOUD: mode 2 gives BAD_ARG and out_mode is unchanged; mode 1 is OK.
- [ ] Run `make test`; expect FAIL (no field yet).
- [ ] Implement: `build_hello` writes `w32(caps)`; SET_OUTPUT_MODE checks the bit; main.c sets all six bits.
- [ ] Task 2 regenerates the golden header; then `make test` passes.

### Task 2: Companion protocol (caps + tolerant decoding) and golden frames

**Files:** `companion/js/protocol.js`, `companion/js/mock-device.js`, `tests/gen_golden.mjs`, `tests/golden_frames.h` (regenerated), `companion/tests/protocol.test.mjs`

**Interfaces:**
- Produces: exports `CAP = {SYNC, OUT_LINE, OUT_LOUD, BATTERY, SAVE, POWER_FLAGS}` and `CAPS_RC6`. The HELLO_RSP message has a `caps` field (number). `decodePayload(type, payload)` ignores trailing bytes when `type >= 0x80` (device->host), including the EVENT inner setter.

- [ ] Tests:
  - HELLO_RSP round trip with `caps: 0x17`;
  - HELLO_RSP without caps decodes as `caps === CAPS_RC6`;
  - HELLO_RSP with caps 0xFFFFFFFF and 3 extra bytes;
  - METERS and STATE with 2 extra trailing bytes decode;
  - a SET_PARAM request with a trailing byte still throws 'trailing bytes';
  - an EVENT whose setter has a trailing byte decodes.
- [ ] Implement. The encoder writes caps when `msg.caps !== undefined`. The mock device sends `caps: this.caps ?? CAPS_RC6`.
- [ ] gen_golden: hello gets `caps: 0x17` and emits `#define G_HELLO_CAPS 0x17u`. Regenerate with `node tests/gen_golden.mjs > tests/golden_frames.h`.
- [ ] Run `make clean && make && make test` and `node --test companion/tests/*.test.mjs`: all pass, 0 warnings. Commit tasks 1 and 2 together (the golden header couples them).

### Task 3: Companion UI graying

**Files:** `companion/js/controller.js`, `companion/js/state.js`, `companion/js/ui.js`, `companion/index.html`, `companion/css/style.css`, `companion/tests/mock.test.mjs`

**Interfaces:**
- Consumes: `CAP`, `CAPS_RC6`, `hello.caps`.
- Produces: `model.caps` (number; the default is CAPS_RC6). `has(m, bit)` lives in state.js, alongside a pure `unsupported(m)` that returns an array of control ids for the UI and tests.

- [ ] Test in mock.test.mjs: connect to a MockDevice with `caps = CAP.OUT_LINE|CAP.SAVE`. Then `model.caps === 0x12` and `unsupported(model)` contains `'gSync'`, `'outMode:2'`, `'battery'`, but not `'outMode:1'`.
- [ ] Implement:
  - The controller stores `m.caps = hello.caps`.
  - renderConn: after the synced/disabled loop, disable the controls `unsupported(m)` lists.
  - Disabled `<option>`s for Loud and Line out.
  - Battery text "battery n/a" when connected without the BATTERY cap.
  - A hint `<p class="hint" id="capsHint" hidden>` with the text "Grayed-out controls need newer firmware (this device: <fw>)." Shown only when something is unsupported.
  - Controller `saveNow` throws an error message if SAVE is missing.
- [ ] Run the companion tests: they pass.

### Task 4: Date version, build, docs, release

**Files:** `firmware/app/zephyr_sp1fx/Kconfig`, `docs/STATUS.md`, `README.md`, `docs/BRINGUP_SP1FX.md`

- [ ] Kconfig default `"sp1fx-2026.11.25"`.
- [ ] Run `firmware/scripts/build_firmware.sh --app sp1fx repro`: REPRODUCIBLE, check PASS.
- [ ] STATUS.md: a new top entry with SHA-256, size and changes, labelled NOT RUN ON HARDWARE, plus a note on the version scheme change. README and BRINGUP: newest image name.
- [ ] Commit, merge `companion-caps` into main, push main, tag `v2026.11.25`, push the tag. (`pages-companion` stays unmerged until the repo is public: its workflow would run on the tag and fail while Pages is off.)
- [ ] Release: `gh release create v2026.11.25 --prerelease` with the `.bin`, `.elf`, companion zip (`git archive` at tag + LICENSE) and SHA256SUMS. Download the files back and run `shasum -c`.
