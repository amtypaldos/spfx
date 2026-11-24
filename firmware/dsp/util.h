/* SP-1 FX — small DSP helpers. Plain C99, no platform dependencies.
 * Everything here is safe to call from the real-time audio thread:
 * no allocation, no locks, bounded execution time. */
#ifndef SP1FX_UTIL_H
#define SP1FX_UTIL_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define FX_PI 3.14159265358979f

static inline float fx_clamp(float x, float lo, float hi)
{
	return x < lo ? lo : (x > hi ? hi : x);
}

/* true for NaN and +-Inf */
static inline int fx_bad(float x)
{
	return !(x - x == 0.0f);
}

/* one-pole smoothing coefficient for time constant tau_s at the given rate */
static inline float fx_coef(float tau_s, float rate)
{
	return 1.0f - expf(-1.0f / (tau_s * rate));
}

/* Flush tiny values to zero so recirculating state never goes denormal. */
static inline float fx_flush(float x)
{
	return (x > 1e-18f || x < -1e-18f) ? x : 0.0f;
}

/* Rational tanh approximation, exact at 0 and +-3 (where it saturates to +-1). */
static inline float fx_tanh(float x)
{
	if (x > 3.0f) return 1.0f;
	if (x < -3.0f) return -1.0f;
	float x2 = x * x;
	return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Cheap sine-ish LFO, phase in [0,1), output [-1,1]. ~3% shape error, fine for modulation. */
static inline float fx_sin01(float p)
{
	float t = p < 0.25f ? 4.0f * p : (p < 0.75f ? 2.0f - 4.0f * p : 4.0f * p - 4.0f);
	return t * (1.5f - 0.5f * t * t);
}

static inline float fx_wrap01(float p)
{
	if (p >= 1.0f) p -= 1.0f;
	if (p >= 1.0f) p -= 1.0f;
	if (p < 0.0f) p += 1.0f;
	return p;
}

/* Fast log2 / exp2 (error well under 0.05 dB for dynamics use). */
static inline float fx_log2(float x)
{
	uint32_t i;
	float m;
	memcpy(&i, &x, 4);
	float e = (float)((int)(i >> 23) - 127);
	i = (i & 0x007FFFFFu) | 0x3F800000u;
	memcpy(&m, &i, 4);
	/* m in [1,2). The polynomial approximates log2(m)+1, hence the -1. Max error ~0.005. */
	return e + ((-0.34484843f * m + 2.02466578f) * m - 1.67487759f);
}

static inline float fx_exp2(float x)
{
	x = fx_clamp(x, -120.0f, 120.0f);
	float fl = floorf(x);
	float f = x - fl;
	float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f)));
	uint32_t bits = (uint32_t)((int)fl + 127) << 23;
	float s;
	memcpy(&s, &bits, 4);
	return s * p;
}

/* xorshift32 white noise in [-1,1) */
static inline float fx_noise(uint32_t *state)
{
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

/* ---- 16-bit delay line (RAM-frugal; +-2.0 full scale) -------------------
 * Convention: read BEFORE write. A sample written k calls ago has delay k. */
#define FX_DL_SCALE 16384.0f
typedef struct {
	int16_t *buf;
	uint32_t size, w;
} fx_dl;

static inline void fx_dl_init(fx_dl *d, int16_t *buf, uint32_t n)
{
	d->buf = buf;
	d->size = n;
	d->w = 0;
	memset(buf, 0, n * sizeof(int16_t));
}

static inline void fx_dl_clear(fx_dl *d)
{
	memset(d->buf, 0, d->size * sizeof(int16_t));
	d->w = 0;
}

static inline void fx_dl_write(fx_dl *d, float x)
{
	float v = x * FX_DL_SCALE;
	int s = (int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
	if (s > 32767) s = 32767;
	if (s < -32768) s = -32768;
	d->buf[d->w] = (int16_t)s;
	if (++d->w >= d->size) d->w = 0;
}

/* delay in samples, 1 <= delay <= size-2, linear interpolation */
static inline float fx_dl_read(const fx_dl *d, float delay)
{
	if (delay < 1.0f) delay = 1.0f;
	float maxd = (float)(d->size - 2);
	if (delay > maxd) delay = maxd;
	int32_t i = (int32_t)delay;
	float fr = delay - (float)i;
	int32_t a = (int32_t)d->w - i;
	if (a < 0) a += (int32_t)d->size;
	int32_t b = a - 1;
	if (b < 0) b += (int32_t)d->size;
	float x0 = (float)d->buf[a], x1 = (float)d->buf[b];
	return (x0 + fr * (x1 - x0)) * (1.0f / FX_DL_SCALE);
}

#endif
