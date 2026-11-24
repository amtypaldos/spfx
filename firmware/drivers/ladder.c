/* SP-1 FX -- resistor-ladder button decode and fader normalisation (pure C).
 * Thresholds: chattock/sp1-tape-looper @0359372e (MIT), see ladder.h. */
#include "ladder.h"
#include "../controls/controls.h"
#include <string.h>

void ladder_cfg_ladder1(ladder_cfg_t *c, int play_chords)
{
	memset(c, 0, sizeof *c);
	static const int16_t mx[] = { LADDER1_IDLE_MAX, LADDER1_T1_MAX, LADDER1_T2_MAX,
				      LADDER1_T3_MAX, LADDER1_T4_MAX };
	static const uint16_t mk[] = { 0, BTN_BIT(BTN_T1), BTN_BIT(BTN_T2), BTN_BIT(BTN_T3),
				       BTN_BIT(BTN_T4), BTN_BIT(BTN_PLAY) };
	c->nbands = 5;
	for (int i = 0; i < 5; i++) c->max[i] = mx[i];
	for (int i = 0; i < 6; i++) c->mask[i] = mk[i];
	if (play_chords) {
		c->max[5] = LADDER1_PLAY_MAX;
		c->max[6] = LADDER1_PT1_MAX;
		c->max[7] = LADDER1_PT2_MAX;
		c->max[8] = LADDER1_PT3_MAX;
		c->nbands = 9;
		c->mask[6] = BTN_BIT(BTN_PLAY) | BTN_BIT(BTN_T1);
		c->mask[7] = BTN_BIT(BTN_PLAY) | BTN_BIT(BTN_T2);
		c->mask[8] = BTN_BIT(BTN_PLAY) | BTN_BIT(BTN_T3);
		c->mask[9] = BTN_BIT(BTN_PLAY) | BTN_BIT(BTN_T4);
	}
	c->hyst = LADDER_HYST_DEFAULT;
	c->stable_n = LADDER_STABLE_DEFAULT;
}

void ladder_cfg_ladder2(ladder_cfg_t *c)
{
	memset(c, 0, sizeof *c);
	static const int16_t mx[] = { LADDER2_IDLE_MAX, LADDER2_REV_MAX, LADDER2_VOLDN_MAX,
				      LADDER2_FWD_MAX, LADDER2_VOLUP_MAX };
	static const uint16_t mk[] = { 0, BTN_BIT(BTN_REV), BTN_BIT(BTN_VOLDN), BTN_BIT(BTN_FWD),
				       BTN_BIT(BTN_VOLUP),
				       BTN_BIT(BTN_VOLUP) | BTN_BIT(BTN_VOLDN) };
	c->nbands = 5;
	for (int i = 0; i < 5; i++) c->max[i] = mx[i];
	for (int i = 0; i < 6; i++) c->mask[i] = mk[i];
	c->hyst = LADDER_HYST_DEFAULT;
	c->stable_n = LADDER_STABLE_DEFAULT;
}

static int band_of(const ladder_cfg_t *c, int raw)
{
	for (int i = 0; i < c->nbands; i++)
		if (raw < c->max[i]) return i;
	return c->nbands;
}

void ladder_init(ladder_t *l, const ladder_cfg_t *cfg)
{
	memset(l, 0, sizeof *l);
	l->cfg = *cfg;
	if (l->cfg.stable_n == 0) l->cfg.stable_n = 1;
}

uint16_t ladder_decode_raw(const ladder_cfg_t *cfg, int raw)
{
	if (raw < 0) return 0;
	return cfg->mask[band_of(cfg, raw)];
}

uint16_t ladder_update(ladder_t *l, int raw)
{
	if (raw < 0) return ladder_mask(l); /* ADC error: hold */
	int cur = l->band;
	int up = band_of(&l->cfg, raw - l->cfg.hyst);
	int dn = band_of(&l->cfg, raw + l->cfg.hyst);
	int target = up > cur ? up : (dn < cur ? dn : cur);

	if (target == cur) {
		l->cand = (int8_t)cur;
		l->cand_n = 0;
	} else if (target == l->cand && l->cand_n > 0) {
		if (++l->cand_n >= l->cfg.stable_n) {
			l->band = (int8_t)target;
			l->cand_n = 0;
		}
	} else {
		l->cand = (int8_t)target;
		l->cand_n = 1;
		if (l->cfg.stable_n <= 1) {
			l->band = (int8_t)target;
			l->cand_n = 0;
		}
	}
	return ladder_mask(l);
}

uint16_t ladder_mask(const ladder_t *l)
{
	return l->cfg.mask[l->band];
}

/* ------------------------------------------------------------- faders */

void fader_init(fader_t *f, float raw_lo, float raw_hi)
{
	memset(f, 0, sizeof *f);
	f->raw_lo = raw_lo;
	f->raw_hi = raw_hi > raw_lo + 1.0f ? raw_hi : raw_lo + 1.0f;
	f->snap = FADER_SNAP_DEFAULT;
	f->alpha = FADER_ALPHA_DEFAULT;
	f->hyst = FADER_HYST_DEFAULT;
}

float fader_update(fader_t *f, int raw)
{
	if (raw < 0) return f->out; /* ADC error: hold */
	float r = (float)raw;
	if (!f->primed) {
		f->filt = r;
		f->held = r;
		f->primed = 1;
	} else {
		f->filt += f->alpha * (r - f->filt);
		float d = f->filt - f->held;
		if (d >= f->hyst || d <= -f->hyst) f->held = f->filt;
	}
	float x = (f->held - f->raw_lo) / (f->raw_hi - f->raw_lo);
	if (x <= f->snap) x = 0.0f;
	else if (x >= 1.0f - f->snap) x = 1.0f;
	f->out = x;
	return x;
}

void ladder_mask_names(uint16_t mask, char *buf, int len)
{
	static const char *const nm[BTN_COUNT] = { "T1", "T2", "T3", "T4", "PLAY", "FN",
						   "VOL+", "VOL-", "FWD", "REV" };
	int p = 0;
	if (len <= 0) return;
	buf[0] = 0;
	for (int b = 0; b < BTN_COUNT; b++) {
		if (!(mask & BTN_BIT(b))) continue;
		const char *s = nm[b];
		if (p > 0 && p < len - 1) buf[p++] = '+';
		while (*s && p < len - 1) buf[p++] = *s++;
	}
	if (p == 0 && len > 1) buf[p++] = '-';
	buf[p] = 0;
}
