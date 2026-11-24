#include "engine.h"
#include "util.h"
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ config */

void engine_config_default(engine_config_t *c)
{
	memset(c, 0, sizeof *c);
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		c->wet[s] = 0.5f;
		c->bypass[s] = 1; /* start safe: nothing audible until the user enables it */
		c->order[s] = (uint8_t)s;
		for (int f = 0; f < FX_COUNT; f++)
			for (int i = 0; i < FX_MAX_PARAMS; i++)
				c->params[s][f][i] = fx_param_default_norm(fx_get(f), i);
	}
	c->fx_id[0] = FX_FILTER;
	c->fx_id[1] = FX_CHORUS;
	c->fx_id[2] = FX_DELAY;
	c->fx_id[3] = FX_REVERB;
	c->master = 0.75f;
}

static int fix_unit(float *x, float def)
{
	if (!(*x >= 0.0f && *x <= 1.0f)) { *x = def; return 1; }
	return 0;
}

int engine_config_sanitize(engine_config_t *c)
{
	int fixes = 0;
	int seen[ENGINE_SLOTS] = {0};
	int order_ok = 1;
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		if (c->order[s] >= ENGINE_SLOTS || seen[c->order[s]]) order_ok = 0;
		else seen[c->order[s]] = 1;
	}
	if (!order_ok) {
		for (int s = 0; s < ENGINE_SLOTS; s++) c->order[s] = (uint8_t)s;
		fixes++;
	}
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		if (c->fx_id[s] >= FX_COUNT) { c->fx_id[s] = FX_NONE; fixes++; }
		if (c->bypass[s] > 1) { c->bypass[s] = 1; fixes++; }
		fixes += fix_unit(&c->wet[s], 0.5f);
		for (int f = 0; f < FX_COUNT; f++)
			for (int i = 0; i < FX_MAX_PARAMS; i++)
				fixes += fix_unit(&c->params[s][f][i], fx_param_default_norm(fx_get(f), i));
	}
	if (c->global_bypass > 1) { c->global_bypass = 0; fixes++; }
	if (!(c->in_gain_db >= -12.0f && c->in_gain_db <= 12.0f)) { c->in_gain_db = 0.0f; fixes++; }
	fixes += fix_unit(&c->master, 0.75f);
	return fixes;
}

/* ------------------------------------------------------------------ setup */

/* Full memory barrier: orders the mailbox accesses between the control and the audio
 * thread (DMB on Cortex-M; also a compiler barrier). */
#if defined(__GNUC__) || defined(__clang__)
#define ENGINE_BARRIER() __sync_synchronize()
#else
#error "ENGINE_BARRIER: no memory barrier for this compiler"
#endif

#define HOOK(e, pt) do { if ((e)->hook) (e)->hook((e), (pt), (e)->hook_ctx); } while (0)

void engine_init(engine_t *e, void *pool, uint32_t pool_bytes, float sr, uint32_t cpu_budget)
{
	memset(e, 0, sizeof *e);
	e->pool = pool;
	e->pool_bytes = pool_bytes;
	e->sr = sr;
	e->cpu_budget = cpu_budget;
	engine_config_default(&e->cfg);
	for (int s = 0; s < ENGINE_SLOTS; s++) e->cfg.fx_id[s] = FX_NONE;
	e->run = e->cfg;               /* nothing live, nothing posted */
	e->k_mix = fx_coef(0.008f, sr);
	e->k_in = fx_coef(0.005f, sr);
	e->k_gain = fx_coef(0.010f, sr);
	e->k_master = fx_coef(0.020f, sr);
	e->k_duck = fx_coef(0.004f, sr);
	e->duck_cur = 1.0f;
	e->ingain_cur = 1.0f;
	e->master_cur = 0.0f; /* fade up from silence on start */
	e->ig_db_cached = -1000.0f; /* never a valid setting: forces the first computation */
	e->mg_cached = -1.0f;
	e->overload_n = ENGINE_OVERLOAD_BLOCKS;
}

static uint32_t fx_cycles(int fx_id)
{
	const fx_desc *d = fx_get(fx_id);
	return d ? d->cpu_cycles : 0;
}

void engine_resources(const engine_t *e, uint32_t *mem_used, uint32_t *cpu_used)
{
	uint32_t m = 0, c = 0;
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		m += e->blk_size[s];
		c += fx_cycles(e->cfg.fx_id[s]);
	}
	if (mem_used) *mem_used = m;
	if (cpu_used) *cpu_used = c;
}

