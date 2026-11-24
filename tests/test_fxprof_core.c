/* Profiling core (firmware/audio/fxprof_core.c), the overload guard (engine_overload_feed)
 * and the accuracy of the fast-math helpers added for the CPU optimisation. */
#include "t.h"
#include "../firmware/audio/fxprof_core.h"
#include "../firmware/dsp/util.h"

#define POOL (150 * 1024)
static uint8_t pool[POOL] __attribute__((aligned(8)));
static engine_t eng;

static void run_blocks(int nb, float level)
{
	float l[64], r[64];
	static uint32_t rng = 99;
	for (int b = 0; b < nb; b++) {
		for (int i = 0; i < 64; i++) { l[i] = level * fx_noise(&rng); r[i] = level * fx_noise(&rng); }
		engine_process(&eng, l, r, l, r, 64);
	}
}

/* every case builds a config the engine admits completely, runs finite and is not silent */
static void test_cases(void)
{
	CHECK(fxprof_case(-1) == NULL && fxprof_case(FXPROF_CASES) == NULL);
	int seen_fx[FX_COUNT] = {0}, kinds[5] = {0};
	for (int c = 0; c < FXPROF_CASES; c++) {
		const fxprof_case_t *k = fxprof_case(c);
		CHECK(k != NULL);
		kinds[k->kind]++;
		if (k->kind != FXP_CLEAN && k->kind != FXP_PRESET) seen_fx[k->fx_id]++;
		engine_config_t cfg;
		CHECK(fxprof_case_config(c, &cfg) == 0);
		engine_init(&eng, pool, POOL, 48000.0f, 0xFFFFFFFFu);
		CHECK_MSG(engine_post_config(&eng, &cfg) == 0, "case %d rejected slots", c);
		engine_pump(&eng);
		fxprof_sig_t sig;
		fxprof_sig_init(&sig);
		float l[64], r[64], ol[64], orr[64];
		double e = 0;
		int ok = 1;
		for (uint32_t b = 0; b < 300; b++) {
			fxprof_case_tick(&eng, c, b);
			fxprof_sig_block(&sig, l, r, 64);
			engine_process(&eng, l, r, ol, orr, 64);
			for (int i = 0; i < 64; i++) {
				if (fx_bad(ol[i]) || fx_bad(orr[i]) || fabsf(ol[i]) > 1.0f) ok = 0;
				if (b >= 200) e += ol[i] * ol[i] + orr[i] * orr[i];
			}
		}
		CHECK_MSG(ok, "case %d (%s %s): non-finite or > 1", c, fxprof_case_name(c), fxprof_kind_name(k->kind));
		CHECK_MSG(e > 1e-3, "case %d (%s): silent", c, fxprof_case_name(c));
		CHECK(eng.diag.nan_events == 0);
		if (k->kind == FXP_CLEAN) CHECK(fxprof_case_declared(c) == 0);
		else if (k->kind != FXP_PRESET) CHECK(fxprof_case_declared(c) == fx_get(k->fx_id)->cpu_cycles);
	}
	for (int f = 1; f < FX_COUNT; f++) CHECK_MSG(seen_fx[f] == 3, "effect %d has %d cases", f, seen_fx[f]);
	CHECK(kinds[FXP_CLEAN] == 1 && kinds[FXP_PRESET] == 6);
	CHECK(strcmp(fxprof_case_name(0), "Clean") == 0);
	CHECK(strcmp(fxprof_case_name(FXPROF_CASES - 6), "Dub Station") == 0);
}

