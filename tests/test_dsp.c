/* Effect-level DSP tests: boundaries, silence, impulse, isolation, response, numerics. */
#include "t.h"
#include "../firmware/dsp/fx.h"
#include "../firmware/dsp/util.h"

#define SR 48000.0f

typedef struct {
	const fx_desc *d;
	void *st;
	void *buf;
} inst_t;

static inst_t make(int id)
{
	inst_t i;
	i.d = fx_get(id);
	uint32_t sz = fx_block_bytes(i.d);
	i.buf = calloc(1, sz ? sz : 8);
	i.st = i.buf;
	i.d->init(i.st, (uint8_t *)i.buf + fx_align8(i.d->state_bytes), SR);
	return i;
}
static void destroy(inst_t *i) { free(i->buf); }

static void setp(inst_t *i, int idx, float real_value)
{
	i->d->set_param(i->st, idx, fx_param_unmap(&i->d->params[idx], real_value));
}

/* run a mono-in/stereo process over buffers in blocks of 64 */
static void run(inst_t *i, float *l, float *r, int n)
{
	for (int p = 0; p < n; p += 64) {
		int m = n - p < 64 ? n - p : 64;
		i->d->process(i->st, l + p, r + p, m);
	}
}

static uint32_t rng = 12345;
static float noise(void) { return fx_noise(&rng); }

static double rms(const float *x, int a, int b)
{
	double s = 0;
	for (int i = a; i < b; i++) s += (double)x[i] * x[i];
	return sqrt(s / (b - a));
}

static void test_param_roundtrip(void)
{
	for (int id = 1; id < FX_COUNT; id++) {
		const fx_desc *d = fx_get(id);
		CHECK(d->id == id);
		CHECK(d->nparams == 4);
		for (int p = 0; p < d->nparams; p++) {
			const fx_param_desc *q = &d->params[p];
			CHECK_MSG(q->def >= q->min && q->def <= q->max, "%s.%s default out of range", d->name, q->name);
			if (q->scale == FX_SCALE_STEP) {
				for (int v = (int)q->min; v <= (int)q->max; v++)
					CHECK((int)fx_param_map(q, fx_param_unmap(q, (float)v)) == v);
			} else {
				for (float n = 0.0f; n <= 1.0f; n += 0.125f)
					CHECK_NEAR(fx_param_unmap(q, fx_param_map(q, n)), n, 1e-4);
			}
			CHECK(fx_param_map(q, -5.0f) >= q->min - 1e-3f); /* out-of-range input clamps */
			CHECK(fx_param_map(q, 7.0f) <= q->max + 1e-3f);
			CHECK(fx_param_map(q, NAN) == fx_param_map(q, 0.0f));
		}
	}
}

static void test_silence(void)
{
	for (int id = 1; id < FX_COUNT; id++) {
		inst_t i = make(id);
		if (id == FX_TAPE_WOBBLE) setp(&i, 3, 0.0f); /* hiss off */
		static float l[48000], r[48000];
		memset(l, 0, sizeof l);
		memset(r, 0, sizeof r);
		run(&i, l, r, 48000);
		double e = rms(l, 0, 48000) + rms(r, 0, 48000);
		CHECK_MSG(e < 1e-6, "%s: silence in gave rms %g", i.d->name, e);
		destroy(&i);
	}
}

/* every parameter corner combination, loud noise: output must stay finite and bounded */
static void test_boundaries(void)
{
	for (int id = 1; id < FX_COUNT; id++) {
		for (int corner = 0; corner < 16; corner++) {
			inst_t i = make(id);
			for (int p = 0; p < 4; p++) i.d->set_param(i.st, p, (corner >> p) & 1 ? 1.0f : 0.0f);
			static float l[24000], r[24000];
			for (int n = 0; n < 24000; n++) { l[n] = 0.9f * noise(); r[n] = 0.9f * noise(); }
			run(&i, l, r, 24000);
			int ok = 1;
			float mx = 0;
			for (int n = 0; n < 24000; n++) {
				if (fx_bad(l[n]) || fx_bad(r[n])) ok = 0;
				if (fabsf(l[n]) > mx) mx = fabsf(l[n]);
				if (fabsf(r[n]) > mx) mx = fabsf(r[n]);
			}
			CHECK_MSG(ok && mx < 60.0f, "%s corner %d: finite=%d max=%g", i.d->name, corner, ok, mx);
			destroy(&i);
		}
	}
}

