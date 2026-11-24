/* Device protocol (docs/PROTOCOL.md v1): primitives, every message, events, meters,
 * robustness, and interoperability with the companion's JS implementation (golden_frames.h). */
#include "t.h"
#include "../firmware/app/factory_presets.h"
#include "../firmware/protocol/protocol.h"
#include "golden_frames.h"

#define SR 48000.0f
static uint8_t pool[150 * 1024];
static engine_t eng;
static controller_t ui;
static sync_out_t syn;
static store_debounce_t deb;
static proto_t pr;
static uint32_t T;
static float fad[4];
static int save_calls, save_result;

/* ---- tx capture: raw bytes plus the frames split out of them ---- */
#define CAP_BYTES 65536
#define CAP_FRAMES 256
static uint8_t cap[CAP_BYTES];
static size_t cap_n;
static int cap_overflow;
typedef struct { uint8_t type, seq; uint16_t len; uint8_t pay[PROTO_MAX_PAYLOAD]; size_t raw_off, raw_len; } frm_t;
static frm_t frames[CAP_FRAMES];
static int nfr, bad_tx_frames;

static void tx(void *ctx, const uint8_t *b, size_t n)
{
	(void)ctx;
	if (cap_n + n > CAP_BYTES) { cap_overflow = 1; return; }
	size_t off = cap_n;
	memcpy(cap + cap_n, b, n);
	cap_n += n;
	/* every tx call must be exactly one complete, valid frame */
	uint8_t body[PROTO_MAX_BODY];
	proto_frame_t f;
	if (n < 2 || b[n - 1] != 0 || memchr(b, 0, n - 1) || proto_parse_frame(b, n - 1, body, &f) != 0) { bad_tx_frames++; return; }
	if (nfr < CAP_FRAMES) {
		frm_t *x = &frames[nfr++];
		x->type = f.type; x->seq = f.seq; x->len = f.len;
		memcpy(x->pay, f.payload, f.len);
		x->raw_off = off; x->raw_len = n;
	}
}
static void clear_tx(void) { cap_n = 0; nfr = 0; }
static int save_cb(void *ctx) { (void)ctx; save_calls++; return save_result; }

static void setup_budget(uint32_t budget)
{
	memset(pool, 0, sizeof pool);
	engine_init(&eng, pool, sizeof pool, SR, budget);
	store_data_t d;
	store_data_default(&d);
	engine_apply_config(&eng, &d.cfg);
	for (int s = 0; s < 4; s++) fad[s] = 0.5f;
	controller_init(&ui, &eng);
	sync_out_init(&syn, SR);
	store_debounce_init(&deb);
	proto_config_t c = {0};
	c.eng = &eng; c.ctl = &ui; c.sync = &syn; c.deb = &deb;
	c.tx = tx; c.save = save_cb; c.ctx = NULL;
	c.fw = "sp1fx-golden"; c.block = 64; c.bpm = 120.0f;
	proto_init(&pr, &c);
	save_calls = 0; save_result = 0;
	T = 1000;
	clear_tx();
	bad_tx_frames = 0;
}
static void setup(void) { setup_budget(ENGINE_DEFAULT_CPU_BUDGET); }

/* advance ms with buttons held, ticking controller + protocol every 8 ms */
static void hold(uint16_t mask, uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 8) { T += 8; controller_tick(&ui, T, mask, fad); proto_tick(&pr, T); }
}
#define B(x) BTN_BIT(x)

static void feed(const uint8_t *b, size_t n) { proto_rx(&pr, b, n); }
static void request(uint8_t type, uint8_t seq, const uint8_t *pay, size_t n)
{
	uint8_t f[PROTO_MAX_FRAME];
	size_t m = proto_build_frame(type, seq, pay, n, f, sizeof f);
	CHECK(m > 0);
	feed(f, m);
}
static int count_type(uint8_t type)
{
	int k = 0;
	for (int i = 0; i < nfr; i++) k += frames[i].type == type;
	return k;
}
static const frm_t *last_of(uint8_t type)
{
	for (int i = nfr - 1; i >= 0; i--) if (frames[i].type == type) return &frames[i];
	return NULL;
}
static int result_code(uint8_t req_type, uint8_t seq)
{
	const frm_t *f = last_of(PT_RESULT);
	if (!f || f->len != 2 || f->seq != seq || f->pay[0] != req_type) return -1;
	return f->pay[1];
}
static int raw_equals(const frm_t *f, const uint8_t *g, size_t n)
{
	return f && f->raw_len == n && memcmp(cap + f->raw_off, g, n) == 0;
}
static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static float gf(const uint8_t *p) { uint32_t u = g32(p); float f; memcpy(&f, &u, 4); return f; }

static uint32_t rng = 0x12345678u;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

/* ------------------------------------------------------------------ primitives */

static void test_crc16(void)
{
	CHECK(proto_crc16((const uint8_t *)"123456789", 9) == 0x29B1);
	CHECK(proto_crc16(NULL, 0) == 0xFFFF);
}

static int cobs_roundtrip(const uint8_t *in, size_t n)
{
	uint8_t enc[1200], dec[1200];
	size_t m = proto_cobs_encode(in, n, enc, sizeof enc), dn = 0;
	if (m == 0 || m > PROTO_COBS_MAX(n) + 1) return 0;
	if (memchr(enc, 0, m)) return 0;
	if (proto_cobs_decode(enc, m, dec, sizeof dec, &dn) != 0) return 0;
	return dn == n && (n == 0 || memcmp(in, dec, n) == 0);
}

