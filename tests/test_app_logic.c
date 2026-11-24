/* Decision logic of the integrated firmware (firmware/app/app_logic.c): save/erase scheduling
 * against audio activity, console text/binary mode, quiet detector, line rate limiter. Also an
 * end-to-end run of the scheduler with the real journal (config_store) on a mock flash that
 * records whether an erase ever happened while audio was audible. */
#include "t.h"
#include "../firmware/app/app_logic.h"
#include "../firmware/storage/config_store.h"
#include "../firmware/protocol/protocol.h"

static void test_save_sched_rules(void)
{
	save_sched_t s;
	save_sched_init(&s);
	/* nothing to do */
	CHECK(save_sched_next(&s, 0, 0, 0, 0, 0) == SAVE_IDLE);
	CHECK(save_sched_next(&s, 0, 0, 0, 0, 1) == SAVE_IDLE);
	/* a due save that needs no erase is written at once, quiet or not */
	CHECK(save_sched_next(&s, 10, 1, 0, 0, 0) == SAVE_WRITE);
	CHECK(save_sched_next(&s, 20, 0, 0, 0, 0) == SAVE_IDLE);
	/* a due save that needs an erase waits for quiet, however long */
	CHECK(save_sched_next(&s, 30, 1, 1, 1, 0) == SAVE_IDLE);
	for (uint32_t t = 40; t < 600000; t += 8) CHECK(save_sched_next(&s, t, 0, 1, 1, 0) == SAVE_IDLE);
	CHECK(s.pending == 1 && s.deferred > 1000);
	CHECK(save_sched_next(&s, 600000, 0, 1, 1, 1) == SAVE_WRITE_WITH_ERASE);
	save_sched_erased(&s, 600000);
	CHECK(s.pending == 0);
	/* if the erase need disappears (e.g. the spare got pre-erased) the deferred save just writes */
	CHECK(save_sched_next(&s, 600010, 1, 1, 0, 0) == SAVE_IDLE);
	CHECK(save_sched_next(&s, 600020, 0, 0, 0, 0) == SAVE_WRITE);
	/* spare pre-erase only when quiet, and not again within the retry time */
	CHECK(save_sched_next(&s, 600030, 0, 0, 1, 0) == SAVE_IDLE);
	CHECK(save_sched_next(&s, 600030, 0, 0, 1, 1) == SAVE_IDLE); /* < SAVE_ERASE_RETRY_MS after the last erase */
	CHECK(save_sched_next(&s, 600000 + SAVE_ERASE_RETRY_MS, 0, 0, 1, 1) == SAVE_ERASE_SPARE);
	save_sched_erased(&s, 600000 + SAVE_ERASE_RETRY_MS);
	CHECK(save_sched_next(&s, 600000 + SAVE_ERASE_RETRY_MS + 8, 0, 0, 1, 1) == SAVE_IDLE);
	/* a pending save has priority over a pre-erase */
	CHECK(save_sched_next(&s, 700000, 1, 0, 1, 1) == SAVE_WRITE);
}

/* ---- end to end: journal + scheduler + simulated audio, erase never while audible ---- */
typedef struct {
	uint8_t page[2][STORE_PAGE_BYTES];
	int audible;            /* simulated: is audio playing right now */
	uint32_t erases, erases_while_audible;
} eflash_t;
static eflash_t F;
static int f_read(void *c, uint32_t p, uint32_t off, void *b, uint32_t n)
{ (void)c; if (p > 1 || off + n > STORE_PAGE_BYTES) return -1; memcpy(b, F.page[p] + off, n); return 0; }
static int f_write(void *c, uint32_t p, uint32_t off, const void *b, uint32_t n)
{
	(void)c;
	if (p > 1 || off + n > STORE_PAGE_BYTES) return -1;
	const uint8_t *s = b;
	for (uint32_t i = 0; i < n; i++) F.page[p][off + i] &= s[i];
	return 0;
}
static int f_erase(void *c, uint32_t p)
{
	(void)c;
	if (p > 1) return -1;
	memset(F.page[p], 0xFF, STORE_PAGE_BYTES);
	F.erases++;
	if (F.audible) F.erases_while_audible++;
	return 0;
}

