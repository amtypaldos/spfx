/* Multimode state-variable filter (Zavalishin TPT topology) with input drive.
 * Coefficients are recomputed once per block with smoothed cutoff. */
#include "../fx.h"
#include "../util.h"

typedef struct {
	float sr;
	float p[4];
	int dirty;
	float lfc_cur, lfc_tgt; /* ln(cutoff Hz) */
	float k;
	float drive_cur, drive_tgt;
	int type;
	float ic1[2], ic2[2];
} filter_t;

static const fx_param_desc P[4] = {
	{"Cutoff", "Hz", 20.0f, 20000.0f, 1500.0f, FX_SCALE_LOG, 0},
	{"Resonance", "%", 0.0f, 100.0f, 40.0f, FX_SCALE_LIN, 0},
	{"Drive", "%", 0.0f, 100.0f, 0.0f, FX_SCALE_LIN, 0},
	{"Type", "", 0.0f, 3.0f, 0.0f, FX_SCALE_STEP, "Low-pass,High-pass,Band-pass,Notch"},
};

static void update(filter_t *s)
{
	s->lfc_tgt = logf(fx_param_map(&P[0], s->p[0]));
	float res = fx_param_map(&P[1], s->p[1]) * 0.01f;
	s->k = 1.4142f - 1.3142f * res; /* res 0 = Butterworth (Q 0.71), res 1 = Q 10 */
	s->drive_tgt = 1.0f + 7.0f * fx_param_map(&P[2], s->p[2]) * 0.01f;
	s->type = (int)fx_param_map(&P[3], s->p[3]);
	s->dirty = 0;
}

static void reset(void *st)
{
	filter_t *s = st;
	for (int c = 0; c < 2; c++) s->ic1[c] = s->ic2[c] = 0.0f;
}

static void init(void *st, void *mem, float sr)
{
	filter_t *s = st;
	(void)mem;
	s->sr = sr;
	for (int i = 0; i < 4; i++) s->p[i] = fx_param_unmap(&P[i], P[i].def);
	update(s);
	s->lfc_cur = s->lfc_tgt;
	s->drive_cur = s->drive_tgt;
	reset(s);
}

static void set_param(void *st, int i, float v)
{
	filter_t *s = st;
	if (i < 0 || i > 3) return;
	s->p[i] = v;
	s->dirty = 1;
}

static void process(void *st, float *l, float *r, int n)
{
	filter_t *s = st;
	if (s->dirty) update(s);
	s->lfc_cur += 0.4f * (s->lfc_tgt - s->lfc_cur);
	s->drive_cur += 0.4f * (s->drive_tgt - s->drive_cur);
	float fc = expf(s->lfc_cur);
	float fmax = 0.45f * s->sr;
	if (fc > fmax) fc = fmax;
	float g = tanf(FX_PI * fc / s->sr);
	float k = s->k;
	float a1 = 1.0f / (1.0f + g * (g + k));
	float a2 = g * a1, a3 = g * a2;
	float dg = s->drive_cur;
	int use_drive = dg > 1.001f;
	float comp = use_drive ? 1.0f / sqrtf(dg) : 1.0f;
	/* output = c0*v0 + c1*v1 + c2*v2 for every mode (no per-sample switch) */
	float c0, c1, c2;
	switch (s->type) {
	case 0: c0 = 0.0f; c1 = 0.0f; c2 = 1.0f; break; /* low-pass */
	case 1: c0 = 1.0f; c1 = -k; c2 = -1.0f; break;  /* high-pass */
	case 2: c0 = 0.0f; c1 = k; c2 = 0.0f; break;    /* band-pass */
	default: c0 = 1.0f; c1 = -k; c2 = 0.0f; break;  /* notch */
	}
	float *ch[2] = {l, r};

	for (int c = 0; c < 2; c++) {
		float ic1 = s->ic1[c], ic2 = s->ic2[c];
		float *x = ch[c];
		if (use_drive) {
			for (int i = 0; i < n; i++) {
				float v0 = fx_softclip(x[i] * dg) * comp;
				float v3 = v0 - ic2;
				float v1 = a1 * ic1 + a2 * v3;
				float v2 = ic2 + a2 * ic1 + a3 * v3;
				ic1 = 2.0f * v1 - ic1;
				ic2 = 2.0f * v2 - ic2;
				x[i] = c0 * v0 + c1 * v1 + c2 * v2;
			}
		} else {
			for (int i = 0; i < n; i++) {
				float v0 = x[i];
				float v3 = v0 - ic2;
				float v1 = a1 * ic1 + a2 * v3;
				float v2 = ic2 + a2 * ic1 + a3 * v3;
				ic1 = 2.0f * v1 - ic1;
				ic2 = 2.0f * v2 - ic2;
				x[i] = c0 * v0 + c1 * v1 + c2 * v2;
			}
		}
		s->ic1[c] = fx_flush(ic1);
		s->ic2[c] = fx_flush(ic2);
	}
}

const fx_desc fx_desc_filter = {
	FX_FILTER, "Filter", 4, P, sizeof(filter_t), 0, 130, 0, FX_MIX_CROSSFADE, 1,
	init, reset, set_param, process,
};