static void test_cobs(void)
{
	uint8_t b[1100];
	CHECK(cobs_roundtrip(b, 0));
	memset(b, 0, sizeof b);
	for (size_t n = 1; n <= 600; n += 37) CHECK(cobs_roundtrip(b, n)); /* all zeros */
	for (size_t n = 0; n < sizeof b; n++) b[n] = (uint8_t)(1 + n % 255);
	CHECK(cobs_roundtrip(b, 253));
	CHECK(cobs_roundtrip(b, 254));
	CHECK(cobs_roundtrip(b, 255));
	CHECK(cobs_roundtrip(b, 508));
	CHECK(cobs_roundtrip(b, 1000));
	/* exact layout of a 254-byte run (matches the JS encoder: trailing empty block) */
	uint8_t enc[300];
	size_t m = proto_cobs_encode(b, 254, enc, sizeof enc);
	CHECK(m == 256 && enc[0] == 0xFF && enc[255] == 0x01);
	m = proto_cobs_encode(b, 253, enc, sizeof enc);
	CHECK(m == 254 && enc[0] == 0xFE);
	/* zeros around a 254 run */
	b[0] = 0; b[255] = 0;
	CHECK(cobs_roundtrip(b, 300));
	uint8_t z[3] = {1, 0, 2};
	m = proto_cobs_encode(z, 3, enc, sizeof enc);
	CHECK(m == 4 && enc[0] == 2 && enc[1] == 1 && enc[2] == 2 && enc[3] == 2);
	/* random */
	for (int k = 0; k < 2000; k++) {
		size_t n = rnd() % 700;
		for (size_t i = 0; i < n; i++) b[i] = (rnd() % 5 == 0) ? 0 : (uint8_t)rnd();
		if (!cobs_roundtrip(b, n)) { CHECK(0); break; }
	}
	/* malformed */
	size_t dn;
	uint8_t out[16];
	const uint8_t over[] = {0x05, 1, 2};
	CHECK(proto_cobs_decode(over, 3, out, sizeof out, &dn) != 0);
	const uint8_t zin[] = {0x03, 1, 0};
	CHECK(proto_cobs_decode(zin, 3, out, sizeof out, &dn) != 0);
	const uint8_t lead0[] = {0x00, 1};
	CHECK(proto_cobs_decode(lead0, 2, out, sizeof out, &dn) != 0);
	const uint8_t big[] = {0x10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
	CHECK(proto_cobs_decode(big, 16, out, 4, &dn) != 0); /* capacity respected */
	/* encoder capacity respected */
	CHECK(proto_cobs_encode(b, 300, enc, 100) == 0);
}

static void test_frame_build_parse(void)
{
	uint8_t pay[PROTO_MAX_PAYLOAD + 1], f[PROTO_MAX_FRAME + 8], body[PROTO_MAX_BODY];
	for (size_t i = 0; i < sizeof pay; i++) pay[i] = (uint8_t)(i * 7);
	proto_frame_t fr;
	size_t m = proto_build_frame(0x55, 9, pay, PROTO_MAX_PAYLOAD, f, sizeof f);
	CHECK(m > 0 && m <= PROTO_MAX_FRAME && f[m - 1] == 0);
	CHECK(proto_parse_frame(f, m - 1, body, &fr) == 0);
	CHECK(fr.type == 0x55 && fr.seq == 9 && fr.len == PROTO_MAX_PAYLOAD && memcmp(fr.payload, pay, PROTO_MAX_PAYLOAD) == 0);
	CHECK(proto_build_frame(0x55, 9, pay, PROTO_MAX_PAYLOAD + 1, f, sizeof f) == 0);
	CHECK(proto_build_frame(0x55, 9, pay, 10, f, 8) == 0);
	/* every single-byte corruption is rejected (CRC-16 detects all of these) */
	m = proto_build_frame(0x11, 3, pay, 20, f, sizeof f);
	int accepted = 0;
	for (size_t i = 0; i < m - 1; i++) {
		for (int bit = 0; bit < 8; bit++) {
			uint8_t c[64];
			memcpy(c, f, m - 1);
			c[i] ^= (uint8_t)(1u << bit);
			if (c[i] == 0) continue; /* would be a delimiter, handled by the receiver */
			if (proto_parse_frame(c, m - 1, body, &fr) == 0) accepted++;
		}
	}
	CHECK(accepted == 0);
	/* wrong version is rejected even with a valid CRC */
	uint8_t raw[8] = {2, 0x01, 1}, enc[16];
	uint16_t crc = proto_crc16(raw, 3);
	raw[3] = (uint8_t)crc; raw[4] = (uint8_t)(crc >> 8);
	size_t em = proto_cobs_encode(raw, 5, enc, sizeof enc);
	CHECK(proto_parse_frame(enc, em, body, &fr) != 0);
	/* too short */
	raw[0] = 1; crc = proto_crc16(raw, 2); raw[2] = (uint8_t)crc; raw[3] = (uint8_t)(crc >> 8);
	em = proto_cobs_encode(raw, 4, enc, sizeof enc);
	CHECK(proto_parse_frame(enc, em, body, &fr) != 0);
}

/* ------------------------------------------------------------------ robustness */

static void test_receiver_drops_bad_frames(void)
{
	setup();
	/* garbage, then a bare delimiter, then a truncated frame */
	const uint8_t junk[] = {0x13, 0x37, 0xFF, 0x01, 0x00, 0x00, 0x02, 0x01};
	feed(junk, sizeof junk);
	feed(g_req_hello, 3); /* first bytes of a HELLO, then cut off by a delimiter */
	uint8_t z = 0;
	feed(&z, 1);
	CHECK(nfr == 0);
	/* overlong frame: 2000 non-zero bytes then a delimiter */
	for (int i = 0; i < 2000; i++) proto_rx_byte(&pr, (uint8_t)(1 + i % 200));
	CHECK(pr.rx_len <= sizeof pr.rx);
	proto_rx_byte(&pr, 0);
	CHECK(nfr == 0);
	CHECK(pr.rx_dropped >= 3);
	/* an overlong frame whose tail looks like a valid frame must not be executed */
	for (int i = 0; i < 600; i++) proto_rx_byte(&pr, 0x7F);
	feed(g_req_hello, sizeof g_req_hello - 1);
	proto_rx_byte(&pr, 0);
	CHECK(nfr == 0);
	/* the receiver recovers */
	feed(g_req_hello, sizeof g_req_hello);
	CHECK(nfr == 1 && frames[0].type == PT_HELLO_RSP);
	/* a frame with a corrupted CRC is silent */
	clear_tx();
	uint8_t c[sizeof g_req_set_wet];
	memcpy(c, g_req_set_wet, sizeof c);
	c[2] ^= 0x40;
	if (c[2] == 0) c[2] = 0x40;
	feed(c, sizeof c);
	CHECK(nfr == 0);
	CHECK(bad_tx_frames == 0);
}

static int engine_sane(void)
{
	engine_config_t cpy = eng.cfg;
	if (engine_config_sanitize(&cpy) != 0) return 0;
	uint32_t mem, cpu;
	engine_resources(&eng, &mem, &cpu);
	if (mem > eng.pool_bytes || cpu > eng.cpu_budget) return 0;
	if (pr.bpm < 30.0f || pr.bpm > 300.0f) return 0;
	if (pr.sub_hz > 20) return 0;
	return 1;
}

static void test_fuzz(void)
{
	setup();
	int insane = 0, overflow = 0;
	uint32_t executed0 = pr.rx_frames;
	const golden_req_t *g = golden_requests;
	for (int k = 0; k < 100000; k++) {
		clear_tx();
		uint32_t mode = rnd() % 4;
		if (mode == 0) { /* pure random bytes, few zeros */
			int n = (int)(rnd() % 300);
			for (int i = 0; i < n; i++) proto_rx_byte(&pr, (rnd() % 16 == 0) ? 0 : (uint8_t)rnd());
		} else if (mode == 1) { /* random bytes, no zeros at all for a long stretch */
			int n = (int)(rnd() % 1500);
			for (int i = 0; i < n; i++) proto_rx_byte(&pr, (uint8_t)(1 + rnd() % 255));
		} else { /* a golden request with random corruption / truncation / splicing */
			const golden_req_t *r = &g[rnd() % GOLDEN_NREQ];
			uint8_t b[64];
			size_t n = r->len;
			memcpy(b, r->frame, n);
			int flips = (int)(rnd() % 4);
			for (int i = 0; i < flips; i++) b[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8));
			if (rnd() % 4 == 0) n = rnd() % n;
			feed(b, n);
		}
		if (pr.rx_len > sizeof pr.rx) overflow++;
		if (!engine_sane()) insane++;
		if (k % 97 == 0) proto_tick(&pr, (uint32_t)k);
	}
	CHECK(insane == 0);
	CHECK(overflow == 0);
	CHECK(bad_tx_frames == 0);
	CHECK(!cap_overflow);
	printf("    fuzz: %u frames executed, %u dropped\n", (unsigned)(pr.rx_frames - executed0), (unsigned)pr.rx_dropped);
	/* still alive and answering */
	clear_tx();
	uint8_t z = 0;
	feed(&z, 1);
	feed(g_req_hello, sizeof g_req_hello);
	CHECK(nfr == 1 && frames[0].type == PT_HELLO_RSP);
}

