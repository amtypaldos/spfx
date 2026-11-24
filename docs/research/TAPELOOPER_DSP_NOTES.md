# Tape Looper DSP notes (what makes it cheap, and what we can borrow)

Source studied: github.com/chattock/sp1-tape-looper, commit 0359372 (the pinned 0359372e, reachable in a
depth-50 clone). Full clone kept at `~/spfx/upstream/tl_full` (read-only reference, nothing run).
All paths below are relative to `tools/capacity422/runtime/src/` in that clone (the "v4 runtime"; see
`firmware/README.md`). The DSP is NOT in `firmware/`; it is in `main.c` (20,128 lines) plus `comb.h`.
Line numbers are for `main.c` unless noted. Cycle numbers marked (est.) are my estimates from reading the
code, NOT measurements; the upstream repo publishes no per-effect cycle counts.

## 1. Architecture facts

| Item | Tape Looper | Ours |
|---|---|---|
| Core / rate | nRF52840 64 MHz, 48 kHz stereo, I2S out, USB UAC2 in | same |
| Block | `BLK_FRAMES 256` (5.33 ms) :412, whole chain runs once per block | `FX_MAX_BLOCK 64` (fx.h:13), per-effect call per 64 |
| Sample format | int32 stereo mix bus (`mix32`, `mix32R`), int16-range values with headroom; int16 delay lines; Q8/Q12/Q15/Q16 coefficients | float everywhere, int16 only in delay lines |
| Float use | none in main.c (grep for float/double/sinf/expf/powf: zero hits). `CONFIG_FPU=y` is set but unused by the app. | float + libm (expf, powf, tanf, floorf, sqrtf) |
| Compiler | global `-Os` (prj.conf:77-81 says -O2 globally crashed on latent UB). Hot functions get `__attribute__((optimize("O2"), noinline))`: `fx_chain_run` :4657, `rv_step` :4619, `in_wobble_block` :5831, `healthy_mix_block`, `looper_audio_block` :6979. `comb_render` uses O3 (comb.h:74). `-fno-strict-aliasing` only. | see our CMake; not changed by this study |
| Threads | audio thread `K_PRIO_PREEMPT(0)` (:13007-13009), streamer PREEMPT(5), MIDI PREEMPT(6), main 8; preemptible on purpose so the COOP USB threads can run their ~100 us ISO service (comment at :13010+). Soft-PWM LED timer is a zero-latency IRQ (prj.conf). | n/a |
| Kernel overhead trimmed | `CONFIG_THREAD_RUNTIME_STATS=n` (~0.5%/switch), `HW_STACK_PROTECTION=n` (~1%), hardware PWM LEDs | check ours |
| Measurement | DWT->CYCCNT around `looper_audio_block` (:12927-12930), per-phase cycle accumulators (M73/M81/M82), per-FX "which single effect is up" census `g_fxs_aus[]` with overrun count when block > 5333 us (:12950-12965). Instrument name "W4Y / corner" = 4 tracks at 1.5x with USB in. | declared `cpu_cycles` estimates only |
| Stated limits | CHANGELOG.md:120/158: "Four stereo tracks at 1.5x with USB in and echo + reverb at max: six minutes, zero dropouts. Heavy stacks on top of that can still drop out." :178, :220: a bounce at the full corner can overrun. A code comment at :4980 says the chain runs with "~4% idle" at the corner. So THEY ARE ALSO CPU-TIGHT; they get there by doing very little per sample, not by magic. |

Important context: Tape Looper's mixer, 4 track streams, USB, eMMC and the FX all share the 64 MHz core, and
it has no 11-effect parallel stack: page-based FX where at most a handful run at once, each gated off by a
per-block test (see 2.1).

## 2. Per effect

### 2.1 Chain structure (`fx_chain_run` :4657-5772)
* Per-block constants are computed once in the preamble (:4672-4990): LFO increments (one udiv per engaged lane,
  :5001-5006), gains, filter mode/coeff target, drive curve (exp via shift + linear interp of 2^x, :4925-4938),
  hiss levels. Fader values (volatile) are read once into locals.