static void test_save_sched_with_journal(void)
{
	memset(&F, 0, sizeof F);
	memset(F.page, 0xFF, sizeof F.page);
	memset(F.page[0], 0x5A, 64);  /* foreign data in the first page */
	flash_ops_t ops = {NULL, f_read, f_write, f_erase};
	store_t st;
	store_init(&st, ops);
	store_debounce_t deb;
	store_debounce_init(&deb);
	save_sched_t ss;
	save_sched_init(&ss);
	quiet_t q = {0};
	store_data_t d;
	store_data_default(&d);
	uint32_t saves = 0, changes = 0;
	const uint32_t QUIET_BLOCKS = 1125; /* 1.5 s of 64-frame blocks */
	/* 2 hours at an 8 ms tick: audio plays for 3 min, pauses 4 s, repeat; the user changes
	 * something every ~3 s while playing */
	for (uint32_t now = 0; now < 2u * 3600u * 1000u; now += 8) {
		uint32_t cyc = now % 184000u;
		F.audible = cyc < 180000u;
		for (int b = 0; b < 6; b++) quiet_feed(&q, F.audible ? 0.3f : 0.0f, F.audible ? 0.3f : 0.0f);
		if (F.audible && now % 3000u == 0u) {
			d.cfg.wet[0] = (float)((now / 3000u) % 100u) / 100.0f;
			store_debounce_mark(&deb, now);
			changes++;
		}
		int due = store_debounce_due(&deb, now);
		save_action_t a = save_sched_next(&ss, now, due, store_save_needs_erase(&st),
						  store_spare_needs_erase(&st), quiet_ok(&q, QUIET_BLOCKS));
		if (a == SAVE_WRITE || a == SAVE_WRITE_WITH_ERASE) {
			CHECK(store_save(&st, &d) == 0);
			saves++;
			if (a == SAVE_WRITE_WITH_ERASE) save_sched_erased(&ss, now);
		} else if (a == SAVE_ERASE_SPARE) {
			CHECK(store_erase_spare(&st) == 0);
			save_sched_erased(&ss, now);
		}
	}
	CHECK_MSG(F.erases_while_audible == 0, "%u erases while audible", F.erases_while_audible);
	CHECK(F.erases > 0);
	/* while music plays without a pause a page pair holds 16 saves; the rest wait for a pause */
	CHECK_MSG(saves > 200, "saves %u changes %u", saves, changes);
	CHECK(ss.deferred > 0);
	/* the user stops playing: within a few seconds of quiet the latest state is saved */
	F.audible = 0;
	for (uint32_t now = 2u * 3600u * 1000u; now < 2u * 3600u * 1000u + 8000u; now += 8) {
		for (int b = 0; b < 6; b++) quiet_feed(&q, 0.0f, 0.0f);
		int due = store_debounce_due(&deb, now);
		save_action_t a = save_sched_next(&ss, now, due, store_save_needs_erase(&st),
						  store_spare_needs_erase(&st), quiet_ok(&q, QUIET_BLOCKS));
		if (a == SAVE_WRITE || a == SAVE_WRITE_WITH_ERASE) CHECK(store_save(&st, &d) == 0);
		else if (a == SAVE_ERASE_SPARE) CHECK(store_erase_spare(&st) == 0);
		if (a != SAVE_IDLE && a != SAVE_WRITE) save_sched_erased(&ss, now);
	}
	CHECK(ss.pending == 0);
	/* the latest state is on flash */
	store_t r;
	store_data_t out;
	store_init(&r, ops);
	CHECK(store_load(&r, &out) == 0 && fabsf(out.cfg.wet[0] - d.cfg.wet[0]) < 1e-4f);
}

static void test_conmode(void)
{
	con_mode_t m;
	conmode_init(&m);
	CHECK(!conmode_text_ok(&m) && !conmode_binary_ok(&m));
	/* a terminal opens the port: text */
	CHECK(conmode_update(&m, 1, 5) == 0);
	CHECK(conmode_text_ok(&m) && !conmode_binary_ok(&m));
	CHECK(conmode_update(&m, 1, 5) == 0 && conmode_text_ok(&m));
	/* the companion's first valid frame: switch once (caller sends one 0x00), text stops */
	CHECK(conmode_update(&m, 1, 6) == 1);
	CHECK(!conmode_text_ok(&m) && conmode_binary_ok(&m));
	CHECK(conmode_update(&m, 1, 7) == 0 && conmode_binary_ok(&m));
	/* port closed: nothing; opened again: text until a frame */
	CHECK(conmode_update(&m, 0, 7) == 0);
	CHECK(!conmode_text_ok(&m) && !conmode_binary_ok(&m));
	CHECK(conmode_update(&m, 1, 7) == 0 && conmode_text_ok(&m));
	CHECK(conmode_update(&m, 1, 8) == 1 && conmode_binary_ok(&m));
	CHECK(m.switches == 2);
	/* a host that never raises DTR but sends frames still gets binary answers */
	conmode_init(&m);
	CHECK(conmode_update(&m, 0, 0) == 0);
	CHECK(conmode_update(&m, 0, 1) == 1 && conmode_binary_ok(&m) && !conmode_text_ok(&m));
}

/* The 0x00 separator really protects the first frame: text, 0x00, frame through the
 * protocol's own COBS parser gives one bad frame then one good frame. */
