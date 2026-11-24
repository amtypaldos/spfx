/* Lightweight reverb: pre-delay -> 2 input allpass diffusers -> 4-line feedback
 * delay network (Hadamard mixing, per-line damping, light modulation).
 * ~47 KB. Mono-summed input, decorrelated stereo output.
 * Changing Size glides the line lengths (brief pitch smear is expected). */
#include "../fx.h"
#include "../util.h"

#define NL 4
static const int base_len[NL] = {1031, 1327, 1523, 1871};
#define SIZE_MAX_SCALE 1.4f
#define PRE_MAX 4800 /* 100 ms */
#define AP1 142
#define AP2 107

/* per-line buffer capacity at max size (+guard) */
#define CAP0 1460
#define CAP1 1880
#define CAP2 2150
#define CAP3 2640
#define CAP_TOTAL (CAP0 + CAP1 + CAP2 + CAP3)
#define MEM_FLOATS (CAP_TOTAL + AP1 + AP2)

typedef struct {
	float sr;
	float p[4];
	int dirty;
	float *line[NL];
	uint32_t cap[NL], w[NL];
	float len_cur[NL], len_tgt[NL];
	float g[NL];
	float damp_a, rt60, size_scale;
	float lp[NL];
	float *ap1, *ap2;
	uint32_t ap1_i, ap2_i;
	fx_dl pre;
	float pre_samples;
	float lfo1, lfo2;
	int16_t *pre_mem;
} rev_t;

static const fx_param_desc P[4] = {
	{"Decay", "s", 0.3f, 12.0f, 2.5f, FX_SCALE_LOG, 0},
	{"Size", "%", 0.0f, 100.0f, 50.0f, FX_SCALE_LIN, 0},
	{"Damping", "%", 0.0f, 100.0f, 50.0f, FX_SCALE_LIN, 0},
	{"Pre-delay", "ms", 0.0f, 100.0f, 15.0f, FX_SCALE_LIN, 0},
};

static const uint32_t caps[NL] = {CAP0, CAP1, CAP2, CAP3};

static void update(rev_t *s)
{
	s->rt60 = fx_param_map(&P[0], s->p[0]);
	s->size_scale = 0.4f + 1.0f * fx_param_map(&P[1], s->p[1]) * 0.01f;
	float d = fx_param_map(&P[2], s->p[2]) * 0.01f;
	float fc = 12000.0f * powf(1500.0f / 12000.0f, d);
	s->damp_a = 1.0f - expf(-2.0f * FX_PI * fc / s->sr);
	s->pre_samples = fx_param_map(&P[3], s->p[3]) * 0.001f * s->sr;
	float sr_k = s->sr / 48000.0f;
	for (int i = 0; i < NL; i++) s->len_tgt[i] = (float)base_len[i] * s->size_scale * sr_k;
	s->dirty = 0;
}

static void reset(void *st)
{
	rev_t *s = st;
	for (int i = 0; i < NL; i++) {
		memset(s->line[i], 0, s->cap[i] * sizeof(float));
		s->w[i] = 0;
		s->lp[i] = 0.0f;
	}
	memset(s->ap1, 0, AP1 * sizeof(float));
	memset(s->ap2, 0, AP2 * sizeof(float));
	s->ap1_i = s->ap2_i = 0;
	fx_dl_clear(&s->pre);
	s->lfo1 = 0.0f;
	s->lfo2 = 0.37f;
}

static void init(void *st, void *mem, float sr)
{
	rev_t *s = st;
	s->sr = sr;
	float *f = mem;
	for (int i = 0; i < NL; i++) {
		s->line[i] = f;
		s->cap[i] = caps[i];
		f += caps[i];
	}
	s->ap1 = f; f += AP1;
	s->ap2 = f; f += AP2;
	s->pre_mem = (int16_t *)f;
	fx_dl_init(&s->pre, s->pre_mem, PRE_MAX);
	for (int i = 0; i < 4; i++) s->p[i] = fx_param_unmap(&P[i], P[i].def);
	update(s);
	for (int i = 0; i < NL; i++) s->len_cur[i] = s->len_tgt[i];
	reset(s);
}