* `fx_any` (:4998) gates the whole chain; each effect is then its own tight loop over the 256-frame buffers
  ("effect-major", comment :4982-4996: bit-identical to per-frame order, but only that effect's constants are live
  in registers). An off effect costs one branch per BLOCK. Order: wobble -> hiss -> crackle -> pink -> comp ->
  drive -> tone -> filter -> chorus/flanger -> distortion -> gate -> slow gear -> lo-fi/radio -> bitcrush -> ring
  -> wah -> phaser -> sweep -> tremolo -> echo+reverb -> EQ.
* Effects run IN PLACE on the shared int32 bus. There is no per-effect copy, input-gain smoothing, NaN check or
  dry/wet crossfade loop; the wet amount is a per-block integer applied inside the kernel (`x += (w*mix)>>8`).
* Parameter changes ramp per block (e.g. filter coefficient moves 1/8 of the way per block, :4690-4700;
  distortion gain same, :4944-4950) -- zipper-free with ZERO per-sample smoothing cost.

### 2.2 Per effect table

| Effect | Algorithm | Format | Coeff update | LFO / transcendental | Interp | Memory | Rate tricks | Where |
|---|---|---|---|---|---|---|---|---|
| Reverb | Clouds-style: 4 input allpasses, 2 loops each (damp LP + 2 allpasses + delay), all in ONE 4096-word int16 ring with one shared decrementing pointer | int32 math, int16 store, Q8 gains (kap=160), `soft_limit` on loop stores | per block: krt, klp | none (no modulation) | none (integer taps), wet interpolated over 4 frames | 8 KB RAM `g_space` union (shared with comb engine) :4594; lengths are Clouds' at 32 kHz rescaled to 12 kHz :4586-4589 | **runs at 12 kHz mono-in/stereo-out: 64 `rv_step` per 256-frame block**, input is boxcar-4 sum of L+R, output linearly interpolated back across the 4 frames from the PREVIOUS step (1 step latency) :5725-5748 | :4619-4638, :5725 |
| Echo | mono line, soft-limited feedback | int16 line at 12 kHz | per block | none | linear, 2-bit fractional (`_dd>>2`,`>>1`) across the 4 frames :5700-5716 | 4544 x int16 = 9 KB (`EC2_LINE`) :3861 | **12 kHz boxcar-4 down/up**, 64 line samples per block | :5670-5722 |
| Echo+Reverb | fused into one group loop so the boxcar-4 sum is computed once (CPU-780 C3) | | | | | | | :5640-5688 |
| Chorus | one MONO int16 line, two reads at LFO phases 90 deg apart = stereo | int | span per block | **triangle LFO** from phase top bits (no table, no sin) :5290-5301 | linear Q8 | 1024 x int16 = 2 KB `g_chr_buf` :2403 | none | :5272-5319 |
| Flanger | same line, 0.25-5 ms, feedback 0.55, half sweep rate | int | per block | triangle | linear Q8 | shared with chorus (no extra RAM) | none | :5249-5271 |
| Filter | Chamberlin SVF, L and R, LP/HP/BP crossfade | int32, coefficient `f<<17` so `(f*v)>>14` = ONE `smmul` (`CPU780_MULH14` :4447) | per block, coefficient slews 1/8 of gap per block; state re-primed on mode change :4690-4720 | exp tables for cutoff (`flt_lp_tab[14]`, `flt_hp_tab[14]`, Q14 f values) :4041-4046; no tanf/expf at runtime | n/a | 28 B table | none | :5211-5247 |
| Distortion | cubic soft clip / hard clip / fold, drive+trim | int32 with int64 product on the gain | per block, ramped; drive 2^x by shifts+linear lerp, trim = 16777216/dst_g once per block | none | n/a | 0 | none | :5322-5370 |
| Tape drive | Pade tanh on Q12 (`tp_tanh_q12`) with one integer divide | int32 | per block | rational, no libm :3825-3833 | n/a | 0 | none | :5182-5192 |
| Bitcrush | sample-and-hold + AND mask | int32 | per block mask/hold | none | n/a | 0 | none | :5450-5470 |
| Lo-fi / Radio | crush + 1-2 pole LP, band + tanh + envelope-driven static; **block gate: if peak < 96 the block is skipped** :5420-5424 | int32/int64 | per block | none | n/a | 0 | peak scan every 2nd frame | :5410-5449 |
| Tremolo | triangle LFO from phase bits, gain `128-(tri*depth>>9)`, R phase offset, shape via `trm_shape` only when shape != 128 (two loops) | int32 | inc per block (1 udiv) | triangle | n/a | 0 | none | :5598-5637 |
| Sweep (auto-pan) | triangle LFO | int | per block | triangle | n/a | 0 | | :5580-5597 |
| Phaser | 4 first-order allpass stages, per-channel pass with all 8 state words in registers; the coefficient LFO is computed ONCE into `_pas[256]` and used by both channels (CPU-781 C4) | int32 | per frame table, not per channel | triangle, 700..3800 Q12 | n/a | 512 B stack | none | :5536-5579 |
| Tape wobble | NOT per-sample in the effect: `wob_tick_block` (:5774) runs once per block: two table-sine LFOs (256-entry Q15 `wob_sin_q15`, linear interp, :3771-3809) + two slewed noise terms, builds a target read offset; `in_wobble_block` (:5831) then ramps the offset LINEARLY per frame (`_o += _d`) and does a 2-tap interp on an interleaved stereo int16 line. Range check hoisted out of the loop (CPU-780 C5) | Q16 offset, int16 line | per block | table sine, per block only | linear, Q15 frac | 768 frames x 2 x int16 = 3 KB :2515 | none | :5774-5830, :5831-5888 |
| Compressor | peak of every 4TH frame over the block -> one envelope update (instant attack, release 1/16 per block) -> one target gain via ONE integer divide -> per-frame gain slew `g += (tg-g)>>6` + 2 mults | int32 | per block | none (T/(T+over*k) is a rational) | n/a | 0 | peak scan at 1/4 rate | :5154-5181 |
| Hiss / noise | 16384-entry int16 table, index +1 with random skip; R reads backward from half a table away. No filter at runtime | | | table (flash) | n/a | 32 KB flash `tp_hiss_tbl` :2655 | | :5012-5035 |
| EQ | shelf/peak pairs with 3 loop variants chosen per block so a gain-0 band costs nothing (CPU-781 C8); `EQ_STEP` = ONE smmul | int32 Q30 | per block | none | n/a | 0 | | :3920-4040 |
| Comb engine (alt reverb) | 4 voices, dual-16-bit `smlad` 32-tap FIR decimator, effect-major | int | per block with 64-step ramps | none | 12-bit fraction | shares reverb 8 KB | 12 kHz line (64 steps per 256) | comb.h:60-100 |