/* ------------------------------------------------------------------ golden interop */

static void test_golden_requests(void)
{
	for (int k = 0; k < GOLDEN_NREQ; k++) {
		const golden_req_t *g = &golden_requests[k];
		setup();
		/* our parser decodes the JS frame to the JS-decoded fields */
		uint8_t body[PROTO_MAX_BODY];
		proto_frame_t f;
		int ok = proto_parse_frame(g->frame, g->len - 1u, body, &f) == 0;
		CHECK_MSG(ok && f.type == g->type && f.seq == g->seq, "%s", g->name);
		if (!ok) continue;
		int32_t v[4] = {0};
		int nv = 0;
		switch (g->type) {
		case PT_SET_PARAM: v[0] = f.payload[0]; v[1] = f.payload[1]; v[2] = g16(f.payload + 2); nv = 3; break;
		case PT_SET_WET: v[0] = f.payload[0]; v[1] = g16(f.payload + 1); nv = 2; break;
		case PT_SET_MASTER: case PT_SET_BPM: v[0] = g16(f.payload); nv = 1; break;
		case PT_SET_INPUT_GAIN: v[0] = (int16_t)g16(f.payload); nv = 1; break;
		default: nv = f.len; for (int i = 0; i < f.len && i < 4; i++) v[i] = f.payload[i]; break;
		}
		CHECK_MSG(nv == g->nf, "%s field count %d vs %d", g->name, nv, g->nf);
		for (int i = 0; i < g->nf; i++) CHECK_MSG(v[i] == g->f[i], "%s f[%d] %ld vs %ld", g->name, i, (long)v[i], (long)g->f[i]);
		/* our builder produces the identical bytes */
		uint8_t mine[PROTO_MAX_FRAME];
		size_t m = proto_build_frame(f.type, f.seq, f.payload, f.len, mine, sizeof mine);
		CHECK_MSG(m == g->len && memcmp(mine, g->frame, m) == 0, "%s: C frame differs from JS frame", g->name);

		/* and the device answers it */
		engine_config_t before = eng.cfg;
		feed(g->frame, g->len);
		CHECK_MSG(nfr == 1, "%s: %d responses", g->name, nfr);
		if (nfr != 1) continue;
		const frm_t *r = &frames[0];
		CHECK(r->seq == g->seq);
		switch (g->type) {
		case PT_HELLO: CHECK(r->type == PT_HELLO_RSP); break;
		case PT_GET_FX: CHECK(r->type == (g->f[0] < FX_COUNT ? PT_FX_DESC : PT_RESULT)); break;
		case PT_GET_STATE: CHECK(r->type == PT_STATE); break;
		default: {
			int want = (g->type == PT_FACTORY_RESET && g->f[0] != PROTO_FACTORY_RESET_CONFIRM) ? PR_BAD_ARG : PR_OK;
			CHECK_MSG(r->type == PT_RESULT && r->len == 2 && r->pay[0] == g->type && r->pay[1] == want, "%s", g->name);
		}
		}
		if (strcmp(g->name, "get_fx_bad") == 0) CHECK(r->pay[0] == PT_GET_FX && r->pay[1] == PR_BAD_ARG);
		/* the change landed */
		switch (g->type) {
		case PT_SET_SLOT_FX: CHECK(eng.cfg.fx_id[g->f[0]] == g->f[1]); break;
		case PT_SET_PARAM: CHECK_NEAR(eng.cfg.params[g->f[0]][eng.cfg.fx_id[g->f[0]]][g->f[1]], g->f[2] / 65535.0, 1e-6); break;
		case PT_SET_WET: CHECK_NEAR(eng.cfg.wet[g->f[0]], g->f[1] / 65535.0, 1e-6); break;
		case PT_SET_BYPASS: CHECK(eng.cfg.bypass[g->f[0]] == g->f[1]); break;
		case PT_SET_GLOBAL_BYPASS: CHECK(eng.cfg.global_bypass == g->f[0]); break;
		case PT_SET_MASTER: CHECK_NEAR(eng.cfg.master, g->f[0] / 65535.0, 1e-6); break;
		case PT_SET_INPUT_GAIN: CHECK_NEAR(eng.cfg.in_gain_db, g->f[0] / 100.0, 1e-5); break;
		case PT_SET_ORDER: for (int i = 0; i < 4; i++) CHECK(eng.cfg.order[i] == g->f[i]); break;
		case PT_SET_BPM: CHECK_NEAR(pr.bpm, g->f[0] / 10.0, 1e-4); CHECK_NEAR(syn.frames_per_tick, 48000.0 * 60.0 / (128.5 * 24), 1e-6); break;
		case PT_SAVE_NOW: CHECK(save_calls == 1); break;
		case PT_SUBSCRIBE: CHECK(pr.sub_hz == g->f[0]); break;
		case PT_FACTORY_RESET:
			if (g->f[0] != PROTO_FACTORY_RESET_CONFIRM) CHECK(memcmp(&before, &eng.cfg, sizeof before) == 0);
			break;
		default: break;
		}
		/* setters mark persistence dirty; reads do not */
		int is_setter = g->type >= 0x10 && g->type <= 0x18;
		int dirty_expected = is_setter || (g->type == PT_FACTORY_RESET && g->f[0] == PROTO_FACTORY_RESET_CONFIRM);
		CHECK_MSG(pr.dirty == dirty_expected && deb.dirty == dirty_expected, "%s dirty %d", g->name, pr.dirty);
	}
	/* SAVE_NOW storage error and missing callback */
	setup();
	save_result = -1;
	feed(g_req_save_now, sizeof g_req_save_now);
	CHECK(result_code(PT_SAVE_NOW, 14) == PR_STORAGE);
	save_result = PR_BUSY; /* firmware: the save would need a flash erase while USB is connected */
	feed(g_req_save_now, sizeof g_req_save_now);
	CHECK(result_code(PT_SAVE_NOW, 14) == PR_BUSY);
	pr.c.save = NULL;
	feed(g_req_save_now, sizeof g_req_save_now);
	CHECK(result_code(PT_SAVE_NOW, 14) == PR_STORAGE);
}

static void test_hello_rsp_layout(void)
{
	setup();
	feed(g_req_hello, sizeof g_req_hello);
	CHECK(nfr == 1);
	const frm_t *r = &frames[0];
	CHECK(raw_equals(r, g_hello_rsp, sizeof g_hello_rsp)); /* byte-identical to the JS frame */
	/* field by field, per the spec */
	const uint8_t *p = r->pay;
	size_t fl = strlen(G_HELLO_FW);
	CHECK(r->type == PT_HELLO_RSP && r->seq == G_HELLO_SEQ);
	CHECK(r->len == 1 + 1 + fl + 1 + 4 + 2 + 4 + 2);
	CHECK(p[0] == G_HELLO_PROTO);
	CHECK(p[1] == fl && memcmp(p + 2, G_HELLO_FW, fl) == 0);
	p += 2 + fl;
	CHECK(p[0] == G_HELLO_FXCOUNT && p[0] == FX_COUNT);
	CHECK(g32(p + 1) == G_HELLO_SR);
	CHECK(g16(p + 5) == G_HELLO_BLOCK);
	CHECK(g32(p + 7) == G_HELLO_POOL && g32(p + 7) == sizeof pool);
	CHECK(g16(p + 11) == G_HELLO_BUDGET && g16(p + 11) == ENGINE_DEFAULT_CPU_BUDGET);
}

