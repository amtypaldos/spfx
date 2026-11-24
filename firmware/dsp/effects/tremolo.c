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

/* One loop per waveform (the shape switch is out of the per-sample path); state in locals. */
#define TREM_LOOP(WAVE)                                                              \
	for (int i = 0; i < n; i++) {                                                \
		float p2 = ph + off;                                                 \
		if (p2 >= 1.0f) p2 -= 1.0f;                                          \
		float tl = 1.0f - dep * (1.0f - WAVE(ph));                           \
		float tr = 1.0f - dep * (1.0f - WAVE(p2));                           \
		g0 += sl * (tl - g0);                                                \
		g1 += sl * (tr - g1);                                                \
		l[i] *= g0;                                                          \
		r[i] *= g1;                                                          \
		ph += inc;                                                           \
		if (ph >= 1.0f) ph -= 1.0f;                                          \
	}

static inline float w_sine(float p) { return 0.5f + 0.5f * fx_sin01(p); }
static inline float w_tri(float p) { return p < 0.5f ? 2.0f * p : 2.0f - 2.0f * p; }
static inline float w_sq(float p) { return p < 0.5f ? 1.0f : 0.0f; }
static inline float w_ramp(float p) { return 1.0f - p; }

static void process(void *st, float *l, float *r, int n)
{
	trem_t *s = st;
	if (s->dirty) update(s);
	float ph = s->ph, g0 = s->g[0], g1 = s->g[1];
	const float dep = s->depth, off = s->off, sl = s->slew, inc = s->inc;
	switch (s->shape) {
	case 0: TREM_LOOP(w_sine) break;
	case 1: TREM_LOOP(w_tri) break;
	case 2: TREM_LOOP(w_sq) break;
	default: TREM_LOOP(w_ramp) break;
	}
	s->ph = ph;
	s->g[0] = g0;
	s->g[1] = g1;
}

const fx_desc fx_desc_tremolo = {
	FX_TREMOLO, "Tremolo", 4, P, sizeof(trem_t), 0, 115, 0, FX_MIX_CROSSFADE, 1,
	init, reset, set_param, process,
};
