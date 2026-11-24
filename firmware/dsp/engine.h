/* SP-1 FX — four-slot serial effects engine.
 *
 * THREADING RULES (firmware: one control thread, one audio thread, single core)
 *
 *   The engine has two disjoint halves and one mailbox between them.
 *
 *   Control half -- written ONLY by the control thread, NEVER read by engine_process():
 *     cfg, blk_off, blk_size (the desired config and its memory layout) and the mailbox
 *     write side. Every control call below runs admission (CPU + memory) synchronously on
 *     this half, so return values (engine_status, rejected count) are immediate, and then
 *     publishes a complete snapshot {cfg, layout} into the mailbox.
 *
 *   Audio half -- written ONLY by engine_process()/engine_pump() (the audio thread):
 *     run (the config the DSP uses), live_*, pend_*, the pool contents, the smoothing
 *     state. At the START of every 64-frame chunk the audio thread takes the newest
 *     complete snapshot (if any), stops every slot whose effect or offset changed, keeps
 *     every slot whose effect and offset are unchanged, and initialises the changed slots
 *     at their new offsets (optionally a few per block, see init_budget). Because a
 *     snapshot's layout is non-overlapping and every live slot always sits exactly at the
 *     offset of the snapshot it was initialised from, no effect can ever run on, or be
 *     initialised over, memory another live effect is using.
 *
 *   Mailbox -- a two-buffer sequence lock (box[2], box_seq). The control thread writes the
 *     buffer the audio thread is NOT allowed to read (box_seq is odd while writing), the
 *     audio thread copies the newest complete buffer and re-checks box_seq afterwards; a
 *     copy that may have been overwritten is discarded and retried next chunk. Neither side
 *     ever blocks or locks interrupts, and correctness does not depend on thread priorities.
 *     ENGINE_BARRIER() (a full memory barrier, DMB on Cortex-M) orders the accesses.
 *
 *   Exceptions (single-byte / single-word, deliberately shared):
 *     duck       control writes, audio reads (one byte).
 *     ack_post   audio writes, control reads: number of the newest post that is fully live.
 *     diag       mixed, see engine_diag_t; counters only, never used for decisions.
 *   init_budget, hook: set before the audio thread starts (or in single-threaded tests).
 *
 *   Every function except engine_process()/engine_pump() is a CONTROL function: call it
 *   from one control thread only (it is not re-entrant against another control thread).
 *   Single-threaded hosts (tests, simulator) simply call both; engine_pump() applies a
 *   pending snapshot without processing audio.
 *
 * The pool is caller-provided static memory; nothing here allocates. The engine zeroes an
 * effect's state struct before init(); init() must initialise its whole state and clear
 * all delay memory it uses (tests/test_engine_race.c checks this for every effect). */
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
	/* audio thread: config hand-over */
	uint32_t cfg_applies;          /* snapshots taken from the mailbox */
	uint32_t slot_inits;           /* effect init() calls */
	uint32_t init_bytes_max;       /* most effect memory initialised in one chunk */
	uint32_t torn_retries;         /* mailbox copies discarded because of a concurrent post */
	uint32_t layout_errors;        /* snapshot slots refused by the audio-side check (must stay 0) */
	/* audio thread: overload guard (engine_overload_feed) */
	uint32_t overload_blocks;      /* single blocks over the limit (total; informational only) */
	uint32_t overload_windows;     /* guard windows whose MEAN was over the limit (total) */
	uint32_t ovl_last_mean;        /* mean block cycles of the last complete guard window */
	uint32_t ovl_max_mean;         /* highest window mean since boot */
	uint32_t overload_sheds;       /* slots switched off by the guard (total) */
	uint32_t overload_forced;      /* times the guard forced global bypass */
	uint8_t shed_mask;             /* bit s = slot s is switched off by the guard now */
	uint8_t forced_bypass;         /* 1 = guard forced global bypass now */
} engine_diag_t;

/* One complete config snapshot as handed to the audio thread. */
typedef struct {
	engine_config_t cfg;           /* fx_id already admitted (rejected slots are FX_NONE) */
	uint32_t blk_off[ENGINE_SLOTS], blk_size[ENGINE_SLOTS];
	uint32_t post;                 /* 1, 2, 3 ... */
} engine_msg_t;

