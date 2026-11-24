/* Engine tests: mixing, bypass, assignment, admission control, multiple instances, numerics. */
#include "t.h"
#include "../firmware/dsp/engine.h"
#include "../firmware/dsp/util.h"

#define SR 48000.0f
#define POOL (150 * 1024)

static uint8_t pool[POOL];
static engine_t eng;
static uint32_t rng = 777;

static void fresh(uint32_t budget)
{
	memset(pool, 0, sizeof pool);
	engine_init(&eng, pool, POOL, SR, budget);
	engine_set_master(&eng, 1.0f);
	eng.master_cur = 1.0f;
}

static void run_noise(int frames, float amp, float *outl, float *outr, float *inl_keep)
{
	for (int p = 0; p < frames; p += 64) {
		int m = frames - p < 64 ? frames - p : 64;
		float l[64], r[64];
		for (int i = 0; i < m; i++) { l[i] = amp * fx_noise(&rng); r[i] = amp * fx_noise(&rng); if (inl_keep) inl_keep[p + i] = l[i]; }
		engine_process(&eng, l, r, l, r, m);
		if (outl) memcpy(outl + p, l, m * sizeof(float));
		if (outr) memcpy(outr + p, r, m * sizeof(float));
	}
}

static void test_wet_dry_and_bypass(void)
{
	fresh(850);
	CHECK(engine_assign(&eng, 0, FX_FILTER) == ENG_OK);
	const fx_desc *d = fx_get(FX_FILTER);
	engine_set_param(&eng, 0, 0, fx_param_unmap(&d->params[0], 300.0f));
	engine_set_bypass(&eng, 0, 0);

	static float in[24000], ol[24000], orr[24000];
	/* wet = 0: output equals input */
	engine_set_wet(&eng, 0, 0.0f);
	run_noise(12000, 0.3f, 0, 0, 0);
	run_noise(24000, 0.3f, ol, orr, in);
	double maxd = 0;
	for (int i = 0; i < 24000; i++) if (fabs(ol[i] - in[i]) > maxd) maxd = fabs(ol[i] - in[i]);
	CHECK_MSG(maxd < 1e-4, "wet=0 differs from dry by %g", maxd);

	/* wet = 1: heavy low-pass changes the signal a lot */
	engine_set_wet(&eng, 0, 1.0f);
	run_noise(24000, 0.3f, 0, 0, 0);
	run_noise(24000, 0.3f, ol, orr, in);
	double e = 0;
	for (int i = 0; i < 24000; i++) e += fabs(ol[i] - in[i]);
	CHECK(e / 24000 > 0.05);

	/* bypass: back to dry */
	engine_set_bypass(&eng, 0, 1);
	run_noise(12000, 0.3f, 0, 0, 0);
	run_noise(24000, 0.3f, ol, orr, in);
	maxd = 0;
	for (int i = 0; i < 24000; i++) if (fabs(ol[i] - in[i]) > maxd) maxd = fabs(ol[i] - in[i]);
	CHECK_MSG(maxd < 1e-4, "bypass differs from dry by %g", maxd);

	/* global bypass too */
	engine_set_bypass(&eng, 0, 0);
	run_noise(24000, 0.3f, 0, 0, 0);
	engine_set_global_bypass(&eng, 1);
	run_noise(12000, 0.3f, 0, 0, 0);
	run_noise(24000, 0.3f, ol, orr, in);
	maxd = 0;
	for (int i = 0; i < 24000; i++) if (fabs(ol[i] - in[i]) > maxd) maxd = fabs(ol[i] - in[i]);
	CHECK_MSG(maxd < 1e-4, "global bypass differs from dry by %g", maxd);
}

static void test_assignment_preserves_state(void)
{
	fresh(850);
	CHECK(engine_assign(&eng, 1, FX_FILTER) == ENG_OK);
	engine_set_wet(&eng, 1, 0.31f);
	engine_set_bypass(&eng, 1, 1);
	engine_set_param(&eng, 1, 0, 0.8f);
	CHECK(engine_assign(&eng, 1, FX_CHORUS) == ENG_OK);
	CHECK_NEAR(eng.cfg.wet[1], 0.31, 1e-6);
	CHECK(eng.cfg.bypass[1] == 1);
	engine_set_param(&eng, 1, 0, 0.2f);
	CHECK(engine_assign(&eng, 1, FX_FILTER) == ENG_OK);
	/* the filter's cutoff remembered; the chorus's rate remembered separately */
	CHECK_NEAR(eng.cfg.params[1][FX_FILTER][0], 0.8, 1e-6);
	CHECK_NEAR(eng.cfg.params[1][FX_CHORUS][0], 0.2, 1e-6);
	/* other slots untouched */
	CHECK(eng.cfg.fx_id[0] == FX_NONE);
	/* invalid arguments */
	CHECK(engine_assign(&eng, 9, FX_FILTER) == ENG_ERR_ARG);
	CHECK(engine_assign(&eng, 0, 99) == ENG_ERR_ARG);
}