/* the signal is deterministic, bounded and loops */
static void test_signal(void)
{
	fxprof_sig_t a, b;
	fxprof_sig_init(&a);
	fxprof_sig_init(&b);
	float l1[64], r1[64], l2[64], r2[64];
	float pk = 0;
	double e = 0;
	for (int k = 0; k < 2000; k++) {
		fxprof_sig_block(&a, l1, r1, 64);
		fxprof_sig_block(&b, l2, r2, 64);
		for (int i = 0; i < 64; i++) {
			CHECK(l1[i] == l2[i] && r1[i] == r2[i]);
			pk = fmaxf(pk, fmaxf(fabsf(l1[i]), fabsf(r1[i])));
			e += l1[i] * l1[i];
		}
		if (t_fail) break;
	}
	CHECK(pk < 0.6f && pk > 0.3f);
	double rms_db = 10 * log10(e / (2000.0 * 64));
	CHECK_MSG(rms_db > -18 && rms_db < -9, "signal rms %g dBFS", rms_db);
	/* loops: frame 2^20 equals frame 0 */
	fxprof_sig_init(&a);
	float first[4], x[64], y[64];
	fxprof_sig_block(&a, x, y, 64);
	memcpy(first, x, sizeof first);
	for (uint32_t n = 64; n < (1u << 20); n += 64) fxprof_sig_block(&a, x, y, 64);
	fxprof_sig_block(&a, x, y, 64);
	CHECK(memcmp(first, x, sizeof first) == 0);
}

static void setup_three(void)
{
	engine_init(&eng, pool, POOL, 48000.0f, 0xFFFFFFFFu);
	engine_config_t c;
	engine_config_default(&c);
	c.fx_id[0] = FX_FILTER; c.fx_id[1] = FX_REVERB; c.fx_id[2] = FX_TREMOLO; c.fx_id[3] = FX_NONE;
	for (int s = 0; s < 4; s++) { c.bypass[s] = 0; c.wet[s] = 0.5f; }
	engine_post_config(&eng, &c);
	run_blocks(4, 0.3f); /* inits happen here */
	engine_overload_feed(&eng, 0, 100);
}

/* feed n blocks of `cycles`; returns how many calls acted */
static int feed_n(uint32_t n, uint32_t cycles, uint32_t lim)
{
	int acted = 0;
	for (uint32_t j = 0; j < n; j++) acted += engine_overload_feed(&eng, cycles, lim);
	return acted;
}

