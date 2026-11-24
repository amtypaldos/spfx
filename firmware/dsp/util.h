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

/* Branch-free flush for per-sample use: returns x unchanged for |x| > ~1e-11 and rounds
 * smaller values to a multiple of ~1e-25 (exactly 0 below ~5e-26), so nothing recirculating
 * through it can become denormal. Relies on IEEE evaluation (no -ffast-math). */
static inline float fx_flush_fast(float x)
{
	return (x + 1e-18f) - 1e-18f; /* not folded: the build never uses -ffast-math */
}

/* floor() for |x| < 2^31 without a libm call (newlib/picolibc floorf is an out-of-line
 * function on Cortex-M4). Exact for every float in range. */
static inline int fx_floori(float x)
{
	int i = (int)x;          /* truncates toward zero */
	return ((float)i > x) ? i - 1 : i;
}

/* Word-wise clear / copy for the audio path. The firmware links a size-optimised libc whose
 * memset/memcpy move one byte per loop iteration (about 5 cycles/byte on the M4); the DSP
 * sources are compiled with -fno-tree-loop-distribute-patterns so these loops stay loops.
 * n is in 32-bit words; pointers must be 4-byte aligned. */
static inline void fx_zero32(void *dst, uint32_t n)
{
	uint32_t *d = (uint32_t *)dst;
	while (n >= 4) { d[0] = 0; d[1] = 0; d[2] = 0; d[3] = 0; d += 4; n -= 4; }
	while (n--) *d++ = 0;
}

static inline void fx_copy32(void *dst, const void *src, uint32_t n)
{
	uint32_t *d = (uint32_t *)dst;
	const uint32_t *s = (const uint32_t *)src;
	while (n >= 4) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; d += 4; s += 4; n -= 4; }
	while (n--) *d++ = *s++;
}

/* Rational tanh approximation, exact at 0 and +-3 (where it saturates to +-1). */
static inline float fx_tanh(float x)
{
	if (x > 3.0f) return 1.0f;
	if (x < -3.0f) return -1.0f;
	float x2 = x * x;
	return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Cubic soft clipper, no division: x - x^3/6.75 on |x| <= 1.5, +-1 beyond (smooth: slope 0
 * at the knee). Harder knee than tanh; used where a cheap saturation curve is wanted
 * (saturation effect, filter drive). |fx_softclip(x) - fx_tanh(x)| < 0.1 everywhere
 * (tests/test_dsp.c). */
static inline float fx_softclip(float x)
{
	if (x > 1.5f) return 1.0f;
	if (x < -1.5f) return -1.0f;
	return x * (1.0f - x * x * (1.0f / 6.75f));
}

/* Feedback-path saturation (rc5): exactly linear up to |x| = 1.0, then a smooth knee (slope 1
 * at the join) that approaches 1.9 -- below the +-2.0 rail of the 16-bit delay lines, so hot
 * input plus high feedback rounds off instead of hard-clipping in fx_dl_q(). */
static inline float fx_fbsat(float x)
{
	float a = fabsf(x);
	if (a <= 1.0f) return x;
	float y = 1.0f + 0.9f * fx_softclip((a - 1.0f) * (1.0f / 0.9f));
	return x < 0.0f ? -y : y;
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
	int fl = fx_floori(x);
	float f = x - (float)fl;
	float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f)));
	uint32_t bits = (uint32_t)(fl + 127) << 23;
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

static inline void fx_zero16(int16_t *buf, uint32_t n)
{
	if (((uintptr_t)buf & 3u) && n) { *buf++ = 0; n--; }
	fx_zero32(buf, n >> 1);
	if (n & 1u) buf[n - 1] = 0;
}

static inline void fx_dl_init(fx_dl *d, int16_t *buf, uint32_t n)
{
	d->buf = buf;
	d->size = n;
	d->w = 0;
	fx_zero16(buf, n);
}

static inline void fx_dl_clear(fx_dl *d)
{
	fx_zero16(d->buf, d->size);
	d->w = 0;
}

/* float -> int16 delay-line sample (round half away from zero, saturating) */
static inline int16_t fx_dl_q(float x)
{
	float v = x * FX_DL_SCALE;
	if (v > 32767.0f) v = 32767.0f;
	if (v < -32768.0f) v = -32768.0f;
	return (int16_t)(int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

static inline void fx_dl_write(fx_dl *d, float x)
{
	d->buf[d->w] = fx_dl_q(x);
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

/* Same as fx_dl_read() for a write position w and a delay the caller has already clamped
 * to 1 <= delay <= size-2 (no clamping here). */
static inline float fx_dl_read_at(const int16_t *buf, uint32_t size, uint32_t w, float delay)
{
	int32_t i = (int32_t)delay;
	float fr = delay - (float)i;
	int32_t a = (int32_t)w - i;
	if (a < 0) a += (int32_t)size;
	int32_t b = a - 1;
	if (b < 0) b += (int32_t)size;
	float x0 = (float)buf[a], x1 = (float)buf[b];
	return (x0 + fr * (x1 - x0)) * (1.0f / FX_DL_SCALE);
}

#endif