static void test_separator_with_protocol_parser(void)
{
	const char *text = "t=12 preset=2:Boom Bap cpu%=49.1\r\n";
	uint8_t frame[64], body[PROTO_MAX_BODY];
	uint8_t pl[2] = {PT_HELLO, 0};
	size_t n = proto_build_frame(PT_RESULT, 9, pl, 2, frame, sizeof frame);
	uint8_t stream[256];
	size_t k = 0;
	memcpy(stream, text, strlen(text)); k += strlen(text);
	stream[k++] = 0x00; /* separator */
	memcpy(stream + k, frame, n); k += n;
	int good = 0, bad = 0;
	size_t start = 0;
	for (size_t i = 0; i < k; i++) {
		if (stream[i] != 0) continue;
		if (i > start) {
			proto_frame_t f;
			if (proto_parse_frame(stream + start, i - start, body, &f) == 0) { good++; CHECK(f.type == PT_RESULT && f.seq == 9); }
			else bad++;
		}
		start = i + 1;
	}
	CHECK(good == 1 && bad == 1);
}

static void test_quiet(void)
{
	quiet_t q = {0};
	for (int i = 0; i < 1124; i++) quiet_feed(&q, 0.0001f, 0.0002f);
	CHECK(!quiet_ok(&q, 1125));
	quiet_feed(&q, 0.0f, 0.0f);
	CHECK(quiet_ok(&q, 1125));
	quiet_feed(&q, 0.0f, 0.01f); /* a reverb tail on the output is not quiet */
	CHECK(!quiet_ok(&q, 1) && q.silent_blocks == 0);
	quiet_feed(&q, NAN, 0.0f);
	CHECK(q.silent_blocks == 0);
}

static void test_ratelimit(void)
{
	ratelimit_t r;
	ratelimit_init(&r, 10, 5, 0);
	int ok = 0;
	for (int i = 0; i < 20; i++) ok += ratelimit_take(&r, 0);
	CHECK(ok == 5 && r.dropped == 15);
	ok = 0;
	for (uint32_t t = 0; t <= 10000; t += 10) ok += ratelimit_take(&r, t);
	CHECK_MSG(ok >= 99 && ok <= 106, "ok=%d", ok); /* ~10 lines/s */
	CHECK(ratelimit_take(&r, 0xFFFFFFF0u) == 1);   /* clock wrap / long gap: refills to burst */
}

/* ---- console TX queue with a simulated host --------------------------------------------
 * The host side is modelled as: reads at most `rate` bytes per 8 ms tick (0 = stalled: port
 * open but nobody reads, or a second program holds it), and its bytes go through the
 * protocol's own COBS frame parser. The producer side is the real contx_t. */
#include "../firmware/controls/controller.h"

typedef struct {
	uint8_t acc[2048];
	size_t n;
	int good, bad;
	uint8_t body[PROTO_MAX_BODY];
} host_t;

static void host_rx(host_t *h, const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		if (p[i] == 0) {
			if (h->n) {
				proto_frame_t f;
				if (proto_parse_frame(h->acc, h->n, h->body, &f) == 0) h->good++;
				else h->bad++;
			}
			h->n = 0;
		} else if (h->n < sizeof h->acc) {
			h->acc[h->n++] = p[i];
		}
	}
}

/* drain up to `rate` bytes like cdc_cb does (64-byte claims) */
static void host_drain(contx_t *c, host_t *h, uint32_t rate)
{
	while (rate > 0) {
		const uint8_t *p;
		uint32_t n = txq_peek(&c->q, &p, rate < 64 ? rate : 64);
		if (n == 0) break;
		host_rx(h, p, n);
		txq_consume(&c->q, n);
		rate -= n;
	}
}

static size_t mk_frame(uint8_t *out, size_t cap, uint8_t type, size_t payload_len)
{
	uint8_t pl[PROTO_MAX_PAYLOAD];
	for (size_t i = 0; i < payload_len; i++) pl[i] = (uint8_t)(i * 7u + 1u);
	return proto_build_frame(type, 3, pl, payload_len, out, cap);
}

static void test_txq_basic(void)
{
	uint8_t buf[100];
	txq_t q;
	txq_init(&q, buf, sizeof buf);
	uint8_t m[60];
	memset(m, 0x41, sizeof m);
	CHECK(txq_put(&q, m, 60) == 1 && txq_used(&q) == 60);
	CHECK(txq_put(&q, m, 41) == 0 && txq_used(&q) == 60);  /* whole or nothing */
	CHECK(txq_put(&q, m, 40) == 1 && txq_space(&q) == 0);
	const uint8_t *p;
	CHECK(txq_peek(&q, &p, 1000) == 100);
	txq_consume(&q, 70);
	CHECK(txq_put(&q, m, 60) == 1);                         /* wraps */
	uint32_t got = 0;
	for (int i = 0; i < 4; i++) { uint32_t n = txq_peek(&q, &p, 1000); got += n; txq_consume(&q, n); }
	CHECK(got == 90 && txq_used(&q) == 0 && q.max_used == 100);
	/* free-running indices survive wrap-around of the 32-bit counters */
	q.head = q.tail = 0xFFFFFFF0u;
	CHECK(txq_put(&q, m, 60) == 1 && txq_used(&q) == 60);
	CHECK(txq_peek(&q, &p, 1000) + 0u > 0u);
}

/* A host that stops reading: the producer never waits, frames are dropped whole, and once the
 * host reads again every byte it gets parses into valid frames (no half frames, no garbage). */
