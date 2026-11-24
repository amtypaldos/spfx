/* Chorus and flanger share one modulated-delay engine (~6 KB of memory). */
#include "../fx.h"
#include "../util.h"

#define MD_SAMPLES 1536 /* 32 ms at 48 kHz, per channel */

typedef struct {
	float sr;
	float p[4];
	int dirty;
	int is_flanger;
	fx_dl dl[2];
	float phase, inc;
	float base, depth;  /* samples */
	float fb;
	float phase_off;
	float y_prev[2];
} md_t;

static const fx_param_desc PC[4] = {
	{"Rate", "Hz", 0.1f, 5.0f, 0.8f, FX_SCALE_LOG, 0},
	{"Depth", "%", 0.0f, 100.0f, 50.0f, FX_SCALE_LIN, 0},
	{"Feedback", "%", 0.0f, 70.0f, 0.0f, FX_SCALE_LIN, 0},
	{"Stereo width", "%", 0.0f, 100.0f, 70.0f, FX_SCALE_LIN, 0},
};

static const fx_param_desc PF[4] = {
	{"Rate", "Hz", 0.02f, 10.0f, 0.25f, FX_SCALE_LOG, 0},
	{"Depth", "%", 0.0f, 100.0f, 70.0f, FX_SCALE_LIN, 0},
	{"Feedback", "%", -90.0f, 90.0f, 50.0f, FX_SCALE_LIN, 0},
	{"Base delay", "ms", 0.2f, 6.0f, 1.5f, FX_SCALE_LOG, 0},
};

static void update(md_t *s)
{
	const fx_param_desc *P = s->is_flanger ? PF : PC;
	float rate = fx_param_map(&P[0], s->p[0]);
	s->inc = rate / s->sr;
	float depth = fx_param_map(&P[1], s->p[1]) * 0.01f;
	s->fb = fx_param_map(&P[2], s->p[2]) * 0.01f;
	float ms = s->sr * 0.001f;
	if (s->is_flanger) {
		s->base = fx_param_map(&P[3], s->p[3]) * ms;
		s->depth = depth * 0.9f * s->base;   /* never reaches zero delay */
		s->phase_off = 0.25f;                /* fixed quadrature stereo */
	} else {
		s->base = 14.0f * ms;
		s->depth = depth * 6.0f * ms;
		s->phase_off = fx_param_map(&P[3], s->p[3]) * 0.01f * 0.5f;
	}
	s->dirty = 0;
}

static void reset(void *st)
{
	md_t *s = st;
	fx_dl_clear(&s->dl[0]);
	fx_dl_clear(&s->dl[1]);
	s->y_prev[0] = s->y_prev[1] = 0.0f;
	s->phase = 0.0f;
}

static void init_common(md_t *s, void *mem, float sr, int fl)
{
	s->sr = sr;
	s->is_flanger = fl;
	int16_t *b = mem;
	fx_dl_init(&s->dl[0], b, MD_SAMPLES);
	fx_dl_init(&s->dl[1], b + MD_SAMPLES, MD_SAMPLES);
	const fx_param_desc *P = fl ? PF : PC;
	for (int i = 0; i < 4; i++) s->p[i] = fx_param_unmap(&P[i], P[i].def);
	update(s);
	s->phase = 0.0f;
	s->y_prev[0] = s->y_prev[1] = 0.0f;
}

static void init_c(void *st, void *mem, float sr) { init_common(st, mem, sr, 0); }
static void init_f(void *st, void *mem, float sr) { init_common(st, mem, sr, 1); }

static void set_param(void *st, int i, float v)
{
	md_t *s = st;
	if (i < 0 || i > 3) return;
	s->p[i] = v;
	s->dirty = 1;
}

/* LFO evaluated every SUB samples, delay ramped linearly in between (chorus <= 5 Hz,
 * flanger <= 10 Hz: the deviation from the per-sample curve stays below 0.02 samples). */
#define SUB 16

static inline float md_delay(const md_t *s, float ph, float maxd)
{
	float d = s->base + s->depth * fx_sin01(ph);
	return d < 2.0f ? 2.0f : (d > maxd ? maxd : d);
}

static void process(void *st, float *l, float *r, int n)
{
	md_t *s = st;
	if (s->dirty) update(s);
	float ph = s->phase;
	const float fb = s->fb, inc = s->inc, off = s->phase_off;
	const float maxd = (float)(MD_SAMPLES - 3);
	int16_t *const bl = s->dl[0].buf, *const br = s->dl[1].buf;
	const uint32_t size = s->dl[0].size;
	uint32_t w = s->dl[0].w;
	float dl = md_delay(s, ph, maxd), dr = md_delay(s, fx_wrap01(ph + off), maxd);
	for (int i0 = 0; i0 < n; i0 += SUB) {
		int m = n - i0 < SUB ? n - i0 : SUB;
		float ph_end = ph + inc * (float)m;
		if (ph_end >= 1.0f) ph_end -= 1.0f;
		float dl_end = md_delay(s, ph_end, maxd);
		float dr_end = md_delay(s, fx_wrap01(ph_end + off), maxd);
		float sl = (dl_end - dl) / (float)m, sr = (dr_end - dr) / (float)m;
		for (int i = i0; i < i0 + m; i++) {
			float yl = fx_dl_read_at(bl, size, w, dl);
			float yr = fx_dl_read_at(br, size, w, dr);
			bl[w] = fx_dl_q(fx_fbsat(l[i] + fb * yl));
			br[w] = fx_dl_q(fx_fbsat(r[i] + fb * yr));
			if (++w >= size) w = 0;
			l[i] = yl;
			r[i] = yr;
			dl += sl;
			dr += sr;
		}
		ph = ph_end;
		dl = dl_end; /* no drift from the summed steps */
		dr = dr_end;
	}
	s->dl[0].w = w;
	s->dl[1].w = w;
	s->phase = ph;
}

const fx_desc fx_desc_chorus = {
	FX_CHORUS, "Chorus", 4, PC, sizeof(md_t), MD_SAMPLES * 2 * sizeof(int16_t), 200, 100,
	FX_MIX_CROSSFADE, 1, init_c, reset, set_param, process,
};

const fx_desc fx_desc_flanger = {
	FX_FLANGER, "Flanger", 4, PF, sizeof(md_t), MD_SAMPLES * 2 * sizeof(int16_t), 200, 500,
	FX_MIX_CROSSFADE, 1, init_f, reset, set_param, process,
};
