/* Feed-forward stereo-linked peak compressor with automatic make-up gain. Detector and gain
 * computer at 12 kHz (one per 4 frames, gain ramped in between).
 * Hard knee, no look-ahead (the engine's output limiter catches fast overshoots). */
#include "../fx.h"
#include "../util.h"

typedef struct {
	float sr;
	float p[4];
	int dirty;
	float thr, slope, makeup_lin, ka, kr;
	float gr; /* smoothed gain reduction, dB */
	float g_last; /* linear gain at the end of the last group */
} comp_t;

static const fx_param_desc P[4] = {
	{"Threshold", "dB", -40.0f, 0.0f, -18.0f, FX_SCALE_LIN, 0},
	{"Ratio", ":1", 1.0f, 20.0f, 4.0f, FX_SCALE_LOG, 0},
	{"Attack", "ms", 0.5f, 80.0f, 8.0f, FX_SCALE_LOG, 0},
	{"Release", "ms", 20.0f, 1000.0f, 160.0f, FX_SCALE_LOG, 0},
};

#define DB_PER_LOG2 6.0206f

static void update(comp_t *s)
{
	s->thr = fx_param_map(&P[0], s->p[0]);
	float ratio = fx_param_map(&P[1], s->p[1]);
	s->slope = 1.0f - 1.0f / ratio;
	s->ka = fx_coef(fx_param_map(&P[2], s->p[2]) * 0.001f, s->sr);
	s->kr = fx_coef(fx_param_map(&P[3], s->p[3]) * 0.001f, s->sr);
	float mk_db = -0.5f * s->thr * s->slope;
	s->makeup_lin = powf(10.0f, mk_db / 20.0f);
	s->dirty = 0;
}

static void reset(void *st)
{
	comp_t *s = st;
	s->gr = 0.0f;
	s->g_last = s->makeup_lin;
}

static void init(void *st, void *mem, float sr)
{
	comp_t *s = st;
	(void)mem;
	s->sr = sr;
	for (int i = 0; i < 4; i++) s->p[i] = fx_param_unmap(&P[i], P[i].def);
	update(s);
	reset(s);
}

static void set_param(void *st, int i, float v)
{
	comp_t *s = st;
	if (i < 0 || i > 3) return;
	s->p[i] = v;
	s->dirty = 1;
}

/* The detector and gain computer run once per GRP frames (peak of the group, 12 kHz);
 * the gain is ramped linearly across the group, so there are no steps. */
#define GRP 4

static void process(void *st, float *l, float *r, int n)
{
	comp_t *s = st;
	if (s->dirty) update(s);
	float gr = s->gr, g = s->g_last;
	const float thr = s->thr, slope = s->slope, mk = s->makeup_lin;
	/* per-group coefficients: 1 - (1 - k)^GRP */
	float qa = 1.0f - s->ka, qr = 1.0f - s->kr;
	qa *= qa; qa *= qa;
	qr *= qr; qr *= qr;
	const float ka = 1.0f - qa, kr = 1.0f - qr;
	for (int i0 = 0; i0 < n; i0 += GRP) {
		int m = n - i0 < GRP ? n - i0 : GRP;
		float pk = 0.0f;
		for (int i = i0; i < i0 + m; i++) {
			float a = fabsf(l[i]), b = fabsf(r[i]);
			if (a > pk) pk = a;
			if (b > pk) pk = b;
		}
		float lvl_db = DB_PER_LOG2 * fx_log2(pk + 1e-9f);
		float over = lvl_db - thr;
		float target = over > 0.0f ? over * slope : 0.0f;
		gr += (target > gr ? ka : kr) * (target - gr);
		float gn = fx_exp2(-gr * (1.0f / DB_PER_LOG2)) * mk;
		float dg = (gn - g) / (float)m;
		for (int i = i0; i < i0 + m; i++) {
			g += dg;
			l[i] *= g;
			r[i] *= g;
		}
		g = gn;
	}
	s->gr = fx_flush(gr);
	s->g_last = g;
}

const fx_desc fx_desc_compressor = {
	FX_COMPRESSOR, "Compressor", 4, P, sizeof(comp_t), 0, 110, 0, FX_MIX_CROSSFADE, 0,
	init, reset, set_param, process,
};
