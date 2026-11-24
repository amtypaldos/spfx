#include "app_logic.h"
#include <string.h>

/* ---- save/erase scheduling ---- */

void save_sched_init(save_sched_t *s)
{
	memset(s, 0, sizeof *s);
}

static int erase_allowed(const save_sched_t *s, uint32_t now)
{
	return !s->erase_tried || now - s->last_erase_try_ms >= SAVE_ERASE_RETRY_MS;
}

save_action_t save_sched_next(save_sched_t *s, uint32_t now, int due, int needs_erase,
			      int spare_dirty, int quiet)
{
	if (due) s->pending = 1;
	if (s->pending) {
		if (!needs_erase) {
			s->pending = 0;
			return SAVE_WRITE;
		}
		if (quiet && erase_allowed(s, now)) {
			s->pending = 0;
			return SAVE_WRITE_WITH_ERASE;
		}
		s->deferred++;
		return SAVE_IDLE;
	}
	if (spare_dirty && quiet && erase_allowed(s, now)) return SAVE_ERASE_SPARE;
	return SAVE_IDLE;
}

void save_sched_erased(save_sched_t *s, uint32_t now)
{
	s->erase_tried = 1;
	s->last_erase_try_ms = now;
}

/* ---- event-based saving ---- */

void saveev_init(saveev_t *s, uint32_t now)
{
	memset(s, 0, sizeof *s);
	s->last_save_ms = now;
}

void saveev_tweak(saveev_t *s, uint32_t now)
{
	(void)now;
	s->dirty = 1;
}

void saveev_event(saveev_t *s, uint32_t now)
{
	s->dirty = 1;
	s->event = 1;
	s->event_ms = now; /* a burst coalesces: the settle time counts from the last event */
}

int saveev_due(saveev_t *s, uint32_t now, int load_ok)
{
	if (!s->dirty) return 0;
	int want = (s->event && now - s->event_ms >= SAVE_EVENT_SETTLE_MS) ||
		   now - s->last_save_ms >= SAVE_SAFETY_MS;
	if (!want) return 0;
	if (!load_ok) { s->deferred_load++; return 0; }
	return 1;
}

void saveev_saved(saveev_t *s, uint32_t now)
{
	s->dirty = 0;
	s->event = 0;
	s->last_save_ms = now;
}

/* ---- console mode ---- */

void conmode_init(con_mode_t *m)
{
	memset(m, 0, sizeof *m);
}

int conmode_update(con_mode_t *m, int dtr, uint32_t rx_frames)
{
	if (dtr && !m->open) {
		m->open = 1;
		m->binary = 0;
		m->mark = rx_frames;
	} else if (!dtr && m->open) {
		m->open = 0;
		m->binary = 0;
		m->mark = rx_frames;
	}
	if (!m->open && !m->binary) {
		/* frames while the port looks closed: a host that never raises DTR still works */
		if (rx_frames != m->mark) {
			m->binary = 1;
			m->mark = rx_frames;
			m->switches++;
			return 1;
		}
		return 0;
	}
	if (!m->binary && rx_frames != m->mark) {
		m->binary = 1;
		m->switches++;
		return 1;
	}
	return 0;
}

int conmode_text_ok(const con_mode_t *m)
{
	return m->open && !m->binary;
}

int conmode_binary_ok(const con_mode_t *m)
{
	return m->binary;
}

/* ---- TX queue ---- */

void txq_init(txq_t *q, uint8_t *buf, uint32_t cap)
{
	q->buf = buf;
	q->cap = cap;
	q->head = q->tail = 0;
	q->max_used = 0;
}

uint32_t txq_used(const txq_t *q)
{
	return q->head - q->tail;
}

uint32_t txq_space(const txq_t *q)
{
	return q->cap - txq_used(q);
}

int txq_put(txq_t *q, const uint8_t *p, uint32_t n)
{
	if (n == 0u || n > txq_space(q)) return 0;
	uint32_t h = q->head;
	for (uint32_t i = 0; i < n; i++) q->buf[(h + i) % q->cap] = p[i];
	q->head = h + n; /* publish after the data (single core; volatile orders the store) */
	uint32_t u = txq_used(q);
	if (u > q->max_used) q->max_used = u;
	return 1;
}

