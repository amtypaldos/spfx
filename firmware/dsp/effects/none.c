#include "../fx.h"

static void n_init(void *st, void *mem, float sr) { (void)st; (void)mem; (void)sr; }
static void n_reset(void *st) { (void)st; }
static void n_set(void *st, int i, float v) { (void)st; (void)i; (void)v; }
static void n_proc(void *st, float *l, float *r, int n) { (void)st; (void)l; (void)r; (void)n; }

const fx_desc fx_desc_none = {
	FX_NONE, "None", 0, 0, 0, 0, 0, 0, FX_MIX_CROSSFADE, 1,
	n_init, n_reset, n_set, n_proc,
};