static void test_contx_stalled_host(void)
{
	static uint8_t buf[4096];
	contx_t c;
	contx_init(&c, buf, sizeof buf);
	host_t h;
	memset(&h, 0, sizeof h);
	contx_poll(&c, 1, 0);
	CHECK(contx_text(&c, (const uint8_t *)"I banner\r\n", 10) == 1);
	contx_poll(&c, 1, 1); /* companion's first frame: binary, separator queued */
	CHECK(c.sep_sent == 1 && conmode_binary_ok(&c.m));
	uint8_t f[600];
	int sent = 0, tried = 0;
	/* 60 s of METERS at 20 Hz + a STATE every second, host completely stalled */
	for (uint32_t t = 0; t < 60000; t += 8) {
		if (t % 50 == 0) { size_t n = mk_frame(f, sizeof f, PT_METERS, 33); tried++; sent += contx_frame(&c, f, (uint32_t)n); }
		if (t % 1000 == 0) { size_t n = mk_frame(f, sizeof f, PT_STATE, 397); tried++; sent += contx_frame(&c, f, (uint32_t)n); }
		CHECK(txq_used(&c.q) <= sizeof buf);
		CHECK(contx_text(&c, (const uint8_t *)"x\r\n", 3) == 0); /* binary: no text */
	}
	CHECK(c.frame_drops > 0 && (int)c.frame_drops == tried - sent);
	CHECK(txq_used(&c.q) > sizeof buf - 600);
	/* host resumes (slow: 64 bytes per tick) */
	for (int k = 0; k < 2000 && txq_used(&c.q); k++) host_drain(&c, &h, 64);
	CHECK(txq_used(&c.q) == 0);
	CHECK_MSG(h.bad == 1, "bad %d", h.bad);   /* only the text banner before the 0x00 */
	CHECK_MSG(h.good == sent, "good %d sent %d", h.good, sent);
	/* and new frames after the resume arrive */
	size_t n = mk_frame(f, sizeof f, PT_RESULT, 2);
	CHECK(contx_frame(&c, f, (uint32_t)n) == 1);
	host_drain(&c, &h, 1000);
	CHECK(h.good == sent + 1);
}

/* The 0x00 separator cannot be queued (queue full of text from a stalled terminal): frames
 * are held back (dropped) until it can, never sent without it. */
static void test_contx_separator_when_full(void)
{
	static uint8_t buf[256];
	contx_t c;
	contx_init(&c, buf, sizeof buf);
	host_t h;
	memset(&h, 0, sizeof h);
	contx_poll(&c, 1, 0);
	uint8_t line[64];
	memset(line, 'a', sizeof line);
	line[62] = '\r'; line[63] = '\n';
	while (contx_text(&c, line, sizeof line)) {}
	CHECK(txq_space(&c.q) < 64);
	/* fill to the last byte so even the separator does not fit */
	while (txq_space(&c.q) && contx_text(&c, line, 1)) {}
	CHECK(txq_space(&c.q) == 0);
	contx_poll(&c, 1, 1);
	CHECK(conmode_binary_ok(&c.m) && c.sep_pending == 1);
	uint8_t f[64];
	size_t n = mk_frame(f, sizeof f, PT_HELLO_RSP, 20);
	CHECK(contx_frame(&c, f, (uint32_t)n) == 0 && c.frame_drops == 1);
	host_drain(&c, &h, 8);              /* a little room */
	CHECK(contx_frame(&c, f, (uint32_t)n) == 0); /* separator now in, frame still does not fit */
	CHECK(c.sep_pending == 0);
	for (int k = 0; k < 100; k++) host_drain(&c, &h, 64);
	CHECK(contx_frame(&c, f, (uint32_t)n) == 1);
	host_drain(&c, &h, 1000);
	CHECK(h.good == 1 && h.bad == 1);   /* text, then exactly the frame */
	/* port closed while the separator is pending: no separator in text mode later */
	contx_init(&c, buf, sizeof buf);
	contx_poll(&c, 1, 0);
	while (contx_text(&c, line, 1)) {}
	contx_poll(&c, 1, 1);
	CHECK(c.sep_pending == 1);
	contx_poll(&c, 0, 1);
	CHECK(c.sep_pending == 0 && !conmode_binary_ok(&c.m));
}

/* The companion's connect burst (HELLO, GET_FX for every effect, GET_STATE, SUBSCRIBE) through
 * the REAL protocol, with the host not reading anything: every answer fits the 4 KB queue,
 * nothing is dropped; and a reconnect (port closed and opened again) starts in text mode and
 * switches again with a fresh separator. */
static uint8_t burst_pool[150 * 1024];
static contx_t bc;
static uint8_t bc_buf[4096];
static proto_t bp;
static int bdtr = 1;
static void btx(void *ctx, const uint8_t *b, size_t n)
{
	(void)ctx;
	contx_poll(&bc, bdtr, bp.rx_frames);
	contx_frame(&bc, b, (uint32_t)n);
}