Delay memory is all RAM (int16 arrays; RAM image 262,044 B total in `release.json`). Only constant tables
(hiss 32 KB, sine 512 B, filter coeffs, comb tables) are in flash.

## 3. Techniques that make theirs cheaper, ranked by estimated saving for OUR code

Baseline: ours is ~2,400 cycles/frame for 4 effects, i.e. ~600 cycles/frame/effect (measured by the other
agent), budget ~1,333 total. Estimates below are per effect per 48 kHz frame, from instruction counting by eye.

1. **Quarter-rate (12 kHz) mono reverb and echo.** Reverb: ours = 4 lines, 2 allpasses + pre-delay, 4 LFO
   interpolated reads (float), Hadamard, per-frame at 48 kHz (`reverb.c:132-176`, declared 320, probably more in
   practice). Theirs: `rv_step` once per 4 frames, ~25 integer ops + 10 int16 loads/stores, plus a 4-frame
   linear fade (`main.c:4619-4638`, `:5725-5748`). Est. 250-350 -> ~50 cycles/frame, **saves ~200-300/frame for
   the reverb alone.** Same for delay: our `delay.c:79-96` does 2 float interp reads + 2 float->int16 writes +
   glide per frame (~150+ est.); theirs 12 kHz mono boxcar (`main.c:5700-5722`) ~25/frame, **saves ~100+**.
   Cost: 6 kHz bandwidth on reverb/echo; the "dark repeats" character. Acceptable for reverb tails, delay
   optional (keep 24 kHz if tone matters).
