#include "fx.h"
#include <math.h>

extern const fx_desc fx_desc_none, fx_desc_filter, fx_desc_chorus, fx_desc_delay,
	fx_desc_reverb, fx_desc_saturation, fx_desc_flanger, fx_desc_bitcrusher,
	fx_desc_tremolo, fx_desc_compressor, fx_desc_tape_wobble;

static const fx_desc *const table[FX_COUNT] = {
	&fx_desc_none, &fx_desc_filter, &fx_desc_chorus, &fx_desc_delay,
	&fx_desc_reverb, &fx_desc_saturation, &fx_desc_flanger, &fx_desc_bitcrusher,
	&fx_desc_tremolo, &fx_desc_compressor, &fx_desc_tape_wobble,
};

const fx_desc *fx_get(int id)
{
	return (id >= 0 && id < FX_COUNT) ? table[id] : (const fx_desc *)0;
}

static float clamp01(float x)
{
	if (!(x == x)) return 0.0f; /* NaN */
	return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

float fx_param_map(const fx_param_desc *p, float norm)
{
	norm = clamp01(norm);
	switch (p->scale) {
	case FX_SCALE_LOG:
		return p->min * powf(p->max / p->min, norm);
	case FX_SCALE_STEP: {
		int n = (int)(p->max - p->min) + 1;
		int i = (int)(norm * (float)n);
		if (i >= n) i = n - 1;
		return p->min + (float)i;
	}
	default:
		return p->min + norm * (p->max - p->min);
	}
}

float fx_param_unmap(const fx_param_desc *p, float v)
{
	switch (p->scale) {
	case FX_SCALE_LOG:
		if (v < p->min) v = p->min;
		if (v > p->max) v = p->max;
		return clamp01(logf(v / p->min) / logf(p->max / p->min));
	case FX_SCALE_STEP: {
		int n = (int)(p->max - p->min) + 1;
		return clamp01((v - p->min + 0.5f) / (float)n);
	}
	default:
		return clamp01((v - p->min) / (p->max - p->min));
	}
}

float fx_param_default_norm(const fx_desc *d, int idx)
{
	if (!d || idx < 0 || idx >= d->nparams) return 0.0f;
	return fx_param_unmap(&d->params[idx], d->params[idx].def);
}

uint32_t fx_align8(uint32_t n) { return (n + 7u) & ~7u; }

uint32_t fx_block_bytes(const fx_desc *d)
{
	return d->id == FX_NONE ? 0 : fx_align8(d->state_bytes) + fx_align8(d->mem_bytes);
}
