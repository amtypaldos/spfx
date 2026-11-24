/* SP-1 FX: USB audio clock-domain core. See usb_audio_core.h.
 * SPDX-License-Identifier: MIT */
#include "usb_audio_core.h"

#include <string.h>

/* Single-core Cortex-M4 (and the host test): a compiler barrier orders the
 * data copy before the index publish. Volatile indices do the rest. */
#if defined(__GNUC__) || defined(__clang__)
#define UA_BARRIER() __asm__ __volatile__("" ::: "memory")
#else
#define UA_BARRIER() do { } while (0)
#endif

/* ------------------------------------------------------------------ ring */
int ua_ring_init(ua_ring_t *r, int16_t *storage, uint32_t cap_frames)
{
	if (!r || !storage || cap_frames < 2u || (cap_frames & (cap_frames - 1u)) != 0u) return -1;
	r->buf = storage;
	r->cap = cap_frames;
	r->head = 0u;
	r->tail = 0u;
	return 0;
}

uint32_t ua_ring_fill(const ua_ring_t *r)
{
	uint32_t h = r->head, t = r->tail;
	uint32_t f = h - t;
	return f > r->cap ? r->cap : f;
}

uint32_t ua_ring_space(const ua_ring_t *r) { return r->cap - ua_ring_fill(r); }

uint32_t ua_ring_write(ua_ring_t *r, const int16_t *frames, uint32_t n)
{
	uint32_t h = r->head;
	uint32_t space = r->cap - (h - r->tail);
	if (n > space) n = space;
	uint32_t i0 = h & (r->cap - 1u);
	uint32_t first = r->cap - i0;
	if (first > n) first = n;
	memcpy(&r->buf[2u * i0], frames, (size_t)first * 2u * sizeof(int16_t));
	if (n > first) memcpy(&r->buf[0], &frames[2u * first], (size_t)(n - first) * 2u * sizeof(int16_t));
	UA_BARRIER();
	r->head = h + n;
	return n;
}

uint32_t ua_ring_read(ua_ring_t *r, int16_t *out, uint32_t n)
{
	uint32_t t = r->tail;
	uint32_t avail = r->head - t;
	UA_BARRIER();
	if (n > avail) n = avail;
	if (out) {
		uint32_t i0 = t & (r->cap - 1u);
		uint32_t first = r->cap - i0;
		if (first > n) first = n;
		memcpy(out, &r->buf[2u * i0], (size_t)first * 2u * sizeof(int16_t));
		if (n > first) memcpy(&out[2u * first], &r->buf[0], (size_t)(n - first) * 2u * sizeof(int16_t));
	}
	UA_BARRIER();
	r->tail = t + n;
	return n;
}

void ua_ring_flush(ua_ring_t *r)
{
	UA_BARRIER();
	r->tail = r->head;
}

/* --------------------------------------------------------- consumer side */
void ua_cons_stats_window_reset(ua_cons_t *c)
{
	c->fill_min = 0xFFFFFFFFu;
	c->fill_max = 0u;
	c->fill_n = 0u;
	c->fill_sum = 0u;
}

void ua_cons_init(ua_cons_t *c, ua_ring_t *ring, uint32_t target, uint32_t low, uint32_t high)
{
	memset(c, 0, sizeof *c);
	c->ring = ring;
	c->target = target;
	c->low = low;
	c->high = high;
	c->state = UA_PREBUF;
	ua_cons_stats_window_reset(c);
}

void ua_cons_restart(ua_cons_t *c)
{
	ua_ring_flush(c->ring);
	c->state = UA_PREBUF;
}

int ua_cons_read(ua_cons_t *c, int16_t *out, uint32_t n)
{
	uint32_t fill = ua_ring_fill(c->ring);
	int flags = 0;

	if (fill < c->fill_min) c->fill_min = fill;
	if (fill > c->fill_max) c->fill_max = fill;
	c->fill_sum += fill;
	c->fill_n++;

	if (c->state == UA_PREBUF) {
		if (fill < c->target || fill < n) return UA_BLK_SILENT;
		c->state = UA_RUN;
		c->starts++;
		flags |= UA_BLK_STARTED;
	}
	if (fill + 1u < n) {
		/* Ran dry (one missing frame is still covered by a duplicate). */
		c->underruns++;
		c->state = UA_PREBUF;
		return UA_BLK_SILENT | UA_BLK_UNDERRUN;
	}
	if (fill < c->low || fill < n) {
		/* Consume one frame less, repeat the last frame. */
		uint32_t got = ua_ring_read(c->ring, out, n - 1u);
		out[2u * got] = out[2u * got - 2u];
		out[2u * got + 1u] = out[2u * got - 1u];
		c->dups++;
		return flags | UA_BLK_AUDIO | UA_BLK_DUP;
	}
	(void)ua_ring_read(c->ring, out, n);
	if (fill > c->high) {
		/* Consume one frame more: discard the next one. */
		(void)ua_ring_read(c->ring, (int16_t *)0, 1u);
		c->drops++;
		flags |= UA_BLK_DROP;
	}
	return flags | UA_BLK_AUDIO;
}

