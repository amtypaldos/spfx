/* Waveshaping saturation: drive -> biased cubic soft clip (fx_softclip) -> DC blocker -> tone (LP) -> output gain.
 * No oversampling (aliasing at high drive is a known, documented trade-off). */
#include "../fx.h"
#include "../util.h"

typedef struct {
	float sr;
	float p[4];
	int dirty;
	float g_cur, g_tgt, comp_cur, comp_tgt, out_cur, out_tgt;
	float bias, tone_a;
	float dc_x1[2], dc_y1[2], lp[2];
} sat_t;

static const fx_param_desc P[4] = {
	{"Drive", "%", 0.0f, 100.0f, 35.0f, FX_SCALE_LIN, 0},
	{"Tone", "%", 0.0f, 100.0f, 70.0f, FX_SCALE_LIN, 0},
	{"Bias", "", -0.3f, 0.3f, 0.0f, FX_SCALE_LIN, 0},
	{"Output gain", "dB", -12.0f, 6.0f, 0.0f, FX_SCALE_LIN, 0},
};

static void update(sat_t *s)
{
	float d = fx_param_map(&P[0], s->p[0]) * 0.01f;
	s->g_tgt = fx_exp2(d * 4.5f);          /* 1x .. ~23x */
	s->comp_tgt = fx_exp2(-1.575f * d);    /* rough loudness compensation */
	float t = fx_param_map(&P[1], s->p[1]) * 0.01f;
	float fc = 800.0f * powf(20.0f, t);    /* 800 Hz .. 16 kHz */
	s->tone_a = 1.0f - expf(-2.0f * FX_PI * fc / s->sr);
	s->bias = fx_param_map(&P[2], s->p[2]);
	s->out_tgt = powf(10.0f, fx_param_map(&P[3], s->p[3]) / 20.0f);
	s->dirty = 0;
}

static void reset(void *st)
{
	sat_t *s = st;
	for (int c = 0; c < 2; c++) s->dc_x1[c] = s->dc_y1[c] = s->lp[c] = 0.0f;
}

static void init(void *st, void *mem, float sr)
{
	sat_t *s = st;
	(void)mem;
	s->sr = sr;
	for (int i = 0; i < 4; i++) s->p[i] = fx_param_unmap(&P[i], P[i].def);
	update(s);
	s->g_cur = s->g_tgt;
	s->comp_cur = s->comp_tgt;
	s->out_cur = s->out_tgt;
	reset(s);
}

static void set_param(void *st, int i, float v)
{
	sat_t *s = st;
	if (i < 0 || i > 3) return;
	s->p[i] = v;
	s->dirty = 1;
}

static void process(void *st, float *l, float *r, int n)
{
	sat_t *s = st;
	if (s->dirty) update(s);
	float g0 = s->g_cur, c0 = s->comp_cur, o0 = s->out_cur;
	s->g_cur += 0.3f * (s->g_tgt - s->g_cur);
	s->comp_cur += 0.3f * (s->comp_tgt - s->comp_cur);
	s->out_cur += 0.3f * (s->out_tgt - s->out_cur);
	float dg = (s->g_cur - g0) / (float)n;
	float dc = (s->comp_cur - c0) / (float)n;
	float dout = (s->out_cur - o0) / (float)n;
	float b = s->bias, tb = fx_softclip(b), a = s->tone_a;
	float *ch[2] = {l, r};

	for (int c = 0; c < 2; c++) {
		float g = g0, cp = c0, o = o0;
		float x1 = s->dc_x1[c], y1 = s->dc_y1[c], lp = s->lp[c];
		float *x = ch[c];
		for (int i = 0; i < n; i++) {
			g += dg; cp += dc; o += dout;
			float y = fx_softclip(x[i] * g + b) - tb;
			float d = y - x1 + 0.9987f * y1; /* DC blocker */
			x1 = y;
			y1 = d;
			lp += a * (d - lp);
			x[i] = lp * cp * o;
		}
		s->dc_x1[c] = x1;
		s->dc_y1[c] = fx_flush(y1);
		s->lp[c] = fx_flush(lp);
	}
}

const fx_desc fx_desc_saturation = {
	FX_SATURATION, "Saturation", 4, P, sizeof(sat_t), 0, 115, 0, FX_MIX_CROSSFADE, 1,
	init, reset, set_param, process,
};