uint32_t txq_peek(const txq_t *q, const uint8_t **p, uint32_t max)
{
	uint32_t used = txq_used(q);
	uint32_t off = q->tail % q->cap;
	uint32_t n = q->cap - off; /* contiguous up to the end of the buffer */
	if (n > used) n = used;
	if (n > max) n = max;
	*p = q->buf + off;
	return n;
}

void txq_consume(txq_t *q, uint32_t n)
{
	uint32_t used = txq_used(q);
	q->tail += n > used ? used : n;
}

/* ---- console TX: mode + queue ---- */

void contx_init(contx_t *c, uint8_t *buf, uint32_t cap)
{
	memset(c, 0, sizeof *c);
	conmode_init(&c->m);
	txq_init(&c->q, buf, cap);
}

static void try_sep(contx_t *c)
{
	static const uint8_t z = 0;
	if (c->sep_pending && txq_put(&c->q, &z, 1)) {
		c->sep_pending = 0;
		c->sep_sent++;
	}
}

void contx_poll(contx_t *c, int dtr, uint32_t rx_frames)
{
	if (conmode_update(&c->m, dtr, rx_frames)) c->sep_pending = 1;
	if (!conmode_binary_ok(&c->m)) c->sep_pending = 0;
	try_sep(c);
}

int contx_text(contx_t *c, const uint8_t *s, uint32_t n)
{
	if (!conmode_text_ok(&c->m)) return 0;
	if (txq_put(&c->q, s, n)) return 1;
	c->text_drops++;
	return 0;
}

int contx_frame(contx_t *c, const uint8_t *f, uint32_t n)
{
	if (!conmode_binary_ok(&c->m)) return 0;
	try_sep(c);
	if (c->sep_pending || !txq_put(&c->q, f, n)) {
		c->frame_drops++;
		return 0;
	}
	c->frames_sent++;
	return 1;
}

/* ---- FN hold fill / power-on gate ---- */

int holdfill_count(uint32_t held_ms)
{
	uint32_t n = held_ms / HOLD_STEP_MS;
	return n > 8u ? 8 : (int)n;
}

boot_mode_t boot_decide(uint32_t rr, int crash_pending, int poweroff_flag)
{
	if (poweroff_flag) return BOOT_STANDBY;                 /* our own plugged power off */
	if (rr & (RR_DOG | RR_LOCKUP)) return BOOT_NOW;         /* crash recovery */
	if ((rr & RR_SREQ) && crash_pending) return BOOT_NOW;   /* fatal handler's reset */
	if (rr & (RR_RESETPIN | RR_SREQ)) return BOOT_NOW;
	if (rr & (RR_OFF | RR_VBUS)) return BOOT_STANDBY;       /* FN wake, USB plugged in */
	return BOOT_NOW;                                        /* power-on / flash / unknown */
}

sb_action_t standby_step(int fn_down, int usb_power, uint32_t held_ms)
{
	if (fn_down) return held_ms >= HOLD_MS ? SB_BOOT : SB_WAIT;
	return usb_power ? SB_WAIT : SB_OFF;
}

/* ---- SAADC scan state machine ---- */

void scanfsm_init(scanfsm_t *f)
{
	memset(f, 0, sizeof *f);
}

scan_act_t scanfsm_step(scanfsm_t *f, uint32_t now, int ended)
{
	if (!f->running) {
		f->running = 1;
		f->t0 = now;
		f->starts++;
		return SCAN_ACT_START;
	}
	if (ended) {
		f->ok++;
		f->last_ok_ms = now;
		f->have_ok = 1;
		f->t0 = now;
		f->starts++;
		return SCAN_ACT_COLLECT_START;
	}
	if (now - f->t0 >= SCAN_TIMEOUT_MS) {
		f->timeouts++;
		f->t0 = now;
		f->starts++;
		return SCAN_ACT_ABORT_START;
	}
	return SCAN_ACT_NONE;
}

uint32_t scanfsm_age(const scanfsm_t *f, uint32_t now)
{
	return f->have_ok ? now - f->last_ok_ms : 0xFFFFFFFFu;
}

/* ---- battery fuel gauge ---- */

int batt_mv_from_raw(int raw)
{
	if (raw < 0) return -1;
	/* raw * 3600 mV / 4096 at the pin, times the divider ratio */
	return (int)(((int64_t)raw * 3600 * BATT_DIVIDER_X1000) / (4096 * 1000));
}