static void test_overload_guard(void)
{
	const uint32_t LIM = 72533u;
	setup_three();
	CHECK(eng.overload_n == ENGINE_OVERLOAD_WINDOWS && eng.overload_win == ENGINE_OVERLOAD_WIN_BLOCKS);
	const uint32_t W = eng.overload_win, N = eng.overload_n;
	/* the hardware case (Ambient, usbaudio v3): ~49 % average with blocks spiking to ~89 %
	 * (above the 85 % limit), even several in a row: never acts, for a long time */
	const uint32_t avg = LIM * 49u / 85u, spike = LIM * 89u / 85u;
	for (int k = 0; k < 20000; k++) {
		uint32_t c = (k % 7 == 0 || (k % 500) < 8) ? spike : avg;
		CHECK(engine_overload_feed(&eng, c, LIM) == 0);
	}
	CHECK(eng.diag.shed_mask == 0 && eng.diag.overload_sheds == 0);
	CHECK(eng.diag.overload_blocks > 0);          /* the spikes were seen ... */
	CHECK(eng.diag.overload_windows == 0);        /* ... but no window mean was over */
	CHECK(eng.diag.ovl_max_mean < LIM);
	/* 8 consecutive blocks at 2x the limit, the rest calm: never acts (the old rule shed here) */
	for (int k = 0; k < 50; k++) {
		CHECK(feed_n(8, LIM * 2u, LIM) == 0);
		CHECK(feed_n(W * 2u - 8u, avg, LIM) == 0);
	}
	CHECK(eng.diag.shed_mask == 0);
	/* at the limit is not over */
	CHECK(feed_n(W * N * 4u, LIM, LIM) == 0);
	/* N-1 over windows then one calm window: the run restarts */
	CHECK(feed_n(W * (N - 1u), LIM + LIM / 10u, LIM) == 0);
	CHECK(feed_n(W, avg, LIM) == 0);
	CHECK(feed_n(W * (N - 1u), LIM + LIM / 10u, LIM) == 0);
	CHECK(eng.diag.shed_mask == 0);
	feed_n(W, avg, LIM);
	/* sustained mild overload (110 %): acts exactly at the end of the N-th window (~128 ms) */
	CHECK(feed_n(W * N - 1u, LIM + LIM / 10u, LIM) == 0);
	CHECK(engine_overload_feed(&eng, LIM + LIM / 10u, LIM) == 1);
	/* the most expensive declared slot (reverb, slot 1) goes first */
	CHECK_MSG(eng.diag.shed_mask == 2u, "shed mask %x", eng.diag.shed_mask);
	run_blocks(1, 0.3f); /* fade-out chunk */
	CHECK(eng.shed[1] == 2 && eng.active[1] == 1);
	run_blocks(1, 0.3f);
	CHECK(eng.active[1] == 0); /* stopped and reset, no tail */
	/* safety net: windows whose mean is over 9/8 of the limit act after TWO windows (rc3:
	 * one 114 % window on hardware shed three slots) */
	uint32_t order[3] = {0, 0, 0};
	for (int step = 0; step < 3; step++) {
		uint8_t before = eng.diag.shed_mask;
		CHECK(feed_n(2u * W - 1u, LIM * 2u, LIM) == 0);
		CHECK(engine_overload_feed(&eng, LIM * 2u, LIM) == 1);
		run_blocks(2, 0.3f);
		order[step] = eng.diag.shed_mask & ~before;
	}
	const fx_desc *f = fx_get(FX_FILTER), *t = fx_get(FX_TREMOLO);
	CHECK(order[0] == (f->cpu_cycles >= t->cpu_cycles ? 1u : 4u));
	CHECK(order[1] == (f->cpu_cycles >= t->cpu_cycles ? 4u : 1u));
	CHECK(order[2] == 0 && eng.diag.forced_bypass == 1 && eng.diag.overload_forced == 1);
	CHECK(eng.diag.shed_mask == 7u && eng.diag.overload_sheds == 3);
	/* everything off: output = input * master (dry), finite */
	float l[64], r[64];
	for (int i = 0; i < 64; i++) { l[i] = 0.25f; r[i] = -0.25f; }
	for (int b = 0; b < 200; b++) {
		float a[64], c[64];
		memcpy(a, l, sizeof a); memcpy(c, r, sizeof c);
		engine_process(&eng, a, c, a, c, 64);
		if (b == 199) CHECK_NEAR(a[63] / c[63], -1.0, 1e-4);
	}
	/* clear: control request taken at the next chunk; the partial window is dropped too */
	feed_n(W / 2u, LIM * 2u, LIM);
	engine_overload_clear(&eng);
	run_blocks(1, 0.3f);
	CHECK(eng.diag.shed_mask == 0 && eng.diag.forced_bypass == 0 && eng.ov_cnt == 0);
	run_blocks(3, 0.3f);
	CHECK(eng.active[0] && eng.active[1] && eng.active[2]);
	/* blocks that initialised an effect never count (neither in the sum nor the window) */
	engine_assign(&eng, 3, FX_DELAY);
	uint32_t cnt0 = eng.ov_cnt;
	run_blocks(1, 0.1f);
	engine_overload_feed(&eng, 0xFFFFFFFFu, LIM);
	CHECK(eng.ov_cnt == cnt0 && eng.diag.shed_mask == 0);
	for (int k = 0; k < 200; k++) {
		run_blocks(1, 0.1f);
		engine_overload_feed(&eng, 0, LIM);
	}
	CHECK(eng.diag.shed_mask == 0);
	/* one bad window then a normal one: nothing (the rc2 hardware case: 114 % once) */
	setup_three();
	CHECK(feed_n(W, LIM * 114u / 85u, LIM) == 0);
	CHECK(feed_n(W, LIM / 2u, LIM) == 0);
	CHECK(eng.diag.shed_mask == 0);
	/* a new effect in a shed slot starts unshed */
	setup_three();
	feed_n(2u * W, LIM * 2u, LIM);
	CHECK(eng.diag.shed_mask == 2u);
	engine_assign(&eng, 1, FX_CHORUS);
	run_blocks(2, 0.3f);
	CHECK(eng.diag.shed_mask == 0 && eng.shed[1] == 0 && eng.active[1]);
	/* auto-recovery: after ~10 s of calm windows one slot comes back (cheapest first);
	 * shedding the same slot again doubles the wait */
	setup_three();
	eng.overload_recover_win = 10u;
	feed_n(2u * W, LIM * 2u, LIM);          /* sheds slot 1 (reverb) */
	feed_n(2u * W, LIM * 2u, LIM);          /* and the next */
	run_blocks(2, 0.3f);
	uint8_t m2 = eng.diag.shed_mask;
	CHECK(__builtin_popcount(m2) == 2);
	CHECK(feed_n(W * 9u, LIM / 2u, LIM) == 0);     /* 9 calm windows: not yet */
	CHECK(engine_overload_feed(&eng, LIM / 2u, LIM) == 0 && feed_n(W - 1u, LIM / 2u, LIM) == 2); /* 10th */
	CHECK(__builtin_popcount(eng.diag.shed_mask) == 1 && eng.diag.overload_recovers == 1);
	run_blocks(2, 0.3f);
	/* the remaining one after another 10 calm windows */
	CHECK(feed_n(W * 10u, LIM / 2u, LIM) == 2);
	CHECK(eng.diag.shed_mask == 0);
	run_blocks(4, 0.3f);
	CHECK(eng.active[0] && eng.active[1] && eng.active[2]);
	/* not calm enough (80 %+ of the limit) never recovers */
	feed_n(2u * W, LIM * 2u, LIM);
	CHECK(eng.diag.shed_mask != 0);
	CHECK(feed_n(W * 50u, LIM * 82u / 100u, LIM) == 0 && eng.diag.shed_mask != 0);
	/* back-off: that was the reverb's 2nd shed -> 20 calm windows; a 3rd shed -> 40 */
	CHECK(feed_n(W * 19u, LIM / 2u, LIM) == 0 && eng.diag.shed_mask != 0);
	CHECK(feed_n(W, LIM / 2u, LIM) == 2 && eng.diag.shed_mask == 0);
	run_blocks(2, 0.3f);
	feed_n(2u * W, LIM * 2u, LIM);           /* same slot shed a third time */
	CHECK(eng.diag.shed_mask != 0);
	CHECK(feed_n(W * 39u, LIM / 2u, LIM) == 0 && eng.diag.shed_mask != 0);
	CHECK(feed_n(W, LIM / 2u, LIM) == 2 && eng.diag.shed_mask == 0);
	/* skipped blocks (flash stall) are not in the window */
	setup_three();
	uint32_t c0 = eng.ov_cnt;
	engine_overload_skip(&eng);
	CHECK(eng.ov_cnt == c0);
	/* disabled guard */
	setup_three();
	eng.overload_n = 0;
	for (int j = 0; j < 1000; j++) CHECK(engine_overload_feed(&eng, LIM * 3, LIM) == 0);
	CHECK(eng.diag.shed_mask == 0);
}

