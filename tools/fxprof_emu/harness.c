/* Bare-metal harness for tools/fxprof_emu/fxprof_emu.py (instruction-count emulation of the
 * firmware's DSP, NOT a hardware measurement). Same cases as the fxprof firmware image.
 * SPDX-License-Identifier: MIT */
#include "audio/fxprof_core.h"

static uint8_t pool[150 * 1024] __attribute__((aligned(8)));
static engine_t eng;
static fxprof_sig_t sig;
static float il[64], ir[64], ol[64], orr[64];
static uint32_t blk;
static int cur;
volatile float h_sink;

int h_ncases(void) { return FXPROF_CASES; }

void h_setup(int i)
{
	engine_config_t c;
	engine_init(&eng, pool, sizeof pool, 48000.0f, 0xFFFFFFFFu);
	fxprof_case_config(i, &c);
	engine_post_config(&eng, &c);
	engine_pump(&eng);
	fxprof_sig_init(&sig);
	blk = 0;
	cur = i;
}

void h_prep(void)
{
	fxprof_case_tick(&eng, cur, blk);
	fxprof_sig_block(&sig, il, ir, 64);
	blk++;
}

void h_block(void)
{
	engine_process(&eng, il, ir, ol, orr, 64);
	h_sink = ol[0] + orr[63];
}

/* the only entry the linker needs */
void _start(void) { for (;;) {} }