static void send_req(uint8_t type, const uint8_t *pl, size_t n, uint8_t seq)
{
	uint8_t f[64];
	size_t m = proto_build_frame(type, seq, pl, n, f, sizeof f);
	proto_rx(&bp, f, m);
}

static void test_connect_burst_and_reconnect(void)
{
	static engine_t e;
	static controller_t ui;
	engine_init(&e, burst_pool, sizeof burst_pool, 48000.0f, 850);
	controller_init(&ui, &e);
	proto_config_t pc;
	memset(&pc, 0, sizeof pc);
	pc.eng = &e; pc.ctl = &ui; pc.tx = btx; pc.fw = "sp1fx-v1-rc2"; pc.block = 64; pc.bpm = 120.0f;
	proto_init(&bp, &pc);
	contx_init(&bc, bc_buf, sizeof bc_buf);
	host_t h;
	memset(&h, 0, sizeof h);
	contx_poll(&bc, 1, bp.rx_frames);
	contx_text(&bc, (const uint8_t *)"D t=12 diag line\r\n", 18);
	uint8_t seq = 1;
	send_req(PT_HELLO, NULL, 0, seq++);
	for (uint8_t i = 0; i < FX_COUNT; i++) send_req(PT_GET_FX, &i, 1, seq++);
	send_req(PT_GET_STATE, NULL, 0, seq++);
	uint8_t hz = 20;
	send_req(PT_SUBSCRIBE, &hz, 1, seq++);
	for (uint32_t t = 0; t < 200; t += 8) proto_tick(&bp, t); /* meters while the host is stalled */
	CHECK_MSG(bc.frame_drops == 0, "dropped %u, queue max %u", bc.frame_drops, bc.q.max_used);
	CHECK(bc.q.max_used < sizeof bc_buf);
	host_drain(&bc, &h, 100000);
	CHECK(h.bad == 1);                       /* the diag line before the separator */
	CHECK(h.good == 1 + FX_COUNT + 1 + 1 + (int)bc.frames_sent - (3 + FX_COUNT));
	/* reconnect: port closed (DTR low) and opened again by the companion */
	bdtr = 0; contx_poll(&bc, 0, bp.rx_frames);
	CHECK(!conmode_binary_ok(&bc.m));
	bdtr = 1; contx_poll(&bc, 1, bp.rx_frames);
	CHECK(conmode_text_ok(&bc.m));
	uint32_t sep0 = bc.sep_sent;
	int good0 = h.good;
	send_req(PT_HELLO, NULL, 0, seq++);
	CHECK(conmode_binary_ok(&bc.m) && bc.sep_sent == sep0 + 1);
	host_drain(&bc, &h, 100000);
	CHECK(h.good == good0 + 1);
}

/* Host simulation of the rc1 hang scenario: preset changes and effect cycling from the
 * buttons (duck -> post config -> fade back), device + host changes, debounced saves,
 * with the AUDIO THREAD STOPPED (engine_process / engine_pump never run, so nothing posted
 * ever becomes "settled" and the duck never completes). Every control-loop step must return
 * without waiting on the audio side; saves that need no erase still happen; erases are never
 * chosen (not quiet). Also: a fader pickup at the stored value is not a change (no boot save). */
#include "../firmware/app/factory_presets.h"
static int sim_fill(void *c, int idx, store_data_t *out)
{
	(void)c;
	if (idx < 0 || idx >= FACTORY_PRESET_COUNT) return -1;
	factory_preset_fill(&factory_presets[idx], out);
	return 0;
}