/* Test hook points (see engine_t.hook). */
enum {
	ENGINE_HOOK_APPLY_SANITIZED = 1, /* control: engine_apply_config, config sanitised */
	ENGINE_HOOK_APPLY_HALF,          /* control: new fx ids written, layout not yet (old race window) */
	ENGINE_HOOK_APPLY_WRITTEN,       /* control: desired state complete, not yet posted */
	ENGINE_HOOK_POST_BEGIN,          /* control: mailbox buffer claimed, nothing copied */
	ENGINE_HOOK_POST_MID,            /* control: half of the snapshot copied */
	ENGINE_HOOK_POST_DONE,           /* control: snapshot published */
	ENGINE_HOOK_TAKE_COPIED          /* audio: snapshot copied, not yet re-checked */
};

typedef struct engine_s engine_t;

struct engine_s {
	/* ---- constant after engine_init */
	uint8_t *pool;
	uint32_t pool_bytes;
	uint32_t cpu_budget;   /* declared cycles per stereo frame */
	float sr;

	/* ---- control half (control thread only) */
	engine_config_t cfg;   /* desired config */
	uint32_t blk_off[ENGINE_SLOTS], blk_size[ENGINE_SLOTS]; /* desired layout */

	/* ---- mailbox */
	engine_msg_t box[2];
	volatile uint32_t box_seq;     /* 2 * completed posts, +1 while a post is being written */

	/* ---- audio half (engine_process / engine_pump only) */
	engine_msg_t stage;            /* copy target, validated before use */
	engine_config_t run;           /* config the DSP uses */
	uint32_t run_off[ENGINE_SLOTS], run_size[ENGINE_SLOTS]; /* layout of the snapshot in use */
	uint32_t run_post, pend_post;
	uint8_t live_id[ENGINE_SLOTS];
	void *live_st[ENGINE_SLOTS];
	uint32_t live_off[ENGINE_SLOTS];
	uint8_t pend_id[ENGINE_SLOTS]; /* != FX_NONE: init at run_off[s] still to do */
	float pushed[ENGINE_SLOTS][FX_MAX_PARAMS];
	float mix_cur[ENGINE_SLOTS], in_cur[ENGINE_SLOTS];
	uint32_t tail_rem[ENGINE_SLOTS];
	uint8_t active[ENGINE_SLOTS];
	float ingain_cur, master_cur;
	float ig_db_cached, ig_t_cached, mg_cached, mg_t_cached; /* per-block gain targets */
	/* overload guard (audio half): 0 = running, 1 = fading out this chunk, 2 = off */
	uint8_t shed[ENGINE_SLOTS];
	uint8_t forced_bypass;         /* every slot off (tails too), dry signal only */
	uint8_t chunk_inits;           /* an effect init ran in the last chunk */
	uint32_t over_run;             /* consecutive guard windows whose mean was over the limit */
	uint64_t ov_sum;               /* cycles summed in the current guard window */
	uint32_t ov_cnt;               /* blocks in the current guard window */
	uint32_t shed_clear_ack;
	float k_mix, k_in, k_gain, k_master, k_duck;
	float duck_cur;

	/* ---- shared single-word fields */
	volatile uint8_t duck;         /* control: 1 = fade output to silence (~20 ms), 0 = back */
	volatile uint32_t ack_post;    /* audio: newest post whose effects are all initialised */
	volatile uint32_t shed_clear_req; /* control: ++ to undo the overload guard's actions */

	/* Max effect memory (bytes) initialised per chunk; 0 = unlimited (all pending inits run
	 * at the start of the next chunk). At least one init always runs per chunk, so a delay
	 * (75 KB) still starts with a budget below its size. Set before the audio thread runs. */
	uint32_t init_budget;

	/* Overload guard (engine_overload_feed): the block cost is averaged over windows of
	 * overload_win blocks; overload_n consecutive windows whose MEAN is over the limit trigger
	 * one action (a window whose mean is over limit * 9/8 acts at once). Set before the audio
	 * thread runs; engine_init sets ENGINE_OVERLOAD_WIN_BLOCKS / ENGINE_OVERLOAD_WINDOWS;
	 * overload_n = 0 disables the guard. */
	uint32_t overload_n;
	uint32_t overload_win;

