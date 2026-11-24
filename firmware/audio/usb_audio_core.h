/* SP-1 FX: USB audio clock-domain core (no Zephyr dependency; host-tested by
 * tests/test_usb_audio_core.c).
 *
 *   - ua_ring_t: lock-free single-producer / single-consumer ring of
 *     interleaved stereo int16 frames, caller-provided static storage.
 *   - ua_cons_t: consumer-side stream state (prebuffer to a target fill,
 *     underrun detection, drop/duplicate-one-frame safeguard at the limits).
 *   - ua_fb_t:   USB Full-Speed explicit feedback value (Q10.14 frames per
 *     1 ms SOF) from the ring fill: EMA low-pass + gentle proportional law,
 *     clamped around nominal. Same law as chattock/sp1-tape-looper's
 *     feedback_update() (MIT, commit 0359372e); see NOTICES.md of the app.
 *   - ua_pkt_frames(): turns a Q10.14 rate into whole-frame packet sizes
 *     (implicit-feedback IN stream: the IN packet sizes ARE the feedback).
 *   - int16 <-> float conversion with saturation, gain ramps (fades).
 *
 * Threading: ring producer and consumer may run in different threads (one
 * each); indices are 32-bit free-running words written by one side only.
 * Nothing here allocates.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SP1FX_USB_AUDIO_CORE_H
#define SP1FX_USB_AUDIO_CORE_H

#include <stdint.h>

/* ------------------------------------------------------------------ ring */
typedef struct {
	int16_t *buf;              /* cap * 2 int16 (L, R interleaved) */
	uint32_t cap;              /* frames, power of two */
	volatile uint32_t head;    /* written by the producer only */
	volatile uint32_t tail;    /* written by the consumer only */
} ua_ring_t;

/* cap_frames must be a power of two >= 2. Returns 0 ok, -1 bad size. */
int ua_ring_init(ua_ring_t *r, int16_t *storage, uint32_t cap_frames);
uint32_t ua_ring_fill(const ua_ring_t *r);    /* frames readable */
uint32_t ua_ring_space(const ua_ring_t *r);   /* frames writable */
/* Producer: writes up to n frames, returns frames written (rest lost). */
uint32_t ua_ring_write(ua_ring_t *r, const int16_t *frames, uint32_t n);
/* Consumer: reads up to n frames into out (may be NULL = discard). */
uint32_t ua_ring_read(ua_ring_t *r, int16_t *out, uint32_t n);
/* Consumer: discard everything currently readable. */
void ua_ring_flush(ua_ring_t *r);

/* --------------------------------------------------------- consumer side */
enum { UA_PREBUF = 0, UA_RUN = 1 };

enum {
	UA_BLK_SILENT   = 0,   /* nothing read: prebuffering (out untouched) */
	UA_BLK_AUDIO    = 1,   /* n frames written to out */
	UA_BLK_STARTED  = 2,   /* flag: first block after prebuffer (start fade-in) */
	UA_BLK_UNDERRUN = 4,   /* flag: ran dry, back to prebuffer (decay/fade out) */
	UA_BLK_DUP      = 8,   /* flag: one frame duplicated (fill below low) */
	UA_BLK_DROP     = 16   /* flag: one frame dropped (fill above high) */
};

typedef struct {
	ua_ring_t *ring;
	uint32_t target, low, high;   /* frames */
	int state;
	/* statistics (consumer thread) */
	uint32_t underruns, dups, drops, starts;
	uint32_t fill_min, fill_max, fill_n;
	uint64_t fill_sum;
} ua_cons_t;

void ua_cons_init(ua_cons_t *c, ua_ring_t *ring, uint32_t target, uint32_t low, uint32_t high);
/* Consumer reset (stream (re)start): flush the ring, back to prebuffer. */
void ua_cons_restart(ua_cons_t *c);
/* Read n frames (n >= 2) with the safeguards. Returns UA_BLK_* bits. */
int ua_cons_read(ua_cons_t *c, int16_t *out, uint32_t n);
/* Clear fill min/avg/max window (consumer thread). */
void ua_cons_stats_window_reset(ua_cons_t *c);

/* ---------------------------------------------------------------- feedback */
#define UA_FB_FRAC 14                         /* Full-Speed Q10.14 */
#define UA_FB_ONE  (1L << UA_FB_FRAC)

typedef struct {
	int32_t nominal;     /* Q10.14 frames per SOF, e.g. 48 << 14 */
	int32_t setpoint;    /* target fill, frames */
	int32_t kp;          /* feedback LSBs per frame of smoothed fill error */
	int32_t clamp;       /* max |value - nominal| in LSBs */
	int ema_shift;       /* fill EMA: tau = 2^ema_shift SOFs */
	int32_t fill_avg;    /* frames, Q8 */
	int32_t value;       /* last computed Q10.14 */
	int primed;
} ua_fb_t;

void ua_fb_init(ua_fb_t *f, int32_t nominal_q14, int32_t setpoint, int32_t kp,
		int32_t clamp, int ema_shift);
void ua_fb_reset(ua_fb_t *f);                   /* back to nominal, EMA re-primed */
int32_t ua_fb_update(ua_fb_t *f, uint32_t fill); /* once per SOF; returns value */

/* Implicit feedback: whole frames for the next packet from a Q10.14 rate. */
uint32_t ua_pkt_frames(uint32_t *acc_q14, int32_t rate_q14);

/* -------------------------------------------------------------- samples */
/* int16 interleaved -> two float planes, full scale = 1.0. */
void ua_s16_to_f32(const int16_t *in, float *l, float *r, int n);
/* Two float planes * gain -> int16 interleaved, rounded, saturated; NaN/Inf
 * -> 0. Returns the number of samples that had to be saturated/replaced. */
uint32_t ua_f32_to_s16(const float *l, const float *r, int16_t *out, int n, float gain);

/* Linear gain ramp. up/down: per-sample increments (1/(seconds*fs)). */
typedef struct {
	float g, up, down;
} ua_ramp_t;

void ua_ramp_init(ua_ramp_t *r, float fs, float up_s, float down_s);
/* Multiplies l/r by the ramp moving toward target (0..1). Returns new g. */
float ua_ramp_apply(ua_ramp_t *r, float target, float *l, float *r_, int n);
/* Fill l/r with a linear decay from (last_l, last_r) to 0 over n frames
 * (click-free tail when the input runs dry). */
void ua_decay(float last_l, float last_r, float *l, float *r, int n);

#endif
