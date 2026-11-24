/* Lo-fi reverb at a quarter of the sample rate (12 kHz at 48 kHz):
 *   mono sum -> 4:1 average (decimate) -> pre-delay -> 2 input allpass diffusers ->
 *   4-line feedback delay network (Hadamard mixing, per-line damping, light modulation) ->
 *   linear-interpolated 1:4 upsampling, decorrelated stereo out.
 * Running the network at 12 kHz cuts its cost by 4x (the owner chose cheap and gritty over
 * hi-fi): the tail is band-limited to ~5 kHz and the crude decimation / interpolation lets
 * some aliasing and imaging through, which suits dub. 4 samples (83 us) extra latency.
 * Changing Size glides the line lengths (brief pitch smear is expected). Uses ~10.9 KB. */
#include "../fx.h"
#include "../util.h"

#define DEC 4 /* decimation factor */
#define NL 4
/* line lengths in samples at 48 kHz (the network runs them at 48 kHz / DEC) */
static const int base_len[NL] = {1031, 1327, 1523, 1871};
#define PRE_MAX 1200 /* 100 ms at 12 kHz */
#define AP1 36
#define AP2 27

/* per-line buffer capacity at max size (1.4 x base / DEC, + guard) */
#define CAP0 372
#define CAP1 474
#define CAP2 544
#define CAP3 664
#define CAP_TOTAL (CAP0 + CAP1 + CAP2 + CAP3)
#define MEM_FLOATS (CAP_TOTAL + AP1 + AP2)
/* Memory the descriptor declares. The 12 kHz network needs only
 * MEM_FLOATS * 4 + PRE_MAX * 2 = 10,868 B, but the declaration stays at the 48 kHz version's
 * 43,116 B for now so memory admission, the protocol's FX_DESC and the companion's table are
 * unchanged (shrinking it is a separate, protocol-visible change). */
#define REV_MEM_RESERVED 43116u
typedef char rev_mem_fits[(MEM_FLOATS * 4 + PRE_MAX * 2 <= REV_MEM_RESERVED) ? 1 : -1];

typedef struct {
	float sr;    /* host rate */
	float srd;   /* network rate = sr / DEC */
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
	/* rate conversion */
	int ph;              /* input samples collected for the next network step, 0..DEC-1 */
	float acc;           /* their sum */
	float yl0, yr0, yl1, yr1; /* previous and newest network outputs */
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
	if (fc > 0.45f * s->srd) fc = 0.45f * s->srd;
	s->damp_a = 1.0f - expf(-2.0f * FX_PI * fc / s->srd);
	s->pre_samples = fx_param_map(&P[3], s->p[3]) * 0.001f * s->srd;
	float k = s->srd / 48000.0f;
	for (int i = 0; i < NL; i++) s->len_tgt[i] = (float)base_len[i] * s->size_scale * k;
	s->dirty = 0;
}

static void reset(void *st)
{
	rev_t *s = st;
	for (int i = 0; i < NL; i++) {
		fx_zero32(s->line[i], s->cap[i]);
		s->w[i] = 0;
		s->lp[i] = 0.0f;
	}
	fx_zero32(s->ap1, AP1);
	fx_zero32(s->ap2, AP2);
	s->ap1_i = s->ap2_i = 0;
	fx_dl_clear(&s->pre);
	s->lfo1 = 0.0f;
	s->lfo2 = 0.37f;
	s->ph = 0;
	s->acc = 0.0f;
	s->yl0 = s->yr0 = s->yl1 = s->yr1 = 0.0f;
}

