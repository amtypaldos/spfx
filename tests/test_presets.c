/* Factory presets (presets/<name>.json -> firmware/app/factory_presets.h) through the real engine:
 * admission with the firmware pool and CPU budget, then 5 s of noise + clicks at 48 kHz. */
#include "t.h"
#include "../firmware/app/factory_presets.h"

#define SR 48000
#define POOL_BYTES (150u * 1024u)
#define SECONDS 5
#define BLOCK 48

static uint8_t pool[POOL_BYTES];
static engine_t eng;

static uint32_t rng = 0xC0FFEEu;
static float noise(void)
{
	rng = rng * 1664525u + 1013904223u;
	return ((float)(rng >> 8) / 8388608.0f) - 1.0f; /* -1..1 */
}

static void run_preset(int k, int stress)
{
	const factory_preset_t *p = &factory_presets[k];
	store_data_t d;
	factory_preset_fill(p, &d);
	if (stress) { /* same preset, driven hard: master at full and +12 dB input gain */
		d.cfg.master = 1.0f;
		d.cfg.in_gain_db = 12.0f;
	}

	memset(pool, 0, sizeof pool);
	engine_init(&eng, pool, POOL_BYTES, (float)SR, ENGINE_DEFAULT_CPU_BUDGET);
	int rejected = engine_apply_config(&eng, &d.cfg);
	CHECK_MSG(rejected == 0, "%s: %d slot(s) rejected", p->name, rejected);

	/* the engine holds exactly the preset */
	engine_config_t chk = d.cfg;
	CHECK_MSG(engine_config_sanitize(&chk) == 0, "%s: config needed repair", p->name);
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		CHECK(eng.cfg.fx_id[s] == p->fx_id[s]);
		CHECK(eng.cfg.bypass[s] == p->bypass[s]);
		CHECK(eng.cfg.order[s] == p->order[s]);
		CHECK_NEAR(eng.cfg.wet[s], p->wet[s], 0);
		const fx_desc *fd = fx_get(p->fx_id[s]);
		CHECK(fd != NULL && fd->id != FX_NONE); /* every factory slot has a real effect */
		for (int i = 0; fd && i < fd->nparams; i++) CHECK_NEAR(eng.cfg.params[s][p->fx_id[s]][i], p->params[s][i], 0);
	}

	uint32_t mem, cpu;
	engine_resources(&eng, &mem, &cpu);
	CHECK(mem <= POOL_BYTES && cpu <= ENGINE_DEFAULT_CPU_BUDGET);
	if (!stress) printf("    %-12s mem %6u / %u B (margin %6u B, %4.1f%%)  cpu %3u / %u cycles (margin %3u)  [",
	       p->name, (unsigned)mem, POOL_BYTES, (unsigned)(POOL_BYTES - mem), 100.0 * (POOL_BYTES - mem) / POOL_BYTES,
	       (unsigned)cpu, ENGINE_DEFAULT_CPU_BUDGET, (unsigned)(ENGINE_DEFAULT_CPU_BUDGET - cpu));
	for (int s = 0; s < ENGINE_SLOTS && !stress; s++) {
		const fx_desc *fd = fx_get(p->fx_id[s]);
		printf("%s%s%s %u B/%u c", s ? ", " : "", fd->name, p->bypass[s] ? "(byp)" : "", (unsigned)fx_block_bytes(fd), fd->cpu_cycles);
	}
	if (!stress) printf("]\n");

	/* survives the persistence round trip unchanged (what the device stores and reports) */
	uint8_t buf[512];
	uint16_t n = store_encode(&d, buf, sizeof buf);
	store_data_t back;
	CHECK(n > 0 && store_decode(buf, n, STORE_LAYOUT_VERSION, &back) == 0);
	CHECK(memcmp(back.cfg.fx_id, d.cfg.fx_id, 4) == 0 && memcmp(back.cfg.order, d.cfg.order, 4) == 0);
	CHECK_NEAR(back.bpm, p->bpm, 0.05);

	/* 5 s of noise (-12 dBFS-ish) plus a full-scale click every 250 ms */
	float il[BLOCK], ir[BLOCK], ol[BLOCK], orr[BLOCK];
	long frames = 0, nonfinite = 0, over = 0;
	double peak = 0.0, energy = 0.0;
	for (int b = 0; b < SR * SECONDS / BLOCK; b++) {
		for (int i = 0; i < BLOCK; i++, frames++) {
			float x = 0.25f * noise(), y = 0.25f * noise();
			if (frames % (SR / 4) == 0) x = y = 0.99f;
			il[i] = x;
			ir[i] = y;
		}
		engine_process(&eng, il, ir, ol, orr, BLOCK);
		for (int i = 0; i < BLOCK; i++) {
			float a = ol[i], c = orr[i];
			if (!isfinite(a) || !isfinite(c)) { nonfinite++; continue; }
			if (fabsf(a) > 1.0f || fabsf(c) > 1.0f) over++;
			if (fabsf(a) > peak) peak = fabsf(a);
			if (fabsf(c) > peak) peak = fabsf(c);
			energy += (double)a * a + (double)c * c;
		}
	}
	double rms = sqrt(energy / (2.0 * frames));
	CHECK_MSG(nonfinite == 0, "%s: %ld non-finite samples", p->name, nonfinite);
	CHECK_MSG(over == 0, "%s: %ld samples above 1.0", p->name, over);
	CHECK_MSG(eng.diag.nan_events == 0, "%s: %u NaN events", p->name, (unsigned)eng.diag.nan_events);
	CHECK_MSG(rms > 0.003, "%s: output nearly silent (rms %g)", p->name, rms);
	printf("    %-12s %s out peak %.3f rms %.3f, out_clips (pre-limiter) %u, nan_events %u\n",
	       p->name, stress ? "[stress +12 dB, master 1.0]" : "[as set]", peak, rms, (unsigned)eng.diag.out_clips, (unsigned)eng.diag.nan_events);
}

static void test_factory_presets(void)
{
	CHECK(FACTORY_PRESET_COUNT == 6);
	for (int k = 0; k < FACTORY_PRESET_COUNT; k++) run_preset(k, 0);
	for (int k = 0; k < FACTORY_PRESET_COUNT; k++) run_preset(k, 1);
}

static void test_clean_is_bypassed(void)
{
	for (int k = 0; k < FACTORY_PRESET_COUNT; k++) {
		const factory_preset_t *p = &factory_presets[k];
		if (strcmp(p->name, "Clean") != 0) continue;
		for (int s = 0; s < ENGINE_SLOTS; s++) CHECK(p->bypass[s] == 1 && p->fx_id[s] != FX_NONE);
		return;
	}
	CHECK(!"Clean preset missing");
}

int main(void)
{
	RUN(test_factory_presets);
	RUN(test_clean_is_bypassed);
	T_DONE();
}
