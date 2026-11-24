#include "protocol.h"
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------ primitives */

uint16_t proto_crc16(const uint8_t *p, size_t n)
{
	uint16_t crc = 0xFFFFu;
	while (n--) {
		crc ^= (uint16_t)(*p++ << 8);
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
	}
	return crc;
}

/* Mirrors companion/js/protocol.js: a full 254-byte block always starts a new block,
 * so frames are byte-identical between the two implementations. */
size_t proto_cobs_encode(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
	if (cap < 1) return 0;
	size_t o = 1, code_idx = 0;
	uint8_t code = 1;
	for (size_t i = 0; i < n; i++) {
		uint8_t b = in[i];
		if (b == 0) {
			out[code_idx] = code;
			if (o >= cap) return 0;
			code_idx = o++;
			code = 1;
		} else {
			if (o >= cap) return 0;
			out[o++] = b;
			code++;
			if (code == 0xFF) {
				out[code_idx] = code;
				if (o >= cap) return 0;
				code_idx = o++;
				code = 1;
			}
		}
	}
	out[code_idx] = code;
	return o;
}

int proto_cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *out_len)
{
	size_t i = 0, o = 0;
	while (i < n) {
		uint8_t code = in[i++];
		if (code == 0) return -1;
		size_t end = i + code - 1u;
		if (end > n) return -1;
		for (; i < end; i++) {
			if (in[i] == 0 || o >= cap) return -1;
			out[o++] = in[i];
		}
		if (code != 0xFF && i < n) {
			if (o >= cap) return -1;
			out[o++] = 0;
		}
	}
	*out_len = o;
	return 0;
}

size_t proto_build_frame(uint8_t type, uint8_t seq, const uint8_t *payload, size_t n,
                         uint8_t *out, size_t cap)
{
	uint8_t body[PROTO_MAX_BODY];
	if (n > PROTO_MAX_PAYLOAD) return 0;
	body[0] = PROTO_VERSION;
	body[1] = type;
	body[2] = seq;
	if (n) memcpy(body + 3, payload, n);
	uint16_t crc = proto_crc16(body, 3 + n);
	body[3 + n] = (uint8_t)crc;
	body[4 + n] = (uint8_t)(crc >> 8);
	if (cap < 2) return 0;
	size_t m = proto_cobs_encode(body, n + 5, out, cap - 1);
	if (m == 0) return 0;
	out[m] = 0;
	return m + 1;
}

int proto_parse_frame(const uint8_t *enc, size_t n, uint8_t *body, proto_frame_t *f)
{
	size_t len = 0;
	if (proto_cobs_decode(enc, n, body, PROTO_MAX_BODY, &len) != 0) return -1;
	if (len < 5) return -1;
	uint16_t want = (uint16_t)(body[len - 2] | (body[len - 1] << 8));
	if (proto_crc16(body, len - 2) != want) return -1;
	if (body[0] != PROTO_VERSION) return -1;
	f->type = body[1];
	f->seq = body[2];
	f->payload = body + 3;
	f->len = (uint16_t)(len - 5);
	return 0;
}

/* ------------------------------------------------------------- writer */

typedef struct {
	uint8_t *p;
	size_t n, cap;
	int over;
} wr_t;

