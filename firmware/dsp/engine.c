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

void engine_init(engine_t *e, void *pool, uint32_t pool_bytes, float sr, uint32_t cpu_budget)
{
	memset(e, 0, sizeof *e);
	e->pool = pool;
	e->pool_bytes = pool_bytes;
	e->sr = sr;
	e->cpu_budget = cpu_budget;
	engine_config_default(&e->cfg);
	for (int s = 0; s < ENGINE_SLOTS; s++) e->cfg.fx_id[s] = FX_NONE;
	e->k_mix = fx_coef(0.008f, sr);
	e->k_in = fx_coef(0.005f, sr);
	e->k_gain = fx_coef(0.010f, sr);
	e->k_master = fx_coef(0.020f, sr);
	e->k_duck = fx_coef(0.004f, sr);
	e->duck_cur = 1.0f;
	e->ingain_cur = 1.0f;
	e->master_cur = 0.0f; /* fade up from silence on start */
}

static uint32_t slot_cycles(const engine_t *e, int s)
{
	const fx_desc *d = fx_get(e->cfg.fx_id[s]);
	return d ? d->cpu_cycles : 0;
}

void engine_resources(const engine_t *e, uint32_t *mem_used, uint32_t *cpu_used)
{
	uint32_t m = 0, c = 0;
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		m += e->blk_size[s];
		c += slot_cycles(e, s);
	}
	if (mem_used) *mem_used = m;
	if (cpu_used) *cpu_used = c;
}

/* first-fit placement among the desired blocks of every slot except `exclude` */
static int find_offset(const engine_t *e, int exclude, uint32_t size, uint32_t *off)
{
	uint32_t o[ENGINE_SLOTS], z[ENGINE_SLOTS];
	int n = 0;
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		if (s == exclude || e->blk_size[s] == 0) continue;
		int j = n++;
		while (j > 0 && o[j - 1] > e->blk_off[s]) { o[j] = o[j - 1]; z[j] = z[j - 1]; j--; }
		o[j] = e->blk_off[s];
		z[j] = e->blk_size[s];
	}
	uint32_t cur = 0;
	for (int i = 0; i < n; i++) {
		if (o[i] >= cur && o[i] - cur >= size) { *off = cur; return 1; }
		if (o[i] + z[i] > cur) cur = o[i] + z[i];
	}
	if (e->pool_bytes >= cur && e->pool_bytes - cur >= size) { *off = cur; return 1; }
	return 0;
}

engine_status engine_assign(engine_t *e, int slot, int fx_id)
{
	if (slot < 0 || slot >= ENGINE_SLOTS) return ENG_ERR_ARG;
	const fx_desc *d = fx_get(fx_id);
	if (!d) return ENG_ERR_ARG;
	if (e->cfg.fx_id[slot] == fx_id) return ENG_OK;

	uint32_t cpu = d->cpu_cycles;
	for (int s = 0; s < ENGINE_SLOTS; s++)
		if (s != slot) cpu += slot_cycles(e, s);
	if (cpu > e->cpu_budget) return ENG_ERR_CPU;

	uint32_t size = fx_block_bytes(d), off = 0;
	if (size > 0 && !find_offset(e, slot, size, &off)) return ENG_ERR_MEM;

	/* commit: layout first, then the id the audio thread watches */
	e->blk_off[slot] = off;
	e->blk_size[slot] = size;
	e->cfg.fx_id[slot] = (uint8_t)fx_id;
	return ENG_OK;
}

int engine_apply_config(engine_t *e, const engine_config_t *c)
{
	engine_config_t tmp = *c;
	engine_config_sanitize(&tmp);
	int rejected = 0;
	/* free everything first so ordering of slots cannot cause spurious failures */
	for (int s = 0; s < ENGINE_SLOTS; s++) engine_assign(e, s, FX_NONE);
	e->cfg = tmp;
	for (int s = 0; s < ENGINE_SLOTS; s++) e->cfg.fx_id[s] = FX_NONE;
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		if (tmp.fx_id[s] == FX_NONE) continue;
		if (engine_assign(e, s, tmp.fx_id[s]) != ENG_OK) rejected++;
	}
	return rejected;
}