int batt_pct_from_mv(int mv)
{
	static const int t[][2] = {
		{3300, 0}, {3450, 5}, {3600, 10}, {3680, 20}, {3730, 30}, {3770, 40},
		{3800, 50}, {3840, 60}, {3900, 70}, {3960, 80}, {4050, 90}, {4200, 100},
	};
	const int n = (int)(sizeof t / sizeof t[0]);
	if (mv <= t[0][0]) return 0;
	if (mv >= t[n - 1][0]) return 100;
	for (int i = 1; i < n; i++)
		if (mv <= t[i][0])
			return t[i - 1][1] + (mv - t[i - 1][0]) * (t[i][1] - t[i - 1][1]) / (t[i][0] - t[i - 1][0]);
	return 100;
}

void batt_init(batt_t *b)
{
	memset(b, 0, sizeof *b);
	b->shown = -1;
}

void batt_feed(batt_t *b, int raw, int charging, int done, uint32_t now)
{
	int mv = batt_mv_from_raw(raw);
	if (mv < 0) return;
	if (!b->have) { b->mv = (float)mv; b->have = 1; }
	else b->mv += ((float)mv - b->mv) * (1.0f / 200.0f); /* ~2 s at 10 ms per reading */
	int est = (int)b->mv - (charging ? BATT_CHARGE_OFFSET_MV : 0);
	int pct = batt_pct_from_mv(est);
	if (charging && pct > BATT_CHARGING_CAP) pct = BATT_CHARGING_CAP;
	if (done) pct = 100;                       /* the charger says full */
	if (b->shown < 0) { b->shown = pct; b->last_upd = now; return; }
	if (now - b->last_upd < BATT_UPDATE_MS) return;
	int d = pct - b->shown;
	if (d >= BATT_HYST_PCT || d <= -BATT_HYST_PCT || pct == 100 || (b->shown == 100 && pct < 100 && d <= -BATT_HYST_PCT)) {
		b->shown = pct;
	}
	if (b->shown == 100 && !done && charging) b->shown = BATT_CHARGING_CAP; /* never 100 % while charging */
	b->last_upd = now;
}

int batt_shown(const batt_t *b)
{
	return b->shown;
}

void batt_leds(int pct, int charging, uint32_t now, uint8_t play[4], uint8_t track[4])
{
	int n = pct <= 0 ? 1 : (pct * 8 + 99) / 100; /* 1..8 */
	if (n < 1) n = 1;
	if (n > 8) n = 8;
	const uint32_t ph = now % 2400u;
	const uint32_t tri = ph < 1200u ? ph : 2400u - ph;
	for (int i = 0; i < 8; i++) {
		uint8_t v = 0;
		if (i < n) v = 60;
		if (i == n - 1 && charging) v = (uint8_t)(8u + tri * 112u / 1200u); /* slow pulse */
		if (i < 4) play[i] = v; else track[i - 4] = v;
	}
}

/* ---- quiet detector ---- */

void quiet_feed(quiet_t *q, float in_peak, float out_peak)
{
	/* NaN peaks count as loud */
	if (in_peak < QUIET_PEAK && out_peak < QUIET_PEAK) {
		if (q->silent_blocks < 0xFFFFFFFFu) q->silent_blocks++;
	} else {
		q->silent_blocks = 0;
	}
}

int quiet_ok(const quiet_t *q, uint32_t blocks)
{
	return q->silent_blocks >= blocks;
}

/* ---- rate limiter ---- */

void ratelimit_init(ratelimit_t *r, uint32_t per_s, uint32_t burst, uint32_t now)
{
	memset(r, 0, sizeof *r);
	r->per_s = per_s;
	r->burst = burst;
	r->tokens_x1000 = burst * 1000u;
	r->last_ms = now;
}

int ratelimit_take(ratelimit_t *r, uint32_t now)
{
	uint32_t dt = now - r->last_ms;
	r->last_ms = now;
	if (dt > 100000u) dt = 100000u;
	uint32_t add = dt * r->per_s; /* tokens x1000 per ms = per_s */
	uint32_t cap = r->burst * 1000u;
	r->tokens_x1000 = (cap - r->tokens_x1000 < add) ? cap : r->tokens_x1000 + add;
	if (r->tokens_x1000 >= 1000u) {
		r->tokens_x1000 -= 1000u;
		return 1;
	}
	r->dropped++;
	return 0;
}