static void test_preset_sequence_with_audio_stopped(void)
{
	static uint8_t pool[150 * 1024];
	static engine_t e;
	static controller_t ui;
	engine_init(&e, pool, sizeof pool, 48000.0f, 850);
	store_data_t d;
	store_data_default(&d);
	engine_apply_config(&e, &d.cfg);
	controller_init(&ui, &e);
	controller_set_presets(&ui, FACTORY_PRESET_COUNT, sim_fill, NULL);
	ui.preset_keep_master = 1;
	memset(&F, 0, sizeof F);
	memset(F.page, 0xFF, sizeof F.page);
	flash_ops_t ops = {NULL, f_read, f_write, f_erase};
	store_t st;
	store_init(&st, ops);
	store_debounce_t deb;
	store_debounce_init(&deb);
	save_sched_t ss;
	save_sched_init(&ss);
	float fad[4] = {0.5f, 0.5f, 0.5f, 0.5f}; /* = the stored wet values */
	uint32_t seen = ui.change_seq, saves = 0, steps = 0;
	/* boot: faders read at the stored values -> pickup, but no change, so no save */
	for (uint32_t t = 0; t < 3000; t += 8) controller_tick(&ui, t, 0, fad);
	CHECK(ui.change_seq == seen);
	for (int s = 0; s < 4; s++) CHECK(ui.to[s].picked);
	/* 10 minutes of the owner's use: FN+FWD preset changes, T2+FWD effect cycling, VOL */
	for (uint32_t t = 3000; t < 600000; t += 8) {
		uint16_t b = 0;
		uint32_t ph = t % 4000;
		if (ph >= 100 && ph < 140) b = BTN_BIT(BTN_FN);
		else if (ph >= 140 && ph < 170) b = BTN_BIT(BTN_FN) | BTN_BIT(BTN_FWD);
		else if (ph >= 170 && ph < 220) b = BTN_BIT(BTN_FN);
		else if (ph >= 2000 && ph < 2150) b = BTN_BIT(BTN_T2);
		else if (ph >= 2150 && ph < 2180) b = BTN_BIT(BTN_T2) | BTN_BIT(BTN_FWD);
		else if (ph >= 2180 && ph < 2250) b = BTN_BIT(BTN_T2);
		else if (ph >= 3000 && ph < 3040) b = BTN_BIT(BTN_VOLDN);
		controller_tick(&ui, t, b, fad);
		if (ui.change_seq != seen) { seen = ui.change_seq; store_debounce_mark(&deb, t); }
		int due = store_debounce_due(&deb, t);
		int settled = engine_config_settled(&e); /* never: audio stopped */
		CHECK(!settled || t < 3000);
		save_action_t a = save_sched_next(&ss, t, due, store_save_needs_erase(&st), store_spare_needs_erase(&st),
						  0 /* quiet requires settled audio */);
		CHECK(a == SAVE_IDLE || a == SAVE_WRITE);
		if (a == SAVE_WRITE) { CHECK(store_save(&st, &d) == 0); saves++; }
		steps++;
	}
	CHECK(steps > 70000);
	CHECK(ui.preset_idx >= 0);
	CHECK(F.erases == 0);
	/* 16 records fit without an erase; after that saves wait (pending), nothing blocks */
	CHECK_MSG(saves == 16, "saves %u", saves);
	CHECK(ss.pending == 1);
	CHECK(e.duck == 0); /* every preset load released its duck */
}

static void test_boot_and_standby(void)
{
	/* fill count */
	CHECK(holdfill_count(0) == 0 && holdfill_count(499) == 0 && holdfill_count(500) == 1);
	CHECK(holdfill_count(3999) == 7 && holdfill_count(4000) == 8 && holdfill_count(99999) == 8);
	/* boot decision: wake reason x crash record x power-off flag */
	CHECK(boot_decide(RR_OFF, 0, 0) == BOOT_STANDBY);          /* FN wake */
	CHECK(boot_decide(RR_VBUS, 0, 0) == BOOT_STANDBY);         /* USB plugged into an off unit */
	CHECK(boot_decide(RR_SREQ, 0, 1) == BOOT_STANDBY);         /* our plugged power off */
	CHECK(boot_decide(0, 0, 1) == BOOT_STANDBY);
	CHECK(boot_decide(RR_DOG, 0, 0) == BOOT_NOW);              /* watchdog recovery */
	CHECK(boot_decide(RR_DOG | RR_OFF, 0, 0) == BOOT_NOW);
	CHECK(boot_decide(RR_LOCKUP, 0, 0) == BOOT_NOW);
	CHECK(boot_decide(RR_SREQ, 1, 0) == BOOT_NOW);             /* fatal-error reset */
	CHECK(boot_decide(RR_SREQ, 0, 0) == BOOT_NOW);             /* other soft reset (e.g. after DFU) */
	CHECK(boot_decide(RR_RESETPIN, 0, 0) == BOOT_NOW);
	CHECK(boot_decide(0, 0, 0) == BOOT_NOW);                   /* power-on reset / flash / battery in */
	CHECK(boot_decide(0, 1, 0) == BOOT_NOW);
	/* standby: FN x USB x time held */
	for (uint32_t t = 0; t < HOLD_MS; t += 50) {
		CHECK(standby_step(1, 0, t) == SB_WAIT && standby_step(1, 1, t) == SB_WAIT);
	}
	CHECK(standby_step(1, 0, HOLD_MS) == SB_BOOT && standby_step(1, 1, HOLD_MS) == SB_BOOT);
	CHECK(standby_step(0, 0, 0) == SB_OFF);    /* battery, nothing held: back to System OFF */
	CHECK(standby_step(0, 1, 0) == SB_WAIT);   /* plugged in: stay, show charging */
	/* simulated presses from a FN wake on battery: tap / 2 s / 3.99 s -> off; > 4 s -> on */
	const uint32_t press[] = {80, 2000, 3990, 4010, 7000};
	const sb_action_t want[] = {SB_OFF, SB_OFF, SB_OFF, SB_BOOT, SB_BOOT};
	for (int k = 0; k < 5; k++) {
		sb_action_t a = SB_WAIT;
		uint32_t held = 0;
		for (uint32_t t = 0; a == SB_WAIT && t < 10000; t += 10) {
			int fn = t < press[k];
			held = fn ? t : 0;
			a = standby_step(fn, 0, held);
		}
		CHECK_MSG(a == want[k], "press %u -> %d", press[k], a);
	}
	/* plugged in: a short press, release, later a 4 s hold -> on; never off meanwhile */
	{
		sb_action_t a = SB_WAIT;
		uint32_t t0 = 0;
		for (uint32_t t = 0; t < 20000 && a == SB_WAIT; t += 10) {
			int fn = (t >= 1000 && t < 1300) || t >= 5000;
			if (fn && !(t > 1000 && t < 1300) && t != 5000 && t0 == 0) t0 = 5000;
			uint32_t held = fn ? (t >= 5000 ? t - 5000 : t - 1000) : 0;
			a = standby_step(fn, 1, held);
			CHECK(a != SB_OFF);
		}
		CHECK(a == SB_BOOT);
	}
}