	/* Tests only (NULL in firmware): called at the ENGINE_HOOK_* points. */
	void (*hook)(engine_t *e, int point, void *ctx);
	void *hook_ctx;

	engine_diag_t diag;
};

/* Admission budget for the sum of the effects' DECLARED cpu_cycles (cycles per stereo frame).
 * The M4F has 64e6/48e3 = 1333 cycles per frame; the engine's own fixed cost, the USB/I2S
 * interrupts and the int16<->float conversions take part of it (docs/STATUS.md). The declared
 * per-effect values are UNMEASURED estimates (static analysis + instruction-count emulation),
 * so the runtime overload guard below is the safety net, not this number. */
#define ENGINE_DEFAULT_CPU_BUDGET 850u
/* Guard window: 32 blocks of 64 frames = 42.7 ms at 48 kHz; 3 windows = 128 ms of sustained
 * overload (mean over the limit) before a slot is shed. Single-block spikes (interrupts, USB)
 * cannot trigger it unless they raise the window mean over the limit. */
#define ENGINE_OVERLOAD_WIN_BLOCKS 32u
#define ENGINE_OVERLOAD_WINDOWS 3u

void engine_config_default(engine_config_t *c);
/* Repair a config of unknown provenance (corrupt flash, bad import). Returns number of fixes. */
int engine_config_sanitize(engine_config_t *c);

void engine_init(engine_t *e, void *pool, uint32_t pool_bytes, float sr, uint32_t cpu_budget);

/* Control: stage a whole config (sanitised copy; slots that do not fit are set to None).
 * Returns how many slots were rejected. The audio thread applies it at its next chunk.
 * Same-effect-same-slot entries keep their memory (and tails) when the layout allows. */
int engine_post_config(engine_t *e, const engine_config_t *c);
/* Same path (kept for startup, tests and existing callers). */
int engine_apply_config(engine_t *e, const engine_config_t *c);
/* Control: 1 when everything posted so far is live in the audio thread. */
int engine_config_settled(const engine_t *e);

/* Control: assign an effect to a slot. On any error the previous assignment is untouched. */
engine_status engine_assign(engine_t *e, int slot, int fx_id);
void engine_set_param(engine_t *e, int slot, int idx, float norm);
void engine_set_wet(engine_t *e, int slot, float wet);
void engine_set_bypass(engine_t *e, int slot, int bypass);
void engine_set_global_bypass(engine_t *e, int on);
void engine_set_input_gain_db(engine_t *e, float db);
void engine_set_master(engine_t *e, float v);
/* Fade the output to silence (and back). Used around preset swaps so tails are not cut with a click. */
void engine_set_duck(engine_t *e, int on);
int engine_set_order(engine_t *e, const uint8_t order[ENGINE_SLOTS]); /* 0 ok, -1 invalid */

/* Control: how much of the budget is spoken for (declared values of the desired config). */
void engine_resources(const engine_t *e, uint32_t *mem_used, uint32_t *cpu_used);

/* Audio: process n frames (any n). in and out may alias. */
void engine_process(engine_t *e, const float *in_l, const float *in_r, float *out_l, float *out_r, int n);
/* Audio: overload guard. Call once after every engine_process() block with the cycles that
 * block's work took and the limit (e.g. 85 % of the block period). The cycles are averaged
 * over windows of overload_win blocks (blocks that initialised an effect are left out). After
 * overload_n consecutive windows whose mean is over the limit (~128 ms with the defaults), or
 * one window whose mean is over limit * 9/8 (safety net: the audio thread cannot keep up), the
 * guard fades out and switches off the live slot with the largest declared cpu_cycles
 * (diag.shed_mask); when no slot is left it forces global bypass (diag.forced_bypass). Returns
 * 1 when it acted on this call. The actions last until the slot gets a new effect, or
 * engine_overload_clear(). */
int engine_overload_feed(engine_t *e, uint32_t cycles, uint32_t limit);
/* Control: undo the guard's actions (taken at the audio thread's next chunk). */
void engine_overload_clear(engine_t *e);

/* Audio (or single-threaded host): take a pending snapshot and run ALL pending inits now,
 * ignoring init_budget, without processing audio. */
void engine_pump(engine_t *e);

#endif