/* the guard's fade-out has no step larger than the signal itself allows */
static void test_overload_fade_no_click(void)
{
	engine_init(&eng, pool, POOL, 48000.0f, 0xFFFFFFFFu);
	engine_config_t c;
	engine_config_default(&c);
	for (int s = 0; s < 4; s++) c.fx_id[s] = FX_NONE;
	c.fx_id[0] = FX_TREMOLO; c.bypass[0] = 0; c.wet[0] = 1.0f;
	c.params[0][FX_TREMOLO][1] = 0.0f; /* depth 0: wet == dry, so shedding must be inaudible */
	engine_post_config(&eng, &c);
	float l[64], r[64], prev = 0;
	float maxstep = 0;
	for (int b = 0; b < 400; b++) {
		for (int i = 0; i < 64; i++) l[i] = r[i] = 0.5f;
		if (b == 300) for (uint32_t j = 0; j < eng.overload_win; j++) engine_overload_feed(&eng, 1u << 30, 1000);
		engine_process(&eng, l, r, l, r, 64);
		engine_overload_feed(&eng, 0, 1000); /* as the firmware: once per block */
		for (int i = 0; i < 64; i++) {
			if (b > 250) maxstep = fmaxf(maxstep, fabsf(l[i] - prev));
			prev = l[i];
		}
	}
	CHECK(eng.diag.shed_mask == 1u);
	CHECK_MSG(maxstep < 1e-3f, "step %g", maxstep);
}