/* rapid parameter automation must stay finite too */
static void test_param_sweep_stability(void)
{
	for (int id = 1; id < FX_COUNT; id++) {
		inst_t i = make(id);
		int ok = 1;
		for (int blk = 0; blk < 800; blk++) {
			for (int p = 0; p < 4; p++) i.d->set_param(i.st, p, 0.5f + 0.5f * sinf((float)blk * 0.37f + (float)p));
			float l[64], r[64];
			for (int n = 0; n < 64; n++) { l[n] = 0.7f * noise(); r[n] = 0.7f * noise(); }
			i.d->process(i.st, l, r, 64);
			for (int n = 0; n < 64; n++) if (fx_bad(l[n]) || fx_bad(r[n]) || fabsf(l[n]) > 60.0f) ok = 0;
		}
		CHECK_MSG(ok, "%s unstable under automation", i.d->name);
		destroy(&i);
	}
}

static void test_stereo_isolation(void)
{
	for (int id = 1; id < FX_COUNT; id++) {
		inst_t i = make(id);
		if (!i.d->stereo_independent) { destroy(&i); continue; }
		if (id == FX_TAPE_WOBBLE) setp(&i, 3, 0.0f);
		static float l[24000], r[24000];
		for (int n = 0; n < 24000; n++) { l[n] = 0.5f * noise(); r[n] = 0.0f; }
		run(&i, l, r, 24000);
		double er = rms(r, 0, 24000);
		CHECK_MSG(er < 1e-6, "%s leaks L into R: %g", i.d->name, er);
		destroy(&i);
	}
}

static double tone_gain_db(inst_t *i, float hz)
{
	static float l[48000], r[48000];
	for (int n = 0; n < 48000; n++) { l[n] = 0.25f * sinf(2.0f * FX_PI * hz * (float)n / SR); r[n] = l[n]; }
	run(i, l, r, 48000);
	double o = rms(l, 12000, 48000), in = 0.25 / sqrt(2.0);
	return 20.0 * log10(o / in);
}

static void test_filter_response(void)
{
	inst_t lp = make(FX_FILTER);
	setp(&lp, 0, 1000.0f); setp(&lp, 1, 0.0f); setp(&lp, 2, 0.0f); setp(&lp, 3, 0.0f);
	/* let cutoff glide settle */
	{ static float z[24000], zz[24000]; run(&lp, z, zz, 24000); }
	CHECK_NEAR(tone_gain_db(&lp, 100.0f), 0.0, 1.0);
	CHECK_NEAR(tone_gain_db(&lp, 1000.0f), -3.0, 2.0);
	CHECK(tone_gain_db(&lp, 8000.0f) < -30.0);
	destroy(&lp);

	inst_t hp = make(FX_FILTER);
	setp(&hp, 0, 1000.0f); setp(&hp, 1, 0.0f); setp(&hp, 2, 0.0f); setp(&hp, 3, 1.0f);
	{ static float z[24000], zz[24000]; run(&hp, z, zz, 24000); }
	CHECK(tone_gain_db(&hp, 100.0f) < -30.0);
	CHECK_NEAR(tone_gain_db(&hp, 8000.0f), 0.0, 1.0);
	destroy(&hp);

	/* cutoff parameter is logarithmic: mid position is the geometric mean (~632 Hz) */
	const fx_param_desc *q = &fx_get(FX_FILTER)->params[0];
	CHECK_NEAR(fx_param_map(q, 0.5f), sqrt(20.0 * 20000.0), 1.0);
}

static void test_delay_time_and_feedback(void)
{
	inst_t i = make(FX_DELAY);
	setp(&i, 0, 100.0f); setp(&i, 1, 0.0f); setp(&i, 3, 0.0f);
	{ static float z[96000], zz[96000]; run(&i, z, zz, 96000); } /* settle time glide */
	static float l[24000], r[24000];
	memset(l, 0, sizeof l); memset(r, 0, sizeof r);
	l[0] = 1.0f; r[0] = 1.0f;
	run(&i, l, r, 24000);
	int pk = 0;
	for (int n = 0; n < 24000; n++) if (fabsf(l[n]) > fabsf(l[pk])) pk = n;
	CHECK_MSG(pk == 4800, "echo at %d (want exactly 4800)", pk);
	CHECK_MSG(fabsf(l[pk]) > 0.99f, "echo amplitude %g (glide must land on the target)", l[pk]);
	/* feedback 0: exactly one echo */
	double later = rms(l, pk + 10, 24000);
	CHECK(later < 1e-3);
	destroy(&i);

	/* with feedback the second echo is quieter than the first */
	inst_t j = make(FX_DELAY);
	setp(&j, 0, 100.0f); setp(&j, 1, 50.0f); setp(&j, 2, 0.0f); setp(&j, 3, 0.0f);
	{ static float z[96000], zz[96000]; run(&j, z, zz, 96000); }
	memset(l, 0, sizeof l); memset(r, 0, sizeof r);
	l[0] = 1.0f;
	run(&j, l, r, 24000);
	/* damping 0 still leaves a 16 kHz one-pole in the loop (0.88 at DC-ish), so the repeat is ~0.5*0.88 */
	float e1 = fabsf(l[4800]), e2 = 0;
	for (int n = 9598; n < 9606; n++) if (fabsf(l[n]) > e2) e2 = fabsf(l[n]);
	CHECK_MSG(e2 / e1 > 0.35f && e2 / e1 < 0.55f, "repeat ratio %g", e2 / e1);
	destroy(&j);
}

