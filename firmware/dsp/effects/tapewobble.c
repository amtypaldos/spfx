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

static void process(void *st, float *l, float *r, int n)
{
	tw_t *s = st;
	if (s->dirty) update(s);
	for (int i = 0; i < n; i++) {
		/* slow random drift riding on the wow */
		s->wander += s->wander_k * (fx_noise(&s->rng[0]) * 6.0f - s->wander);
		float wow = fx_sin01(s->wow_ph) + 0.5f * fx_clamp(s->wander, -1.0f, 1.0f);
		float fl = fx_sin01(s->flt_ph);
		float d = s->base + s->wow_amp * wow + s->flt_amp * fl;
		float d2 = s->base + s->wow_amp * wow + s->flt_amp * fx_sin01(fx_wrap01(s->flt_ph + 0.07f));
		float yl = fx_dl_read(&s->dl[0], d);
		float yr = fx_dl_read(&s->dl[1], d2);
		fx_dl_write(&s->dl[0], l[i]);
		fx_dl_write(&s->dl[1], r[i]);
		if (s->noise_gain > 0.0f) {
			for (int c = 0; c < 2; c++) {
				float nz = fx_noise(&s->rng[c]);
				s->hp[c] += 0.06f * (nz - s->hp[c]);
				float h = (nz - s->hp[c]) * s->noise_gain;
				if (c == 0) yl += h; else yr += h;
			}
		}
		l[i] = yl;
		r[i] = yr;
		s->wow_ph = fx_wrap01(s->wow_ph + s->wow_inc);
		s->flt_ph = fx_wrap01(s->flt_ph + s->flt_inc);
	}
}

const fx_desc fx_desc_tape_wobble = {
	FX_TAPE_WOBBLE, "Tape wobble", 4, P, sizeof(tw_t), TW_SAMPLES * 2 * sizeof(int16_t), 120, 100,
	FX_MIX_CROSSFADE, 1, init, reset, set_param, process,
};
