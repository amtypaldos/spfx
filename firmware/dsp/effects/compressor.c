/* Feed-forward stereo-linked peak compressor with automatic make-up gain.
 * Hard knee, no look-ahead (the engine's output limiter catches fast overshoots). */
#include "../fx.h"
#include "../util.h"

typedef struct {
	float sr;
	float p[4];
	int dirty;
	float thr, slope, makeup_lin, ka, kr;
	float gr; /* smoothed gain reduction, dB */
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

static void reset(void *st) { ((comp_t *)st)->gr = 0.0f; }

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

static void process(void *st, float *l, float *r, int n)
{
	comp_t *s = st;
	if (s->dirty) update(s);
	float gr = s->gr;
	for (int i = 0; i < n; i++) {
		float pk = fabsf(l[i]);
		float pr = fabsf(r[i]);
		if (pr > pk) pk = pr;
		float lvl_db = DB_PER_LOG2 * fx_log2(pk + 1e-9f);
		float over = lvl_db - s->thr;
		float target = over > 0.0f ? over * s->slope : 0.0f;
		gr += (target > gr ? s->ka : s->kr) * (target - gr);
		float g = fx_exp2(-gr * (1.0f / DB_PER_LOG2)) * s->makeup_lin;
		l[i] *= g;
		r[i] *= g;
	}
	s->gr = fx_flush(gr);
}

const fx_desc fx_desc_compressor = {
	FX_COMPRESSOR, "Compressor", 4, P, sizeof(comp_t), 0, 110, 0, FX_MIX_CROSSFADE, 0,
	init, reset, set_param, process,
};