/* SAADC scan: start, collect, and -- the rc2 hardware failure -- a scan that never ends is
 * aborted and restarted, after which scanning continues normally (never latched). */
static void test_scanfsm_timeout_recovery(void)
{
	scanfsm_t f;
	scanfsm_init(&f);
	CHECK(scanfsm_age(&f, 0) == 0xFFFFFFFFu);
	CHECK(scanfsm_step(&f, 0, 0) == SCAN_ACT_START);
	uint32_t t = 8;
	for (; t < 1000; t += 8) CHECK(scanfsm_step(&f, t, 1) == SCAN_ACT_COLLECT_START);
	CHECK(f.ok == 124 && f.timeouts == 0 && scanfsm_age(&f, t) <= 8);
	/* the hardware stops answering for 5 s: one abort+restart per SCAN_TIMEOUT_MS, no latch */
	int aborts = 0, none = 0;
	for (uint32_t e = t + 5000; t < e; t += 8) {
		scan_act_t a = scanfsm_step(&f, t, 0);
		if (a == SCAN_ACT_ABORT_START) aborts++;
		else { CHECK(a == SCAN_ACT_NONE); none++; }
	}
	CHECK(aborts >= 5000 / 56 - 2 && aborts <= (int)(5000 / SCAN_TIMEOUT_MS) + 1);
	CHECK(none > aborts);
	CHECK(f.timeouts == (uint32_t)aborts && f.running);
	CHECK(scanfsm_age(&f, t) >= 5000);
	/* it answers again: the next tick collects, and scanning goes on */
	CHECK(scanfsm_step(&f, t, 1) == SCAN_ACT_COLLECT_START);
	CHECK(scanfsm_age(&f, t) == 0);
	uint32_t ok0 = f.ok;
	for (int k = 0; k < 100; k++) { t += 8; CHECK(scanfsm_step(&f, t, 1) == SCAN_ACT_COLLECT_START); }
	CHECK(f.ok == ok0 + 100);
	/* a single lost scan costs one timeout, then normal again */
	uint32_t to0 = f.timeouts;
	for (uint32_t k = 0; k < SCAN_TIMEOUT_MS; k += 8) { t += 8; scanfsm_step(&f, t, 0); }
	CHECK(f.timeouts == to0 + 1);
	t += 8;
	CHECK(scanfsm_step(&f, t, 1) == SCAN_ACT_COLLECT_START);
	/* clock wrap */
	scanfsm_init(&f);
	scanfsm_step(&f, 0xFFFFFFF0u, 0);
	CHECK(scanfsm_step(&f, 0xFFFFFFF8u, 0) == SCAN_ACT_NONE);
	CHECK(scanfsm_step(&f, 0x00000040u, 0) == SCAN_ACT_ABORT_START);
}

static void test_saveev(void)
{
	saveev_t s;
	saveev_init(&s, 1000);
	/* boot: nothing to save, ever, without a change */
	for (uint32_t t = 1000; t < 400000; t += 100) CHECK(!saveev_due(&s, t, 1));
	/* tweaks alone: only the 120 s safety timer (from the last save = boot) */
	saveev_init(&s, 0);
	saveev_tweak(&s, 10);
	CHECK(!saveev_due(&s, 119999, 1) && saveev_due(&s, SAVE_SAFETY_MS, 1));
	saveev_saved(&s, SAVE_SAFETY_MS);
	CHECK(!saveev_due(&s, SAVE_SAFETY_MS + 300000, 1));
	/* a preset burst (rocker repeat every 200 ms for 3 s) saves once, 1.5 s after the last */
	saveev_init(&s, 0);
	int saves = 0;
	for (uint32_t t = 0; t < 10000; t += 8) {
		if (t >= 1000 && t <= 4000 && t % 200 == 0) saveev_event(&s, t);
		if (saveev_due(&s, t, 1)) { saves++; saveev_saved(&s, t); CHECK(t >= 4000 + SAVE_EVENT_SETTLE_MS && t < 4000 + SAVE_EVENT_SETTLE_MS + 8); }
	}
	CHECK(saves == 1);
	/* high load: due but deferred until the load drops (no matter how long) */
	saveev_init(&s, 0);
	saveev_event(&s, 100);
	for (uint32_t t = 100; t < 600000; t += 8) CHECK(!saveev_due(&s, t, 0));
	CHECK(s.deferred_load > 1000);
	CHECK(saveev_due(&s, 600000, 1));
	/* a tweak after an event rides along with that event's save */
	saveev_init(&s, 0);
	saveev_event(&s, 0);
	saveev_tweak(&s, 1000);
	CHECK(saveev_due(&s, 1500, 1));
	saveev_saved(&s, 1500);
	CHECK(!s.dirty && !saveev_due(&s, 100000, 1));
	/* power off / SAVE_NOW: the caller saves directly and reports it */
	saveev_init(&s, 0);
	saveev_tweak(&s, 5);
	saveev_saved(&s, 6);
	CHECK(!saveev_due(&s, 200000, 1));
}