/* ------------------------------------------------- control half: layout */

/* first-fit placement among the blocks of every slot except `exclude` */
static int find_offset(const uint32_t *boff, const uint32_t *bsize, uint32_t pool_bytes,
		       int exclude, uint32_t size, uint32_t *off)
{
	uint32_t o[ENGINE_SLOTS], z[ENGINE_SLOTS];
	int n = 0;
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		if (s == exclude || bsize[s] == 0) continue;
		int j = n++;
		while (j > 0 && o[j - 1] > boff[s]) { o[j] = o[j - 1]; z[j] = z[j - 1]; j--; }
		o[j] = boff[s];
		z[j] = bsize[s];
	}
	uint32_t cur = 0;
	for (int i = 0; i < n; i++) {
		if (o[i] >= cur && o[i] - cur >= size) { *off = cur; return 1; }
		if (o[i] + z[i] > cur) cur = o[i] + z[i];
	}
	if (pool_bytes >= cur && pool_bytes - cur >= size) { *off = cur; return 1; }
	return 0;
}

static int overlaps(uint32_t ao, uint32_t az, uint32_t bo, uint32_t bz)
{
	return az && bz && ao < bo + bz && bo < ao + az;
}

/* Publish {cfg, layout} of the control half to the audio thread (sequence lock, see engine.h).
 * Post k (1, 2, ...) is written into box[k & 1]; box_seq is 2k-1 while it is written, 2k after. */
static void post(engine_t *e)
{
	uint32_t n = e->box_seq;               /* even: only this thread writes it */
	uint32_t k = (n >> 1) + 1;
	engine_msg_t *b = &e->box[k & 1];
	e->box_seq = n + 1;
	ENGINE_BARRIER();
	HOOK(e, ENGINE_HOOK_POST_BEGIN);
	b->cfg = e->cfg;
	HOOK(e, ENGINE_HOOK_POST_MID);
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		b->blk_off[s] = e->blk_off[s];
		b->blk_size[s] = e->blk_size[s];
	}
	b->post = k;
	ENGINE_BARRIER();
	e->box_seq = n + 2;
	ENGINE_BARRIER();
	HOOK(e, ENGINE_HOOK_POST_DONE);
}

int engine_config_settled(const engine_t *e)
{
	return e->ack_post == (e->box_seq >> 1);
}

engine_status engine_assign(engine_t *e, int slot, int fx_id)
{
	if (slot < 0 || slot >= ENGINE_SLOTS) return ENG_ERR_ARG;
	const fx_desc *d = fx_get(fx_id);
	if (!d) return ENG_ERR_ARG;
	if (e->cfg.fx_id[slot] == fx_id) return ENG_OK;

	uint32_t cpu = d->cpu_cycles;
	for (int s = 0; s < ENGINE_SLOTS; s++)
		if (s != slot) cpu += fx_cycles(e->cfg.fx_id[s]);
	if (cpu > e->cpu_budget) return ENG_ERR_CPU;

	uint32_t size = fx_block_bytes(d), off = 0;
	if (size > 0 && !find_offset(e->blk_off, e->blk_size, e->pool_bytes, slot, size, &off))
		return ENG_ERR_MEM;

	e->blk_off[slot] = off;
	e->blk_size[slot] = size;
	e->cfg.fx_id[slot] = (uint8_t)fx_id;
	post(e);
	return ENG_OK;
}

