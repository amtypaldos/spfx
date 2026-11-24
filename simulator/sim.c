/* SP-1 FX host simulator: runs the real firmware DSP engine over a WAV file.
 *
 *   sp1fx_sim --list
 *   sp1fx_sim in.wav out.wav --slot 1=filter --param 1.1=800 --wet 1=1 [--slot 2=delay ...]
 *             [--tail 3] [--block 64] [--pool 150] [--bits 16|24|32] [--bench]
 *
 * --param SLOT.IDX=VALUE uses REAL units (Hz, ms, %, dB...), as listed by --list.
 * Host speed is NOT a substitute for benchmarking on the SP-1. */
#define _POSIX_C_SOURCE 200809L
#include "../firmware/dsp/engine.h"
#include "wav.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int ieq(const char *a, const char *b)
{
	while (*a && *b) {
		while (*a == ' ' || *a == '_' || *a == '-') a++;
		while (*b == ' ' || *b == '_' || *b == '-') b++;
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
		a++; b++;
	}
	return !*a && !*b;
}

static int find_fx(const char *name)
{
	for (int i = 0; i < FX_COUNT; i++)
		if (ieq(fx_get(i)->name, name)) return i;
	if (ieq(name, "tape")) return FX_TAPE_WOBBLE;
	return -1;
}

static void list(void)
{
	for (int i = 0; i < FX_COUNT; i++) {
		const fx_desc *d = fx_get(i);
		printf("%2d %-12s mem %6u B  state %4u B  ~%u cyc/frame  tail %u ms  %s\n", i, d->name,
		       (unsigned)fx_align8(d->mem_bytes), (unsigned)d->state_bytes, d->cpu_cycles, (unsigned)d->tail_ms,
		       d->mix_mode == FX_MIX_SEND ? "send" : "crossfade");
		for (int p = 0; p < d->nparams; p++) {
			const fx_param_desc *q = &d->params[p];
			printf("      %d: %-14s %g .. %g %s (default %g)%s%s\n", p + 1, q->name, q->min, q->max, q->unit, q->def,
			       q->labels ? "  [" : "", q->labels ? q->labels : "");
			if (q->labels) printf("]\n");
		}
	}
}

static void json_str(const char *s)
{
	putchar('"');
	for (; s && *s; s++) { if (*s == '"' || *s == '\\') putchar('\\'); putchar(*s); }
	putchar('"');
}