static void test_admission_memory(void)
{
	fresh(100000);
	CHECK(engine_assign(&eng, 0, FX_REVERB) == ENG_OK);
	CHECK(engine_assign(&eng, 1, FX_REVERB) == ENG_OK);
	/* 2 x reverb (~44 KB) + delay (75 KB) does not fit in 150 KB */
	engine_status st = engine_assign(&eng, 2, FX_DELAY);
	CHECK(st == ENG_ERR_MEM);
	CHECK(eng.cfg.fx_id[2] == FX_NONE); /* previous (empty) assignment preserved */
	/* a failed replacement keeps the OLD effect */
	CHECK(engine_assign(&eng, 2, FX_CHORUS) == ENG_OK);
	CHECK(engine_assign(&eng, 2, FX_DELAY) == ENG_ERR_MEM);
	CHECK(eng.cfg.fx_id[2] == FX_CHORUS);
	/* audio still runs fine afterwards */
	static float l[4800], r[4800];
	run_noise(4800, 0.3f, l, r, 0);
	for (int i = 0; i < 4800; i++) CHECK(!fx_bad(l[i]));
	/* freeing a slot makes room again */
	CHECK(engine_assign(&eng, 1, FX_NONE) == ENG_OK);
	CHECK(engine_assign(&eng, 2, FX_DELAY) == ENG_OK);
	uint32_t mem, cpu;
	engine_resources(&eng, &mem, &cpu);
	CHECK(mem <= POOL);
}

static void test_admission_cpu(void)
{
	/* declared: reverb 240, filter 130, tremolo 115, compressor 110 */
	fresh(230);
	CHECK(engine_assign(&eng, 0, FX_REVERB) == ENG_ERR_CPU); /* 240 > 230 */
	CHECK(eng.cfg.fx_id[0] == FX_NONE);
	CHECK(engine_assign(&eng, 0, FX_FILTER) == ENG_OK);
	CHECK(engine_assign(&eng, 1, FX_COMPRESSOR) == ENG_ERR_CPU); /* 240 > 230 */
	fresh(355);
	CHECK(engine_assign(&eng, 0, FX_REVERB) == ENG_OK);
	CHECK(engine_assign(&eng, 1, FX_TREMOLO) == ENG_OK);      /* 355 = budget: admitted */
	CHECK(engine_assign(&eng, 2, FX_COMPRESSOR) == ENG_ERR_CPU);
	CHECK(engine_assign(&eng, 1, FX_COMPRESSOR) == ENG_OK);   /* replacing frees its own share */
}

static void test_blocks_never_overlap(void)
{
	/* repeatedly reassign slots randomly; layout must never overlap or exceed the pool */
	fresh(100000);
	uint32_t r = 99;
	for (int it = 0; it < 2000; it++) {
		r = r * 1664525u + 1013904223u;
		int slot = (int)((r >> 8) % 4), id = (int)((r >> 16) % FX_COUNT);
		engine_assign(&eng, slot, id);
		for (int a = 0; a < 4; a++) {
			CHECK(eng.blk_off[a] + eng.blk_size[a] <= POOL);
			for (int b = a + 1; b < 4; b++) {
				if (!eng.blk_size[a] || !eng.blk_size[b]) continue;
				int ov = eng.blk_off[a] < eng.blk_off[b] + eng.blk_size[b] && eng.blk_off[b] < eng.blk_off[a] + eng.blk_size[a];
				if (ov) { CHECK(!ov); return; }
			}
		}
		if (it % 50 == 0) { float l[64] = {0}, rr[64] = {0}; engine_process(&eng, l, rr, l, rr, 64); }
	}
}