int engine_post_config(engine_t *e, const engine_config_t *c)
{
	engine_config_t tmp = *c;
	engine_config_sanitize(&tmp);
	HOOK(e, ENGINE_HOOK_APPLY_SANITIZED);

	/* 1. admission exactly as assigning slot 0..3 in turn into an empty engine:
	 *    cumulative declared CPU, first-fit memory */
	uint32_t off[ENGINE_SLOTS] = {0}, size[ENGINE_SLOTS] = {0}, cpu = 0;
	int rejected = 0;
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		const fx_desc *d = fx_get(tmp.fx_id[s]);
		if (!d || tmp.fx_id[s] == FX_NONE) { tmp.fx_id[s] = FX_NONE; continue; }
		uint32_t z = fx_block_bytes(d), o = 0;
		if (cpu + d->cpu_cycles > e->cpu_budget ||
		    (z > 0 && !find_offset(off, size, e->pool_bytes, s, z, &o))) {
			tmp.fx_id[s] = FX_NONE;
			rejected++;
			continue;
		}
		cpu += d->cpu_cycles;
		off[s] = o;
		size[s] = z;
	}

	/* 2. same admitted set, but keep every slot that keeps its effect at its current offset
	 *    (no re-init, tails survive) if the others still fit around them */
	uint32_t koff[ENGINE_SLOTS] = {0}, ksize[ENGINE_SLOTS] = {0};
	int keep_ok = 1;
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		if (tmp.fx_id[s] != FX_NONE && tmp.fx_id[s] == e->cfg.fx_id[s] && size[s] == e->blk_size[s]) {
			koff[s] = e->blk_off[s];
			ksize[s] = size[s];
		}
	}
	for (int s = 0; s < ENGINE_SLOTS && keep_ok; s++) {
		if (ksize[s] || !size[s]) continue;
		uint32_t o;
		if (!find_offset(koff, ksize, e->pool_bytes, s, size[s], &o)) keep_ok = 0;
		else { koff[s] = o; ksize[s] = size[s]; }
	}
	if (keep_ok) {
		for (int s = 0; s < ENGINE_SLOTS; s++) { off[s] = koff[s]; size[s] = ksize[s]; }
	}

	/* 3. write the control half (the audio thread never reads it), then publish */
	uint8_t ids[ENGINE_SLOTS];
	for (int s = 0; s < ENGINE_SLOTS; s++) ids[s] = tmp.fx_id[s];
	e->cfg = tmp;
	HOOK(e, ENGINE_HOOK_APPLY_HALF);
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		e->cfg.fx_id[s] = ids[s];
		e->blk_off[s] = size[s] ? off[s] : 0;
		e->blk_size[s] = size[s];
	}
	HOOK(e, ENGINE_HOOK_APPLY_WRITTEN);
	post(e);
	return rejected;
}

int engine_apply_config(engine_t *e, const engine_config_t *c)
{
	return engine_post_config(e, c);
}

void engine_set_param(engine_t *e, int slot, int idx, float norm)
{
	if (slot < 0 || slot >= ENGINE_SLOTS || idx < 0 || idx >= FX_MAX_PARAMS) return;
	if (!(norm == norm)) return;
	e->cfg.params[slot][e->cfg.fx_id[slot]][idx] = fx_clamp(norm, 0.0f, 1.0f);
	post(e);
}

void engine_set_wet(engine_t *e, int slot, float wet)
{
	if (slot < 0 || slot >= ENGINE_SLOTS || !(wet == wet)) return;
	e->cfg.wet[slot] = fx_clamp(wet, 0.0f, 1.0f);
	post(e);
}

void engine_set_bypass(engine_t *e, int slot, int bypass)
{
	if (slot < 0 || slot >= ENGINE_SLOTS) return;
	e->cfg.bypass[slot] = bypass ? 1 : 0;
	post(e);
}

void engine_set_duck(engine_t *e, int on) { e->duck = on ? 1 : 0; }

void engine_set_global_bypass(engine_t *e, int on)
{
	e->cfg.global_bypass = on ? 1 : 0;
	post(e);
}

void engine_set_input_gain_db(engine_t *e, float db)
{
	if (!(db == db)) return;
	e->cfg.in_gain_db = fx_clamp(db, -12.0f, 12.0f);
	post(e);
}

void engine_set_master(engine_t *e, float v)
{
	if (!(v == v)) return;
	e->cfg.master = fx_clamp(v, 0.0f, 1.0f);
	post(e);
}

int engine_set_order(engine_t *e, const uint8_t order[ENGINE_SLOTS])
{
	int seen[ENGINE_SLOTS] = {0};
	for (int i = 0; i < ENGINE_SLOTS; i++) {
		if (order[i] >= ENGINE_SLOTS || seen[order[i]]) return -1;
		seen[order[i]] = 1;
	}
	for (int i = 0; i < ENGINE_SLOTS; i++) e->cfg.order[i] = order[i];
	post(e);
	return 0;
}

/* ---------------------------------------------------------- audio half */