static void set_param(void *st, int i, float v)
{
	rev_t *s = st;
	if (i < 0 || i > 3) return;
	s->p[i] = v;
	s->dirty = 1;
}

static inline float line_read(const rev_t *s, int i, float d)
{
	float cap = (float)s->cap[i];
	if (d > cap - 2.0f) d = cap - 2.0f;
	if (d < 2.0f) d = 2.0f;
	int32_t k = (int32_t)d;
	float fr = d - (float)k;
	int32_t a = (int32_t)s->w[i] - k;
	if (a < 0) a += (int32_t)s->cap[i];
	int32_t b = a - 1;
	if (b < 0) b += (int32_t)s->cap[i];
	return s->line[i][a] + fr * (s->line[i][b] - s->line[i][a]);
}

static void process(void *st, float *l, float *r, int n)
{
	rev_t *s = st;
	if (s->dirty) update(s);
	/* glide lengths per block, then derive per-line loop gain for the target RT60 */
	for (int i = 0; i < NL; i++) {
		s->len_cur[i] += 0.08f * (s->len_tgt[i] - s->len_cur[i]);
		s->g[i] = fx_exp2(-9.9658f * s->len_cur[i] / (s->sr * s->rt60));
	}
	float a = s->damp_a;
	float inc1 = 0.31f / s->sr, inc2 = 0.43f / s->sr;
	for (int n_i = 0; n_i < n; n_i++) {
		float in = 0.5f * (l[n_i] + r[n_i]);
		/* pre-delay */
		float pd = s->pre_samples;
		float x = pd < 1.0f ? in : fx_dl_read(&s->pre, pd);
		fx_dl_write(&s->pre, in);
		/* input diffusion (Schroeder allpass, g=0.6) */
		float z = s->ap1[s->ap1_i];
		float v = x - 0.6f * z;
		s->ap1[s->ap1_i] = v;
		x = z + 0.6f * v;
		if (++s->ap1_i >= AP1) s->ap1_i = 0;
		z = s->ap2[s->ap2_i];
		v = x - 0.6f * z;
		s->ap2[s->ap2_i] = v;
		x = z + 0.6f * v;
		if (++s->ap2_i >= AP2) s->ap2_i = 0;

		s->lfo1 = fx_wrap01(s->lfo1 + inc1);
		s->lfo2 = fx_wrap01(s->lfo2 + inc2);
		float m1 = 1.5f * fx_sin01(s->lfo1);
		float m2 = 1.5f * fx_sin01(s->lfo2);

		float r0 = line_read(s, 0, s->len_cur[0]);
		float r1 = line_read(s, 1, s->len_cur[1] + m1);
		float r2 = line_read(s, 2, s->len_cur[2]);
		float r3 = line_read(s, 3, s->len_cur[3] + m2);
		float rd[NL] = {r0, r1, r2, r3};
		float vv[NL];
		for (int i = 0; i < NL; i++) {
			s->lp[i] += a * (rd[i] - s->lp[i]);
			vv[i] = s->lp[i] * s->g[i];
		}
		float h0 = (vv[0] + vv[1] + vv[2] + vv[3]) * 0.5f;
		float h1 = (vv[0] - vv[1] + vv[2] - vv[3]) * 0.5f;
		float h2 = (vv[0] + vv[1] - vv[2] - vv[3]) * 0.5f;
		float h3 = (vv[0] - vv[1] - vv[2] + vv[3]) * 0.5f;
		float hh[NL] = {h0, h1, h2, h3};
		for (int i = 0; i < NL; i++) {
			s->line[i][s->w[i]] = fx_flush(0.5f * x + hh[i]);
			if (++s->w[i] >= s->cap[i]) s->w[i] = 0;
		}
		l[n_i] = 0.5f * (s->lp[0] + s->lp[1] - s->lp[2] - s->lp[3]);
		r[n_i] = 0.5f * (s->lp[0] - s->lp[1] + s->lp[2] - s->lp[3]);
	}
}

const fx_desc fx_desc_reverb = {
	FX_REVERB, "Reverb", 4, P, sizeof(rev_t),
	MEM_FLOATS * sizeof(float) + PRE_MAX * sizeof(int16_t), 320, 13000,
	FX_MIX_SEND, 0, init, reset, set_param, process,
};