static void test_multiple_instances(void)
{
	fresh(850);
	CHECK(engine_assign(&eng, 0, FX_FILTER) == ENG_OK);
	CHECK(engine_assign(&eng, 1, FX_FILTER) == ENG_OK);
	CHECK(engine_assign(&eng, 2, FX_CHORUS) == ENG_OK);
	CHECK(engine_assign(&eng, 3, FX_CHORUS) == ENG_OK);
	for (int s = 0; s < 4; s++) { engine_set_bypass(&eng, s, 0); engine_set_wet(&eng, s, 1.0f); }
	/* independent parameters per instance */
	const fx_desc *d = fx_get(FX_FILTER);
	engine_set_param(&eng, 0, 0, fx_param_unmap(&d->params[0], 200.0f));
	engine_set_param(&eng, 1, 0, fx_param_unmap(&d->params[0], 9000.0f));
	CHECK(eng.cfg.params[0][FX_FILTER][0] != eng.cfg.params[1][FX_FILTER][0]);
	static float l[24000], r[24000];
	run_noise(24000, 0.3f, l, r, 0);
	for (int i = 0; i < 24000; i++) CHECK(!fx_bad(l[i]) && !fx_bad(r[i]));
	CHECK(eng.live_st[0] != eng.live_st[1]);
	/* two reverbs and a chorus */
	fresh(850);
	CHECK(engine_assign(&eng, 0, FX_REVERB) == ENG_OK);
	CHECK(engine_assign(&eng, 1, FX_REVERB) == ENG_OK);
	CHECK(engine_assign(&eng, 2, FX_CHORUS) == ENG_OK);
}

static void test_numerics(void)
{
	fresh(850);
	engine_assign(&eng, 0, FX_FILTER);
	engine_assign(&eng, 1, FX_DELAY);
	engine_assign(&eng, 2, FX_REVERB);
	for (int s = 0; s < 3; s++) { engine_set_bypass(&eng, s, 0); engine_set_wet(&eng, s, 1.0f); }
	float l[64], r[64];
	for (int i = 0; i < 64; i++) { l[i] = (i & 1) ? NAN : INFINITY; r[i] = -INFINITY; }
	engine_process(&eng, l, r, l, r, 64);
	for (int i = 0; i < 64; i++) CHECK(!fx_bad(l[i]) && !fx_bad(r[i]));
	for (int b = 0; b < 500; b++) {
		for (int i = 0; i < 64; i++) { l[i] = 0.2f * fx_noise(&rng); r[i] = l[i]; }
		engine_process(&eng, l, r, l, r, 64);
		for (int i = 0; i < 64; i++) if (fx_bad(l[i]) || fx_bad(r[i])) { CHECK(0); return; }
	}
	/* hot signal + max input gain never exceeds full scale at the output */
	engine_set_input_gain_db(&eng, 99.0f); /* clamped to +12 */
	CHECK_NEAR(eng.cfg.in_gain_db, 12.0, 1e-6);
	for (int b = 0; b < 200; b++) {
		for (int i = 0; i < 64; i++) { l[i] = (i & 8) ? 1.0f : -1.0f; r[i] = l[i]; }
		engine_process(&eng, l, r, l, r, 64);
		for (int i = 0; i < 64; i++) if (fabsf(l[i]) > 1.0f || fabsf(r[i]) > 1.0f) { CHECK(0); return; }
	}
	CHECK(eng.diag.in_clips > 0);
	/* bad setter inputs are ignored */
	engine_set_wet(&eng, 0, NAN);
	CHECK(eng.cfg.wet[0] == 1.0f);
	engine_set_param(&eng, 0, 0, NAN);
}

static void test_switch_no_pops(void)
{
	fresh(850);
	engine_set_bypass(&eng, 0, 0);
	engine_set_wet(&eng, 0, 1.0f);
	float ph = 0, maxstep = 0, prev = 0;
	for (int k = 0; k < FX_COUNT * 3; k++) {
		engine_assign(&eng, 0, k % FX_COUNT);
		for (int b = 0; b < 40; b++) {
			float l[64], r[64];
			for (int i = 0; i < 64; i++) { l[i] = r[i] = 0.3f * sinf(ph); ph += 2.0f * FX_PI * 200.0f / SR; }
			engine_process(&eng, l, r, l, r, 64);
			for (int i = 0; i < 64; i++) {
				float st = fabsf(l[i] - prev);
				if (k > 0 && !(k == 1 && b == 0 && i < 4000) && st > maxstep && b * 64 + i > 2000) maxstep = st;
				prev = l[i];
			}
		}
	}
	CHECK_MSG(maxstep < 0.2f, "largest sample-to-sample step while switching: %g", maxstep);
}

