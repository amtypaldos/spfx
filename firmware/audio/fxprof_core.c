/* SP-1 FX: effect CPU profiling core. See fxprof_core.h.
 * SPDX-License-Identifier: MIT */
#include "fxprof_core.h"
#include "../app/factory_presets.h"
#include "../dsp/util.h"
#include <string.h>

#define NFX (FX_COUNT - 1)

/* Worst-case normalised params per effect (index fx_id - 1); -1 keeps the default.
 * Chosen from the code paths in dsp/effects (most work per sample), see
 * docs/BRINGUP_FXPROF.md. */
static const float worst[NFX][FX_MAX_PARAMS] = {
	/* Filter: drive on (tanh + divide per sample), max resonance, notch */
	{-1.0f, 1.0f, 1.0f, 1.0f},
	/* Chorus: every parameter at max (cost is nearly constant) */
	{1.0f, 1.0f, 1.0f, 1.0f},
	/* Delay: longest time, max feedback / damping / width */
	{1.0f, 1.0f, 1.0f, 1.0f},
	/* Reverb: longest decay, largest size, pre-delay on */
	{1.0f, 1.0f, 1.0f, 1.0f},
	/* Saturation: drive 0 keeps every sample in the rational (non-clipped) tanh branch */
	{0.0f, 1.0f, 1.0f, 1.0f},
	/* Flanger */
	{1.0f, 1.0f, 1.0f, 1.0f},
	/* Bitcrusher: 16 bit, 48 kHz (re-quantises every sample) */
	{1.0f, 1.0f, 1.0f, 1.0f},
	/* Tremolo: fast sine, full depth, 180 deg stereo phase */
	{1.0f, 1.0f, 0.0f, 1.0f},
	/* Compressor: -40 dB threshold (always compressing), fastest attack / release */
	{0.0f, 1.0f, 0.0f, 0.0f},
	/* Tape wobble: max wow, flutter, rate and noise */
	{1.0f, 1.0f, 1.0f, 1.0f},
};

/* Parameter swept every block by the "auto" variant (index fx_id - 1). */
static const uint8_t auto_param[NFX] = {
	0, /* Filter cutoff */
	0, /* Chorus rate */
	0, /* Delay time (glide) */
	1, /* Reverb size (length glide) */
	0, /* Saturation drive */
	3, /* Flanger base delay */
	2, /* Bitcrusher tone */
	0, /* Tremolo rate */
	0, /* Compressor threshold */
	2, /* Tape wobble rate */
};

static fxprof_case_t mk(uint8_t kind, uint8_t fx, uint8_t preset)
{
	fxprof_case_t c = {kind, fx, preset};
	return c;
}

const fxprof_case_t *fxprof_case(int i)
{
	static fxprof_case_t cur;
	if (i < 0 || i >= FXPROF_CASES) return 0;
	if (i == 0) cur = mk(FXP_CLEAN, 0, 0);
	else if (i <= NFX * 3) {
		int k = i - 1;
		cur = mk((uint8_t)(FXP_DEF + k % 3), (uint8_t)(1 + k / 3), 0);
	} else cur = mk(FXP_PRESET, 0, (uint8_t)(i - 1 - NFX * 3));
	return &cur;
}

const char *fxprof_kind_name(int kind)
{
	static const char *const n[] = {"clean", "def", "worst", "auto", "preset"};
	return (kind >= 0 && kind <= FXP_PRESET) ? n[kind] : "?";
}

const char *fxprof_case_name(int i)
{
	const fxprof_case_t *c = fxprof_case(i);
	if (!c) return "?";
	if (c->kind == FXP_CLEAN) return "Clean";
	if (c->kind == FXP_PRESET) return factory_presets[c->preset].name;
	return fx_get(c->fx_id)->name;
}

uint32_t fxprof_case_declared(int i)
{
	engine_config_t c;
	uint32_t sum = 0;
	if (fxprof_case_config(i, &c) != 0) return 0;
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		const fx_desc *d = fx_get(c.fx_id[s]);
		if (d && c.fx_id[s] != FX_NONE) sum += d->cpu_cycles;
	}
	return sum;
}

int fxprof_case_config(int i, engine_config_t *c)
{
	const fxprof_case_t *k = fxprof_case(i);
	if (!k) return -1;
	if (k->kind == FXP_PRESET) {
		store_data_t d;
		factory_preset_fill(&factory_presets[k->preset], &d);
		*c = d.cfg;
		return 0;
	}
	engine_config_default(c);
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		c->fx_id[s] = FX_NONE;
		c->bypass[s] = 1;
	}
	if (k->kind == FXP_CLEAN) return 0;
	c->fx_id[0] = k->fx_id;
	c->bypass[0] = 0;
	c->wet[0] = 1.0f;
	if (k->kind == FXP_WORST) {
		for (int p = 0; p < FX_MAX_PARAMS; p++) {
			float v = worst[k->fx_id - 1][p];
			if (v >= 0.0f) c->params[0][k->fx_id][p] = v;
		}
	}
	return 0;
}

void fxprof_case_tick(engine_t *e, int i, uint32_t blk)
{
	const fxprof_case_t *k = fxprof_case(i);
	if (!k || k->kind != FXP_AUTO) return;
	/* triangle 0.2 .. 0.8 .. 0.2 over 1500 blocks (2 s): a new value every block */
	uint32_t ph = blk % 1500u;
	float t = ph < 750u ? (float)ph / 750.0f : (float)(1500u - ph) / 750.0f;
	engine_set_param(e, 0, auto_param[k->fx_id - 1], 0.2f + 0.6f * t);
}

void fxprof_sig_init(fxprof_sig_t *s)
{
	s->rng = 0x2545F491u;
	s->n = 0;
	s->p1 = 0.0f;
	s->p2 = 0.25f;
}

void fxprof_sig_block(fxprof_sig_t *s, float *l, float *r, int n)
{
	const float i1 = 220.0f / FX_SAMPLE_RATE, i2 = 3300.0f / FX_SAMPLE_RATE;
	for (int i = 0; i < n; i++) {
		if ((s->n++ & 0xFFFFFu) == 0u) { /* loop: identical signal every 2^20 frames */
			s->rng = 0x2545F491u;
			s->p1 = 0.0f;
			s->p2 = 0.25f;
		}
		float a = fx_noise(&s->rng), b = fx_noise(&s->rng);
		float s1 = fx_sin01(s->p1), s2 = fx_sin01(s->p2);
		/* uniform noise +-0.17 is -20 dBFS RMS */
		l[i] = 0.17f * a + 0.25f * s1 + 0.1f * s2;
		r[i] = 0.17f * b + 0.1f * s1 + 0.25f * s2;
		s->p1 = fx_wrap01(s->p1 + i1);
		s->p2 = fx_wrap01(s->p2 + i2);
	}
}