/* Copy the newest complete snapshot into e->stage. 1 = got a new one. */
static int take(engine_t *e)
{
	for (int tries = 0; tries < 2; tries++) {
		uint32_t s1 = e->box_seq;
		ENGINE_BARRIER();
		uint32_t k = s1 >> 1;              /* newest complete post */
		if (k == 0 || k == e->run_post) return 0;
		/* word copy: the firmware's libc memcpy is byte-wise (see fx_copy32) */
		fx_copy32(&e->stage, &e->box[k & 1], sizeof e->stage / 4u);
		HOOK(e, ENGINE_HOOK_TAKE_COPIED);
		ENGINE_BARRIER();
		uint32_t s2 = e->box_seq;
		/* box[k & 1] is rewritten only by post k+2, which starts at box_seq = 2k+3 */
		if (s2 <= 2u * k + 2u && e->stage.post == k) return 1;
		e->diag.torn_retries++;
	}
	return 0;
}

static void stop_slot(engine_t *e, int s)
{
	e->live_id[s] = FX_NONE;
	e->live_st[s] = 0;
	e->live_off[s] = 0;
	e->active[s] = 0;
	e->tail_rem[s] = 0;
	e->mix_cur[s] = 0.0f;
	e->in_cur[s] = 0.0f;
}

/* Make e->stage the running snapshot. Slots whose effect and offset are unchanged keep
 * running; every other slot stops now and is (re)initialised by run_inits(). */
static void adopt(engine_t *e)
{
	const engine_msg_t *m = &e->stage;
	uint32_t off[ENGINE_SLOTS], size[ENGINE_SLOTS];
	uint8_t id[ENGINE_SLOTS];

	/* defensive check of the snapshot: inside the pool and non-overlapping */
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		id[s] = m->cfg.fx_id[s];
		const fx_desc *d = fx_get(id[s]);
		off[s] = m->blk_off[s];
		size[s] = d ? fx_block_bytes(d) : 0;
		if (!d || id[s] == FX_NONE) { id[s] = FX_NONE; size[s] = 0; continue; }
		int bad = size[s] != m->blk_size[s] || off[s] > e->pool_bytes ||
			  e->pool_bytes - off[s] < size[s] || (off[s] & 7u);
		for (int t = 0; t < s && !bad; t++) bad = overlaps(off[s], size[s], off[t], size[t]);
		if (bad) { id[s] = FX_NONE; size[s] = 0; e->diag.layout_errors++; }
	}

	fx_copy32(&e->run, &m->cfg, sizeof e->run / 4u);
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		e->run.fx_id[s] = id[s];
		e->run_off[s] = off[s];
		e->run_size[s] = size[s];
		if (id[s] != FX_NONE && e->live_id[s] == id[s] && e->live_off[s] == off[s]) {
			e->pend_id[s] = FX_NONE;   /* unchanged: keep running */
			continue;
		}
		stop_slot(e, s);
		e->pend_id[s] = id[s];
	}
	e->run_post = m->post;
	e->pend_post = m->post;
	e->diag.cfg_applies++;
}

static void init_slot(engine_t *e, int s)
{
	const fx_desc *d = fx_get(e->pend_id[s]);
	uint8_t *base = e->pool + e->run_off[s];
	uint32_t st = fx_align8(d->state_bytes);
	fx_zero32(base, st / 4u);
	d->init(base, base + st, e->sr);
	e->live_id[s] = d->id;
	e->live_st[s] = base;
	e->live_off[s] = e->run_off[s];
	e->active[s] = 0;
	e->tail_rem[s] = 0;
	e->mix_cur[s] = 0.0f; /* new effect fades in */
	e->in_cur[s] = 0.0f;
	for (int i = 0; i < FX_MAX_PARAMS; i++) e->pushed[s][i] = -1.0f; /* force push */
	e->pend_id[s] = FX_NONE;
	e->shed[s] = 0;              /* a new effect instance gets a fresh chance */
	e->diag.shed_mask &= (uint8_t)~(1u << s);
	e->chunk_inits = 1;
	e->diag.slot_inits++;
}

static void run_inits(engine_t *e, int unlimited)
{
	uint32_t used = 0;
	int any_left = 0;
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		if (e->pend_id[s] == FX_NONE) continue;
		uint32_t z = e->run_size[s];
		if (!unlimited && e->init_budget && used > 0 && used + z > e->init_budget) { any_left = 1; continue; }
		init_slot(e, s);
		used += z ? z : 1;
	}
	if (used > e->diag.init_bytes_max) e->diag.init_bytes_max = used;
	if (!any_left && e->pend_post) {
		e->ack_post = e->pend_post;
		e->pend_post = 0;
	}
}

