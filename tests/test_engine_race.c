/* Engine config hand-over between the control and the audio thread (docs/BRINGUP_USBAUDIO.md,
 * "Known issue found on hardware"). The host is single-threaded, so the interleavings are made
 * deterministic with engine_t.hook: the engine calls it at every step of a config change
 * (including the window where the old engine_apply_config had new fx ids but the old layout),
 * and in the middle of the audio thread's mailbox copy; the hook runs engine_process() or a
 * nested post exactly there. After every engine_process() the live layout is checked. */
#include "t.h"
#include "../firmware/dsp/engine.h"
#include "../firmware/dsp/util.h"
#include "../firmware/app/factory_presets.h"

#define SR 48000.0f
#define POOL (150 * 1024)
#define GUARD 4096

static uint8_t arena[GUARD + POOL + GUARD] __attribute__((aligned(16)));
static uint8_t *const pool = arena + GUARD;
static engine_t eng;
static uint32_t rng = 12345;

static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static float frand(void) { return (float)(rnd() & 0xFFFF) / 65535.0f; }

static void fresh(uint32_t budget, uint8_t fill)
{
	memset(arena, 0x5A, sizeof arena);
	memset(pool, fill, POOL);
	engine_init(&eng, pool, POOL, SR, budget);
	engine_set_master(&eng, 1.0f);
}

static int guards_ok(void)
{
	for (int i = 0; i < GUARD; i++)
		if (arena[i] != 0x5A || arena[GUARD + POOL + i] != 0x5A) return 0;
	return 1;
}

/* Audio-side invariant: every live slot runs from exactly the layout of the snapshot in use,
 * inside the pool, with no two live slots overlapping. Returns 0 on the first violation. */
static int live_ok(const engine_t *e, char *why, size_t n)
{
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		if (e->live_id[s] == FX_NONE) continue;
		uint32_t z = fx_block_bytes(fx_get(e->live_id[s]));
		if (e->pend_id[s] != FX_NONE) { snprintf(why, n, "slot %d live and pending", s); return 0; }
		if (e->live_id[s] != e->run.fx_id[s]) { snprintf(why, n, "slot %d live fx %d != snapshot fx %d", s, e->live_id[s], e->run.fx_id[s]); return 0; }
		if (e->live_off[s] != e->run_off[s]) { snprintf(why, n, "slot %d STALE layout: live off %u, snapshot off %u", s, e->live_off[s], e->run_off[s]); return 0; }
		if ((uint8_t *)e->live_st[s] != e->pool + e->live_off[s]) { snprintf(why, n, "slot %d state pointer", s); return 0; }
		if (e->live_off[s] + z > e->pool_bytes) { snprintf(why, n, "slot %d outside pool", s); return 0; }
		for (int t = s + 1; t < ENGINE_SLOTS; t++) {
			if (e->live_id[t] == FX_NONE) continue;
			uint32_t zt = fx_block_bytes(fx_get(e->live_id[t]));
			if (e->live_off[s] < e->live_off[t] + zt && e->live_off[t] < e->live_off[s] + z) {
				snprintf(why, n, "slots %d and %d overlap", s, t);
				return 0;
			}
		}
	}
	return 1;
}

/* After everything posted is live: audio half == control half. */
static int settled_ok(const engine_t *e)
{
	if (!engine_config_settled(e)) return 0;
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		if (e->live_id[s] != e->cfg.fx_id[s]) return 0;
		if (e->live_id[s] != FX_NONE && e->live_off[s] != e->blk_off[s]) return 0;
	}
	return 1;
}

static int n_process, n_bad_out, n_bad_live;
static char first_why[160];

static void process_frames(int n)
{
	float l[160], r[160];
	for (int i = 0; i < n; i++) { l[i] = 0.5f * fx_noise(&rng); r[i] = 0.5f * fx_noise(&rng); }
	engine_process(&eng, l, r, l, r, n);
	n_process++;
	for (int i = 0; i < n; i++)
		if (fx_bad(l[i]) || fx_bad(r[i]) || fabsf(l[i]) > 1.0f || fabsf(r[i]) > 1.0f) { n_bad_out++; break; }
	char why[160];
	if (!live_ok(&eng, why, sizeof why)) {
		if (!n_bad_live) snprintf(first_why, sizeof first_why, "%s", why);
		n_bad_live++;
	}
}

static void preset_cfg(int idx, store_data_t *d)
{
	if (idx == 0) store_data_default(d);
	else factory_preset_fill(&factory_presets[idx - 1], d);
}

/* ------------------------------------------------------------------ tests */

