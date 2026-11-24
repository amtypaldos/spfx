/* Tape wobble: wow + flutter pitch modulation via a short modulated delay, plus hiss.
 * Wow and flutter share the transport (same modulation on L and R, flutter slightly
 * offset) so the stereo image stays put. */
#include "../fx.h"
#include "../util.h"

#define TW_SAMPLES 1024 /* 21 ms per channel */

typedef struct {
	float sr;
	float p[4];
	int dirty;
	fx_dl dl[2];
	float wow_ph, flt_ph;
	float wow_inc, flt_inc;
	float wow_amp, flt_amp; /* samples */
	float noise_gain;
	float base;
	uint32_t rng[2];
	float hp[2];
	float wander, wander_k;
	float noise_tgt;
} tw_t;

static const fx_param_desc P[4] = {
	{"Wow depth", "%", 0.0f, 100.0f, 35.0f, FX_SCALE_LIN, 0},
	{"Flutter depth", "%", 0.0f, 100.0f, 25.0f, FX_SCALE_LIN, 0},
	{"Rate", "Hz", 0.2f, 3.0f, 0.6f, FX_SCALE_LOG, 0},
	{"Noise", "%", 0.0f, 100.0f, 15.0f, FX_SCALE_LIN, 0},
};

static void update(tw_t *s)
{
	float ms = s->sr * 0.001f;
	s->wow_amp = fx_param_map(&P[0], s->p[0]) * 0.01f * 3.5f * ms;
	s->flt_amp = fx_param_map(&P[1], s->p[1]) * 0.01f * 0.35f * ms;
	float rate = fx_param_map(&P[2], s->p[2]);
	s->wow_inc = rate / s->sr;
	s->flt_inc = rate * 10.5f / s->sr;
	float nz = fx_param_map(&P[3], s->p[3]) * 0.01f;
	s->noise_gain = nz * nz * 0.0158f; /* up to about -36 dBFS */
	s->dirty = 0;
}

static void reset(void *st)
{
	tw_t *s = st;
	fx_dl_clear(&s->dl[0]);
	fx_dl_clear(&s->dl[1]);
	s->wow_ph = 0.0f;
	s->flt_ph = 0.3f;
	s->hp[0] = s->hp[1] = 0.0f;
	s->wander = 0.0f;
}

static void init(void *st, void *mem, float sr)
{
	tw_t *s = st;
	s->sr = sr;
	int16_t *b = mem;
	fx_dl_init(&s->dl[0], b, TW_SAMPLES);
	fx_dl_init(&s->dl[1], b + TW_SAMPLES, TW_SAMPLES);
	s->base = 8.0f * sr * 0.001f;
	s->rng[0] = 0x1234567u;
	s->rng[1] = 0x7654321u;
	s->wander_k = fx_coef(0.7f, sr);
	for (int i = 0; i < 4; i++) s->p[i] = fx_param_unmap(&P[i], P[i].def);
	update(s);
	reset(s);
}

static void set_param(void *st, int i, float v)
{
	tw_t *s = st;
	if (i < 0 || i > 3) return;
	s->p[i] = v;
	s->dirty = 1;
}

/* The delay modulation (wow <= 3 Hz, flutter <= 31.5 Hz, random drift) is evaluated every
 * SUB samples and ramped linearly in between: the deviation from the per-sample curve is below
 * 0.01 samples. The drift low-pass runs at the SUB rate with an equivalent coefficient and a
 * noise level scaled so its variance matches the per-sample filter (sqrt(SUB) smaller). */
#define SUB 16

static void tw_mod(const tw_t *s, float *d, float *d2)
{
	float wow = fx_sin01(s->wow_ph) + 0.5f * fx_clamp(s->wander, -1.0f, 1.0f);
	float base = s->base + s->wow_amp * wow;
	*d = base + s->flt_amp * fx_sin01(s->flt_ph);
	*d2 = base + s->flt_amp * fx_sin01(fx_wrap01(s->flt_ph + 0.07f));
}

static void process(void *st, float *l, float *r, int n)
{
	tw_t *s = st;
	if (s->dirty) update(s);
	int16_t *const bl = s->dl[0].buf, *const br = s->dl[1].buf;
	const uint32_t size = s->dl[0].size;
	uint32_t w = s->dl[0].w;
	const float ng = s->noise_gain;
	/* sub-rate drift coefficient: 1 - (1 - k)^SUB */
	float q = 1.0f - s->wander_k, q2 = q * q, q4 = q2 * q2, q8 = q4 * q4;
	const float kw = 1.0f - q8 * q8;
	float d, d2;
	tw_mod(s, &d, &d2);
	for (int i0 = 0; i0 < n; i0 += SUB) {
		int m = n - i0 < SUB ? n - i0 : SUB;
		s->wander += kw * (fx_noise(&s->rng[0]) * 1.5f - s->wander);
		s->wow_ph = fx_wrap01(s->wow_ph + s->wow_inc * (float)m);
		s->flt_ph = fx_wrap01(s->flt_ph + s->flt_inc * (float)m);
		float de, d2e;
		tw_mod(s, &de, &d2e);
		float sd = (de - d) / (float)m, sd2 = (d2e - d2) / (float)m;
		for (int i = i0; i < i0 + m; i++) {
			/* base 8 ms +- (3.5 + 1.75 + 0.35) ms stays inside 1 .. size-2 */
			float yl = fx_dl_read_at(bl, size, w, d);
			float yr = fx_dl_read_at(br, size, w, d2);
			bl[w] = fx_dl_q(l[i]);
			br[w] = fx_dl_q(r[i]);
			if (++w >= size) w = 0;
			l[i] = yl;
			r[i] = yr;
			d += sd;
			d2 += sd2;
		}
		d = de;
		d2 = d2e;
	}
	s->dl[0].w = w;
	s->dl[1].w = w;
	if (ng > 0.0f) {
		/* hiss: high-passed white noise, per sample */
		uint32_t r0 = s->rng[0], r1 = s->rng[1];
		float h0 = s->hp[0], h1 = s->hp[1];
		for (int i = 0; i < n; i++) {
			float n0 = fx_noise(&r0), n1 = fx_noise(&r1);
			h0 += 0.06f * (n0 - h0);
			h1 += 0.06f * (n1 - h1);
			l[i] += (n0 - h0) * ng;
			r[i] += (n1 - h1) * ng;
		}
		s->rng[0] = r0;
		s->rng[1] = r1;
		s->hp[0] = fx_flush(h0);
		s->hp[1] = fx_flush(h1);
	}
}

const fx_desc fx_desc_tape_wobble = {
	FX_TAPE_WOBBLE, "Tape wobble", 4, P, sizeof(tw_t), TW_SAMPLES * 2 * sizeof(int16_t), 255, 100,
	FX_MIX_CROSSFADE, 1, init, reset, set_param, process,
};
