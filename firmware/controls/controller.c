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

void controller_set_presets(controller_t *c, int count, int (*fill)(void *, int, store_data_t *), void *ctx)
{
	c->preset_count = (count > 0 && fill) ? count : 0;
	c->preset_fill = fill;
	c->preset_ctx = ctx;
	c->preset_idx = -1;
	c->preset_pending = -1;
}

static void changed(controller_t *c, uint32_t now)
{
	c->change_seq++;
	c->last_change_ms = now;
}

static void push_ev(controller_t *c, uint8_t kind, int slot, int a, int b, float v)
{
	if (c->ev_count == CTL_EVENT_CAP) { /* drop oldest */
		c->ev_head = (uint8_t)((c->ev_head + 1) % CTL_EVENT_CAP);
		c->ev_count--;
		c->ev_dropped++;
	}
	ctl_event_t *e = &c->ev[(c->ev_head + c->ev_count) % CTL_EVENT_CAP];
	e->kind = kind;
	e->slot = (uint8_t)slot;
	e->a = (uint8_t)a;
	e->b = (uint8_t)b;
	e->v = v;
	c->ev_count++;
}

int controller_pop_event(controller_t *c, ctl_event_t *out)
{
	if (c->ev_count == 0) return 0;
	*out = c->ev[c->ev_head];
	c->ev_head = (uint8_t)((c->ev_head + 1) % CTL_EVENT_CAP);
	c->ev_count--;
	return 1;
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
	c->preset_idx = -1;
	c->preset_pending = -1;
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
		push_ev(c, CTL_EV_BYPASS, ev->arg, e->cfg.bypass[ev->arg], 0, 0.0f);
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
			push_ev(c, CTL_EV_SLOT_FX, slot, next, 0, 0.0f);
			if (c->mode == UI_EDIT && c->edit_slot == slot) rearm(c);
		} else {
			c->err_until = now + 700; /* previous assignment stays in force */
			push_ev(c, CTL_EV_ADMISSION, slot, next, (int)st, 0.0f);
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
		push_ev(c, CTL_EV_GLOBAL_BYPASS, 0, e->cfg.global_bypass, 0, 0.0f);
		break;
	case GE_VOL: {
		/* one 2 dB step on the volume grid (controls/volume.h); repeats while held */
		engine_set_master(e, vol_step(e->cfg.master, ev->arg));
		c->vol_until = now + VOL_SHOW_MS;
		changed(c, now);
		push_ev(c, CTL_EV_MASTER, 0, 0, 0, e->cfg.master);
		break;
	}
	case GE_CYCLE_PRESET: {
		if (c->preset_count <= 0) break;
		int base = c->preset_pending >= 0 ? c->preset_pending : c->preset_idx;
		if (base < 0) base = ev->arg > 0 ? -1 : 0; /* first press: forward -> first preset, back -> last */
		int n = c->preset_count;
		c->preset_pending = ((base + ev->arg) % n + n) % n;
		c->preset_apply_at = now + PRESET_FADE_MS; /* every further press re-arms the fade */
		engine_set_duck(c->eng, 1);
		break;
	}
	case GE_POWER_OFF:
		c->power_off_request = 1;
		break;
	case GE_OUT_LEVEL:
		c->out_level_request = 1;
		break;
	default:
		break;
	}
}

void controller_tick(controller_t *c, uint32_t now, uint16_t buttons, const float fader[4])
{
	if (c->preset_pending >= 0 && (int32_t)(now - c->preset_apply_at) >= 0) {
		store_data_t d;
		int idx = c->preset_pending;
		c->preset_pending = -1;
		if (c->preset_fill && c->preset_fill(c->preset_ctx, idx, &d) == 0) {
			if (c->preset_keep_master) d.cfg.master = c->eng->cfg.master;
			int rejected = engine_apply_config(c->eng, &d.cfg);
			c->mode = UI_PERFORM;
			c->preset_idx = idx;
			c->preset_bpm = d.bpm;
			c->preset_flash_until = now + PRESET_FLASH_MS;
			c->preset_bar_until = now + PRESET_BAR_MS;
			if (rejected > 0) c->err_until = now + 700;
			rearm(c);
			push_ev(c, CTL_EV_PRESET, 0, idx, 0, d.bpm);
			changed(c, now);
		}
		engine_set_duck(c->eng, 0);
	}
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
			float old = c->eng->cfg.params[c->edit_slot][id][i];
			if (v == old) continue; /* pickup at the stored value: nothing changed */
			engine_set_param(c->eng, c->edit_slot, i, v);
			float now_v = c->eng->cfg.params[c->edit_slot][id][i];
			if (now_v != old) push_ev(c, CTL_EV_PARAM, c->edit_slot, i, 0, now_v);
		} else {
			float old = c->eng->cfg.wet[i];
			if (v == old) continue; /* pickup at the stored value: nothing changed */
			engine_set_wet(c->eng, i, v);
			if (c->eng->cfg.wet[i] != old) push_ev(c, CTL_EV_WET, i, 0, 0, c->eng->cfg.wet[i]);
		}
		/* only real changes count (rc1 marked the settings dirty on every pickup, so every
		 * boot wrote a record ~1.5 s after the faders were first read) */
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