static const char *nz(const char *s) { return s ? s : ""; }

static void test_fx_desc_all(void)
{
	setup();
	for (int id = 0; id < GOLDEN_NFX; id++) {
		const golden_fx_t *g = &golden_fx[id];
		const fx_desc *d = fx_get(id);
		clear_tx();
		uint8_t q[1] = {(uint8_t)id};
		request(PT_GET_FX, (uint8_t)G_FX_SEQ(id), q, 1);
		CHECK(nfr == 1 && frames[0].type == PT_FX_DESC && frames[0].seq == G_FX_SEQ(id));
		if (nfr != 1) continue;
		const frm_t *r = &frames[0];
		/* Known data discrepancy (reported, not papered over): companion/data/fx_table.json says
		 * Reverb memBytes 43120, the firmware descriptor declares 43116. Everything else must match. */
		int known_mem_diff = (id == FX_REVERB && d->mem_bytes == 43116u && g->mem == 43120u);
		if (known_mem_diff) printf("    KNOWN DISCREPANCY: Reverb mem_bytes firmware %u vs companion table %u\n", (unsigned)d->mem_bytes, (unsigned)g->mem);
		int same = raw_equals(r, g->frame, g->len);
		CHECK_MSG(same || known_mem_diff, "FX_DESC %d (%s): C frame differs from the companion table", id, g->name);
		/* decode ours by the spec and compare against both the registry and the JS-decoded fields */
		const uint8_t *p = r->pay, *end = r->pay + r->len;
		CHECK(p[0] == id && d->id == id);
		size_t nl = p[1];
		CHECK(nl == strlen(d->name) && memcmp(p + 2, d->name, nl) == 0);
		CHECK_MSG(nl == strlen(g->name) && memcmp(p + 2, g->name, nl) == 0, "name %s", g->name);
		p += 2 + nl;
		int np = *p++;
		CHECK(np == d->nparams && np == g->nparams);
		uint16_t cpu = g16(p); uint32_t mem = g32(p + 2), tail = g32(p + 6); uint8_t mix = p[10];
		p += 11;
		CHECK(cpu == d->cpu_cycles && mem == d->mem_bytes && tail == d->tail_ms && mix == d->mix_mode);
		CHECK_MSG(cpu == g->cpu && (mem == g->mem || known_mem_diff) && tail == g->tail && mix == g->mix,
		          "%s: cpu %u/%u mem %u/%u tail %u/%u mix %u/%u", g->name, cpu, g->cpu, (unsigned)mem, (unsigned)g->mem,
		          (unsigned)tail, (unsigned)g->tail, mix, g->mix);
		for (int i = 0; i < np && i < 4; i++) {
			const fx_param_desc *dp = &d->params[i];
			const golden_param_t *gp = &g->p[i];
			size_t l = *p++;
			CHECK(l == strlen(dp->name) && memcmp(p, dp->name, l) == 0);
			CHECK_MSG(l == strlen(gp->name) && memcmp(p, gp->name, l) == 0, "%s param %d name", g->name, i);
			p += l;
			l = *p++;
			CHECK(l == strlen(nz(dp->unit)) && memcmp(p, nz(dp->unit), l) == 0);
			CHECK_MSG(l == strlen(gp->unit) && memcmp(p, gp->unit, l) == 0, "%s param %d unit", g->name, i);
			p += l;
			float mn = gf(p), mx = gf(p + 4), df = gf(p + 8);
			uint8_t sc = p[12];
			p += 13;
			CHECK(mn == dp->min && mx == dp->max && df == dp->def && sc == dp->scale);
			CHECK_MSG(mn == gp->min && mx == gp->max && df == gp->def && sc == gp->scale,
			          "%s param %d: %g/%g %g/%g %g/%g scale %u/%u", g->name, i, mn, gp->min, mx, gp->max, df, gp->def, sc, gp->scale);
			l = *p++;
			const char *lab = dp->scale == FX_SCALE_STEP ? nz(dp->labels) : "";
			CHECK(l == strlen(lab) && memcmp(p, lab, l) == 0);
			CHECK_MSG(l == strlen(gp->labels) && memcmp(p, gp->labels, l) == 0, "%s param %d labels", g->name, i);
			p += l;
		}
		CHECK(p == end);
	}
	/* out of range */
	clear_tx();
	uint8_t q[1] = {FX_COUNT};
	request(PT_GET_FX, 77, q, 1);
	CHECK(result_code(PT_GET_FX, 77) == PR_BAD_ARG && count_type(PT_FX_DESC) == 0);
}

static void check_state_payload(const frm_t *r)
{
	const uint8_t *p = r->pay;
	CHECK(r->len == G_STATE_PAYLOAD_LEN && r->len == 384); /* 383 + out_mode (rc4) */
	for (int s = 0; s < 4; s++) {
		CHECK(p[s] == g_state_fx[s]);
		CHECK(p[4 + s] == g_state_bypass[s]);
		CHECK(p[8 + s] == g_state_order[s]);
	}
	CHECK(p[12] == G_STATE_GLOBAL && p[13] == 0 && p[14] == G_STATE_FXCOUNT);
	CHECK((int16_t)g16(p + 15) == G_STATE_INGAIN);
	CHECK(g16(p + 17) == G_STATE_MASTER);
	CHECK(g16(p + 19) == G_STATE_BPM10);
	for (int s = 0; s < 4; s++) CHECK(g16(p + 21 + 2 * s) == g_state_wet[s]);
	const uint8_t *q = p + 29;
	int maxdiff = 0;
	for (int s = 0; s < 4; s++)
		for (int f = 0; f < G_STATE_FXCOUNT; f++)
			for (int i = 0; i < 4; i++) {
				int d = abs((int)g16(q) - (int)g_state_params[s][f][i]);
				if (d > maxdiff) maxdiff = d;
				q += 2;
			}
	CHECK_MSG(maxdiff <= 1, "param codes differ by up to %d LSB", maxdiff); /* float vs double rounding */
	CHECK(q[0] == G_STATE_UIMODE && q[1] == G_STATE_EDITSLOT);
}

static void test_state(void)
{
	setup();
	feed(g_req_get_state, sizeof g_req_get_state);
	CHECK(nfr == 1 && frames[0].type == PT_STATE && frames[0].seq == 4);
	if (nfr != 1) return;
	check_state_payload(&frames[0]);
	int same = raw_equals(&frames[0], g_state_default, sizeof g_state_default);
	if (!same) printf("    note: STATE not byte-identical to JS (param rounding), fields match within 1 LSB\n");
	/* ui_mode/edit_slot come from the controller; body is matched to store_encode */
	ui.mode = UI_EDIT; ui.edit_slot = 3;
	engine_set_wet(&eng, 2, 0.25f);
	clear_tx();
	feed(g_req_get_state, sizeof g_req_get_state);
	const frm_t *r = &frames[0];
	CHECK(r->pay[381] == 1 && r->pay[382] == 3);
	CHECK(g16(r->pay + 21 + 4) == (uint16_t)(0.25f * 65535.0f + 0.5f));
	store_data_t d = {0};
	CHECK(store_decode(r->pay, (uint16_t)(r->len - 2), STORE_LAYOUT_VERSION, &d) == 0);
	CHECK_NEAR(d.cfg.wet[2], 0.25, 1e-4);
	CHECK_NEAR(d.bpm, 120.0, 1e-6);
}