/* The deterministic bug of the old engine (found by host simulation of the device sequence):
 * an unchanged effect in an unchanged slot kept running at its OLD offset while the layout
 * said it had moved, so later assignments could be placed on top of live memory. */
static void test_layout_follows_config(void)
{
	static store_data_t d;
	fresh(ENGINE_DEFAULT_CPU_BUDGET, 0);
	n_bad_live = 0;
	preset_cfg(0, &d);
	CHECK(engine_apply_config(&eng, &d.cfg) == 0);
	process_frames(64);
	CHECK(settled_ok(&eng));
	/* FUNCTION through all 6 presets and back, twice, with single-slot edits in between */
	for (int k = 1; k <= 13; k++) {
		preset_cfg(k % 6, &d);
		CHECK(engine_apply_config(&eng, &d.cfg) == 0);
		process_frames(64);
		CHECK_MSG(settled_ok(&eng), "preset %d not settled/consistent", k % 6);
		CHECK(engine_assign(&eng, k & 3, (k % 2) ? FX_CHORUS : FX_TREMOLO) != ENG_ERR_ARG);
		process_frames(64);
		CHECK(settled_ok(&eng));
	}
	CHECK_MSG(n_bad_live == 0, "%d live-layout violations, first: %s", n_bad_live, first_why);
	CHECK(eng.diag.layout_errors == 0);
	CHECK(guards_ok());
}

/* Clean -> Dub Station keeps delay and reverb in place (no re-init, no 120 KB clear). */
static void test_unchanged_slots_keep_memory(void)
{
	static store_data_t d;
	fresh(ENGINE_DEFAULT_CPU_BUDGET, 0);
	preset_cfg(0, &d);
	engine_apply_config(&eng, &d.cfg);
	engine_pump(&eng);
	void *dl = eng.live_st[2], *rv = eng.live_st[3];
	uint32_t inits = eng.diag.slot_inits;
	preset_cfg(1, &d);
	CHECK(engine_apply_config(&eng, &d.cfg) == 0);
	engine_pump(&eng);
	CHECK(eng.live_st[2] == dl && eng.live_st[3] == rv);
	CHECK(eng.diag.slot_inits - inits == 2); /* saturation + filter only */
	CHECK(settled_ok(&eng));
}

/* Run the audio thread at every step of a config change, one step at a time. */
static int hook_point_wanted, hook_hits, half_window_mixed;

static void hook_once(engine_t *e, int pt, void *ctx)
{
	(void)ctx;
	if (pt != hook_point_wanted) return;
	if (pt == ENGINE_HOOK_APPLY_HALF) {
		/* prove we are inside the old race window: control-side fx ids already new,
		 * layout still old (the old audio thread initialised effects from exactly this) */
		for (int s = 0; s < ENGINE_SLOTS; s++)
			if (fx_block_bytes(fx_get(e->cfg.fx_id[s])) != e->blk_size[s]) { half_window_mixed++; break; }
	}
	hook_hits++;
	e->hook = 0;          /* exactly one engine_process at this point */
	process_frames(64);
	e->hook = hook_once;
}

static void test_interleave_every_step(void)
{
	static store_data_t d;
	static const int pts[] = { ENGINE_HOOK_APPLY_SANITIZED, ENGINE_HOOK_APPLY_HALF, ENGINE_HOOK_APPLY_WRITTEN,
				   ENGINE_HOOK_POST_BEGIN, ENGINE_HOOK_POST_MID, ENGINE_HOOK_POST_DONE };
	n_bad_live = n_bad_out = 0;
	half_window_mixed = 0;
	for (unsigned p = 0; p < sizeof pts / sizeof pts[0]; p++) {
		for (int budget = 0; budget < 2; budget++) {
			fresh(ENGINE_DEFAULT_CPU_BUDGET, 0xA5);
			eng.init_budget = budget ? 48u * 1024u : 0u;
			preset_cfg(0, &d);
			engine_apply_config(&eng, &d.cfg);
			process_frames(64);
			for (int k = 1; k <= 12; k++) {
				hook_point_wanted = pts[p];
				hook_hits = 0;
				eng.hook = hook_once;
				preset_cfg(k % 6, &d);
				engine_apply_config(&eng, &d.cfg);
				eng.hook = 0;
				CHECK(hook_hits == 1);
				for (int b = 0; b < 4; b++) process_frames(64);
				CHECK_MSG(settled_ok(&eng), "point %d preset %d", pts[p], k % 6);
			}
		}
	}
	CHECK_MSG(half_window_mixed > 0, "the test never reached the old race window");
	CHECK_MSG(n_bad_live == 0, "%d live-layout violations, first: %s", n_bad_live, first_why);
	CHECK(n_bad_out == 0);
	CHECK(eng.diag.layout_errors == 0);
	CHECK(guards_ok());
}