/* ---------------------------------------------------------------- feedback */
void ua_fb_init(ua_fb_t *f, int32_t nominal_q14, int32_t setpoint, int32_t kp,
		int32_t clamp, int ema_shift)
{
	f->nominal = nominal_q14;
	f->setpoint = setpoint;
	f->kp = kp;
	f->clamp = clamp;
	f->ema_shift = ema_shift;
	ua_fb_reset(f);
}

void ua_fb_reset(ua_fb_t *f)
{
	f->fill_avg = f->setpoint << 8;
	f->value = f->nominal;
	f->primed = 0;
}

int32_t ua_fb_update(ua_fb_t *f, uint32_t fill)
{
	int32_t x = (int32_t)fill << 8;

	if (!f->primed) {
		f->fill_avg = x;
		f->primed = 1;
	} else {
		/* arithmetic shift of a possibly negative difference: written as a
		 * division-free, sign-safe form */
		int32_t d = x - f->fill_avg;
		f->fill_avg += (d >= 0) ? (d >> f->ema_shift) : -((-d) >> f->ema_shift);
	}
	int32_t err_q8 = f->fill_avg - (f->setpoint << 8);   /* >0: too full -> ask less */
	int32_t adj = (int32_t)(((int64_t)err_q8 * f->kp) / 256);
	int32_t v = f->nominal - adj;

	if (v > f->nominal + f->clamp) v = f->nominal + f->clamp;
	if (v < f->nominal - f->clamp) v = f->nominal - f->clamp;
	f->value = v;
	return v;
}

uint32_t ua_pkt_frames(uint32_t *acc_q14, int32_t rate_q14)
{
	uint32_t a = *acc_q14 + (uint32_t)rate_q14;
	uint32_t n = a >> UA_FB_FRAC;

	*acc_q14 = a - (n << UA_FB_FRAC);
	return n;
}

/* -------------------------------------------------------------- samples */
void ua_s16_to_f32(const int16_t *in, float *l, float *r, int n)
{
	const float k = 1.0f / 32768.0f;

	for (int i = 0; i < n; i++) {
		l[i] = (float)in[2 * i] * k;
		r[i] = (float)in[2 * i + 1] * k;
	}
}

static inline int16_t sat16(float x, uint32_t *sat)
{
	if (!(x == x) || x > 3.0e9f || x < -3.0e9f) { /* NaN or absurd/Inf */
		(*sat)++;
		return 0;
	}
	if (x >= 32767.0f) {
		if (x > 32767.5f) (*sat)++;
		return 32767;
	}
	if (x <= -32768.0f) {
		if (x < -32768.5f) (*sat)++;
		return -32768;
	}
	return (int16_t)(x >= 0.0f ? (int32_t)(x + 0.5f) : -(int32_t)(-x + 0.5f));
}

uint32_t ua_f32_to_s16(const float *l, const float *r, int16_t *out, int n, float gain)
{
	const float k = gain * 32768.0f;
	uint32_t sat = 0u;

	for (int i = 0; i < n; i++) {
		out[2 * i] = sat16(l[i] * k, &sat);
		out[2 * i + 1] = sat16(r[i] * k, &sat);
	}
	return sat;
}

void ua_ramp_init(ua_ramp_t *r, float fs, float up_s, float down_s)
{
	r->g = 0.0f;
	r->up = up_s > 0.0f ? 1.0f / (up_s * fs) : 1.0f;
	r->down = down_s > 0.0f ? 1.0f / (down_s * fs) : 1.0f;
}

float ua_ramp_apply(ua_ramp_t *r, float target, float *l, float *r_, int n)
{
	float g = r->g;

	if (g == target) {
		if (g == 1.0f) return g;
		for (int i = 0; i < n; i++) {
			l[i] *= g;
			r_[i] *= g;
		}
		return g;
	}
	for (int i = 0; i < n; i++) {
		if (g < target) {
			g += r->up;
			if (g > target) g = target;
		} else if (g > target) {
			g -= r->down;
			if (g < target) g = target;
		}
		l[i] *= g;
		r_[i] *= g;
	}
	r->g = g;
	return g;
}

void ua_decay(float last_l, float last_r, float *l, float *r, int n)
{
	for (int i = 0; i < n; i++) {
		float k = (float)(n - 1 - i) / (float)n;
		l[i] = last_l * k;
		r[i] = last_r * k;
	}
}
