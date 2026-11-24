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