void engine_set_param(engine_t *e, int slot, int idx, float norm)
{
	if (slot < 0 || slot >= ENGINE_SLOTS || idx < 0 || idx >= FX_MAX_PARAMS) return;
	if (!(norm == norm)) return;
	e->cfg.params[slot][e->cfg.fx_id[slot]][idx] = fx_clamp(norm, 0.0f, 1.0f);
}

void engine_set_wet(engine_t *e, int slot, float wet)
{
	if (slot < 0 || slot >= ENGINE_SLOTS || !(wet == wet)) return;
	e->cfg.wet[slot] = fx_clamp(wet, 0.0f, 1.0f);
}

void engine_set_bypass(engine_t *e, int slot, int bypass)
{
	if (slot < 0 || slot >= ENGINE_SLOTS) return;
	e->cfg.bypass[slot] = bypass ? 1 : 0;
}

void engine_set_duck(engine_t *e, int on) { e->duck = on ? 1 : 0; }

void engine_set_global_bypass(engine_t *e, int on) { e->cfg.global_bypass = on ? 1 : 0; }

void engine_set_input_gain_db(engine_t *e, float db)
{
	if (!(db == db)) return;
	e->cfg.in_gain_db = fx_clamp(db, -12.0f, 12.0f);
}

void engine_set_master(engine_t *e, float v)
{
	if (!(v == v)) return;
	e->cfg.master = fx_clamp(v, 0.0f, 1.0f);
}

int engine_set_order(engine_t *e, const uint8_t order[ENGINE_SLOTS])
{
	int seen[ENGINE_SLOTS] = {0};
	for (int i = 0; i < ENGINE_SLOTS; i++) {
		if (order[i] >= ENGINE_SLOTS || seen[order[i]]) return -1;
		seen[order[i]] = 1;
	}
	for (int i = 0; i < ENGINE_SLOTS; i++) e->cfg.order[i] = order[i];
	return 0;
}

/* ------------------------------------------------------------- audio thread */

static void apply_assignments(engine_t *e)
{
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		int want = e->cfg.fx_id[s];
		if (want == e->live_id[s]) continue;
		const fx_desc *d = fx_get(want);
		e->live_id[s] = (uint8_t)want;
		e->active[s] = 0;
		e->tail_rem[s] = 0;
		e->mix_cur[s] = 0.0f; /* new effect fades in */
		e->in_cur[s] = 0.0f;
		for (int i = 0; i < FX_MAX_PARAMS; i++) e->pushed[s][i] = -1.0f; /* force push */
		if (!d || want == FX_NONE) { e->live_st[s] = 0; continue; }
		uint8_t *base = e->pool + e->blk_off[s];
		memset(base, 0, e->blk_size[s]);
		e->live_st[s] = base;
		d->init(base, base + fx_align8(d->state_bytes), e->sr);
	}
}