/* machine-readable effect table for the companion app and tools */
static void list_json(void)
{
	printf("[\n");
	for (int i = 0; i < FX_COUNT; i++) {
		const fx_desc *d = fx_get(i);
		printf("  {\"id\": %d, \"name\": ", i); json_str(d->name);
		printf(", \"cpuCycles\": %u, \"memBytes\": %u, \"tailMs\": %u, \"mixMode\": \"%s\", \"params\": [",
		       d->cpu_cycles, (unsigned)fx_align8(d->mem_bytes), (unsigned)d->tail_ms, d->mix_mode == FX_MIX_SEND ? "send" : "crossfade");
		for (int p = 0; p < d->nparams; p++) {
			const fx_param_desc *q = &d->params[p];
			printf("%s{\"name\": ", p ? ", " : ""); json_str(q->name);
			printf(", \"unit\": "); json_str(q->unit);
			printf(", \"min\": %g, \"max\": %g, \"def\": %g, \"scale\": \"%s\"", q->min, q->max, q->def,
			       q->scale == FX_SCALE_LOG ? "log" : (q->scale == FX_SCALE_STEP ? "step" : "lin"));
			if (q->labels) { printf(", \"labels\": "); json_str(q->labels); }
			printf("}");
		}
		printf("]}%s\n", i + 1 < FX_COUNT ? "," : "");
	}
	printf("]\n");
}

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "--list")) { list(); return 0; }
	if (argc >= 2 && !strcmp(argv[1], "--list-json")) { list_json(); return 0; }
	if (argc < 3) {
		fprintf(stderr, "usage: %s --list | in.wav out.wav [--slot N=effect] [--param N.P=value] [--wet N=0..1]\n"
		                "       [--bypass N] [--tail sec] [--block n] [--pool KB] [--bits 16|24|32] [--bench]\n", argv[0]);
		return 2;
	}
	const char *inp = argv[1], *outp = argv[2];
	int block = 64, bits = 24, bench = 0;
	float tail = 0.0f;
	uint32_t pool_kb = 150;

	struct { int slot, idx; float v; } params[64];
	int nparams = 0;
	int assign[4] = {0, 0, 0, 0};
	float wet[4] = {1, 1, 1, 1};
	int bypass[4] = {0, 0, 0, 0};

	for (int i = 3; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : "";
		if (!strcmp(a, "--slot")) {
			int s; char nm[64];
			if (sscanf(v, "%d=%63s", &s, nm) != 2 || s < 1 || s > 4) { fprintf(stderr, "bad --slot %s\n", v); return 2; }
			int id = find_fx(nm);
			if (id < 0) { fprintf(stderr, "unknown effect %s (see --list)\n", nm); return 2; }
			assign[s - 1] = id; i++;
		} else if (!strcmp(a, "--param")) {
			int s, p; float x;
			if (sscanf(v, "%d.%d=%f", &s, &p, &x) != 3 || s < 1 || s > 4 || p < 1 || p > 4 || nparams >= 64) { fprintf(stderr, "bad --param %s\n", v); return 2; }
			params[nparams].slot = s - 1; params[nparams].idx = p - 1; params[nparams].v = x; nparams++; i++;
		} else if (!strcmp(a, "--wet")) {
			int s; float x;
			if (sscanf(v, "%d=%f", &s, &x) != 2 || s < 1 || s > 4) { fprintf(stderr, "bad --wet %s\n", v); return 2; }
			wet[s - 1] = x; i++;
		} else if (!strcmp(a, "--bypass")) { int s = atoi(v); if (s >= 1 && s <= 4) bypass[s - 1] = 1; i++; }
		else if (!strcmp(a, "--tail")) { tail = (float)atof(v); i++; }
		else if (!strcmp(a, "--block")) { block = atoi(v); i++; }
		else if (!strcmp(a, "--pool")) { pool_kb = (uint32_t)atoi(v); i++; }
		else if (!strcmp(a, "--bits")) { bits = atoi(v); i++; }
		else if (!strcmp(a, "--bench")) bench = 1;
		else { fprintf(stderr, "unknown option %s\n", a); return 2; }
	}
	if (block < 1 || block > 4096) block = 64;

	wav_audio in;
	int rc = wav_read(inp, &in);
	if (rc) { fprintf(stderr, "cannot read %s (error %d). Supported: PCM 16/24/32, float32, mono/stereo\n", inp, rc); return 1; }

	uint8_t *pool = calloc(pool_kb, 1024);
	engine_t *e = malloc(sizeof *e);
	if (!pool || !e) return 1;
	engine_init(e, pool, pool_kb * 1024, (float)in.sample_rate, 100000u); /* host: no CPU admission */
	for (int s = 0; s < 4; s++) {
		engine_status st = engine_assign(e, s, assign[s]);
		if (st != ENG_OK) {
			fprintf(stderr, "slot %d: cannot assign %s: %s\n", s + 1, fx_get(assign[s])->name, st == ENG_ERR_MEM ? "out of DSP memory" : "rejected");
			return 3;
		}
		engine_set_wet(e, s, wet[s]);
		engine_set_bypass(e, s, bypass[s]);
	}
	for (int i = 0; i < nparams; i++) {
		const fx_desc *d = fx_get(assign[params[i].slot]);
		if (params[i].idx >= d->nparams) { fprintf(stderr, "%s has no parameter %d\n", d->name, params[i].idx + 1); return 2; }
		engine_set_param(e, params[i].slot, params[i].idx, fx_param_unmap(&d->params[params[i].idx], params[i].v));
	}
	engine_set_master(e, 1.0f);
	e->master_cur = 1.0f;

	uint32_t total = in.frames + (uint32_t)(tail * (float)in.sample_rate);
	wav_audio out = {malloc(sizeof(float) * (total ? total : 1)), malloc(sizeof(float) * (total ? total : 1)), total, in.sample_rate};
	float *zl = calloc(block, sizeof(float)), *zr = calloc(block, sizeof(float));
	float *tl = malloc(sizeof(float) * (size_t)block), *tr = malloc(sizeof(float) * (size_t)block);

	double t0 = now_s();
	double worst = 0.0;
	for (uint32_t pos = 0; pos < total; pos += (uint32_t)block) {
		int n = (int)((total - pos) < (uint32_t)block ? (total - pos) : (uint32_t)block);
		for (int i = 0; i < n; i++) {
			tl[i] = (pos + (uint32_t)i < in.frames) ? in.l[pos + (uint32_t)i] : zl[i];
			tr[i] = (pos + (uint32_t)i < in.frames) ? in.r[pos + (uint32_t)i] : zr[i];
		}
		double b0 = now_s();
		engine_process(e, tl, tr, out.l + pos, out.r + pos, n);
		double bt = now_s() - b0;
		if (bench && bt > worst) worst = bt;
	}
	double el = now_s() - t0;

	if (wav_write(outp, &out, bits)) { fprintf(stderr, "cannot write %s\n", outp); return 1; }
	uint32_t mem, cpu;
	engine_resources(e, &mem, &cpu);
	fprintf(stderr, "processed %u frames @ %u Hz. DSP memory %u B of %u B, declared %u cyc/frame. in-clips %u out-clips %u nan-events %u\n",
	        total, in.sample_rate, mem, pool_kb * 1024, cpu, e->diag.in_clips, e->diag.out_clips, e->diag.nan_events);
	if (bench) {
		double dur = (double)total / in.sample_rate;
		fprintf(stderr, "BENCH (host, not the SP-1): %.3f s for %.3f s of audio = %.0fx realtime, worst %d-frame block %.1f us (budget %.1f us)\n",
		        el, dur, dur / el, block, worst * 1e6, (double)block / in.sample_rate * 1e6);
	}
	return 0;
}