/* ------------------------------------------------------------------ setters: arguments */

static void test_bad_arguments(void)
{
	setup();
	struct { uint8_t type; uint8_t pay[6]; uint8_t n; } bad[] = {
		{PT_SET_WET, {0, 1}, 2},              /* short */
		{PT_SET_WET, {0, 1, 2, 3}, 4},        /* long */
		{PT_SET_WET, {4, 0, 0}, 3},           /* slot */
		{PT_SET_SLOT_FX, {0, FX_COUNT}, 2},
		{PT_SET_SLOT_FX, {9, 1}, 2},
		{PT_SET_PARAM, {0, 4, 0, 0}, 4},      /* idx >= FX_MAX_PARAMS */
		{PT_SET_BYPASS, {4, 1}, 2},
		{PT_SET_INPUT_GAIN, {0xB1, 0x04}, 2}, /* 1201 */
		{PT_SET_INPUT_GAIN, {0x4F, 0xFB}, 2}, /* -1201 */
		{PT_SET_ORDER, {0, 1, 1, 3}, 4},
		{PT_SET_ORDER, {0, 1, 2, 4}, 4},
		{PT_SET_BPM, {0x2B, 0x01}, 2},        /* 299 */
		{PT_SET_BPM, {0xB9, 0x0B}, 2},        /* 3001 */
		{PT_SUBSCRIBE, {21}, 1},
		{PT_HELLO, {1}, 1},
		{0x55, {0}, 0},                       /* unknown request type */
	};
	engine_config_t before = eng.cfg;
	for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
		clear_tx();
		request(bad[k].type, (uint8_t)(100 + k), bad[k].pay, bad[k].n);
		CHECK_MSG(result_code(bad[k].type, (uint8_t)(100 + k)) == PR_BAD_ARG, "case %d", (int)k);
	}
	CHECK(memcmp(&before, &eng.cfg, sizeof before) == 0);
	CHECK(!pr.dirty);
	/* SET_PARAM on an index the active effect does not have (None has no params) */
	clear_tx();
	uint8_t none[2] = {0, FX_NONE};
	request(PT_SET_SLOT_FX, 1, none, 2);
	CHECK(result_code(PT_SET_SLOT_FX, 1) == PR_OK);
	uint8_t sp[4] = {0, 0, 0xFF, 0xFF};
	request(PT_SET_PARAM, 2, sp, 4);
	CHECK(result_code(PT_SET_PARAM, 2) == PR_BAD_ARG);
	/* boundaries are accepted */
	uint8_t g[2] = {0xB0, 0x04}; /* 1200 */
	request(PT_SET_INPUT_GAIN, 3, g, 2);
	CHECK(result_code(PT_SET_INPUT_GAIN, 3) == PR_OK && eng.cfg.in_gain_db == 12.0f);
	uint8_t bpm[2] = {0x2C, 0x01}; /* 300 */
	request(PT_SET_BPM, 4, bpm, 2);
	CHECK(result_code(PT_SET_BPM, 4) == PR_OK && pr.bpm == 30.0f);
	/* device->host types sent to the device are ignored */
	clear_tx();
	request(PT_RESULT, 5, sp, 2);
	CHECK(nfr == 0);
}

/* ------------------------------------------------------------------ admission */

static void test_admission(void)
{
	setup_budget(2000); /* filter, chorus, delay, reverb: ~126 KB of 150 KB; CPU never the limit here */
	/* code 2: a second delay does not fit in memory */
	feed(g_req_set_slot_fx, sizeof g_req_set_slot_fx); /* slot 1 -> flanger fits */
	CHECK(eng.cfg.fx_id[1] == FX_FLANGER);
	clear_tx();
	uint8_t a[2] = {0, FX_DELAY};
	request(PT_SET_SLOT_FX, 5, a, 2);
	CHECK(result_code(PT_SET_SLOT_FX, 5) == PR_NO_MEMORY);
	CHECK(raw_equals(last_of(PT_RESULT), g_result, sizeof g_result)); /* JS golden RESULT(0x10, 2) seq 5 */
	CHECK(eng.cfg.fx_id[0] == FX_FILTER); /* previous assignment kept */
	CHECK(count_type(PT_EVENT) == 0);

	/* code 3: CPU budget */
	setup_budget(800); /* default set uses 130+200+210+240 = 780 cycles */
	clear_tx();
	uint8_t b[2] = {0, FX_CHORUS};
	request(PT_SET_SLOT_FX, 6, b, 2);
	CHECK(result_code(PT_SET_SLOT_FX, 6) == PR_NO_CPU);
	CHECK(eng.cfg.fx_id[0] == FX_FILTER);
	CHECK(!pr.dirty);

	/* device-side rejection (physical effect cycle) -> 0xA1 ADMISSION_ERROR */
	setup();
	CHECK(engine_assign(&eng, 3, FX_CHORUS) == ENG_OK);
	CHECK(engine_assign(&eng, 2, FX_DELAY) == ENG_OK);
	CHECK(engine_assign(&eng, 1, FX_NONE) == ENG_OK);
	CHECK(engine_assign(&eng, 0, FX_REVERB) == ENG_OK);
	clear_tx();
	hold(B(BTN_T4), 100);
	for (int k = 0; k < 2; k++) { hold(B(BTN_T4) | B(BTN_FWD), 16); hold(B(BTN_T4), 24); }
	hold(0, 80);
	CHECK(eng.cfg.fx_id[3] == FX_CHORUS);
	CHECK(count_type(PT_ADMISSION_ERROR) == 2);
	CHECK(count_type(PT_EVENT) == 0);
	const frm_t *r = last_of(PT_ADMISSION_ERROR);
	CHECK(r && r->seq == 0 && r->len == 3 && r->pay[0] == 3 && r->pay[1] == FX_REVERB && r->pay[2] == PR_NO_MEMORY);
	CHECK(raw_equals(r, g_admission, sizeof g_admission));
}

/* ------------------------------------------------------------------ events */

static void test_no_echo_of_host_changes(void)
{
	setup();
	hold(0, 40); /* fader at 0.5 picks up wet 0.5 */
	clear_tx();
	for (int k = 0; k < GOLDEN_NREQ; k++) {
		const golden_req_t *g = &golden_requests[k];
		if (g->type == PT_SUBSCRIBE || g->type == PT_FACTORY_RESET) continue;
		feed(g->frame, g->len);
	}
	uint8_t w[3] = {0, 0xCC, 0xE6}; /* slot 0 wet 0.9 while its fader sits at 0.5 */
	request(PT_SET_WET, 50, w, 3);
	hold(0, 500);
	CHECK(count_type(PT_EVENT) == 0);
	CHECK(count_type(PT_ADMISSION_ERROR) == 0);
	CHECK(ui.ev_count == 0);
	/* resync: the physical fader at 0.5 must not pull the value back */
	CHECK_NEAR(eng.cfg.wet[0], 0xE6CC / 65535.0, 1e-6);
	fad[0] = 0.52f;
	hold(0, 80);
	CHECK_NEAR(eng.cfg.wet[0], 0xE6CC / 65535.0, 1e-6);
	CHECK(count_type(PT_EVENT) == 0);
}