/* Control thread "pre-empts" the audio thread in the middle of its mailbox copy. */
static int nested_posts;

static void hook_nested(engine_t *e, int pt, void *ctx)
{
	(void)ctx;
	if (pt != ENGINE_HOOK_TAKE_COPIED || nested_posts <= 0) return;
	e->hook = 0;
	for (int i = 0; i < nested_posts; i++) engine_set_master(e, 0.1f * (float)(i + 2));
	nested_posts = 0;
	e->hook = hook_nested;
}

static void test_torn_mailbox_copy(void)
{
	fresh(ENGINE_DEFAULT_CPU_BUDGET, 0);
	engine_set_master(&eng, 0.5f);
	/* one nested post writes the OTHER buffer: the copy is still valid */
	eng.hook = hook_nested;
	nested_posts = 1;
	uint32_t torn = eng.diag.torn_retries;
	engine_pump(&eng);
	CHECK(eng.diag.torn_retries == torn);
	CHECK_NEAR(eng.run.master, 0.5, 1e-6);
	engine_pump(&eng);
	CHECK_NEAR(eng.run.master, 0.2, 1e-6);
	/* two nested posts rewrite the buffer being copied: the copy must be discarded */
	engine_set_master(&eng, 0.9f);
	nested_posts = 2;
	engine_pump(&eng);
	CHECK(eng.diag.torn_retries == torn + 1);
	CHECK_NEAR(eng.run.master, 0.3, 1e-6);   /* the retry took the newest complete post */
	eng.hook = 0;
	CHECK(settled_ok(&eng));
}

/* Init budget: the Dub Station inits are spread over chunks, the config still converges. */
static void test_init_budget(void)
{
	static store_data_t d;
	fresh(ENGINE_DEFAULT_CPU_BUDGET, 0);
	eng.init_budget = 48u * 1024u;
	preset_cfg(1, &d);
	engine_apply_config(&eng, &d.cfg);
	int chunks = 0;
	while (!engine_config_settled(&eng) && chunks < 10) { process_frames(64); chunks++; }
	CHECK(settled_ok(&eng));
	CHECK(chunks == 2); /* saturation+filter+reverb (44 KB in budget), then delay (75 KB alone) */
	CHECK(eng.diag.init_bytes_max <= 76888u);
	fresh(ENGINE_DEFAULT_CPU_BUDGET, 0);
	engine_apply_config(&eng, &d.cfg);
	process_frames(64);
	CHECK(settled_ok(&eng)); /* unlimited: one chunk */
}

/* Engine contract: the engine zeroes only the state struct; init() must clear its mem_bytes
 * (alignment padding after them is never used) and must not write outside its block. Pool pre-filled with garbage. */
static void test_init_clears_memory(void)
{
	for (int f = 1; f < FX_COUNT; f++) {
		const fx_desc *d = fx_get(f);
		fresh(100000, 0xA5);
		CHECK(engine_assign(&eng, 0, FX_TREMOLO) == ENG_OK); /* puts slot 1 at a non-zero offset */
		CHECK(engine_assign(&eng, 1, f) == ENG_OK);
		engine_pump(&eng);
		memset(pool, 0xA5, eng.blk_off[1]);  /* tremolo's block back to garbage for the outside check */
		CHECK(engine_assign(&eng, 1, FX_NONE) == ENG_OK);
		engine_pump(&eng);
		CHECK(engine_assign(&eng, 1, f) == ENG_OK);
		engine_pump(&eng);
		uint32_t off = eng.blk_off[1], z = fx_block_bytes(d), st = fx_align8(d->state_bytes);
		int mem_clean = 1, outside_clean = 1;
		for (uint32_t i = off + st; i < off + st + d->mem_bytes; i++) if (pool[i] != 0) { mem_clean = 0; break; }
		for (uint32_t i = 0; i < off; i++) if (pool[i] != 0xA5) { outside_clean = 0; break; }
		for (uint32_t i = off + z; i < POOL; i++) if (pool[i] != 0xA5) { outside_clean = 0; break; }
		CHECK_MSG(mem_clean, "%s: init() left delay memory uncleared", d->name);
		CHECK_MSG(outside_clean, "%s: init() wrote outside its block", d->name);
		/* and it runs silent-in -> finite-out from garbage-filled memory */
		engine_set_bypass(&eng, 1, 0);
		engine_set_wet(&eng, 1, 1.0f);
		n_bad_out = 0;
		for (int b = 0; b < 50; b++) process_frames(64);
		CHECK(n_bad_out == 0);
	}
	CHECK(guards_ok());
}

