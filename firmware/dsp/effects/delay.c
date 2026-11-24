/* Stereo feedback delay. 16-bit storage keeps 400 ms of stereo audio in ~75 KB.
 * Delay time glides (tape-style) when changed. Width = cross-feedback (ping-pong). */
#include "../fx.h"
#include "../util.h"

#define MAX_MS 400
#define MAX_SAMPLES 19200 /* 400 ms at 48 kHz */

typedef struct {
	float sr;
	float p[4];
	int dirty;
	fx_dl dl[2];
	float d_cur, d_tgt;   /* delay in samples */
	float fb, damp_a, cross;
	float lp[2];
	float k_glide;
} delay_t;

static const fx_param_desc P[4] = {
	{"Time", "ms", 20.0f, (float)MAX_MS, 250.0f, FX_SCALE_LOG, 0},
	{"Feedback", "%", 0.0f, 95.0f, 40.0f, FX_SCALE_LIN, 0},
	{"Damping", "%", 0.0f, 100.0f, 40.0f, FX_SCALE_LIN, 0},
	{"Stereo width", "%", 0.0f, 100.0f, 50.0f, FX_SCALE_LIN, 0},
};

static void update(delay_t *s)
{
	float ms = fx_param_map(&P[0], s->p[0]);
	s->d_tgt = ms * 0.001f * s->sr;
	if (s->d_tgt > (float)(s->dl[0].size - 4)) s->d_tgt = (float)(s->dl[0].size - 4);
	s->fb = fx_param_map(&P[1], s->p[1]) * 0.01f;
	float d = fx_param_map(&P[2], s->p[2]) * 0.01f;
	float fc = 16000.0f * powf(800.0f / 16000.0f, d);
	s->damp_a = 1.0f - expf(-2.0f * FX_PI * fc / s->sr);
	s->cross = fx_param_map(&P[3], s->p[3]) * 0.01f;
	s->dirty = 0;
}

static void reset(void *st)
{
	delay_t *s = st;
	fx_dl_clear(&s->dl[0]);
	fx_dl_clear(&s->dl[1]);
	s->lp[0] = s->lp[1] = 0.0f;
}

static void init(void *st, void *mem, float sr)
{
	delay_t *s = st;
	s->sr = sr;
	uint32_t n = (uint32_t)(MAX_SAMPLES * sr / 48000.0f);
	if (n > MAX_SAMPLES) n = MAX_SAMPLES; /* memory is sized for 48 kHz */
	int16_t *b = mem;
	fx_dl_init(&s->dl[0], b, n);
	fx_dl_init(&s->dl[1], b + MAX_SAMPLES, n);
	for (int i = 0; i < 4; i++) s->p[i] = fx_param_unmap(&P[i], P[i].def);
	update(s);
	s->d_cur = s->d_tgt;
	s->k_glide = fx_coef(0.08f, sr);
	s->lp[0] = s->lp[1] = 0.0f;
}

static void set_param(void *st, int i, float v)
{
	delay_t *s = st;
	if (i < 0 || i > 3) return;
	s->p[i] = v;
	s->dirty = 1;
}

static void process(void *st, float *l, float *r, int n)
{
	delay_t *s = st;
	if (s->dirty) update(s);
	const float fb = s->fb, c = s->cross, a = s->damp_a;
	const float c1 = 1.0f - c;
	float lpl = s->lp[0], lpr = s->lp[1];
	float d = s->d_cur;
	const float tgt = s->d_tgt, kg = s->k_glide;
	int16_t *const bl = s->dl[0].buf, *const br = s->dl[1].buf;
	const uint32_t size = s->dl[0].size;
	uint32_t w = s->dl[0].w; /* both lines always advance together */
	const float maxd = (float)(size - 2);

	if (d == tgt) {
		/* steady time: a whole-sample delay (the time is rounded to 1/48 ms, inaudible for
		 * an echo), so one read per channel and no interpolation */
		float dd = d < 1.0f ? 1.0f : (d > maxd ? maxd : d);
		const int32_t k = (int32_t)(dd + 0.5f);
		int32_t ia = (int32_t)w - k;
		if (ia < 0) ia += (int32_t)size;
		for (int i = 0; i < n; i++) {
			float yl = (float)bl[ia] * (1.0f / FX_DL_SCALE);
			float yr = (float)br[ia] * (1.0f / FX_DL_SCALE);
			if (++ia >= (int32_t)size) ia = 0;
			lpl += a * (yl - lpl);
			lpr += a * (yr - lpr);
			bl[w] = fx_dl_q(l[i] + fb * (c1 * lpl + c * lpr));
			br[w] = fx_dl_q(r[i] + fb * (c1 * lpr + c * lpl));
			if (++w >= size) w = 0;
			l[i] = yl;
			r[i] = yr;
		}
	} else {
		for (int i = 0; i < n; i++) {
			/* One-pole glide, but with a minimum step: a plain one-pole stalls ~1-4 samples
			 * short of the target once k*diff falls below one float ULP (found by test). */
			float diff = tgt - d;
			float step = kg * diff;
			if (fabsf(step) < 0.004f) step = fabsf(diff) < 0.004f ? diff : (diff > 0.0f ? 0.004f : -0.004f);
			d += step;
			float dd = d < 1.0f ? 1.0f : (d > maxd ? maxd : d);
			float yl = fx_dl_read_at(bl, size, w, dd);
			float yr = fx_dl_read_at(br, size, w, dd);
			lpl += a * (yl - lpl);
			lpr += a * (yr - lpr);
			bl[w] = fx_dl_q(l[i] + fb * (c1 * lpl + c * lpr));
			br[w] = fx_dl_q(r[i] + fb * (c1 * lpr + c * lpl));
			if (++w >= size) w = 0;
			l[i] = yl;
			r[i] = yr;
		}
	}
	s->dl[0].w = w;
	s->dl[1].w = w;
	s->d_cur = d;
	s->lp[0] = fx_flush(lpl);
	s->lp[1] = fx_flush(lpr);
}

const fx_desc fx_desc_delay = {
	FX_DELAY, "Delay", 4, P, sizeof(delay_t), MAX_SAMPLES * 2 * sizeof(int16_t), 210, 15000,
	FX_MIX_SEND, 0, init, reset, set_param, process,
};