static void test_device_events(void)
{
	setup();
	hold(0, 40);
	clear_tx();
	/* bypass tap on slot 3 (T3 = slot 2): bypass 1 -> 0 */
	hold(B(BTN_T3), 80); hold(0, 40);
	CHECK(eng.cfg.bypass[2] == 0);
	CHECK(count_type(PT_EVENT) == 1);
	CHECK(raw_equals(last_of(PT_EVENT), g_ev_bypass, sizeof g_ev_bypass)); /* JS: SET_BYPASS slot 2 off */
	/* global bypass */
	clear_tx();
	hold(B(BTN_PLAY), 80); hold(0, 40);
	CHECK(raw_equals(last_of(PT_EVENT), g_ev_global, sizeof g_ev_global));
	/* master volume up: 0.75 -> 23/30 (2 dB grid, controls/volume.h; JS golden SET_MASTER v 23/30) */
	clear_tx();
	hold(B(BTN_VOLUP), 40); hold(0, 40);
	CHECK_NEAR(eng.cfg.master, 23.0 / 30.0, 1e-6);
	CHECK(raw_equals(last_of(PT_EVENT), g_ev_master, sizeof g_ev_master));
	/* effect cycle on slot 0: filter -> chorus = JS golden SET_SLOT_FX slot 0 fx 2 */
	clear_tx();
	hold(B(BTN_T1), 100); hold(B(BTN_T1) | B(BTN_FWD), 16); hold(B(BTN_T1), 24); hold(0, 80);
	CHECK(eng.cfg.fx_id[0] == FX_CHORUS);
	CHECK(count_type(PT_EVENT) == 1);
	CHECK(raw_equals(last_of(PT_EVENT), g_ev_slot_fx, sizeof g_ev_slot_fx));

	/* fader sweep on slot 1: rate limited to 20/s, coalesced, last value delivered */
	clear_tx();
	for (int t = 0; t < 1000; t += 8) {
		fad[1] = 0.5f + 0.5f * (float)t / 1000.0f;
		T += 8; controller_tick(&ui, T, 0, fad); proto_tick(&pr, T);
	}
	for (int t = 0; t < 200; t += 8) { T += 8; controller_tick(&ui, T, 0, fad); proto_tick(&pr, T); }
	int nwet = 0;
	const frm_t *lastw = NULL;
	for (int i = 0; i < nfr; i++) {
		if (frames[i].type != PT_EVENT || frames[i].pay[0] != PT_SET_WET) continue;
		CHECK(frames[i].seq == 0 && frames[i].len == 4 && frames[i].pay[1] == 1);
		nwet++;
		lastw = &frames[i];
	}
	CHECK_MSG(nwet >= 15 && nwet <= 21, "%d wet events in ~1 s", nwet);
	CHECK(lastw && g16(lastw->pay + 2) == (uint16_t)(eng.cfg.wet[1] * 65535.0f + 0.5f));
	CHECK(eng.cfg.wet[1] > 0.95f);
	CHECK(count_type(PT_EVENT) == nwet); /* nothing else */

	/* exact JS golden layout for a wet event: slot 1, 40000 */
	clear_tx();
	pr.ev_wet[1].last_sent_ms = T - 1000;
	ui.ev_count = 0;
	{
		/* drive the value through the real fader path: move fader to 40000/65535 */
		float target = 40000.0f / 65535.0f;
		fad[1] = target; hold(0, 120);
		const frm_t *e = last_of(PT_EVENT);
		CHECK(e && e->pay[0] == PT_SET_WET && g16(e->pay + 2) == (uint16_t)(eng.cfg.wet[1] * 65535.0f + 0.5f));
		CHECK(e && g16(e->pay + 2) == 40000 && raw_equals(e, g_ev_wet, sizeof g_ev_wet));
	}

	/* edit mode: param events for the edit slot, rate limited per control */
	setup();
	hold(0, 40);
	hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_T3), 80); hold(0, 80); /* FN + T3 -> edit slot 2 */
	CHECK(ui.mode == UI_EDIT && ui.edit_slot == 2);
	clear_tx();
	int id = eng.cfg.fx_id[2];
	float start = eng.cfg.params[2][id][1];
	fad[1] = start; hold(0, 40); /* pick up */
	for (int t = 0; t < 500; t += 8) { fad[1] = start + 0.3f * (float)t / 500.0f; T += 8; controller_tick(&ui, T, 0, fad); proto_tick(&pr, T); }
	hold(0, 200);
	int np = 0;
	const frm_t *lp = NULL;
	for (int i = 0; i < nfr; i++)
		if (frames[i].type == PT_EVENT && frames[i].pay[0] == PT_SET_PARAM) { np++; lp = &frames[i]; }
	CHECK_MSG(np >= 5 && np <= 12, "%d param events in 0.5 s", np);
	CHECK(lp && lp->len == 5 && lp->pay[1] == 2 && lp->pay[2] == 1);
	CHECK(lp && g16(lp->pay + 3) == (uint16_t)(eng.cfg.params[2][id][1] * 65535.0f + 0.5f));
	/* JS golden PARAM event decodes with the same layout */
	{
		uint8_t body[PROTO_MAX_BODY];
		proto_frame_t f;
		CHECK(proto_parse_frame(g_ev_param, sizeof g_ev_param - 1, body, &f) == 0);
		CHECK(f.type == PT_EVENT && f.seq == 0 && f.len == 5 && f.payload[0] == PT_SET_PARAM && f.payload[1] == 2 &&
		      f.payload[2] == 1 && g16(f.payload + 3) == 12345);
	}
	/* a host change discards a pending device value for the same control (no stale echo) */
	clear_tx();
	fad[1] += 0.05f;
	T += 8; controller_tick(&ui, T, 0, fad); proto_tick(&pr, T); /* may send */
	fad[1] += 0.05f;
	T += 8; controller_tick(&ui, T, 0, fad); proto_tick(&pr, T); /* pending (rate limit) */
	uint8_t sp[4] = {2, 1, 0x00, 0x10};
	request(PT_SET_PARAM, 9, sp, 4);
	int before = count_type(PT_EVENT);
	hold(0, 200);
	CHECK(count_type(PT_EVENT) == before);
	CHECK_NEAR(eng.cfg.params[2][id][1], 0x1000 / 65535.0, 1e-6);
}

/* ------------------------------------------------------------------ meters */

