#include "controls.h"
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------- debounce */

void debounce_init(debounce_t *d, uint8_t n_required)
{
	memset(d, 0, sizeof *d);
	d->n_required = n_required ? n_required : 1;
}

uint16_t debounce_update(debounce_t *d, uint16_t raw)
{
	for (int b = 0; b < BTN_COUNT; b++) {
		uint16_t bit = BTN_BIT(b);
		if ((raw & bit) != (d->stable & bit)) {
			if (++d->cnt[b] >= d->n_required) {
				d->stable = (uint16_t)((d->stable & ~bit) | (raw & bit));
				d->cnt[b] = 0;
			}
		} else {
			d->cnt[b] = 0;
		}
	}
	return d->stable;
}

/* -------------------------------------------------------------- gestures */

void gesture_init(gesture_t *g)
{
	memset(g, 0, sizeof *g);
	g->last_chan = -1;
	g->rk_dir = 0;
	g->rk_slot = -1;
}

static void emit(gesture_event_t *ev, int *n, int type, int a, int a2)
{
	if (*n >= GESTURE_MAX_EVENTS) return;
	ev[*n].type = (uint8_t)type;
	ev[*n].arg = (int8_t)a;
	ev[*n].arg2 = (int8_t)a2;
	(*n)++;
}

static int any_channel_down(const gesture_t *g)
{
	for (int c = 0; c < 4; c++)
		if (g->chan_down[c]) return 1;
	return 0;
}

static void pick_last_chan(gesture_t *g)
{
	g->last_chan = -1;
	for (int c = 0; c < 4; c++)
		if (g->chan_down[c]) g->last_chan = (int8_t)c;
}