/* 10,000 random configs (some invalid) interleaved with engine_process() at random points of
 * the control path, random nested posts inside the audio thread's copy, random block sizes and
 * init budgets. Never: overlap, stale layout, writes outside the pool, non-finite output. */
static void random_cfg(engine_config_t *c)
{
	engine_config_default(c);
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		uint32_t r = rnd() % 100;
		c->fx_id[s] = (uint8_t)(r < 5 ? 200 : rnd() % FX_COUNT);
		c->bypass[s] = (uint8_t)(rnd() % 20 == 0 ? 7 : rnd() % 2);
		c->wet[s] = rnd() % 20 == 0 ? (rnd() % 2 ? NAN : 3.0f) : frand();
		for (int f = 0; f < FX_COUNT; f++)
			for (int i = 0; i < FX_MAX_PARAMS; i++)
				c->params[s][f][i] = rnd() % 100 == 0 ? -1.0f : frand();
	}
	if (rnd() % 20 == 0) { c->order[0] = 3; c->order[1] = 3; }
	else for (int i = 3; i > 0; i--) { int j = (int)(rnd() % (uint32_t)(i + 1)); uint8_t t = c->order[i]; c->order[i] = c->order[j]; c->order[j] = t; }
	c->global_bypass = (uint8_t)(rnd() % 10 == 0);
	c->in_gain_db = rnd() % 20 == 0 ? 99.0f : -12.0f + 24.0f * frand();
	c->master = 0.5f + 0.5f * frand();
}

static int storm_in_hook;

static void hook_storm(engine_t *e, int pt, void *ctx)
{
	(void)ctx;
	if (storm_in_hook) return;
	storm_in_hook = 1;
	if (pt == ENGINE_HOOK_TAKE_COPIED) {
		if (rnd() % 8 == 0) engine_set_param(e, (int)(rnd() % 4), (int)(rnd() % 4), frand());
		if (rnd() % 16 == 0) engine_set_wet(e, (int)(rnd() % 4), frand());
	} else if (rnd() % 3 == 0) {
		process_frames(1 + (int)(rnd() % 130));
	}
	storm_in_hook = 0;
}

static void test_post_storm(void)
{
	static const uint32_t budgets[] = { 0u, 1u, 8192u, 49152u };
	engine_config_t c;
	fresh(ENGINE_DEFAULT_CPU_BUDGET, 0xA5);
	n_process = n_bad_out = n_bad_live = 0;
	eng.hook = hook_storm;
	for (int it = 0; it < 10000; it++) {
		if (it % 500 == 0) eng.init_budget = budgets[rnd() % 4];
		random_cfg(&c);
		engine_post_config(&eng, &c);
		switch (rnd() % 6) {
		case 0: engine_assign(&eng, (int)(rnd() % 4), (int)(rnd() % FX_COUNT)); break;
		case 1: engine_set_bypass(&eng, (int)(rnd() % 4), (int)(rnd() % 2)); break;
		case 2: engine_set_duck(&eng, (int)(rnd() % 2)); break;
		default: break;
		}
		int k = (int)(rnd() % 3);
		for (int b = 0; b < k; b++) process_frames(1 + (int)(rnd() % 130));
		if (it % 1000 == 999) CHECK(guards_ok());
	}
	eng.hook = 0;
	engine_pump(&eng);
	CHECK(settled_ok(&eng));
	process_frames(64);
	printf("  storm: %d engine_process calls, %u snapshots applied, %u effect inits, %u torn copies retried\n",
	       n_process, eng.diag.cfg_applies, eng.diag.slot_inits, eng.diag.torn_retries);
	CHECK_MSG(n_bad_live == 0, "%d live-layout violations, first: %s", n_bad_live, first_why);
	CHECK_MSG(n_bad_out == 0, "%d blocks with non-finite or >1 output", n_bad_out);
	CHECK(eng.diag.layout_errors == 0);
	CHECK(eng.diag.torn_retries > 0);   /* the storm did exercise the torn-copy path */
	CHECK(guards_ok());
}

int main(void)
{
	RUN(test_layout_follows_config);
	RUN(test_unchanged_slots_keep_memory);
	RUN(test_interleave_every_step);
	RUN(test_torn_mailbox_copy);
	RUN(test_init_budget);
	RUN(test_init_clears_memory);
	RUN(test_post_storm);
	T_DONE();
}