static void test_battery_gauge(void)
{
	/* conversion: Tape Looper's anchor raw 2380 ~ 4.21 V; the owner's 2244 while charging */
	CHECK(abs(batt_mv_from_raw(2380) - 4184) <= 30);
	CHECK(abs(batt_mv_from_raw(2244) - 3944) <= 5);
	CHECK(batt_mv_from_raw(-1) == -1);
	/* table: ends, monotonic */
	CHECK(batt_pct_from_mv(3000) == 0 && batt_pct_from_mv(3300) == 0 && batt_pct_from_mv(4200) == 100 && batt_pct_from_mv(4400) == 100);
	int prev = -1;
	for (int mv = 3200; mv <= 4300; mv += 5) { int p = batt_pct_from_mv(mv); CHECK(p >= prev && p >= 0 && p <= 100); prev = p; }
	CHECK(batt_pct_from_mv(3800) == 50);
	/* hysteresis: +-20 mV noise around a level never moves the shown value by itself */
	batt_t b;
	batt_init(&b);
	int raw0 = 2150; /* ~3.78 V */
	int shown0 = -2, moves = 0, last = -1;
	for (uint32_t t = 0; t < 120000; t += 10) {
		int noise = (int)((t * 7919u) % 21u) - 10; /* +-10 raw = +-18 mV */
		batt_feed(&b, raw0 + noise, 0, 0, t);
		if (shown0 == -2) shown0 = batt_shown(&b);
		if (batt_shown(&b) != last) { moves++; last = batt_shown(&b); }
	}
	CHECK_MSG(moves <= 2, "moves %d", moves);
	/* a real drop shows up within a few seconds */
	for (uint32_t t = 120000; t < 140000; t += 10) batt_feed(&b, 2000, 0, 0, t);
	CHECK(batt_shown(&b) < shown0 - 10);
	/* charging: voltage reads high -> offset, capped at 95 until the charger reports done */
	batt_init(&b);
	for (uint32_t t = 0; t < 30000; t += 10) batt_feed(&b, 2420, 1, 0, t); /* ~4.25 V while charging */
	CHECK(batt_shown(&b) <= BATT_CHARGING_CAP);
	for (uint32_t t = 30000; t < 40000; t += 10) batt_feed(&b, 2420, 0, 1, t);
	CHECK(batt_shown(&b) == 100);
	/* the owner's charging reading: 3.94 V - 0.1 V -> about 60 % */
	batt_init(&b);
	for (uint32_t t = 0; t < 30000; t += 10) batt_feed(&b, 2244, 1, 0, t);
	CHECK_MSG(batt_shown(&b) >= 50 && batt_shown(&b) <= 70, "shown %d", batt_shown(&b));
	/* LED bar: n of 8 steady, the top one pulses while charging, steady when not */
	uint8_t pl[4], tr[4];
	batt_leds(50, 0, 0, pl, tr);
	CHECK(pl[0] && pl[1] && pl[2] && pl[3] && !tr[0] && !tr[3]);   /* 4 of 8 */
	batt_leds(100, 0, 0, pl, tr);
	CHECK(tr[3] == 60 && pl[0] == 60);
	batt_leds(0, 0, 0, pl, tr);
	CHECK(pl[0] && !pl[1]);                                         /* at least one LED */
	uint8_t lo = 255, hi = 0;
	for (uint32_t t = 0; t < 2400; t += 50) { batt_leds(30, 1, t, pl, tr); lo = pl[2] < lo ? pl[2] : lo; hi = pl[2] > hi ? pl[2] : hi; CHECK(pl[0] == 60 && pl[1] == 60 && !pl[3]); }
	CHECK(hi > 100 && lo < 20);                                      /* the 3rd LED pulses */
}

int main(void)
{
	RUN(test_save_sched_rules);
	RUN(test_save_sched_with_journal);
	RUN(test_conmode);
	RUN(test_separator_with_protocol_parser);
	RUN(test_quiet);
	RUN(test_ratelimit);
	RUN(test_txq_basic);
	RUN(test_contx_stalled_host);
	RUN(test_contx_separator_when_full);
	RUN(test_connect_burst_and_reconnect);
	RUN(test_preset_sequence_with_audio_stopped);
	RUN(test_boot_and_standby);
	RUN(test_scanfsm_timeout_recovery);
	RUN(test_saveev);
	RUN(test_battery_gauge);
	T_DONE();
}