static void leds_base(const controller_t *c, uint32_t now, led_frame_t *o)
{
	const engine_config_t *cfg = &c->eng->cfg;
	memset(o, 0, sizeof *o);

	/* ---- effect selection: a channel is held (long enough, or rocker used) ---- */
	int showing_select = c->held_slot >= 0 && (c->selecting || now - c->held_since > 250u);
	if (showing_select && c->mode == UI_PERFORM) {
		int slot = c->held_slot;
		int idx = c->cursor[slot];
		o->track[slot] = clamp8(110.0f + 145.0f * tri(now, 400));
		/* No effect numbers (user decision, ADR-002): the PLAY-side LEDs only confirm that
		 * something happened. A successful change flashes all four; "None" shows a steady dim
		 * row so an emptied slot is recognisable; otherwise they stay dark. */
		if (now < c->flash_until) {
			for (int i = 0; i < 4; i++) o->play[i] = 255;
		} else if (idx == FX_NONE) {
			for (int i = 0; i < 4; i++) o->play[i] = 14;
		}
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
		/* switched off by the overload guard (or all, when it forced bypass): slow 1 Hz
		 * blink, distinct from the 0.5 s takeover cues; it clears when the guard
		 * recovers the slot or the slot gets a new effect */
		if (((c->eng->diag.shed_mask >> i) & 1u) || c->eng->diag.forced_bypass)
			if ((now / 500u) & 1u) o->track[i] = 0;
	}
	/* just-loaded preset: confirm flash on the four PLAY-side LEDs, then a bar showing the place in
	 * the list (1..4 LEDs; a position cue, not a number). Takes priority over the level meter. */
	if (c->preset_idx >= 0 && now < c->preset_bar_until && now >= c->preset_bar_until - PRESET_BAR_MS && now >= c->err_until) {
		if (now < c->preset_flash_until) {
			const uint32_t ph = (now - (c->preset_flash_until - PRESET_FLASH_MS)) / PRESET_FLASH_HALF_MS;
			for (int i = 0; i < 4; i++) o->play[i] = (ph & 1u) ? 0 : 255;
		} else {
			int lit = c->preset_count > 1 ? 1 + (c->preset_idx * 3) / (c->preset_count - 1) : 1;
			for (int i = 0; i < 4; i++) o->play[i] = i < lit ? 255 : 0;
		}
		return;
	}
	/* output level on the four PLAY-side LEDs */
	if (now >= c->err_until) {
		float pk = c->eng->diag.out_peak;
		static const float th[4] = {0.004f, 0.03f, 0.125f, 0.5f};
		for (int i = 0; i < 4; i++) o->play[i] = pk > th[i] ? 120 : 0;
		if (pk > 0.98f) for (int i = 0; i < 4; i++) o->play[i] = 255;
	}
}

/* Volume bar on the four PLAY-side LEDs: each LED is a quarter of the master range (fully lit,
 * partly lit = dimmer, dark); master 0 (mute) = one dim LED blinking. A position cue, no numbers. */
static void vol_bar(float m, uint32_t now, uint8_t play[4])
{
	if (!(m >= VOL_MUTE_BELOW)) {
		for (int i = 0; i < 4; i++) play[i] = 0;
		play[0] = ((now / 150u) & 1u) ? 40 : 0;
		return;
	}
	for (int i = 0; i < 4; i++) {
		float part = m * 4.0f - (float)i; /* 0..1 for this LED */
		if (part >= 1.0f) play[i] = 255;
		else if (part > 0.0f) play[i] = clamp8(30.0f + 225.0f * part);
		else play[i] = 0;
	}
}

void controller_show_out_level(controller_t *c, uint32_t now, int mode)
{
	c->out_level_on = 1;
	c->out_level_shown = mode;
	c->out_level_t0 = now;
}

static void out_level_leds(const controller_t *c, uint32_t now, uint8_t play[4])
{
	const uint32_t dt = now - c->out_level_t0;
	if (dt < OUT_LEVEL_FLASH_MS) {
		for (int i = 0; i < 4; i++) play[i] = ((dt / OUT_LEVEL_FLASH_HALF_MS) & 1u) ? 0 : 255;
		return;
	}
	const int lit = c->out_level_shown == OUT_MODE_LINE ? 3 : c->out_level_shown == OUT_MODE_LOUD ? 2 : 1;
	for (int i = 0; i < 4; i++) play[i] = i < lit ? 255 : 0;
}

int controller_poweroff_countdown(const controller_t *c, uint32_t now)
{
	const gesture_t *g = &c->gest;
	if (!g->fn_down || g->fn_consumed || g->fn_long_sent) return 0;
	uint32_t n = (now - g->fn_t) / GESTURE_FN_STEP_MS;
	return n > 8u ? 8 : (int)n;
}

void controller_fill_leds(int n, led_frame_t *o)
{
	memset(o, 0, sizeof *o);
	for (int i = 0; i < 4; i++) o->play[i] = i < n ? 255 : 0;
	for (int i = 0; i < 4; i++) o->track[i] = i + 4 < n ? 255 : 0;
}

void controller_leds(const controller_t *c, uint32_t now, led_frame_t *o)
{
	leds_base(c, now, o);
	/* power-off fill has priority over everything (from 0.5 s of FN held alone); releasing
	 * FN or using any other button ends it and the normal display returns */
	int cd = controller_poweroff_countdown(c, now);
	if (cd > 0) {
		controller_fill_leds(cd, o);
		return;
	}
	int showing_select = c->held_slot >= 0 && (c->selecting || now - c->held_since > 250u) && c->mode == UI_PERFORM;
	if (c->vol_until != 0u && (int32_t)(c->vol_until - now) > 0 && !showing_select && now >= c->err_until)
		vol_bar(c->eng->cfg.master, now, o->play);
	/* output-level confirmation wins over the volume bar the two VOL presses started */
	if (c->out_level_on && now - c->out_level_t0 < OUT_LEVEL_BAR_MS)
		out_level_leds(c, now, o->play);
}
