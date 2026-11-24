/* SP-1 FX — four-slot serial effects engine.
 *
 * Threading model (firmware):
 *   - control thread: engine_assign / engine_set_* (writes config, never touches DSP state)
 *   - audio thread:   engine_process (applies assignments and parameter changes at block start)
 * All fields shared between them are single-word and written in a safe order, so no locks are
 * needed. The pool is caller-provided static memory; nothing here allocates. */
#ifndef SP1FX_ENGINE_H
#define SP1FX_ENGINE_H

#include "fx.h"
#include <stdint.h>

#define ENGINE_SLOTS 4

typedef struct {
	uint8_t fx_id[ENGINE_SLOTS];
	uint8_t bypass[ENGINE_SLOTS];
	float wet[ENGINE_SLOTS];                               /* 0..1 */
	float params[ENGINE_SLOTS][FX_COUNT][FX_MAX_PARAMS];   /* normalised; remembered per effect */
	uint8_t order[ENGINE_SLOTS];                           /* processing order: permutation of 0..3 */
	uint8_t global_bypass;
	float in_gain_db;                                      /* -12..+12 */
	float master;                                          /* 0..1, dB taper */
} engine_config_t;

typedef enum {
	ENG_OK = 0,
	ENG_ERR_ARG = 1,
	ENG_ERR_MEM = 2,  /* not enough DSP memory */
	ENG_ERR_CPU = 3   /* declared CPU budget exceeded */
} engine_status;

typedef struct {
	float in_peak, out_peak;       /* linear, decaying; read-and-hold by the UI */
	uint32_t in_clips, out_clips;  /* samples at/over full scale */
	uint32_t nan_events;           /* effect output was non-finite and was reset */
	uint32_t blocks;
	uint32_t underruns, overruns;  /* written by the audio driver */
	uint32_t cpu_peak_cycles, cpu_last_cycles; /* written by the audio driver */
} engine_diag_t;

typedef struct {
	engine_config_t cfg;
	uint8_t *pool;
	uint32_t pool_bytes;
	uint32_t cpu_budget;   /* declared cycles per stereo frame */
	float sr;

	/* desired layout (control thread) */
	uint32_t blk_off[ENGINE_SLOTS], blk_size[ENGINE_SLOTS];
	/* live state (audio thread) */
	uint8_t live_id[ENGINE_SLOTS];
	void *live_st[ENGINE_SLOTS];
	float pushed[ENGINE_SLOTS][FX_MAX_PARAMS];
	float mix_cur[ENGINE_SLOTS], in_cur[ENGINE_SLOTS];
	uint32_t tail_rem[ENGINE_SLOTS];
	uint8_t active[ENGINE_SLOTS];
	float ingain_cur, master_cur;
	float k_mix, k_in, k_gain, k_master;

	engine_diag_t diag;
} engine_t;

#define ENGINE_DEFAULT_CPU_BUDGET 850u

void engine_config_default(engine_config_t *c);
/* Repair a config of unknown provenance (corrupt flash, bad import). Returns number of fixes. */
int engine_config_sanitize(engine_config_t *c);

void engine_init(engine_t *e, void *pool, uint32_t pool_bytes, float sr, uint32_t cpu_budget);
/* Apply a whole config. Slots that don't fit are set to None; returns how many were rejected. */
int engine_apply_config(engine_t *e, const engine_config_t *c);

/* Assign an effect to a slot. On any error the previous assignment is untouched. */
engine_status engine_assign(engine_t *e, int slot, int fx_id);
void engine_set_param(engine_t *e, int slot, int idx, float norm);
void engine_set_wet(engine_t *e, int slot, float wet);
void engine_set_bypass(engine_t *e, int slot, int bypass);
void engine_set_global_bypass(engine_t *e, int on);
void engine_set_input_gain_db(engine_t *e, float db);
void engine_set_master(engine_t *e, float v);
int engine_set_order(engine_t *e, const uint8_t order[ENGINE_SLOTS]); /* 0 ok, -1 invalid */

/* How much of the budget is spoken for (declared values). */
void engine_resources(const engine_t *e, uint32_t *mem_used, uint32_t *cpu_used);

/* Process n frames (any n). in and out may alias. Audio thread only. */
void engine_process(engine_t *e, const float *in_l, const float *in_r, float *out_l, float *out_r, int n);

#endif