static void init(void *st, void *mem, float sr)
{
	rev_t *s = st;
	s->sr = sr;
	s->srd = sr / (float)DEC;
	fx_zero32(mem, REV_MEM_RESERVED / 4u); /* the engine contract: clear all declared memory */
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

/* Interpolated read at delay d (2 <= d <= cap-2, checked by the caller). */
static inline float line_rd(const float *buf, int32_t cap, int32_t w, float d)
{
	int32_t k = (int32_t)d;
	float fr = d - (float)k;
	int32_t a = w - k;
	if (a < 0) a += cap;
	int32_t b = a - 1;
	if (b < 0) b += cap;
	return buf[a] + fr * (buf[b] - buf[a]);
}

static inline float clampd(float d, uint32_t cap)
{
	float hi = (float)cap - 3.0f;
	return d > hi ? hi : (d < 3.0f ? 3.0f : d);
}

static void process(void *st, float *l, float *r, int n)
{
	rev_t *s = st;
	if (s->dirty) update(s);
	/* glide lengths per block, then derive per-line loop gain for the target RT60 */
	for (int i = 0; i < NL; i++) {
		s->len_cur[i] += 0.08f * (s->len_tgt[i] - s->len_cur[i]);
		s->g[i] = fx_exp2(-9.9658f * s->len_cur[i] / (s->srd * s->rt60));
	}
	/* LFOs (+-0.5 network samples): block edges, linear in between */
	float steps = (float)n / (float)DEC;
	float inc1 = 0.31f / s->srd, inc2 = 0.43f / s->srd;
	float m1 = 0.5f * fx_sin01(s->lfo1), m2 = 0.5f * fx_sin01(s->lfo2);
	s->lfo1 = fx_wrap01(s->lfo1 + inc1 * steps);
	s->lfo2 = fx_wrap01(s->lfo2 + inc2 * steps);
	float dm1 = (0.5f * fx_sin01(s->lfo1) - m1) / steps;
	float dm2 = (0.5f * fx_sin01(s->lfo2) - m2) / steps;

	/* state in locals (the line writes would otherwise force reloads every step) */
	float *const b0 = s->line[0], *const b1 = s->line[1], *const b2 = s->line[2], *const b3 = s->line[3];
	const int32_t c0 = (int32_t)s->cap[0], c1 = (int32_t)s->cap[1], c2 = (int32_t)s->cap[2], c3 = (int32_t)s->cap[3];
	int32_t w0 = (int32_t)s->w[0], w1 = (int32_t)s->w[1], w2 = (int32_t)s->w[2], w3 = (int32_t)s->w[3];
	const float d0 = clampd(s->len_cur[0], s->cap[0]), d2 = clampd(s->len_cur[2], s->cap[2]);
	const float l1 = clampd(s->len_cur[1], s->cap[1]), l3 = clampd(s->len_cur[3], s->cap[3]);
	const float g0 = s->g[0], g1 = s->g[1], g2 = s->g[2], g3 = s->g[3];
	const float a = s->damp_a;
	float lp0 = s->lp[0], lp1 = s->lp[1], lp2 = s->lp[2], lp3 = s->lp[3];
	float *const ap1 = s->ap1, *const ap2 = s->ap2;
	uint32_t i1 = s->ap1_i, i2 = s->ap2_i;
	int16_t *const pb = s->pre.buf;
	const uint32_t psz = s->pre.size;
	uint32_t pw = s->pre.w;
	int32_t pdi = (int32_t)(s->pre_samples + 0.5f); /* whole network samples */
	if (pdi > (int32_t)psz - 2) pdi = (int32_t)psz - 2;
	int ph = s->ph;
	float acc = s->acc;
	float yl0 = s->yl0, yr0 = s->yr0, yl1 = s->yl1, yr1 = s->yr1;
	const float frac[DEC] = {0.25f, 0.5f, 0.75f, 1.0f};

	for (int i = 0; i < n; i++) {
		acc += l[i] + r[i];
		/* output: linear from the previous to the newest network sample */
		float f = frac[ph];
		l[i] = yl0 + f * (yl1 - yl0);
		r[i] = yr0 + f * (yr1 - yr0);
		if (++ph < DEC) continue;
		ph = 0;

		/* ---- one network step at sr / DEC ---- */
		float in = acc * (0.5f / (float)DEC);
		acc = 0.0f;
		float x;
		if (pdi < 1) {
			x = in;
		} else {
			int32_t ri = (int32_t)pw - pdi;
			if (ri < 0) ri += (int32_t)psz;
			x = (float)pb[ri] * (1.0f / FX_DL_SCALE);
		}
		pb[pw] = fx_dl_q(in);
		if (++pw >= psz) pw = 0;
		/* input diffusion (Schroeder allpass, g=0.6) */
		float z = ap1[i1];
		float v = x - 0.6f * z;
		ap1[i1] = v;
		x = z + 0.6f * v;
		if (++i1 >= AP1) i1 = 0;
		z = ap2[i2];
		v = x - 0.6f * z;
		ap2[i2] = v;
		x = z + 0.6f * v;
		if (++i2 >= AP2) i2 = 0;

		float r0 = line_rd(b0, c0, w0, d0);
		float r1 = line_rd(b1, c1, w1, l1 + m1);
		float r2 = line_rd(b2, c2, w2, d2);
		float r3 = line_rd(b3, c3, w3, l3 + m2);
		m1 += dm1;
		m2 += dm2;
		lp0 += a * (r0 - lp0);
		lp1 += a * (r1 - lp1);
		lp2 += a * (r2 - lp2);
		lp3 += a * (r3 - lp3);
		float v0 = lp0 * g0, v1 = lp1 * g1, v2 = lp2 * g2, v3 = lp3 * g3;
		float s01 = v0 + v1, d01 = v0 - v1, s23 = v2 + v3, d23 = v2 - v3;
		float xi = 0.5f * x;
		/* Hadamard mix; fx_flush_fast keeps the recirculating lines out of denormals */
		b0[w0] = fx_flush_fast(xi + 0.5f * (s01 + s23));
		b1[w1] = fx_flush_fast(xi + 0.5f * (d01 + d23));
		b2[w2] = fx_flush_fast(xi + 0.5f * (s01 - s23));
		b3[w3] = fx_flush_fast(xi + 0.5f * (d01 - d23));
		if (++w0 >= c0) w0 = 0;
		if (++w1 >= c1) w1 = 0;
		if (++w2 >= c2) w2 = 0;
		if (++w3 >= c3) w3 = 0;
		yl0 = yl1;
		yr0 = yr1;
		yl1 = 0.5f * (lp0 + lp1 - lp2 - lp3);
		yr1 = 0.5f * (lp0 - lp1 + lp2 - lp3);
	}
	s->w[0] = (uint32_t)w0; s->w[1] = (uint32_t)w1; s->w[2] = (uint32_t)w2; s->w[3] = (uint32_t)w3;
	s->lp[0] = fx_flush(lp0); s->lp[1] = fx_flush(lp1); s->lp[2] = fx_flush(lp2); s->lp[3] = fx_flush(lp3);
	s->ap1_i = i1;
	s->ap2_i = i2;
	s->pre.w = pw;
	s->ph = ph;
	s->acc = acc;
	s->yl0 = fx_flush(yl0); s->yr0 = fx_flush(yr0); s->yl1 = fx_flush(yl1); s->yr1 = fx_flush(yr1);
}

const fx_desc fx_desc_reverb = {
	FX_REVERB, "Reverb", 4, P, sizeof(rev_t),
	REV_MEM_RESERVED, 240, 13000,
	FX_MIX_SEND, 0, init, reset, set_param, process,
};