/* rc5: hot input + maximum feedback saturates gently (fx_fbsat) instead of pinning the
 * 16-bit delay lines at the +-2.0 rail; below full scale nothing changes */
static void test_feedback_saturation(void)
{
	/* the curve: identity up to 1, continuous, monotonic, bounded under the rail */
	float prev = -10.0f;
	for (float x = -6.0f; x <= 6.0f; x += 0.001f) {
		float y = fx_fbsat(x);
		CHECK(y >= prev - 1e-6f);
		CHECK(fabsf(y) < 1.95f);
		if (fabsf(x) <= 1.0f) CHECK(y == x);
		prev = y;
	}
	CHECK_NEAR(fx_fbsat(1.0005f), 1.0005, 1e-3);   /* slope ~1 at the knee: no corner */
	const int ids[3] = { FX_DELAY, FX_CHORUS, FX_FLANGER };
	for (int k = 0; k < 3; k++) {
		inst_t i = make(ids[k]);
		const fx_desc *d = fx_get(ids[k]);
		/* maximum feedback (Delay: param 1; Chorus/Flanger: param 2) */
		setp(&i, ids[k] == FX_DELAY ? 1 : 2, d->params[ids[k] == FX_DELAY ? 1 : 2].max);
		static float l[48000], r[48000];
		int pinned = 0;
		float peak = 0.0f;
		for (int b = 0; b < 8; b++) {
			for (int n = 0; n < 48000; n++) { l[n] = 1.8f * sinf((float)n * 0.05f); r[n] = -l[n]; }
			run(&i, l, r, 48000);
			for (int n = 0; n < 48000; n++) {
				peak = fmaxf(peak, fabsf(l[n]));
				if (fabsf(l[n]) >= 1.999f) pinned++;
				CHECK(l[n] == l[n]);
			}
		}
		CHECK_MSG(pinned == 0, "%s: %d samples at the +-2 rail", d->name, pinned);
		CHECK(peak < 4.0f);
		destroy(&i);
	}
}

static void test_reverb_tail(void)
{
	inst_t i = make(FX_REVERB);
	setp(&i, 0, 1.0f);
	static float l[192000], r[192000];
	memset(l, 0, sizeof l); memset(r, 0, sizeof r);
	l[0] = 1.0f; r[0] = 1.0f;
	run(&i, l, r, 192000);
	double early = rms(l, 4800, 14400) + rms(r, 4800, 14400);
	double late = rms(l, 96000, 192000) + rms(r, 96000, 192000);
	double late1 = rms(l, 96000, 144000) + rms(r, 96000, 144000);
	CHECK(early > 1e-3);
	CHECK_MSG(late < early * 0.03, "tail not decaying: early %g late %g", early, late);
	/* longer decay leaves more energy */
	inst_t j = make(FX_REVERB);
	setp(&j, 0, 8.0f);
	memset(l, 0, sizeof l); memset(r, 0, sizeof r);
	l[0] = 1.0f; r[0] = 1.0f;
	run(&j, l, r, 192000);
	double late8 = rms(l, 96000, 144000) + rms(r, 96000, 144000);
	CHECK(late8 > 5.0 * late1);
	/* stereo: outputs decorrelated */
	double dif = 0;
	for (int n = 9600; n < 24000; n++) dif += fabs(l[n] - r[n]);
	CHECK(dif > 1e-2);
	destroy(&i); destroy(&j);
}

static void test_chorus_delay_position(void)
{
	inst_t i = make(FX_CHORUS);
	setp(&i, 1, 0.0f); setp(&i, 2, 0.0f);
	static float l[4000], r[4000];
	memset(l, 0, sizeof l); memset(r, 0, sizeof r);
	l[0] = 1.0f;
	run(&i, l, r, 4000);
	int pk = 0;
	for (int n = 0; n < 4000; n++) if (fabsf(l[n]) > fabsf(l[pk])) pk = n;
	CHECK_MSG(abs(pk - 672) <= 2, "chorus base delay at %d (want 672)", pk);
	destroy(&i);
}