static void apply_pending(engine_t *e, int unlimited)
{
	uint32_t req = e->shed_clear_req;
	if (req != e->shed_clear_ack) {
		e->shed_clear_ack = req;
		for (int s = 0; s < ENGINE_SLOTS; s++) e->shed[s] = 0;
		e->forced_bypass = 0;
		e->over_run = 0;
		e->diag.shed_mask = 0;
		e->diag.forced_bypass = 0;
	}
	if (take(e)) adopt(e);
	run_inits(e, unlimited);
}

void engine_overload_clear(engine_t *e)
{
	e->shed_clear_req = e->shed_clear_req + 1u;
}

int engine_overload_feed(engine_t *e, uint32_t cycles, uint32_t limit)
{
	int inits = e->chunk_inits;
	e->chunk_inits = 0;
	if (e->overload_n == 0) return 0;
	if (cycles <= limit) { e->over_run = 0; return 0; }
	e->diag.overload_blocks++;
	if (inits) return 0;           /* effect init (memory clear) is a one-off, not overload */
	if (++e->over_run < e->overload_n) return 0;
	e->over_run = 0;

	/* the most expensive slot that still runs (declared cost; later in the chain on a tie) */
	int best = -1;
	uint32_t best_c = 0;
	for (int k = 0; k < ENGINE_SLOTS; k++) {
		int s = e->run.order[k];
		const fx_desc *d = fx_get(e->live_id[s]);
		if (!d || e->live_id[s] == FX_NONE || e->shed[s] || !e->active[s]) continue;
		if (best < 0 || d->cpu_cycles >= best_c) { best = s; best_c = d->cpu_cycles; }
	}
	if (best >= 0) {
		e->shed[best] = 1;        /* fades out in the next chunk, then stops */
		e->diag.shed_mask |= (uint8_t)(1u << best);
		e->diag.overload_sheds++;
		return 1;
	}
	if (!e->forced_bypass) {
		e->forced_bypass = 1;
		e->diag.forced_bypass = 1;
		e->diag.overload_forced++;
		return 1;
	}
	return 0;
}

void engine_pump(engine_t *e)
{
	apply_pending(e, 1);
}

static void push_params(engine_t *e, int s, const fx_desc *d)
{
	for (int i = 0; i < d->nparams; i++) {
		float v = e->run.params[s][d->id][i];
		if (v != e->pushed[s][i]) {
			d->set_param(e->live_st[s], i, v);
			e->pushed[s][i] = v;
		}
	}
}

static inline float soft_limit(float x)
{
	float a = fabsf(x);
	if (a < 0.8f) return x;
	float y = 0.8f + 0.2f * fx_tanh((a - 0.8f) * 5.0f);
	return x < 0.0f ? -y : y;
}

static float master_gain(float v)
{
	if (v < 0.005f) return 0.0f;
	return powf(10.0f, (v - 1.0f) * 60.0f / 20.0f); /* -60 dB .. 0 dB */
}

/* Smoothed values (one-pole) snap to their target once within this distance, so the
 * steady state runs the cheap constant-gain loops. A float one-pole with k ~ 1e-3 stalls up
 * to ~2e-4 short of a target near 4 (k * diff < half an ULP), so the snap must be at least
 * that wide; 5e-4 of a gain is a -66 dBFS step on a full-scale signal (inaudible). */
#define SNAP 5e-4f