static void test_tail_after_bypass(void)
{
	fresh(850);
	engine_assign(&eng, 0, FX_REVERB);
	engine_set_bypass(&eng, 0, 0);
	engine_set_wet(&eng, 0, 1.0f);
	static float l[48000], r[48000];
	run_noise(24000, 0.5f, 0, 0, 0);
	engine_set_bypass(&eng, 0, 1);
	/* input stops feeding the reverb; its tail keeps ringing audibly */
	for (int i = 0; i < 48000; i += 64) {
		float a[64] = {0}, b[64] = {0};
		engine_process(&eng, a, b, a, b, 64);
		memcpy(l + i, a, 64 * sizeof(float)); memcpy(r + i, b, 64 * sizeof(float));
	}
	double e = 0;
	for (int i = 4800; i < 24000; i++) e += fabs(l[i]);
	CHECK_MSG(e > 0.01, "tail vanished on bypass (sum %g)", e);
	/* and eventually the slot idles */
	for (int i = 0; i < 20 * 750; i++) { float a[64] = {0}, b[64] = {0}; engine_process(&eng, a, b, a, b, 64); }
	CHECK(eng.active[0] == 0);
}

static void test_config_sanitize_and_apply(void)
{
	engine_config_t c;
	engine_config_default(&c);
	CHECK(engine_config_sanitize(&c) == 0);
	c.fx_id[0] = 200;
	c.wet[1] = NAN;
	c.params[2][FX_DELAY][1] = 5.0f;
	c.order[0] = 2; c.order[1] = 2;
	c.in_gain_db = INFINITY;
	c.bypass[3] = 7;
	int fixes = engine_config_sanitize(&c);
	CHECK(fixes >= 6);
	CHECK(c.fx_id[0] == FX_NONE);
	CHECK(c.wet[1] == 0.5f);
	CHECK(c.order[1] == 1);
	CHECK(c.in_gain_db == 0.0f);
	CHECK(engine_config_sanitize(&c) == 0);

	/* a config that cannot fit is applied partially, never crashes */
	fresh(100000);
	engine_config_default(&c);
	for (int s = 0; s < 4; s++) c.fx_id[s] = FX_DELAY; /* 4 x 77 KB */
	int rejected = engine_apply_config(&eng, &c);
	CHECK(rejected == 3);
	CHECK(eng.cfg.fx_id[0] == FX_DELAY);
	float l[64] = {0}, r[64] = {0};
	engine_process(&eng, l, r, l, r, 64);
}

static void test_set_order(void)
{
	fresh(850);
	uint8_t good[4] = {3, 2, 1, 0}, bad[4] = {0, 0, 1, 2};
	CHECK(engine_set_order(&eng, good) == 0);
	CHECK(eng.cfg.order[0] == 3);
	CHECK(engine_set_order(&eng, bad) == -1);
	CHECK(eng.cfg.order[0] == 3);
	/* order matters: compressor then saturation differs from saturation then compressor */
	fresh(850);
	engine_assign(&eng, 0, FX_SATURATION);
	engine_assign(&eng, 1, FX_FILTER);
	const fx_desc *d = fx_get(FX_FILTER);
	engine_set_param(&eng, 1, 0, fx_param_unmap(&d->params[0], 400.0f));
	engine_set_param(&eng, 1, 2, 1.0f); /* filter drive */
	for (int s = 0; s < 2; s++) { engine_set_bypass(&eng, s, 0); engine_set_wet(&eng, s, 1.0f); }
	static float a[12000], b[12000], rr[12000];
	rng = 5; run_noise(24000, 0.3f, 0, 0, 0);
	rng = 5; run_noise(12000, 0.3f, a, rr, 0);
	uint8_t flip[4] = {1, 0, 2, 3};
	engine_set_order(&eng, flip);
	run_noise(24000, 0.3f, 0, 0, 0);
	rng = 5; run_noise(12000, 0.3f, b, rr, 0);
	double diff = 0;
	for (int i = 0; i < 12000; i++) diff += fabs(a[i] - b[i]);
	CHECK(diff > 1.0);
}

int main(void)
{
	RUN(test_wet_dry_and_bypass);
	RUN(test_assignment_preserves_state);
	RUN(test_admission_memory);
	RUN(test_admission_cpu);
	RUN(test_blocks_never_overlap);
	RUN(test_multiple_instances);
	RUN(test_numerics);
	RUN(test_switch_no_pops);
	RUN(test_tail_after_bypass);
	RUN(test_config_sanitize_and_apply);
	RUN(test_set_order);
	T_DONE();
}