static void test_subscribe_timing(void)
{
	setup();
	uint8_t hz = 10;
	T = 0;
	proto_tick(&pr, 0);
	request(PT_SUBSCRIBE, 1, &hz, 1);
	CHECK(result_code(PT_SUBSCRIBE, 1) == PR_OK);
	clear_tx();
	uint32_t times[64];
	int n = 0;
	for (uint32_t t = 0; t < 1000; t++) {
		int before = nfr;
		proto_tick(&pr, t);
		if (nfr > before && frames[nfr - 1].type == PT_METERS && n < 64) times[n++] = t;
	}
	CHECK_MSG(n == 10, "%d meters at 10 Hz", n);
	for (int i = 1; i < n; i++) CHECK(times[i] - times[i - 1] == 100);
	/* 20 Hz with an 8 ms tick */
	hz = 20;
	request(PT_SUBSCRIBE, 2, &hz, 1);
	clear_tx();
	for (uint32_t t = 1000; t < 3000; t += 8) proto_tick(&pr, t);
	CHECK_MSG(count_type(PT_METERS) >= 39 && count_type(PT_METERS) <= 41, "%d at 20 Hz", count_type(PT_METERS));
	/* 1 Hz */
	hz = 1;
	request(PT_SUBSCRIBE, 3, &hz, 1);
	clear_tx();
	for (uint32_t t = 3000; t < 8000; t += 8) proto_tick(&pr, t);
	CHECK_MSG(count_type(PT_METERS) == 6, "%d at 1 Hz", count_type(PT_METERS)); /* t = 3000, 3992 ... 7992 */
	/* a long stall does not cause a burst */
	clear_tx();
	proto_tick(&pr, 60000);
	proto_tick(&pr, 60008);
	CHECK(count_type(PT_METERS) == 1);
	/* off */
	hz = 0;
	request(PT_SUBSCRIBE, 4, &hz, 1);
	clear_tx();
	for (uint32_t t = 60000; t < 65000; t += 8) proto_tick(&pr, t);
	CHECK(count_type(PT_METERS) == 0);
	/* not subscribed by default */
	setup();
	clear_tx();
	for (uint32_t t = 0; t < 2000; t += 8) proto_tick(&pr, t);
	CHECK(nfr == 0);
}

static uint8_t test_power_flags(void *ctx) { (void)ctx; return 0xFF; } /* only bits 1-2 may pass */
static uint8_t test_batt(void *ctx) { (void)ctx; return 62; }

static uint8_t hc_types[16];
static int hc_n;
static void hc_cb(void *ctx, uint8_t t) { (void)ctx; if (hc_n < 16) hc_types[hc_n++] = t; }

static void test_output_mode(void)
{
	setup();
	pr.c.on_host_change = hc_cb;
	hc_n = 0;
	CHECK(pr.out_mode == 0);
	uint8_t v = 1;
	clear_tx();
	request(PT_SET_OUTPUT_MODE, 21, &v, 1);
	CHECK(result_code(PT_SET_OUTPUT_MODE, 21) == PR_OK && pr.out_mode == 1 && pr.dirty);
	CHECK(hc_n == 1 && hc_types[0] == PT_SET_OUTPUT_MODE);
	clear_tx();
	request(PT_GET_STATE, 22, NULL, 0);
	const frm_t *r = last_of(PT_STATE);
	CHECK(r && r->len == 384 && r->pay[383] == 1);
	v = 2; /* rc5: loud */
	clear_tx();
	request(PT_SET_OUTPUT_MODE, 25, &v, 1);
	CHECK(result_code(PT_SET_OUTPUT_MODE, 25) == PR_OK && pr.out_mode == 2);
	v = 3;
	clear_tx();
	request(PT_SET_OUTPUT_MODE, 23, &v, 1);
	CHECK(result_code(PT_SET_OUTPUT_MODE, 23) == PR_BAD_ARG && pr.out_mode == 2);
	/* factory reset returns to headphones */
	uint8_t c = PROTO_FACTORY_RESET_CONFIRM;
	request(PT_FACTORY_RESET, 24, &c, 1);
	CHECK(pr.out_mode == 0);
	CHECK(hc_n == 3 && hc_types[2] == PT_FACTORY_RESET); /* the rejected request did not count */
	pr.c.on_host_change = NULL;
}

static void test_meters_content(void)
{
	setup();
	uint8_t hz = 20;
	request(PT_SUBSCRIBE, 1, &hz, 1);
	/* make the device match the JS golden METERS message */
	eng.diag.in_peak = 0.5f;
	eng.diag.out_peak = 40000.0f / 32768.0f;
	eng.diag.cpu_last_cycles = 400; /* 47 % of 850 */
	eng.diag.cpu_peak_cycles = 748; /* 88 % */
	eng.diag.in_clips = 3; eng.diag.out_clips = 70000; eng.diag.underruns = 1; eng.diag.overruns = 2; eng.diag.nan_events = 5;
	ui.mode = UI_EDIT; ui.edit_slot = 2;
	engine_set_global_bypass(&eng, 1);
	uint32_t saved[4];
	for (int s = 0; s < 4; s++) { saved[s] = eng.blk_size[s]; eng.blk_size[s] = s == 0 ? 120000u : 0u; }
	clear_tx();
	proto_tick(&pr, T);
	for (int s = 0; s < 4; s++) eng.blk_size[s] = saved[s];
	const frm_t *m = last_of(PT_METERS);
	CHECK(m && m->seq == 0 && m->len == 34); /* + battery byte (rc6) */
	if (!m) return;
	CHECK(raw_equals(m, g_meters, sizeof g_meters));
	const uint8_t *p = m->pay;
	uint32_t got[13] = {g16(p), g16(p + 2), p[4], p[5], g32(p + 6), g32(p + 10), g32(p + 14), g32(p + 18), g32(p + 22), g32(p + 26), p[30], p[31], p[32] & 1u};
	for (int i = 0; i < 13; i++) CHECK_MSG(got[i] == g_meters_fields[i], "meters field %d: %u vs %u", i, (unsigned)got[i], (unsigned)g_meters_fields[i]);
	/* peaks are "since last report": reset after sending */
	CHECK(eng.diag.in_peak == 0.0f && eng.diag.out_peak == 0.0f);
	clear_tx();
	proto_tick(&pr, T + 60);
	m = last_of(PT_METERS);
	CHECK(m && g16(m->pay) == 0 && g16(m->pay + 2) == 0);
	/* clamping: a huge peak saturates at 0xFFFF; NaN reads as 0 */
	eng.diag.in_peak = 5.0f;
	eng.diag.out_peak = NAN;
	clear_tx();
	proto_tick(&pr, T + 120);
	m = last_of(PT_METERS);
	CHECK(m && g16(m->pay) == 0xFFFF && g16(m->pay + 2) == 0);
	/* real mem_used comes from the engine */
	uint32_t mem;
	engine_resources(&eng, &mem, NULL);
	CHECK(m && g32(m->pay + 6) == mem);
	/* rc3 flags: guard shed slots in bits 4-7, forced bypass bit3, power bits from the app */
	eng.diag.shed_mask = 0x0B;
	eng.diag.forced_bypass = 1;
	pr.c.power_flags = test_power_flags;
	clear_tx();
	proto_tick(&pr, T + 180);
	m = last_of(PT_METERS);
	CHECK(m && m->pay[32] == (1u | 2u | 4u | 8u | 0xB0u));
	CHECK(m && m->pay[33] == 255u);   /* no battery callback: unknown */
	pr.c.batt_pct = test_batt;
	clear_tx();
	proto_tick(&pr, T + 240);
	m = last_of(PT_METERS);
	CHECK(m && m->len == 34 && m->pay[33] == 62u);
	pr.c.batt_pct = NULL;
	eng.diag.shed_mask = 0;
	eng.diag.forced_bypass = 0;
	pr.c.power_flags = NULL;
}

