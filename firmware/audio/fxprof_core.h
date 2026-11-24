/* SP-1 FX: effect CPU profiling core (no Zephyr dependency).
 *
 * Shared by the profiling firmware (firmware/app/zephyr_fxprof), the host test
 * (tests/test_fxprof_core.c) and the instruction-count emulation
 * (tools/fxprof_emu). It defines the list of measured cases, builds the engine
 * config for each, and generates the synthetic input signal. It never times
 * anything itself: the caller wraps engine_process() with its own clock (DWT
 * cycle counter on the device).
 *
 * Cases (FXPROF_CASES in total):
 *   Clean          the engine with every slot empty (fixed per-block overhead)
 *   <effect> def   effect alone in slot 0, default params, wet 100 %, enabled
 *   <effect> worst effect alone, params chosen for the most work per sample
 *   <effect> auto  default params, one parameter swept EVERY block (a moving
 *                  fader: exercises set_param + the per-block coefficient update)
 *   <preset>       each factory preset as the device loads it
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SP1FX_FXPROF_CORE_H
#define SP1FX_FXPROF_CORE_H

#include <stdint.h>
#include "../dsp/engine.h"

enum { FXP_CLEAN = 0, FXP_DEF, FXP_WORST, FXP_AUTO, FXP_PRESET };

typedef struct {
	uint8_t kind;    /* FXP_* */
	uint8_t fx_id;   /* effect for DEF/WORST/AUTO, else 0 */
	uint8_t preset;  /* factory preset index for FXP_PRESET */
} fxprof_case_t;

/* 1 Clean + 10 effects x 3 variants + 6 factory presets */
#define FXPROF_CASES (1 + (FX_COUNT - 1) * 3 + 6)

const fxprof_case_t *fxprof_case(int i);       /* NULL if out of range */
const char *fxprof_case_name(int i);            /* effect or preset name */
const char *fxprof_kind_name(int kind);         /* "clean", "def", "worst", "auto", "preset" */
/* Sum of declared cpu_cycles of the effects the case runs. */
uint32_t fxprof_case_declared(int i);

/* Build the case's config into c (always succeeds for a valid i; returns -1 otherwise). */
int fxprof_case_config(int i, engine_config_t *c);
/* Control-side per-block action, called BEFORE engine_process() of block blk:
 * FXP_AUTO sweeps a parameter of slot 0 (engine_set_param), others do nothing. */
void fxprof_case_tick(engine_t *e, int i, uint32_t blk);

/* Deterministic, looping test signal: white noise (-20 dBFS RMS) + 220 Hz and 3.3 kHz
 * sines, different on L and R, peaks below 0.6. Generate OUTSIDE the timed region. */
typedef struct {
	uint32_t rng;
	uint32_t n;      /* frames generated so far (loops every 2^20 frames) */
	float p1, p2;    /* sine phases, 0..1 */
} fxprof_sig_t;

void fxprof_sig_init(fxprof_sig_t *s);
void fxprof_sig_block(fxprof_sig_t *s, float *l, float *r, int n);

#endif
