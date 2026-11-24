#include "controller.h"
#include <math.h>
#include <string.h>

static float fx_value(const controller_t *c, int fader)
{
	const engine_config_t *cfg = &c->eng->cfg;
	if (c->mode == UI_EDIT) {
		int id = cfg->fx_id[c->edit_slot];
		const fx_desc *d = fx_get(id);
		if (!d || fader >= d->nparams) return 0.0f;
		return cfg->params[c->edit_slot][id][fader];
	}
	return cfg->wet[fader];
}

static void rearm(controller_t *c)
{
	for (int i = 0; i < 4; i++) takeover_arm(&c->to[i], fx_value(c, i));
}

void controller_resync(controller_t *c) { rearm(c); }

static void changed(controller_t *c, uint32_t now)
{
	c->change_seq++;
	c->last_change_ms = now;
}

void controller_init(controller_t *c, engine_t *e)
{
	memset(c, 0, sizeof *c);
	c->eng = e;
	debounce_init(&c->deb, 2);
	gesture_init(&c->gest);
	c->held_slot = -1;
	c->edit_slot = 0;
	c->last_master_step = -1;
	rearm(c);
}

static int step_effect(int idx, int dir)
{
	int n = FX_COUNT;
	return ((idx + dir) % n + n) % n;
}

static void handle(controller_t *c, const gesture_event_t *ev, uint32_t now)
{
	engine_t *e = c->eng;
	switch (ev->type) {
	case GE_CHAN_DOWN:
		c->held_slot = ev->arg;
		c->held_since = now;
		c->selecting = 0;
		c->cursor[ev->arg] = e->cfg.fx_id[ev->arg];
		break;
	case GE_CHAN_UP:
		if (c->held_slot == ev->arg) c->held_slot = -1;
		c->selecting = 0;
		break;
	case GE_TOGGLE_BYPASS:
		engine_set_bypass(e, ev->arg, !e->cfg.bypass[ev->arg]);
		changed(c, now);
		break;
	case GE_CYCLE_FX: {
		int slot = ev->arg;
		c->selecting = 1;
		c->held_slot = slot;
		int next = step_effect(c->cursor[slot], ev->arg2);
		c->cursor[slot] = next;
		engine_status st = engine_assign(e, slot, next);
		if (st == ENG_OK) {
			c->flash_until = now + 250;
			c->err_until = 0;
			changed(c, now);
			if (c->mode == UI_EDIT && c->edit_slot == slot) rearm(c);
		} else {
			c->err_until = now + 700; /* previous assignment stays in force */
		}
		break;
	}
	case GE_ENTER_EDIT:
		c->mode = UI_EDIT;
		c->edit_slot = ev->arg;
		rearm(c);
		break;
	case GE_FN_TAP:
		if (c->mode == UI_EDIT) {
			c->mode = UI_PERFORM;
			rearm(c);
		}
		break;
	case GE_GLOBAL_BYPASS:
		engine_set_global_bypass(e, !e->cfg.global_bypass);
		changed(c, now);
		break;
	case GE_VOL: {
		float step = 1.0f / MASTER_STEPS;
		float v = e->cfg.master + step * (float)ev->arg;
		/* snap to the step grid so repeated presses are exact */
		v = roundf(v * MASTER_STEPS) / MASTER_STEPS;
		engine_set_master(e, v);
		changed(c, now);
		break;
	}
	case GE_POWER_OFF:
		c->power_off_request = 1;
		break;
	default:
		break;
	}
}

void controller_tick(controller_t *c, uint32_t now, uint16_t buttons, const float fader[4])
{
	uint16_t stable = debounce_update(&c->deb, buttons);
	gesture_event_t ev[GESTURE_MAX_EVENTS];
	int n = gesture_update(&c->gest, stable, now, ev);
	for (int i = 0; i < n; i++) handle(c, &ev[i], now);

	for (int i = 0; i < 4; i++) {
		float v;
		if (!takeover_update(&c->to[i], fader[i], &v)) continue;
		if (c->mode == UI_EDIT) {
			int id = c->eng->cfg.fx_id[c->edit_slot];
			const fx_desc *d = fx_get(id);
			if (!d || i >= d->nparams) continue;
			engine_set_param(c->eng, c->edit_slot, i, v);
		} else {
			engine_set_wet(c->eng, i, v);
		}
		changed(c, now);
	}
}