int gesture_update(gesture_t *g, uint16_t mask, uint32_t now, gesture_event_t *ev)
{
	int n = 0;
	uint16_t pressed = (uint16_t)(mask & ~g->prev);
	uint16_t released = (uint16_t)(g->prev & ~mask);

	/* ---- releases first, so a release+press in one tick is never merged ---- */
	for (int c = 0; c < 4; c++) {
		if (!(released & BTN_BIT(c))) continue;
		if (!g->chan_down[c]) continue;
		g->chan_down[c] = 0;
		emit(ev, &n, GE_CHAN_UP, c, 0);
		if (!g->chan_consumed[c] && (now - g->chan_t[c]) < GESTURE_TAP_MAX_MS)
			emit(ev, &n, GE_TOGGLE_BYPASS, c, 0);
		if (g->rk_slot == c) { g->rk_dir = 0; g->rk_slot = -1; }
		if (g->last_chan == c) pick_last_chan(g);
	}
	if (released & BTN_BIT(BTN_FN)) {
		if (g->fn_down && !g->fn_consumed && !g->fn_long_sent && (now - g->fn_t) < GESTURE_TAP_MAX_MS)
			emit(ev, &n, GE_FN_TAP, 0, 0);
		g->fn_down = 0;
	}
	if (released & BTN_BIT(BTN_PLAY)) {
		if (g->play_down && !g->play_consumed && (now - g->play_t) < GESTURE_TAP_MAX_MS)
			emit(ev, &n, GE_GLOBAL_BYPASS, 0, 0);
		g->play_down = 0;
	}
	if ((released & BTN_BIT(BTN_FWD)) && g->rk_dir == 1) g->rk_dir = 0;
	if ((released & BTN_BIT(BTN_REV)) && g->rk_dir == -1) g->rk_dir = 0;
	if ((released & BTN_BIT(BTN_VOLUP)) && g->vol_dir == 1) g->vol_dir = 0;
	if ((released & BTN_BIT(BTN_VOLDN)) && g->vol_dir == -1) g->vol_dir = 0;

	/* ---- presses ---- */
	if (pressed & BTN_BIT(BTN_FN)) {
		g->fn_down = 1;
		g->fn_t = now;
		g->fn_long_sent = 0;
		/* FN arriving while something else is held is a different gesture: ignore its tap */
		g->fn_consumed = (uint8_t)(any_channel_down(g) || g->play_down);
		if (g->play_down) g->play_consumed = 1;
	}
	for (int c = 0; c < 4; c++) {
		if (!(pressed & BTN_BIT(c))) continue;
		g->chan_down[c] = 1;
		g->chan_t[c] = now;
		g->chan_consumed[c] = 0;
		if (g->fn_down) {
			/* FN + channel */
			g->chan_consumed[c] = 1;
			g->fn_consumed = 1;
			emit(ev, &n, GE_ENTER_EDIT, c, 0);
		} else {
			/* two channels together = chord, reserved: neither toggles */
			for (int o = 0; o < 4; o++) {
				if (o != c && g->chan_down[o]) {
					g->chan_consumed[o] = 1;
					g->chan_consumed[c] = 1;
				}
			}
			if (g->play_down) { g->play_consumed = 1; g->chan_consumed[c] = 1; }
		}
		g->last_chan = (int8_t)c;
		emit(ev, &n, GE_CHAN_DOWN, c, 0);
	}
	if (pressed & BTN_BIT(BTN_PLAY)) {
		g->play_down = 1;
		g->play_t = now;
		g->play_consumed = (uint8_t)(g->fn_down || any_channel_down(g));
		if (g->fn_down) g->fn_consumed = 1;
	}
	for (int d = 0; d < 2; d++) {
		int dir = d == 0 ? 1 : -1;
		uint16_t bit = BTN_BIT(d == 0 ? BTN_FWD : BTN_REV);
		if (!(pressed & bit)) continue;
		/* rocker only acts on a channel that was already held */
		if (!g->fn_down && g->last_chan >= 0 && g->chan_down[g->last_chan]) {
			int slot = g->last_chan;
			g->chan_consumed[slot] = 1;
			g->rk_dir = (int8_t)dir;
			g->rk_slot = (int8_t)slot;
			g->rk_next = now + GESTURE_ROCKER_DELAY_MS;
			emit(ev, &n, GE_CYCLE_FX, slot, dir);
		}
	}
	for (int d = 0; d < 2; d++) {
		int dir = d == 0 ? 1 : -1;
		uint16_t bit = BTN_BIT(d == 0 ? BTN_VOLUP : BTN_VOLDN);
		if (!(pressed & bit)) continue;
		g->vol_dir = (int8_t)dir;
		g->vol_next = now + GESTURE_VOL_DELAY_MS;
		emit(ev, &n, GE_VOL, dir, 0);
	}

	/* ---- timed behaviour ---- */
	if (g->rk_dir != 0 && g->rk_slot >= 0 && g->chan_down[g->rk_slot] && now >= g->rk_next) {
		emit(ev, &n, GE_CYCLE_FX, g->rk_slot, g->rk_dir);
		g->rk_next += GESTURE_ROCKER_REPEAT_MS;
		if (g->rk_next <= now) g->rk_next = now + GESTURE_ROCKER_REPEAT_MS;
	}
	if (g->vol_dir != 0 && now >= g->vol_next) {
		emit(ev, &n, GE_VOL, g->vol_dir, 0);
		g->vol_next += GESTURE_VOL_REPEAT_MS;
		if (g->vol_next <= now) g->vol_next = now + GESTURE_VOL_REPEAT_MS;
	}
	if (g->fn_down && !g->fn_consumed && !g->fn_long_sent && (now - g->fn_t) >= GESTURE_FN_LONG_MS) {
		g->fn_long_sent = 1;
		emit(ev, &n, GE_POWER_OFF, 0, 0);
	}

	g->prev = mask;
	return n;
}

/* ----------------------------------------------------------- soft takeover */

void takeover_arm(takeover_t *t, float v)
{
	t->value = v;
	t->picked = 0;
	t->have_prev = 0;
	t->prev_diff = 0.0f;
	t->need = 0;
}

int takeover_update(takeover_t *t, float phys, float *out)
{
	if (!(phys == phys)) return 0;
	if (t->picked) {
		if (fabsf(phys - t->last_phys) < TAKEOVER_DEADBAND) return 0;
		t->last_phys = phys;
		t->value = phys;
		*out = phys;
		return 1;
	}
	float diff = phys - t->value;
	int crossed = t->have_prev && ((t->prev_diff < 0.0f && diff >= 0.0f) || (t->prev_diff > 0.0f && diff <= 0.0f));
	if (fabsf(diff) <= TAKEOVER_WINDOW || crossed) {
		t->picked = 1;
		t->need = 0;
		t->last_phys = phys;
		t->value = phys;
		*out = phys;
		return 1;
	}
	t->need = diff < 0.0f ? 1 : -1;
	t->prev_diff = diff;
	t->have_prev = 1;
	return 0;
}