static void push_params(engine_t *e, int s, const fx_desc *d)
{
	for (int i = 0; i < d->nparams; i++) {
		float v = e->cfg.params[s][d->id][i];
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

static void process_chunk(engine_t *e, float *L, float *R, int n)
{
	float wl[FX_MAX_BLOCK], wr[FX_MAX_BLOCK];
	float dl[FX_MAX_BLOCK], dr[FX_MAX_BLOCK];

	apply_assignments(e);

	for (int k = 0; k < ENGINE_SLOTS; k++) {
		int s = e->cfg.order[k];
		const fx_desc *d = fx_get(e->live_id[s]);
		if (!d || e->live_id[s] == FX_NONE) continue;

		int enabled = !e->cfg.bypass[s] && !e->cfg.global_bypass;
		int send = d->mix_mode == FX_MIX_SEND;
		uint32_t tail_len = (uint32_t)(((d->tail_ms > 60u ? d->tail_ms : 60u) * e->sr) / 1000.0f);

		if (enabled) e->tail_rem[s] = tail_len;
		else if (e->tail_rem[s] > 0) e->tail_rem[s] = e->tail_rem[s] > (uint32_t)n ? e->tail_rem[s] - (uint32_t)n : 0;

		if (!enabled && e->tail_rem[s] == 0) {
			if (e->active[s]) { d->reset(e->live_st[s]); e->active[s] = 0; }
			e->mix_cur[s] = 0.0f;
			e->in_cur[s] = 0.0f;
			continue;
		}
		e->active[s] = 1;
		push_params(e, s, d);

		float mix_t = e->cfg.global_bypass ? 0.0f : (send ? e->cfg.wet[s] : (enabled ? e->cfg.wet[s] : 0.0f));
		float in_t = (send ? (enabled ? 1.0f : 0.0f) : 1.0f);
		float mc = e->mix_cur[s], ic = e->in_cur[s];
		for (int i = 0; i < n; i++) {
			dl[i] = L[i];
			dr[i] = R[i];
			ic += e->k_in * (in_t - ic);
			wl[i] = L[i] * ic;
			wr[i] = R[i] * ic;
		}
		d->process(e->live_st[s], wl, wr, n);

		float chk = 0.0f;
		for (int i = 0; i < n; i++) chk += wl[i] + wr[i];
		if (fx_bad(chk) || fabsf(chk) > 1e9f) {
			d->reset(e->live_st[s]);
			e->diag.nan_events++;
			for (int i = 0; i < n; i++) wl[i] = wr[i] = 0.0f;
		}

		for (int i = 0; i < n; i++) {
			mc += e->k_mix * (mix_t - mc);
			if (send) {
				L[i] = dl[i] + wl[i] * mc;
				R[i] = dr[i] + wr[i] * mc;
			} else {
				L[i] = dl[i] + (wl[i] - dl[i]) * mc;
				R[i] = dr[i] + (wr[i] - dr[i]) * mc;
			}
		}
		e->mix_cur[s] = mc;
		e->in_cur[s] = ic;
	}
}

void engine_process(engine_t *e, const float *in_l, const float *in_r, float *out_l, float *out_r, int n)
{
	float L[FX_MAX_BLOCK], R[FX_MAX_BLOCK];
	int done = 0;
	while (done < n) {
		int m = n - done;
		if (m > FX_MAX_BLOCK) m = FX_MAX_BLOCK;

		float g_t = powf(10.0f, e->cfg.in_gain_db / 20.0f);
		float ig = e->ingain_cur;
		float ipk = 0.0f;
		for (int i = 0; i < m; i++) {
			float a = in_l[done + i], b = in_r[done + i];
			if (fx_bad(a)) a = 0.0f;
			if (fx_bad(b)) b = 0.0f;
			float pa = fabsf(a), pb = fabsf(b);
			if (pa > ipk) ipk = pa;
			if (pb > ipk) ipk = pb;
			if (pa >= 0.999f || pb >= 0.999f) e->diag.in_clips++;
			ig += e->k_gain * (g_t - ig);
			L[i] = a * ig;
			R[i] = b * ig;
		}
		e->ingain_cur = ig;
		e->diag.in_peak = ipk > e->diag.in_peak ? ipk : e->diag.in_peak;

		process_chunk(e, L, R, m);

		float mg_t = master_gain(e->cfg.master);
		float mg = e->master_cur;
		float dk = e->duck_cur, dk_t = e->duck ? 0.0f : 1.0f;
		float opk = 0.0f;
		for (int i = 0; i < m; i++) {
			mg += e->k_master * (mg_t - mg);
			dk += e->k_duck * (dk_t - dk);
			float a = L[i] * mg * dk, b = R[i] * mg * dk;
			float pa = fabsf(a), pb = fabsf(b);
			if (pa > opk) opk = pa;
			if (pb > opk) opk = pb;
			if (pa > 1.0f || pb > 1.0f) e->diag.out_clips++;
			out_l[done + i] = soft_limit(a);
			out_r[done + i] = soft_limit(b);
		}
		e->master_cur = mg;
		e->duck_cur = dk;
		e->diag.out_peak = opk > e->diag.out_peak ? opk : e->diag.out_peak;
		e->diag.blocks++;
		done += m;
	}
}
