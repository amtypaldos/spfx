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
	float fb = s->fb, c = s->cross, a = s->damp_a;
	float lpl = s->lp[0], lpr = s->lp[1];
	float d = s->d_cur;
	for (int i = 0; i < n; i++) {
		/* One-pole glide, but with a minimum step: a plain one-pole stalls ~1-4 samples
		 * short of the target once k*diff falls below one float ULP (found by test). */
		float diff = s->d_tgt - d;
		float step = s->k_glide * diff;
		if (fabsf(step) < 0.004f) step = fabsf(diff) < 0.004f ? diff : (diff > 0.0f ? 0.004f : -0.004f);
		d += step;
		float yl = fx_dl_read(&s->dl[0], d);
		float yr = fx_dl_read(&s->dl[1], d);
		lpl += a * (yl - lpl);
		lpr += a * (yr - lpr);
		float fl = fb * ((1.0f - c) * lpl + c * lpr);
		float fr = fb * ((1.0f - c) * lpr + c * lpl);
		fx_dl_write(&s->dl[0], l[i] + fl);
		fx_dl_write(&s->dl[1], r[i] + fr);
		l[i] = yl;
		r[i] = yr;
	}
	s->d_cur = d;
	s->lp[0] = fx_flush(lpl);
	s->lp[1] = fx_flush(lpr);
}

const fx_desc fx_desc_delay = {
	FX_DELAY, "Delay", 4, P, sizeof(delay_t), MAX_SAMPLES * 2 * sizeof(int16_t), 80, 15000,
	FX_MIX_SEND, 0, init, reset, set_param, process,
};