2. **No per-effect engine glue.** Our `process_chunk` (`engine.c:452-510`) per slot does: copy dry (n), in-gain
   smoothing + scale (n), a checksum sum (n), then per-sample mix smoothing with a lerp (n) -- 4 extra loops
   over each 64-frame chunk, plus `fx_get()`, `push_params`, and an `fx_bad` check. Est. 25-40 cycles/frame/slot
   = **100-160 for 4 slots.** Theirs mixes in place with per-block integer wet and a block-ramped coefficient.
   Do: ramp wet/in-gain per CHUNK (linear interp over n, or per-block step), not an IIR per sample; do the NaN
   check once per slot per chunk on the state (cheap) or only in debug builds; merge dry copy into the wet loop.
3. **Replace per-sample transcendental/phase work with per-block work or triangle LFOs.** Ours per frame:
   `fx_sin01` + `fx_wrap01` twice (chorus/flanger `mod_delay.c:96-110`, ~40-60), three `fx_sin01` + noise +
   wrap in `tapewobble.c:84-107` (~100), two `wave01` + wraps in `tremolo.c:78-80` (~50), reverb 2 LFOs
   (`reverb.c:150-153`). Theirs: triangle from `phase>>15` (chorus `main.c:5290-5301`), table sine evaluated
   ONCE per block for wobble (`:5774-5830`), tremolo triangle `>>23` (`:5598-5637`), phaser LFO into a 256-entry
   int16 array once and shared by both channels (`:5541-5551`). Est. **~100-200/frame across the four
   modulation effects.** Do: phase in uint32 (wrap free), no `fx_wrap01`; LFO value computed every 8-16 frames
   and linearly interpolated (delay-time modulation is smooth at that rate) or once per block with per-frame
   linear ramp as `in_wobble_block` does.
4. **Compressor: gain computer at block rate.** Ours does `fx_log2` + `fx_exp2` per sample (~110 cycles,
   `compressor.c:60-71`). Theirs: peak every 4th frame, ONE divide per block, per-frame multiply by a slewed gain
   (`main.c:5154-5181`). Est. **~70-90/frame saved.** Do: peak-detect over the chunk (or every 2nd/4th frame),
   compute target gain once per chunk (log2/exp2 or even a divide), slew the gain linearly across the chunk.
5. **Filter: no libm in the audio path, cheap coefficient path.** Ours calls `expf`, `tanf`, `sqrtf`, a
   division and a 4-way `switch` per chunk of 64 (`filter.c:64-100`): ~800-1200 cycles per chunk = 12-20/frame,
   and two fx_tanh drive calls/sample when drive is on. Theirs: 14-entry exp table + per-block slew + SVF with
   `smmul` (`main.c:4041-4046`, `:5211-5247`). Est. **~15-30/frame** (more if drive is on). Do: LUT or
   polynomial for g from a log-cutoff index, hoist the type switch out of the loop, skip `fx_tanh` unless drive.
6. **Integer delay-line access (Q16 frac, int16 interleaved stereo).** Our `fx_dl_write` does float scale,
   round, 2 compares, int16 store; `fx_dl_read` does clamp, 2 casts, 2 int->float, wrap logic, float lerp, scale
   (`util.h:97-138`). Called 2-6 times per frame in several effects (~25-35 cycles per read/write pair). Theirs
   keeps the bus in integers, power-of-2 ring masks (`CHR_MASK`, `RV_MASK`) instead of compare-and-subtract
   wrap, and interleaved L/R (`g_inw_line[2*w]`, :5856). Est. **~15-30/frame/effect, ~60-100 over 4 effects.**
   Do: power-of-2 sizes with `& mask`, a Q16 `{int,frac}` read position, keep the chunk's input in int16/int32
   fixed point for the delay-based effects.
7. **Per-block soft-parameter ramps instead of per-sample one-poles.** Saturation (`saturation.c:37-58`),
   delay glide (`delay.c:80-85`), tremolo gain slew (`tremolo.c:79-83`) and engine mixing all run a one-pole
   per sample per parameter. Theirs ramp once per block (e.g. `:4944-4950`, filter `:4695`). Est. ~5-10/frame
   each; adds up with item 2.
8. **Effect-major loops with all state in registers, and a block gate for off effects.** Their phaser loads and
   stores state once per block per channel (`:5555-5575`); `fx_any` skips the entire chain; lo-fi skips quiet
   blocks (`:5420`). Ours does `s->field` loads/stores via the struct pointer inside float loops (e.g.
   `tremolo.c:75-84` writes `s->g[]` per sample, `tapewobble.c:86,105` writes `s->wander/wow_ph` per sample).
   Est. ~5-15/frame/effect. Do: copy state to locals before the loop, write back after.