static void process_chunk(engine_t *e, float *L, float *R, int n)
{
	float wl[FX_MAX_BLOCK], wr[FX_MAX_BLOCK];
	const float k_in = e->k_in, k_mix = e->k_mix;

	for (int k = 0; k < ENGINE_SLOTS; k++) {
		int s = e->run.order[k];
		const fx_desc *d = fx_get(e->live_id[s]);
		if (!d || e->live_id[s] == FX_NONE) continue;

		int shed = e->shed[s];
		int enabled = !e->run.bypass[s] && !e->run.global_bypass && !e->forced_bypass && shed != 2;
		int send = d->mix_mode == FX_MIX_SEND;
		uint32_t tail_len = (uint32_t)(((d->tail_ms > 60u ? d->tail_ms : 60u) * e->sr) / 1000.0f);

		if (enabled) e->tail_rem[s] = tail_len;
		else if (e->tail_rem[s] > 0) e->tail_rem[s] = e->tail_rem[s] > (uint32_t)n ? e->tail_rem[s] - (uint32_t)n : 0;
		if (shed == 2 || e->forced_bypass) e->tail_rem[s] = 0; /* overload: no tails either */

		if (!enabled && e->tail_rem[s] == 0) {
			if (e->active[s]) { d->reset(e->live_st[s]); e->active[s] = 0; }
			e->mix_cur[s] = 0.0f;
			e->in_cur[s] = 0.0f;
			continue;
		}
		e->active[s] = 1;
		push_params(e, s, d);

		float mix_t = e->run.global_bypass ? 0.0f : (send ? e->run.wet[s] : (enabled ? e->run.wet[s] : 0.0f));
		float in_t = (send ? (enabled ? 1.0f : 0.0f) : 1.0f);
		float mc = e->mix_cur[s], ic = e->in_cur[s];

		if (!send && shed == 0 && ic == 1.0f && in_t == 1.0f && mc == 1.0f && mix_t == 1.0f) {
			/* fully wet insert effect, nothing ramping: process the chain buffers in place */
			d->process(e->live_st[s], L, R, n);
			float chk = 0.0f;
			for (int i = 0; i < n; i++) chk += L[i] + R[i];
			if (fx_bad(chk) || fabsf(chk) > 1e9f) {
				d->reset(e->live_st[s]);
				e->diag.nan_events++;
				for (int i = 0; i < n; i++) L[i] = R[i] = 0.0f; /* = the general path's result */
			}
			continue;
		}

		/* effect input (L/R stay untouched: they are the dry signal for the mix) */
		if (ic == in_t) {
			for (int i = 0; i < n; i++) {
				wl[i] = L[i] * ic;
				wr[i] = R[i] * ic;
			}
		} else {
			for (int i = 0; i < n; i++) {
				ic += k_in * (in_t - ic);
				wl[i] = L[i] * ic;
				wr[i] = R[i] * ic;
			}
			if (fabsf(in_t - ic) < SNAP) ic = in_t;
		}
		d->process(e->live_st[s], wl, wr, n);

		float chk = 0.0f;
		for (int i = 0; i < n; i++) chk += wl[i] + wr[i];
		if (fx_bad(chk) || fabsf(chk) > 1e9f) {
			d->reset(e->live_st[s]);
			e->diag.nan_events++;
			for (int i = 0; i < n; i++) wl[i] = wr[i] = 0.0f;
		}

		if (shed == 1) {
			/* overload guard: fade this slot out linearly over this chunk, then stop it */
			float step = mc / (float)n;
			for (int i = 0; i < n; i++) {
				mc -= step;
				if (mc < 0.0f) mc = 0.0f;
				if (send) {
					L[i] += wl[i] * mc;
					R[i] += wr[i] * mc;
				} else {
					L[i] += (wl[i] - L[i]) * mc;
					R[i] += (wr[i] - R[i]) * mc;
				}
			}
			mc = 0.0f;
			ic = 0.0f;
			e->shed[s] = 2;
			e->tail_rem[s] = 0;
		} else if (mc == mix_t) {
			if (send) {
				for (int i = 0; i < n; i++) {
					L[i] += wl[i] * mc;
					R[i] += wr[i] * mc;
				}
			} else {
				for (int i = 0; i < n; i++) {
					L[i] += (wl[i] - L[i]) * mc;
					R[i] += (wr[i] - R[i]) * mc;
				}
			}
		} else {
			for (int i = 0; i < n; i++) {
				mc += k_mix * (mix_t - mc);
				if (send) {
					L[i] += wl[i] * mc;
					R[i] += wr[i] * mc;
				} else {
					L[i] += (wl[i] - L[i]) * mc;
					R[i] += (wr[i] - R[i]) * mc;
				}
			}
			if (fabsf(mix_t - mc) < SNAP) mc = mix_t;
		}
		e->mix_cur[s] = mc;
		e->in_cur[s] = ic;
	}
}

/* Per-block gain targets: libm powf only when the setting changed. */
static float in_gain_target(engine_t *e)
{
	if (e->run.in_gain_db != e->ig_db_cached) {
		e->ig_db_cached = e->run.in_gain_db;
		e->ig_t_cached = powf(10.0f, e->run.in_gain_db / 20.0f);
	}
	return e->ig_t_cached;
}