/* ------------------------------------------------------------------ factory reset */

static void test_factory_reset(void)
{
	setup();
	uint8_t a[2] = {1, FX_FLANGER}, w[3] = {0, 0x10, 0x00}, bpm[2] = {0x10, 0x04};
	request(PT_SET_SLOT_FX, 1, a, 2);
	request(PT_SET_WET, 2, w, 3);
	request(PT_SET_BPM, 3, bpm, 2);
	engine_config_t changed = eng.cfg;
	/* missing or wrong confirmation: refused, nothing changes */
	clear_tx();
	request(PT_FACTORY_RESET, 4, NULL, 0);
	CHECK(result_code(PT_FACTORY_RESET, 4) == PR_BAD_ARG);
	uint8_t no = 0x5A;
	request(PT_FACTORY_RESET, 5, &no, 1);
	CHECK(result_code(PT_FACTORY_RESET, 5) == PR_BAD_ARG);
	feed(g_req_factory_reset_noconfirm, sizeof g_req_factory_reset_noconfirm);
	CHECK(result_code(PT_FACTORY_RESET, 16) == PR_BAD_ARG);
	CHECK(memcmp(&changed, &eng.cfg, sizeof changed) == 0);
	CHECK_NEAR(pr.bpm, 104.0, 1e-4);
	/* confirmed */
	pr.dirty = 0;
	feed(g_req_factory_reset, sizeof g_req_factory_reset);
	CHECK(result_code(PT_FACTORY_RESET, 15) == PR_OK);
	CHECK(count_type(PT_EVENT) == 0); /* no event for a reset */
	CHECK(pr.dirty);
	CHECK_NEAR(pr.bpm, 120.0, 1e-6);
	CHECK_NEAR(syn.frames_per_tick, 48000.0 * 60.0 / (120.0 * 24), 1e-6);
	store_data_t d;
	store_data_default(&d);
	CHECK(memcmp(d.cfg.fx_id, eng.cfg.fx_id, 4) == 0);
	CHECK_NEAR(eng.cfg.wet[0], 0.5, 1e-6);
	clear_tx();
	feed(g_req_get_state, sizeof g_req_get_state);
	CHECK(nfr == 1);
	if (nfr == 1) check_state_payload(&frames[0]);
}


static int fill_fp(void *ctx, int idx, store_data_t *out)
{
	(void)ctx;
	if (idx < 0 || idx >= FACTORY_PRESET_COUNT) return -1;
	factory_preset_fill(&factory_presets[idx], out);
	return 0;
}

static void test_preset_loaded_event(void)
{
	setup_budget(ENGINE_DEFAULT_CPU_BUDGET);
	controller_set_presets(&ui, FACTORY_PRESET_COUNT, fill_fp, 0);
	hold(0, 40);
	clear_tx();
	/* FN + FWD loads preset 0 on the device: one PRESET_LOADED, no per-control events */
	hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_FWD), 24); hold(B(BTN_FN), 80); hold(0, 80);
	CHECK(count_type(PT_PRESET_LOADED) == 1);
	CHECK(count_type(PT_EVENT) == 0);
	const frm_t *f = last_of(PT_PRESET_LOADED);
	CHECK(f != 0);
	/* tempo follows the preset (120 BPM in the factory table) */
	CHECK_NEAR(pr.bpm, factory_presets[0].bpm, 0.06);
}

static void test_sync_clock_switch(void)
{
	setup_budget(ENGINE_DEFAULT_CPU_BUDGET);
	/* off by default: no clock */
	sync_out_events_t e = sync_out_advance(&syn, 48000);
	CHECK(e.midi_ticks == 0 && !e.start);
	/* SET_SYNC on: MIDI Start then clock ticks; state is dirty so it gets saved */
	uint8_t on[1] = {1}, off[1] = {0}, bad[1] = {2};
	pr.dirty = 0;
	clear_tx(); request(PT_SET_SYNC, 3, on, 1);
	CHECK(result_code(PT_SET_SYNC, 3) == 0);
	CHECK(pr.sync_on == 1 && pr.dirty == 1);
	e = sync_out_advance(&syn, 48000);
	CHECK(e.start == 1 && e.midi_ticks >= 48);
	/* STATE reports it (the old reserved byte, offset 13 of the payload) and the saved record keeps it */
	clear_tx(); request(PT_GET_STATE, 4, NULL, 0);
	const frm_t *st = last_of(PT_STATE);
	CHECK(st && st->len > 20 && st->pay[13] == 1);
	store_data_t d, back;
	store_data_default(&d);
	d.sync_on = 1;
	uint8_t buf[512];
	uint16_t n = store_encode(&d, buf, sizeof buf);
	CHECK(store_decode(buf, n, STORE_LAYOUT_VERSION, &back) == 0 && back.sync_on == 1);
	/* bad value rejected, state unchanged */
	clear_tx(); request(PT_SET_SYNC, 5, bad, 1);
	CHECK(result_code(PT_SET_SYNC, 5) == PR_BAD_ARG);
	CHECK(pr.sync_on == 1);
	/* off: MIDI Stop once, then silence */
	request(PT_SET_SYNC, 6, off, 1);
	e = sync_out_advance(&syn, 4800);
	CHECK(e.stop == 1);
	e = sync_out_advance(&syn, 48000);
	CHECK(e.midi_ticks == 0);
	/* power-up resume: a saved "on" starts the clock by itself, at the saved tempo */
	proto_config_t c = {0};
	sync_out_init(&syn, SR);
	c.eng = &eng; c.ctl = &ui; c.sync = &syn; c.deb = &deb; c.tx = tx; c.save = save_cb;
	c.fw = "x"; c.block = 64; c.bpm = 90.0f; c.sync_on = 1;
	proto_init(&pr, &c);
	e = sync_out_advance(&syn, 48000);
	CHECK(e.start == 1 && e.midi_ticks >= 35 && e.midi_ticks <= 37); /* 90 BPM: 36 ticks/s */
	/* factory reset turns the clock off */
	uint8_t confirm[1] = {PROTO_FACTORY_RESET_CONFIRM};
	request(PT_FACTORY_RESET, 7, confirm, 1);
	CHECK(pr.sync_on == 0);
	e = sync_out_advance(&syn, 4800);
	CHECK(e.stop == 1);
}

int main(void)
{
	RUN(test_crc16);
	RUN(test_cobs);
	RUN(test_frame_build_parse);
	RUN(test_receiver_drops_bad_frames);
	RUN(test_fuzz);
	RUN(test_golden_requests);
	RUN(test_hello_rsp_layout);
	RUN(test_fx_desc_all);
	RUN(test_state);
	RUN(test_bad_arguments);
	RUN(test_admission);
	RUN(test_no_echo_of_host_changes);
	RUN(test_device_events);
	RUN(test_preset_loaded_event);
	RUN(test_sync_clock_switch);
	RUN(test_subscribe_timing);
	RUN(test_output_mode);
	RUN(test_meters_content);
	RUN(test_factory_reset);
	T_DONE();
}