/* ------------------------------------------------------------------ LEDs */

static uint8_t clamp8(float x) { return x < 0.0f ? 0 : (x > 255.0f ? 255 : (uint8_t)(x + 0.5f)); }

static float tri(uint32_t t, uint32_t period)
{
	float p = (float)(t % period) / (float)period;
	return p < 0.5f ? p * 2.0f : 2.0f - p * 2.0f;
}

/* Brightness for a fader LED that shows `level` (0..1), annotated with pickup state.
 * need=+1 (move fader up): short bright flash on a dim base.
 * need=-1 (move fader down): bright with a short dark gap. */
static uint8_t level_led(float level, int need, uint32_t now)
{
	float base = 20.0f + 235.0f * level;
	if (need == 0) return clamp8(base);
	uint32_t ph = now % 500u;
	if (need > 0) return ph < 100u ? 255 : clamp8(base * 0.25f);
	return ph < 100u ? 0 : clamp8(base < 110.0f ? 110.0f : base);
}

void controller_leds(const controller_t *c, uint32_t now, led_frame_t *o)
{
	const engine_config_t *cfg = &c->eng->cfg;
	memset(o, 0, sizeof *o);

	/* ---- effect selection: a channel is held (long enough, or rocker used) ---- */
	int showing_select = c->held_slot >= 0 && (c->selecting || now - c->held_since > 250u);
	if (showing_select && c->mode == UI_PERFORM) {
		int slot = c->held_slot;
		int idx = c->cursor[slot];
		o->track[slot] = clamp8(110.0f + 145.0f * tri(now, 400));
		/* effect number in binary on the four PLAY-side LEDs; None = dim all */
		if (idx == FX_NONE) {
			for (int i = 0; i < 4; i++) o->play[i] = 14;
		} else {
			for (int i = 0; i < 4; i++) o->play[i] = (idx >> i) & 1 ? 255 : 0;
		}
		if (now < c->flash_until)
			for (int i = 0; i < 4; i++) if (i != slot) o->track[i] = 0;
		if (now < c->err_until) {
			int ph = (int)((now / 80u) & 1u);
			for (int i = 0; i < 4; i++) o->play[i] = ((i & 1) == ph) ? 255 : 0;
		}
		return;
	}
	if (now < c->err_until) { /* error while not showing select, e.g. in edit mode */
		int ph = (int)((now / 80u) & 1u);
		for (int i = 0; i < 4; i++) o->play[i] = ((i & 1) == ph) ? 255 : 0;
	}

	if (c->mode == UI_EDIT) {
		int id = cfg->fx_id[c->edit_slot];
		const fx_desc *d = fx_get(id);
		int np = d ? d->nparams : 0;
		for (int i = 0; i < 4; i++) {
			if (i >= np) { o->track[i] = 0; continue; }
			o->track[i] = level_led(cfg->params[c->edit_slot][id][i], c->to[i].picked ? 0 : c->to[i].need, now);
		}
		if (now >= c->err_until) {
			/* one-hot slot marker, slowly pulsing: unmistakably "not performance mode" */
			o->play[c->edit_slot] = clamp8(90.0f + 165.0f * tri(now, 1200));
		}
		return;
	}

	/* ---- performance ---- */
	if (cfg->global_bypass) {
		/* distinct global pattern: one dim LED walking across the four track LEDs */
		o->track[(now / 250u) % 4u] = 150;
		return;
	}
	for (int i = 0; i < 4; i++) {
		if (cfg->fx_id[i] == FX_NONE || cfg->bypass[i]) {
			o->track[i] = 0;
			continue;
		}
		o->track[i] = level_led(cfg->wet[i], c->to[i].picked ? 0 : c->to[i].need, now);
	}
	/* output level on the four PLAY-side LEDs */
	if (now >= c->err_until) {
		float pk = c->eng->diag.out_peak;
		static const float th[4] = {0.004f, 0.03f, 0.125f, 0.5f};
		for (int i = 0; i < 4; i++) o->play[i] = pk > th[i] ? 120 : 0;
		if (pk > 0.98f) for (int i = 0; i < 4; i++) o->play[i] = 255;
	}
}
