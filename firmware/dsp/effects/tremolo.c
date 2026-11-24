/* Amplitude tremolo with four waveforms and adjustable L/R phase offset. */
#include "../fx.h"
#include "../util.h"

typedef struct {
	float sr;
	float p[4];
	int dirty;
	float inc, depth, off, slew;
	int shape;
	float ph;
	float g[2];
} trem_t;

static const fx_param_desc P[4] = {
	{"Rate", "Hz", 0.1f, 20.0f, 4.5f, FX_SCALE_LOG, 0},
	{"Depth", "%", 0.0f, 100.0f, 60.0f, FX_SCALE_LIN, 0},
	{"Waveform", "", 0.0f, 3.0f, 0.0f, FX_SCALE_STEP, "Sine,Triangle,Square,Ramp"},
	{"Stereo phase", "deg", 0.0f, 180.0f, 0.0f, FX_SCALE_LIN, 0},
};

static void update(trem_t *s)
{
	s->inc = fx_param_map(&P[0], s->p[0]) / s->sr;
	s->depth = fx_param_map(&P[1], s->p[1]) * 0.01f;
	s->shape = (int)fx_param_map(&P[2], s->p[2]);
	s->off = fx_param_map(&P[3], s->p[3]) / 360.0f;
	s->dirty = 0;
}

static void reset(void *st)
{
	trem_t *s = st;
	s->ph = 0.0f;
	s->g[0] = s->g[1] = 1.0f;
}

static void init(void *st, void *mem, float sr)
{
	trem_t *s = st;
	(void)mem;
	s->sr = sr;
	s->slew = fx_coef(0.001f, sr);
	for (int i = 0; i < 4; i++) s->p[i] = fx_param_unmap(&P[i], P[i].def);
	update(s);
	reset(s);
}

static void set_param(void *st, int i, float v)
{
	trem_t *s = st;
	if (i < 0 || i > 3) return;
	s->p[i] = v;
	s->dirty = 1;
}

static inline float wave01(int shape, float p)
{
	switch (shape) {
	case 0: return 0.5f + 0.5f * fx_sin01(p);
	case 1: return p < 0.5f ? 2.0f * p : 2.0f - 2.0f * p;
	case 2: return p < 0.5f ? 1.0f : 0.0f;
	default: return 1.0f - p;
	}
}

static void process(void *st, float *l, float *r, int n)
{
	trem_t *s = st;
	if (s->dirty) update(s);
	float ph = s->ph;
	for (int i = 0; i < n; i++) {
		float tl = 1.0f - s->depth * (1.0f - wave01(s->shape, ph));
		float tr = 1.0f - s->depth * (1.0f - wave01(s->shape, fx_wrap01(ph + s->off)));
		s->g[0] += s->slew * (tl - s->g[0]);
		s->g[1] += s->slew * (tr - s->g[1]);
		l[i] *= s->g[0];
		r[i] *= s->g[1];
		ph = fx_wrap01(ph + s->inc);
	}
	s->ph = ph;
}

const fx_desc fx_desc_tremolo = {
	FX_TREMOLO, "Tremolo", 4, P, sizeof(trem_t), 0, 45, 0, FX_MIX_CROSSFADE, 1,
	init, reset, set_param, process,
};