static void test_fast_helpers(void)
{
	/* fx_floori: exact against floorf */
	for (float x = -1000.0f; x < 1000.0f; x += 0.37f) CHECK(fx_floori(x) == (int)floorf(x));
	const float edge[] = {-2.0f, -1.0f, -0.5f, -1e-7f, 0.0f, 1e-7f, 0.5f, 1.0f, 2.0f, -120.0f, 119.99f};
	for (unsigned k = 0; k < sizeof edge / sizeof edge[0]; k++) CHECK(fx_floori(edge[k]) == (int)floorf(edge[k]));
	/* fx_exp2 with fx_floori: same accuracy as before, incl. integer inputs */
	for (float x = -30.0f; x <= 30.0f; x += 1.0f) CHECK(fabsf(fx_exp2(x) / exp2f(x) - 1.0f) < 2e-3f);
	/* fx_softclip: odd, bounded, monotonic, continuous; within 0.1 of tanh */
	float prev = -2.0f;
	for (float x = -5.0f; x <= 5.0f; x += 0.001f) {
		float y = fx_softclip(x);
		CHECK(fabsf(y + fx_softclip(-x)) < 1e-6f);
		CHECK(y >= -1.0f && y <= 1.0f);
		CHECK(y >= prev - 1e-6f);
		CHECK(fabsf(y - prev) < 0.002f || prev == -2.0f);
		CHECK(fabsf(y - fx_tanh(x)) < 0.1f); /* the curve it replaces in saturation/filter */
		prev = y;
	}
	CHECK(fabsf(fx_softclip(0.01f) - 0.01f) < 1e-5f); /* unity slope at 0 */
	/* fx_flush_fast: exact for normal signal levels, never denormal, zero for tiny values */
	for (float x = 1e-10f; x < 10.0f; x *= 1.1f) {
		CHECK(fabsf(fx_flush_fast(x) - x) <= x * 1e-6f);
		CHECK(fx_flush_fast(-x) == -fx_flush_fast(x));
	}
	for (float x = 2e-11f; x < 10.0f; x *= 1.1f) CHECK(fx_flush_fast(x) == x);
	CHECK(fx_flush_fast(1e-30f) == 0.0f && fx_flush_fast(-3e-38f) == 0.0f);
	for (float x = 1e-30f; x < 1e-9f; x *= 1.5f) {
		float y = fx_flush_fast(x);
		CHECK(y == 0.0f || fabsf(y) >= 1e-26f);
	}
	/* fx_zero32 / fx_copy32 / fx_zero16 */
	uint32_t a[37], b[37];
	for (int i = 0; i < 37; i++) { a[i] = 0xA5A5A5A5u + (uint32_t)i; b[i] = 1; }
	fx_copy32(b, a, 37);
	CHECK(memcmp(a, b, sizeof a) == 0);
	fx_zero32(b, 36);
	CHECK(b[35] == 0 && b[0] == 0 && b[36] == a[36]);
	int16_t h[41];
	for (int i = 0; i < 41; i++) h[i] = 7;
	fx_zero16(h + 1, 39); /* odd start, odd length */
	CHECK(h[0] == 7 && h[40] == 7);
	int z = 1;
	for (int i = 1; i < 40; i++) z &= h[i] == 0;
	CHECK(z);
	/* fx_dl_q matches the old round-and-clamp exactly */
	for (float x = -3.0f; x <= 3.0f; x += 0.0000913f) {
		float v = x * FX_DL_SCALE;
		int s = (int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
		if (s > 32767) s = 32767;
		if (s < -32768) s = -32768;
		CHECK(fx_dl_q(x) == (int16_t)s);
		if (t_fail) break;
	}
}

int main(void)
{
	RUN(test_cases);
	RUN(test_signal);
	RUN(test_overload_guard);
	RUN(test_overload_fade_no_click);
	RUN(test_fast_helpers);
	T_DONE();
}