static float master_target(engine_t *e)
{
	if (e->run.master != e->mg_cached) {
		e->mg_cached = e->run.master;
		e->mg_t_cached = master_gain(e->run.master);
	}
	return e->mg_t_cached;
}

void engine_process(engine_t *e, const float *in_l, const float *in_r, float *out_l, float *out_r, int n)
{
	float L[FX_MAX_BLOCK], R[FX_MAX_BLOCK];
	int done = 0;
	while (done < n) {
		int m = n - done;
		if (m > FX_MAX_BLOCK) m = FX_MAX_BLOCK;
		const float *il = in_l + done, *ir = in_r + done;

		/* config hand-over: only here, at the start of a chunk, on this thread */
		apply_pending(e, 0);

		/* input: NaN/Inf screen once per chunk (a sum is non-finite iff a term is, or it
		 * overflowed), then gain, peak and clip count with everything in registers */
		float chk = 0.0f;
		for (int i = 0; i < m; i++) chk += il[i] + ir[i];
		int bad = fx_bad(chk);
		float g_t = in_gain_target(e);
		float ig = e->ingain_cur, k_gain = e->k_gain;
		float ipk = 0.0f;
		uint32_t clips = 0;
		int ig_const = ig == g_t;
		/* peaks as the integer bits of |x| (orders like the float for finite x >= 0) */
		uint32_t pk_bits = 0;
		for (int i = 0; i < m; i++) {
			float a = il[i], b = ir[i];
			if (bad) {
				if (fx_bad(a)) a = 0.0f;
				if (fx_bad(b)) b = 0.0f;
			}
			uint32_t ua, ub;
			memcpy(&ua, &a, 4);
			memcpy(&ub, &b, 4);
			ua &= 0x7FFFFFFFu;
			ub &= 0x7FFFFFFFu;
			if (ua > pk_bits) pk_bits = ua;
			if (ub > pk_bits) pk_bits = ub;
			if (!ig_const) ig += k_gain * (g_t - ig);
			L[i] = a * ig;
			R[i] = b * ig;
		}
		memcpy(&ipk, &pk_bits, 4);
		if (ipk >= 0.999f) {
			for (int i = 0; i < m; i++) {
				float pa = fabsf(il[i]), pb = fabsf(ir[i]);
				if ((pa >= 0.999f || pb >= 0.999f) && !fx_bad(pa) && !fx_bad(pb)) clips++;
			}
		}
		if (!ig_const && fabsf(g_t - ig) < SNAP) ig = g_t;
		e->ingain_cur = ig;
		e->diag.in_clips += clips;
		e->diag.in_peak = ipk > e->diag.in_peak ? ipk : e->diag.in_peak;

		process_chunk(e, L, R, m);

		float mg_t = master_target(e);
		float mg = e->master_cur;
		float dk = e->duck_cur, dk_t = e->duck ? 0.0f : 1.0f;
		const float k_master = e->k_master, k_duck = e->k_duck;
		float opk = 0.0f;
		uint32_t oclips = 0;
		float *ol = out_l + done, *orr = out_r + done;
		if (mg == mg_t && dk == dk_t) {
			const float g = mg * dk;
			for (int i = 0; i < m; i++) {
				float a = L[i] * g, b = R[i] * g;
				float pa = fabsf(a), pb = fabsf(b);
				float p = pa > pb ? pa : pb;
				if (p > opk) opk = p;
				if (p > 1.0f) oclips++;
				ol[i] = p < 0.8f ? a : soft_limit(a);
				orr[i] = p < 0.8f ? b : soft_limit(b);
			}
		} else {
			for (int i = 0; i < m; i++) {
				mg += k_master * (mg_t - mg);
				dk += k_duck * (dk_t - dk);
				float a = L[i] * mg * dk, b = R[i] * mg * dk;
				float pa = fabsf(a), pb = fabsf(b);
				float p = pa > pb ? pa : pb;
				if (p > opk) opk = p;
				if (p > 1.0f) oclips++;
				ol[i] = soft_limit(a);
				orr[i] = soft_limit(b);
			}
			if (fabsf(mg_t - mg) < SNAP) mg = mg_t;
			if (fabsf(dk_t - dk) < SNAP) dk = dk_t;
		}
		e->master_cur = mg;
		e->duck_cur = dk;
		e->diag.out_clips += oclips;
		e->diag.out_peak = opk > e->diag.out_peak ? opk : e->diag.out_peak;
		e->diag.blocks++;
		done += m;
	}
}
