/* Bit-depth and sample-rate reducer with post low-pass and output gain.
 * Aliasing is the point of this effect; there is deliberately no anti-alias filter. */
#include "../fx.h"
#include "../util.h"

typedef struct {
	float sr;
	float p[4];
	int dirty;
	float levels, ratio, tone_a, out;
	float ph[2], hold[2], lp[2];
} bc_t;

static const fx_param_desc P[4] = {
	{"Bit depth", "bit", 1.0f, 16.0f, 8.0f, FX_SCALE_LIN, 0},
	{"Sample rate", "Hz", 1000.0f, 48000.0f, 12000.0f, FX_SCALE_LOG, 0},
	{"Tone", "%", 0.0f, 100.0f, 70.0f, FX_SCALE_LIN, 0},
	{"Output gain", "dB", -12.0f, 6.0f, 0.0f, FX_SCALE_LIN, 0},
};

static void update(bc_t *s)
{
	float bits = fx_param_map(&P[0], s->p[0]);
	s->levels = fx_exp2(bits - 1.0f);
	float fs = fx_param_map(&P[1], s->p[1]);
	s->ratio = fs / s->sr;
	if (s->ratio > 1.0f) s->ratio = 1.0f;
	float t = fx_param_map(&P[2], s->p[2]) * 0.01f;
	float fc = 1000.0f * powf(16.0f, t);
	s->tone_a = 1.0f - expf(-2.0f * FX_PI * fc / s->sr);
	s->out = powf(10.0f, fx_param_map(&P[3], s->p[3]) / 20.0f);
	s->dirty = 0;
}

static void reset(void *st)
{
	bc_t *s = st;
	for (int c = 0; c < 2; c++) s->ph[c] = s->hold[c] = s->lp[c] = 0.0f;
}

static void init(void *st, void *mem, float sr)
{
	bc_t *s = st;
	(void)mem;
	s->sr = sr;
	for (int i = 0; i < 4; i++) s->p[i] = fx_param_unmap(&P[i], P[i].def);
	update(s);
	reset(s);
}

static void set_param(void *st, int i, float v)
{
	bc_t *s = st;
	if (i < 0 || i > 3) return;
	s->p[i] = v;
	s->dirty = 1;
}

static void process(void *st, float *l, float *r, int n)
{
	bc_t *s = st;
	if (s->dirty) update(s);
	float *ch[2] = {l, r};
	const float L = s->levels, iL = 1.0f / s->levels, ratio = s->ratio, ta = s->tone_a, out = s->out;
	for (int c = 0; c < 2; c++) {
		float ph = s->ph[c], hold = s->hold[c], lp = s->lp[c];
		float *x = ch[c];
		for (int i = 0; i < n; i++) {
			ph += ratio;
			if (ph >= 1.0f) {
				ph -= 1.0f;
				hold = (float)fx_floori(fx_clamp(x[i], -1.0f, 1.0f) * L + 0.5f) * iL;
			}
			lp += ta * (hold - lp);
			x[i] = lp * out;
		}
		s->ph[c] = ph;
		s->hold[c] = hold;
		s->lp[c] = fx_flush(lp);
	}
}

const fx_desc fx_desc_bitcrusher = {
	FX_BITCRUSHER, "Bitcrusher", 4, P, sizeof(bc_t), 0, 125, 0, FX_MIX_CROSSFADE, 1,
	init, reset, set_param, process,
};