9. **Bigger block (256 vs 64) amortises per-call and per-chunk fixed costs.** Per-chunk coefficient work (item 5,
   reverb `fx_exp2` x4 at `reverb.c:126-129`, `powf`) is paid 4x as often with a 64 chunk. Raising
   `FX_MAX_BLOCK` to 128-256 costs stack (4 float arrays x n x 4 B = 4 KB at 256) and 2.7-5.3 ms latency
   granularity, which they already live with. Est. ~10-20/frame total.
10. **Integer approximations already proven bit-exact on their host harness**: `smmul` fixed-point multiplies
    (`CPU780_MULH14`, `EQ_STEP`), Pade tanh in int32 (`:3825`), rational compressor curve. On an M4F, FP MAC is
    single-cycle, so float is NOT inherently slower per op; our cost is concentrated in conversions, libm,
    branches, wraps and memory traffic. Converting the multiply-adds to fixed-point alone will give little;
    items 1-8 are where the savings are. (This is my inference, not something upstream states.)

Rough aggregate if items 1-6 are adopted for chorus+delay+reverb+compressor (our "4 effects ~2,400"): the
reverb ~300 -> ~60, delay ~150 -> ~30, compressor ~110 -> ~25, modulation ~120 -> ~50, engine glue ~120 -> ~30
=> on the order of 600-900 cycles/frame, i.e. inside the 1,333 budget for 4 effects but not leaving room for a
mixer/USB; treat as optimistic and re-measure with DWT after each step. Upstream's own headroom is thin and
documented as such (section 1).

## 4. Additional lessons from their code
* They ship zero-float, so lazy FPU stacking never costs them; if our audio thread runs float while other
  threads/ISRs also touch the FPU, check `CONFIG_FPU_SHARING` cost (not measured here).
* Disable kernel features that tax every context switch (runtime stats, HW stack guard) once debugging is done.
* Keep hot loops out of larger functions (`noinline` + per-function `-O2`): they saw +0.23 ms/block regressions
  from the compiler inlining/spilling (:4431-4440, :4982-4996). Verify with disassembly/spill counts if we copy
  the effect-major pattern.
* Make the fast path the default: the "lean" path costs ~nothing with all faders down; only engaged effects pay.
* Instrument the way they do: DWT cycle counts around the block, a per-active-effect worst-case table and an
  "over budget" counter, rather than declared estimates.

## 5. License and attribution
* Tape Looper (`LICENSE`, `LICENSES/Tape-Looper-LICENSE.txt`): MIT, "Copyright (c) 2026 chattock". Other component
  notices in `LICENSES/`: Zephyr (Apache-2.0), Nordic HAL/nrfx, CMSIS, Wakes-derived storage (MIT),
  Tim Knapen SP1-dev (MIT), FatFs, dr_libs, Three.js. Check each if we ever pull code near those.
* MIT permits copying, modifying and redistributing, including commercially, provided the copyright notice and
  permission text accompany "all copies or substantial portions". Ideas, algorithms and structure (the techniques
  above) are not copyrightable and need no credit, but crediting is courteous.
* If we COPY or closely adapt code (e.g. `rv_step`, the `CPU780_MULH14` macro, `wob_sin_q15` table, the phaser
  loop), add at the top of that file: `/* Portions derived from sp1-tape-looper (c) 2026 chattock, MIT License,
  commit 0359372; see THIRD_PARTY_LICENSES */` and append the full MIT text with that copyright line to our
  third-party notices file. Do not copy large blocks verbatim; re-implement against our own interfaces.
* The reverb network is Mutable Instruments Clouds' (FxEngine, lengths "Clouds' at 32 kHz scaled", main.c:4586).
  Clouds/stmlib are by Emilie Gillet and, as far as I recall, MIT-licensed, but that was NOT verified here;
  if we reproduce the topology and constants, check the Mutable Instruments repo license and credit it too.
* No files in `~/spfx/sp1-fx` other than this report were modified; no scripts from the clone were run.