static void test_tremolo_depth(void)
{
	inst_t i = make(FX_TREMOLO);
	setp(&i, 0, 5.0f); setp(&i, 1, 100.0f); setp(&i, 2, 0.0f);
	static float l[48000], r[48000];
	for (int n = 0; n < 48000; n++) l[n] = r[n] = 1.0f;
	run(&i, l, r, 48000);
	float mn = 9, mx = -9;
	for (int n = 4800; n < 48000; n++) { if (l[n] < mn) mn = l[n]; if (l[n] > mx) mx = l[n]; }
	CHECK(mn < 0.05f);
	CHECK(mx > 0.95f);
	destroy(&i);
}

static void test_compressor_reduces(void)
{
	inst_t i = make(FX_COMPRESSOR);
	setp(&i, 0, -30.0f); setp(&i, 1, 4.0f); setp(&i, 2, 2.0f); setp(&i, 3, 100.0f);
	static float l[48000], r[48000];
	for (int n = 0; n < 48000; n++) { l[n] = 0.5f * sinf(2.0f * FX_PI * 1000.0f * (float)n / SR); r[n] = l[n]; }
	run(&i, l, r, 48000);
	double o = rms(l, 24000, 48000) * sqrt(2.0);
	/* -6 dBFS in, 24 dB over a -30 dB threshold at 4:1 -> 18 dB reduction, +11.25 dB make-up
	   = -6.75 dB net. Peak-detector ripple makes this approximate. */
	double expect = 0.5 * pow(10.0, -6.75 / 20.0);
	CHECK_MSG(fabs(20 * log10(o / expect)) < 1.5, "compressed level %g expected ~%g", o, expect);
	destroy(&i);
}

static void test_fast_math_accuracy(void)
{
	for (float x = 1e-6f; x < 1e6f; x *= 1.37f) {
		CHECK_MSG(fabsf(fx_log2(x) - log2f(x)) < 0.01f, "fx_log2(%g)=%g want %g", x, fx_log2(x), log2f(x));
	}
	for (float x = -30.0f; x < 30.0f; x += 0.173f) {
		float rel = fabsf(fx_exp2(x) / exp2f(x) - 1.0f);
		CHECK_MSG(rel < 2e-3f, "fx_exp2(%g) rel err %g", x, rel);
	}
	for (float x = -6.0f; x <= 6.0f; x += 0.01f) CHECK(fabsf(fx_tanh(x) - tanhf(x)) < 0.03f);
	CHECK(fabsf(fx_sin01(0.25f) - 1.0f) < 1e-6f && fabsf(fx_sin01(0.75f) + 1.0f) < 1e-6f && fabsf(fx_sin01(0.0f)) < 1e-6f);
}

static void test_bitcrusher_levels(void)
{
	inst_t i = make(FX_BITCRUSHER);
	setp(&i, 0, 3.0f); setp(&i, 1, 48000.0f); setp(&i, 2, 100.0f);
	static float l[4800], r[4800];
	for (int n = 0; n < 4800; n++) l[n] = r[n] = 0.8f * sinf(2.0f * FX_PI * 100.0f * (float)n / SR);
	run(&i, l, r, 4800);
	/* error against clean sine is large at 3 bits */
	double err = 0;
	for (int n = 2400; n < 4800; n++) { double c = 0.8 * sin(2.0 * FX_PI * 100.0 * n / SR); err += (l[n] - c) * (l[n] - c); }
	CHECK(sqrt(err / 2400) > 0.03);
	destroy(&i);
}

static void test_nan_inf_input_to_effects(void)
{
	/* effects themselves are not required to survive NaN (the engine sanitises input),
	   but after a reset() they must recover completely. */
	for (int id = 1; id < FX_COUNT; id++) {
		inst_t i = make(id);
		float l[64], r[64];
		for (int n = 0; n < 64; n++) l[n] = r[n] = NAN;
		i.d->process(i.st, l, r, 64);
		i.d->reset(i.st);
		for (int n = 0; n < 64; n++) l[n] = r[n] = 0.1f;
		for (int b = 0; b < 100; b++) i.d->process(i.st, l, r, 64);
		CHECK_MSG(!fx_bad(l[63]) && !fx_bad(r[63]), "%s does not recover after reset", i.d->name);
		destroy(&i);
	}
}

int main(void)
{
	RUN(test_param_roundtrip);
	RUN(test_silence);
	RUN(test_boundaries);
	RUN(test_param_sweep_stability);
	RUN(test_stereo_isolation);
	RUN(test_filter_response);
	RUN(test_delay_time_and_feedback);
	RUN(test_feedback_saturation);
	RUN(test_reverb_tail);
	RUN(test_chorus_delay_position);
	RUN(test_tremolo_depth);
	RUN(test_compressor_reduces);
	RUN(test_fast_math_accuracy);
	RUN(test_bitcrusher_levels);
	RUN(test_nan_inf_input_to_effects);
	T_DONE();
}
