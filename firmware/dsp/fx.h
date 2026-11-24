/* SP-1 FX — effect interface.
 *
 * Every effect is a table of function pointers plus a descriptor. State and
 * delay memory are placed by the engine in one static pool (no malloc).
 * Parameters are exchanged as normalised values in [0,1]; the descriptor
 * says how a normalised value maps to a real unit. */
#ifndef SP1FX_FX_H
#define SP1FX_FX_H

#include <stdint.h>

#define FX_SAMPLE_RATE 48000.0f
#define FX_MAX_BLOCK 64 /* max frames per process() call */
#define FX_MAX_PARAMS 4

typedef enum { FX_SCALE_LIN = 0, FX_SCALE_LOG = 1, FX_SCALE_STEP = 2 } fx_scale;

/* How the slot's wet/dry level is applied */
typedef enum {
	FX_MIX_CROSSFADE = 0, /* out = dry*(1-m) + wet*m   (filter, drive, modulation...) */
	FX_MIX_SEND = 1       /* out = dry + wet*m         (delay, reverb; input is gated on bypass so tails ring out) */
} fx_mix_mode;

typedef struct {
	const char *name;
	const char *unit;   /* "Hz", "ms", "%", "dB", "" ... */
	float min, max, def; /* real units; for FX_SCALE_STEP integers min..max */
	uint8_t scale;
	const char *labels; /* comma separated names for STEP params, else NULL */
} fx_param_desc;

typedef struct fx_desc {
	uint8_t id;
	const char *name;
	uint8_t nparams;
	const fx_param_desc *params;
	uint32_t state_bytes;   /* sizeof(state struct) */
	uint32_t mem_bytes;     /* extra delay memory, sized for 48 kHz */
	uint16_t cpu_cycles;    /* declared estimate, cycles per stereo frame on Cortex-M4F */
	uint32_t tail_ms;       /* time the effect keeps ringing after input stops */
	uint8_t mix_mode;
	uint8_t stereo_independent; /* 1 if L and R never interact (for tests) */
	void (*init)(void *st, void *mem, float sr);
	void (*reset)(void *st);
	void (*set_param)(void *st, int idx, float norm);
	void (*process)(void *st, float *l, float *r, int n);
} fx_desc;

typedef enum {
	FX_NONE = 0,
	FX_FILTER,
	FX_CHORUS,
	FX_DELAY,
	FX_REVERB,
	FX_SATURATION,
	FX_FLANGER,
	FX_BITCRUSHER,
	FX_TREMOLO,
	FX_COMPRESSOR,
	FX_TAPE_WOBBLE,
	FX_COUNT
} fx_id;

const fx_desc *fx_get(int id);              /* NULL if id out of range */
float fx_param_map(const fx_param_desc *p, float norm);   /* norm -> real units */
float fx_param_unmap(const fx_param_desc *p, float value); /* real units -> norm */
float fx_param_default_norm(const fx_desc *d, int idx);
uint32_t fx_align8(uint32_t n);
uint32_t fx_block_bytes(const fx_desc *d); /* state (aligned) + memory */

#endif