static void w8(wr_t *w, uint32_t v)
{
	if (w->n >= w->cap) { w->over = 1; return; }
	w->p[w->n++] = (uint8_t)v;
}
static void w16(wr_t *w, uint32_t v) { w8(w, v); w8(w, v >> 8); }
static void w32(wr_t *w, uint32_t v) { w16(w, v); w16(w, v >> 16); }
static void wf32(wr_t *w, float f)
{
	uint32_t u;
	memcpy(&u, &f, 4); /* host and Cortex-M4 are both IEEE-754 */
	w32(w, u);
}
static void wstr(wr_t *w, const char *s)
{
	size_t n = s ? strlen(s) : 0;
	if (n > 255) n = 255;
	w8(w, (uint32_t)n);
	for (size_t i = 0; i < n; i++) w8(w, (uint8_t)s[i]);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint16_t q16(float x)
{
	if (!(x >= 0.0f)) return 0;
	if (x > 1.0f) x = 1.0f;
	return (uint16_t)(x * 65535.0f + 0.5f);
}
static float uq16(uint16_t v) { return (float)v / 65535.0f; }

/* -------------------------------------------------------------- sending */

static void send(proto_t *p, uint8_t type, uint8_t seq, const uint8_t *pay, size_t n)
{
	size_t m = proto_build_frame(type, seq, pay, n, p->txbuf, sizeof p->txbuf);
	if (m && p->c.tx) p->c.tx(p->c.ctx, p->txbuf, m);
}

static void send_result(proto_t *p, uint8_t seq, uint8_t req, uint8_t code)
{
	uint8_t b[2] = {req, code};
	send(p, PT_RESULT, seq, b, 2);
}

/* ------------------------------------------------------------ lifecycle */

void proto_init(proto_t *p, const proto_config_t *cfg)
{
	memset(p, 0, sizeof *p);
	p->c = *cfg;
	if (!p->c.fw) p->c.fw = "sp1fx";
	if (!p->c.block) p->c.block = FX_MAX_BLOCK;
	p->bpm = (cfg->bpm >= 30.0f && cfg->bpm <= 300.0f) ? cfg->bpm : 120.0f;
}

/* ------------------------------------------------------------ requests */

static void host_changed(proto_t *p)
{
	p->dirty = 1;
	if (p->c.deb) store_debounce_mark(p->c.deb, p->now_ms);
	if (p->c.ctl) controller_resync(p->c.ctl);
}

static void build_hello(proto_t *p, wr_t *w)
{
	const engine_t *e = p->c.eng;
	size_t fl = strlen(p->c.fw);
	if (fl > 64) fl = 64;
	w8(w, PROTO_VERSION);
	w8(w, (uint32_t)fl);
	for (size_t i = 0; i < fl; i++) w8(w, (uint8_t)p->c.fw[i]);
	w8(w, FX_COUNT);
	w32(w, (uint32_t)lrintf(e->sr));
	w16(w, p->c.block);
	w32(w, e->pool_bytes);
	w16(w, e->cpu_budget > 0xFFFFu ? 0xFFFFu : e->cpu_budget);
}

static void build_fx_desc(const fx_desc *d, wr_t *w)
{
	w8(w, d->id);
	wstr(w, d->name);
	w8(w, d->nparams);
	w16(w, d->cpu_cycles);
	w32(w, d->mem_bytes);
	w32(w, d->tail_ms);
	w8(w, d->mix_mode);
	for (int i = 0; i < d->nparams; i++) {
		const fx_param_desc *q = &d->params[i];
		wstr(w, q->name);
		wstr(w, q->unit);
		wf32(w, q->min);
		wf32(w, q->max);
		wf32(w, q->def);
		w8(w, q->scale);
		wstr(w, q->scale == FX_SCALE_STEP ? q->labels : "");
	}
}

static size_t build_state(proto_t *p, uint8_t *out, size_t cap)
{
	store_data_t d;
	d.cfg = p->c.eng->cfg;
	d.bpm = p->bpm;
	uint16_t n = store_encode(&d, out, (uint16_t)(cap - 2));
	if (n == 0) return 0;
	out[n] = p->c.ctl ? (uint8_t)p->c.ctl->mode : 0;
	out[n + 1] = p->c.ctl ? (uint8_t)p->c.ctl->edit_slot : 0;
	return (size_t)n + 2;
}

static uint8_t map_status(engine_status s)
{
	switch (s) {
	case ENG_OK: return PR_OK;
	case ENG_ERR_MEM: return PR_NO_MEMORY;
	case ENG_ERR_CPU: return PR_NO_CPU;
	default: return PR_BAD_ARG;
	}
}

static void drop_pending(proto_fader_ev_t *f) { f->pending = 0; }

static void handle(proto_t *p, const proto_frame_t *f)
{
	engine_t *e = p->c.eng;
	const uint8_t *a = f->payload;
	uint16_t n = f->len;
	uint8_t code = PR_OK;
	wr_t w = {p->pay, 0, sizeof p->pay, 0};

#define NEED(k) do { if (n != (k)) { send_result(p, f->seq, f->type, PR_BAD_ARG); return; } } while (0)
	switch (f->type) {
	case PT_HELLO:
		NEED(0);
		build_hello(p, &w);
		send(p, PT_HELLO_RSP, f->seq, w.p, w.n);
		return;
	case PT_GET_FX: {
		NEED(1);
		const fx_desc *d = fx_get(a[0]);
		if (!d) { send_result(p, f->seq, f->type, PR_BAD_ARG); return; }
		build_fx_desc(d, &w);
		if (w.over) { send_result(p, f->seq, f->type, PR_BAD_ARG); return; }
		send(p, PT_FX_DESC, f->seq, w.p, w.n);
		return;
	}
	case PT_GET_STATE: {
		NEED(0);
		size_t m = build_state(p, p->pay, sizeof p->pay);
		if (!m) { send_result(p, f->seq, f->type, PR_BUSY); return; }
		send(p, PT_STATE, f->seq, p->pay, m);
		return;
	}
	case PT_SET_SLOT_FX:
		NEED(2);
		if (a[0] >= ENGINE_SLOTS || a[1] >= FX_COUNT) { code = PR_BAD_ARG; break; }
		code = map_status(engine_assign(e, a[0], a[1]));
		if (code == PR_OK)
			for (int i = 0; i < FX_MAX_PARAMS; i++) drop_pending(&p->ev_param[a[0]][i]);
		break;
	case PT_SET_PARAM: {
		NEED(4);
		const fx_desc *d = a[0] < ENGINE_SLOTS ? fx_get(e->cfg.fx_id[a[0]]) : NULL;
		if (!d || a[1] >= d->nparams) { code = PR_BAD_ARG; break; }
		engine_set_param(e, a[0], a[1], uq16(rd16(a + 2)));
		drop_pending(&p->ev_param[a[0]][a[1]]);
		break;
	}
	case PT_SET_WET:
		NEED(3);
		if (a[0] >= ENGINE_SLOTS) { code = PR_BAD_ARG; break; }
		engine_set_wet(e, a[0], uq16(rd16(a + 1)));
		drop_pending(&p->ev_wet[a[0]]);
		break;
	case PT_SET_BYPASS:
		NEED(2);
		if (a[0] >= ENGINE_SLOTS) { code = PR_BAD_ARG; break; }
		engine_set_bypass(e, a[0], a[1] != 0);
		break;
	case PT_SET_GLOBAL_BYPASS:
		NEED(1);
		engine_set_global_bypass(e, a[0] != 0);
		break;
	case PT_SET_MASTER:
		NEED(2);
		engine_set_master(e, uq16(rd16(a)));
		break;
	case PT_SET_INPUT_GAIN: {
		NEED(2);
		int16_t cdb = (int16_t)rd16(a);
		if (cdb < -1200 || cdb > 1200) { code = PR_BAD_ARG; break; }
		engine_set_input_gain_db(e, (float)cdb / 100.0f);
		break;
	}
	case PT_SET_ORDER:
		NEED(4);
		if (engine_set_order(e, a) != 0) code = PR_BAD_ARG;
		break;
	case PT_SET_BPM: {
		NEED(2);
		uint16_t b10 = rd16(a);
		if (b10 < 300 || b10 > 3000) { code = PR_BAD_ARG; break; }
		p->bpm = (float)b10 / 10.0f;
		if (p->c.sync) sync_out_set_bpm(p->c.sync, p->bpm);
		break;
	}
	case PT_SAVE_NOW:
		NEED(0);
		code = (p->c.save && p->c.save(p->c.ctx) == 0) ? PR_OK : PR_STORAGE;
		send_result(p, f->seq, f->type, code);
		return; /* saving is not a state change */
	case PT_FACTORY_RESET: {
		NEED(1);
		if (a[0] != PROTO_FACTORY_RESET_CONFIRM) { code = PR_BAD_ARG; break; }
		store_data_t d;
		store_data_default(&d);
		int rej = engine_apply_config(e, &d.cfg);
		p->bpm = d.bpm;
		if (p->c.sync) sync_out_set_bpm(p->c.sync, p->bpm);
		memset(p->ev_wet, 0, sizeof p->ev_wet);
		memset(p->ev_param, 0, sizeof p->ev_param);
		code = rej ? PR_NO_MEMORY : PR_OK;
		host_changed(p);
		send_result(p, f->seq, f->type, code);
		return;
	}
	case PT_SUBSCRIBE: {
		NEED(1);
		if (a[0] > 20) { code = PR_BAD_ARG; break; }
		p->sub_hz = a[0];
		p->next_meter_ms = p->now_ms; /* first report on the next tick */
		send_result(p, f->seq, f->type, PR_OK);
		return;
	}
	default:
		/* Unknown request type (or a device->host type echoed back): answer "bad argument"
		 * so the host does not wait for a timeout. Not specified in v1. */
		if (f->type >= 0x80) return;
		send_result(p, f->seq, f->type, PR_BAD_ARG);
		return;
	}
#undef NEED
	if (code == PR_OK) host_changed(p);
	send_result(p, f->seq, f->type, code);
}

/* ------------------------------------------------------------- receiver */

void proto_rx_byte(proto_t *p, uint8_t b)
{
	if (b == 0) {
		if (!p->rx_overflow && p->rx_len > 0) {
			proto_frame_t f;
			if (proto_parse_frame(p->rx, p->rx_len, p->body, &f) == 0) {
				p->rx_frames++;
				handle(p, &f);
			} else {
				p->rx_dropped++;
			}
		} else if (p->rx_overflow) {
			p->rx_dropped++;
		}
		p->rx_len = 0;
		p->rx_overflow = 0;
		return;
	}
	if (p->rx_overflow) return;
	if (p->rx_len >= sizeof p->rx) { p->rx_overflow = 1; p->rx_len = 0; return; }
	p->rx[p->rx_len++] = b;
}

void proto_rx(proto_t *p, const uint8_t *bytes, size_t n)
{
	for (size_t i = 0; i < n; i++) proto_rx_byte(p, bytes[i]);
}

/* --------------------------------------------------------- device events */

static void send_event(proto_t *p, uint8_t setter, const uint8_t *pl, size_t n)
{
	uint8_t b[8];
	b[0] = setter;
	memcpy(b + 1, pl, n);
	send(p, PT_EVENT, 0, b, n + 1);
}

static void send_wet(proto_t *p, int s, float v)
{
	uint16_t q = q16(v);
	uint8_t pl[3] = {(uint8_t)s, (uint8_t)q, (uint8_t)(q >> 8)};
	send_event(p, PT_SET_WET, pl, 3);
}

static void send_param(proto_t *p, int s, int i, float v)
{
	uint16_t q = q16(v);
	uint8_t pl[4] = {(uint8_t)s, (uint8_t)i, (uint8_t)q, (uint8_t)(q >> 8)};
	send_event(p, PT_SET_PARAM, pl, 4);
}

static void flush_one(proto_t *p, proto_fader_ev_t *f, int force, int kind, int s, int i)
{
	if (!f->pending) return;
	if (!force && f->ever_sent && p->now_ms - f->last_sent_ms < PROTO_EVENT_MIN_INTERVAL_MS) return;
	if (kind == 0) send_wet(p, s, f->value);
	else send_param(p, s, i, f->value);
	f->pending = 0;
	f->ever_sent = 1;
	f->last_sent_ms = p->now_ms;
}

static void drain_events(proto_t *p)
{
	controller_t *c = p->c.ctl;
	if (!c) return;
	ctl_event_t ev;
	while (controller_pop_event(c, &ev)) {
		if (ev.slot >= ENGINE_SLOTS) continue;
		switch (ev.kind) {
		case CTL_EV_WET:
			p->ev_wet[ev.slot].pending = 1;
			p->ev_wet[ev.slot].value = ev.v;
			break;
		case CTL_EV_PARAM:
			if (ev.a >= FX_MAX_PARAMS) break;
			p->ev_param[ev.slot][ev.a].pending = 1;
			p->ev_param[ev.slot][ev.a].value = ev.v;
			break;
		case CTL_EV_BYPASS: {
			uint8_t pl[2] = {ev.slot, ev.a ? 1 : 0};
			send_event(p, PT_SET_BYPASS, pl, 2);
			break;
		}
		case CTL_EV_SLOT_FX: {
			/* params pending for the previous effect must reach the host before the switch */
			for (int i = 0; i < FX_MAX_PARAMS; i++) flush_one(p, &p->ev_param[ev.slot][i], 1, 1, ev.slot, i);
			uint8_t pl[2] = {ev.slot, ev.a};
			send_event(p, PT_SET_SLOT_FX, pl, 2);
			break;
		}
		case CTL_EV_GLOBAL_BYPASS: {
			uint8_t pl[1] = {ev.a ? 1 : 0};
			send_event(p, PT_SET_GLOBAL_BYPASS, pl, 1);
			break;
		}
		case CTL_EV_MASTER: {
			uint16_t q = q16(ev.v);
			uint8_t pl[2] = {(uint8_t)q, (uint8_t)(q >> 8)};
			send_event(p, PT_SET_MASTER, pl, 2);
			break;
		}
		case CTL_EV_ADMISSION: {
			uint8_t pl[3] = {ev.slot, ev.a, map_status((engine_status)ev.b)};
			send(p, PT_ADMISSION_ERROR, 0, pl, 3);
			break;
		}
		default:
			break;
		}
	}
	for (int s = 0; s < ENGINE_SLOTS; s++) {
		flush_one(p, &p->ev_wet[s], 0, 0, s, 0);
		for (int i = 0; i < FX_MAX_PARAMS; i++) flush_one(p, &p->ev_param[s][i], 0, 1, s, i);
	}
}

/* ---------------------------------------------------------------- meters */

static uint16_t peak16(float x)
{
	if (!(x > 0.0f)) return 0;
	float v = x * 32768.0f;
	return v >= 65535.0f ? 0xFFFFu : (uint16_t)(v + 0.5f);
}

static uint8_t pct(uint32_t cycles, uint32_t budget)
{
	if (!budget) return 0;
	uint32_t v = (uint32_t)(((uint64_t)cycles * 100u + budget / 2u) / budget);
	return v > 255u ? 255u : (uint8_t)v;
}

static void send_meters(proto_t *p)
{
	engine_t *e = p->c.eng;
	wr_t w = {p->pay, 0, sizeof p->pay, 0};
	uint32_t mem = 0;
	engine_resources(e, &mem, NULL);
	w16(&w, peak16(e->diag.in_peak));
	w16(&w, peak16(e->diag.out_peak));
	e->diag.in_peak = 0.0f; /* peak since the last report */
	e->diag.out_peak = 0.0f;
	w8(&w, pct(e->diag.cpu_last_cycles, e->cpu_budget));
	w8(&w, pct(e->diag.cpu_peak_cycles, e->cpu_budget));
	w32(&w, mem);
	w32(&w, e->diag.in_clips);
	w32(&w, e->diag.out_clips);
	w32(&w, e->diag.underruns);
	w32(&w, e->diag.overruns);
	w32(&w, e->diag.nan_events);
	w8(&w, p->c.ctl ? (uint32_t)p->c.ctl->mode : 0u);
	w8(&w, p->c.ctl ? (uint32_t)p->c.ctl->edit_slot : 0u);
	w8(&w, e->cfg.global_bypass ? 1u : 0u);
	send(p, PT_METERS, 0, w.p, w.n);
}

void proto_tick(proto_t *p, uint32_t now_ms)
{
	p->now_ms = now_ms;
	drain_events(p);
	if (p->sub_hz) {
		uint32_t period = 1000u / p->sub_hz;
		if ((int32_t)(now_ms - p->next_meter_ms) >= 0) {
			send_meters(p);
			p->next_meter_ms += period;
			/* fell behind by more than a period (long stall): resynchronise, don't burst */
			if ((int32_t)(now_ms - p->next_meter_ms) >= 0) p->next_meter_ms = now_ms + period;
		}
	}
}
