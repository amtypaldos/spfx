/* SP-1 FX — small decision logic of the integrated firmware (zephyr_sp1fx), kept in pure C so
 * it is host-tested (tests/test_app_logic.c). No I/O, no globals.
 *
 *  1. Save/erase scheduling: when a debounced settings save may run, and when a flash page
 *     erase (which stalls the nRF52840 CPU, USB and I2S servicing for up to ~85 ms) may run.
 *  2. Console mode: one CDC ACM port carries both the human-readable diagnostic lines and the
 *     binary companion protocol (docs/PROTOCOL.md). Text goes out only until the first valid
 *     protocol frame arrives; from then on only binary frames, until the host closes the port
 *     (DTR low).
 *  3. Audio quiet detector: "nothing audible is playing" for long enough that an erase stall
 *     cannot be heard.
 *  4. Line rate limiter for event lines on the text console. */
#ifndef SP1FX_APP_LOGIC_H
#define SP1FX_APP_LOGIC_H

#include <stdint.h>

/* ---- 1. save/erase scheduling ---------------------------------------------------------- */
typedef enum {
	SAVE_IDLE = 0,
	SAVE_WRITE,            /* append a record: no erase, chunked writes, safe at any time */
	SAVE_ERASE_SPARE,      /* pre-erase the spare page now (audio is quiet) */
	SAVE_WRITE_WITH_ERASE  /* the save itself needs an erase; audio is quiet */
} save_action_t;

#define SAVE_ERASE_RETRY_MS 10000u /* after a failed erase, wait before trying again */

typedef struct {
	int pending;               /* a save is due but was deferred (it needs an erase, not quiet) */
	uint32_t deferred;         /* ticks a due save waited for a quiet moment (diagnostics) */
	uint32_t last_erase_try_ms;
	int erase_tried;
} save_sched_t;

void save_sched_init(save_sched_t *s);
/* Call once per control tick.
 *   due          store_debounce_due() said "write now" on this tick
 *   needs_erase  store_save_needs_erase()
 *   spare_dirty  store_spare_needs_erase()
 *   quiet        the audio has been quiet long enough (quiet_ok())
 * The caller performs the returned action and reports an erase via save_sched_erased(). */
save_action_t save_sched_next(save_sched_t *s, uint32_t now_ms, int due, int needs_erase,
			      int spare_dirty, int quiet);
void save_sched_erased(save_sched_t *s, uint32_t now_ms);

/* ---- 2. console mode ---------------------------------------------------------------- */
typedef struct {
	uint8_t open;              /* host has the port open (DTR) */
	uint8_t binary;            /* companion detected: binary frames only */
	uint32_t mark;             /* protocol rx frame counter when the port was opened */
	uint32_t switches;         /* text -> binary transitions */
} con_mode_t;

void conmode_init(con_mode_t *m);
/* Update from the DTR line and the protocol's valid-frame counter. Returns 1 exactly when the
 * mode switches to binary: the caller then sends one 0x00 first, so a host-side COBS decoder
 * drops any half-received text line as one bad frame instead of merging it with the next
 * frame. */
int conmode_update(con_mode_t *m, int dtr, uint32_t rx_frames);
int conmode_text_ok(const con_mode_t *m);    /* diagnostic text may be sent */
int conmode_binary_ok(const con_mode_t *m);  /* protocol frames may be sent */

/* ---- 3. audio quiet detector -------------------------------------------------------- */
#define QUIET_PEAK 0.0005f         /* -66 dBFS: below this a block counts as silent */
typedef struct {
	uint32_t silent_blocks;    /* consecutive silent blocks */
} quiet_t;
/* Feed once per audio block with the block's input and output peaks (linear, 1.0 = FS). */
void quiet_feed(quiet_t *q, float in_peak, float out_peak);
/* 1 if at least `blocks` consecutive blocks were silent. */
int quiet_ok(const quiet_t *q, uint32_t blocks);

/* ---- 4. line rate limiter (token bucket) -------------------------------------------- */
typedef struct {
	uint32_t tokens_x1000, burst, per_s, last_ms;
	uint32_t dropped;
} ratelimit_t;
void ratelimit_init(ratelimit_t *r, uint32_t per_s, uint32_t burst, uint32_t now_ms);
int ratelimit_take(ratelimit_t *r, uint32_t now_ms); /* 1 = may send a line now */

#endif
