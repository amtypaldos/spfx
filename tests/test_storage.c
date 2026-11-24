/* Persistence tests: encode/decode, journal behaviour, power loss, corruption, wear. */
#include "t.h"
#include "../firmware/storage/config_store.h"

/* ---- mock NOR flash: 2 pages, bits only clear on write, erase sets 0xFF ---- */
typedef struct {
	uint8_t page[2][STORE_PAGE_BYTES];
	uint32_t erases[2];
	long fail_after_bytes; /* <0: never; else number of bytes that still succeed before power is cut */
	int erase_cut;         /* if set, the next erase only wipes half the page then fails */
	uint32_t writes;
} mock_t;

static int m_read(void *c, uint32_t p, uint32_t off, void *b, uint32_t n)
{
	mock_t *m = c;
	if (p > 1 || off + n > STORE_PAGE_BYTES) return -1;
	memcpy(b, m->page[p] + off, n);
	return 0;
}
static int m_write(void *c, uint32_t p, uint32_t off, const void *b, uint32_t n)
{
	mock_t *m = c;
	if (p > 1 || off + n > STORE_PAGE_BYTES || (off & 3) || (n & 3)) { printf("  mock: illegal write geometry\n"); return -1; }
	const uint8_t *s = b;
	for (uint32_t i = 0; i < n; i++) {
		if (m->fail_after_bytes == 0) return -1; /* power cut */
		if (m->fail_after_bytes > 0) m->fail_after_bytes--;
		m->page[p][off + i] &= s[i]; /* NOR: can only clear bits */
	}
	m->writes++;
	return 0;
}
static int m_erase(void *c, uint32_t p)
{
	mock_t *m = c;
	if (p > 1) return -1;
	if (m->erase_cut) {
		memset(m->page[p], 0xFF, STORE_PAGE_BYTES / 2);
		m->erase_cut = 0;
		return -1;
	}
	memset(m->page[p], 0xFF, STORE_PAGE_BYTES);
	m->erases[p]++;
	return 0;
}
static mock_t M;
static flash_ops_t ops(void) { flash_ops_t o = {&M, m_read, m_write, m_erase}; return o; }
static void blank(void) { memset(&M, 0, sizeof M); memset(M.page, 0xFF, sizeof M.page); M.fail_after_bytes = -1; }

static store_data_t sample(int n)
{
	store_data_t d;
	store_data_default(&d);
	d.cfg.fx_id[0] = FX_SATURATION; d.cfg.fx_id[1] = (uint8_t)(1 + n % 10); d.cfg.fx_id[2] = FX_DELAY; d.cfg.fx_id[3] = FX_NONE;
	d.cfg.wet[0] = 0.25f + 0.001f * (float)(n % 100);
	d.cfg.params[2][FX_DELAY][1] = 0.123f;
	d.cfg.bypass[1] = 0; d.cfg.global_bypass = 1;
	d.cfg.in_gain_db = -3.5f; d.cfg.master = 0.4375f;
	d.cfg.order[0] = 3; d.cfg.order[1] = 2; d.cfg.order[2] = 1; d.cfg.order[3] = 0;
	d.bpm = 97.5f;
	return d;
}

static int same(const store_data_t *a, const store_data_t *b)
{
	for (int s = 0; s < 4; s++) {
		if (a->cfg.fx_id[s] != b->cfg.fx_id[s] || a->cfg.bypass[s] != b->cfg.bypass[s] || a->cfg.order[s] != b->cfg.order[s]) return 0;
		if (fabsf(a->cfg.wet[s] - b->cfg.wet[s]) > 2e-5f) return 0;
		for (int f = 0; f < FX_COUNT; f++)
			for (int i = 0; i < 4; i++) if (fabsf(a->cfg.params[s][f][i] - b->cfg.params[s][f][i]) > 2e-5f) return 0;
	}
	return a->cfg.global_bypass == b->cfg.global_bypass && fabsf(a->cfg.in_gain_db - b->cfg.in_gain_db) < 0.006f &&
	       fabsf(a->cfg.master - b->cfg.master) < 2e-5f && fabsf(a->bpm - b->bpm) < 0.06f;
}

static void test_crc32_vector(void)
{
	CHECK(sp1fx_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
}

static void test_encode_decode_roundtrip(void)
{
	store_data_t a = sample(3), b;
	uint8_t buf[400];
	uint16_t n = store_encode(&a, buf, sizeof buf);
	CHECK(n > 300 && n < 420);
	CHECK(store_decode(buf, n, STORE_LAYOUT_VERSION, &b) == 0);
	CHECK(same(&a, &b));
	CHECK(store_encode(&a, buf, 10) == 0);              /* too small a buffer */
	CHECK(store_decode(buf, n, 99, &b) != 0);           /* unknown layout version */
	CHECK(store_decode(buf, 20, STORE_LAYOUT_VERSION, &b) != 0); /* truncated */
	/* hostile payload: bad ids and order are repaired by sanitising */
	buf[0] = 250; buf[8] = 1; buf[9] = 1;
	CHECK(store_decode(buf, n, STORE_LAYOUT_VERSION, &b) == 0);
	CHECK(b.cfg.fx_id[0] == FX_NONE);
	CHECK(b.cfg.order[0] == 0 && b.cfg.order[1] == 1);
	/* NaN / out-of-range inputs encode to safe values */
	a.cfg.master = NAN; a.bpm = INFINITY; a.cfg.in_gain_db = 1e9f;
	n = store_encode(&a, buf, sizeof buf);
	CHECK(store_decode(buf, n, STORE_LAYOUT_VERSION, &b) == 0);
	CHECK(b.bpm >= 30 && b.bpm <= 300);
	CHECK(b.cfg.in_gain_db <= 12.0f);
}

static void test_blank_flash_gives_defaults(void)
{
	blank();
	store_t s; store_data_t d, def;
	store_init(&s, ops());
	CHECK(store_load(&s, &d) == -1);
	store_data_default(&def);
	CHECK(same(&d, &def));
}

static void test_save_load_across_reboots(void)
{
	blank();
	store_t s; store_data_t a = sample(1), out;
	store_init(&s, ops());
	CHECK(store_save(&s, &a) == 0);
	/* "reboot": new store_t over the same flash */
	store_t s2; store_init(&s2, ops());
	CHECK(store_load(&s2, &out) == 0);
	CHECK(same(&a, &out));
	/* newest wins */
	store_data_t b = sample(2);
	CHECK(store_save(&s2, &b) == 0);
	store_t s3; store_init(&s3, ops());
	CHECK(store_load(&s3, &out) == 0 && same(&b, &out));
	CHECK(s3.seq == 2);
}

static void test_page_rotation_and_wear(void)
{
	blank();
	store_t s; store_data_t d, out;
	store_init(&s, ops());
	for (int i = 0; i < 1000; i++) {
		d = sample(i);
		CHECK(store_save(&s, &d) == 0);
	}
	store_t r; store_init(&r, ops());
	CHECK(store_load(&r, &out) == 0 && same(&d, &out));
	CHECK(r.seq == 1000);
	/* 1000 saves, 8 per page: ~125 erases total spread across both pages */
	uint32_t tot = M.erases[0] + M.erases[1];
	CHECK_MSG(tot >= 120 && tot <= 130, "erases=%u", tot);
	CHECK(abs((int)M.erases[0] - (int)M.erases[1]) <= 1);
}

/* power cut at every possible byte of a save: the previous record must always survive */
static void test_power_loss_during_write(void)
{
	for (long cut = 0; cut < (long)STORE_RECORD_BYTES; cut += 7) {
		blank();
		store_t s; store_data_t a = sample(10), b = sample(11), out;
		store_init(&s, ops());
		CHECK(store_save(&s, &a) == 0);
		M.fail_after_bytes = cut;
		int rc = store_save(&s, &b);
		M.fail_after_bytes = -1;
		store_t r; store_init(&r, ops());
		int ok = store_load(&r, &out) == 0;
		CHECK_MSG(ok, "cut %ld: nothing loadable", cut);
		if (ok) {
			/* Invariant: old or new, never garbage. A cut inside the trailing 0xFF padding leaves a
			   complete, valid new record even though the write reported failure, so "failed" does
			   not imply "old" (the host must re-read state after a storage error). */
			CHECK_MSG(same(&out, &a) || same(&out, &b), "cut %ld: loaded neither old nor new", cut);
			if (rc == 0) CHECK_MSG(same(&out, &b), "cut %ld: successful save not loaded", cut);
		}
		/* and the store keeps working afterwards (torn slot is skipped) */
		store_data_t c = sample(12);
		CHECK_MSG(store_save(&r, &c) == 0, "cut %ld: save after recovery failed", cut);
		store_t r2; store_init(&r2, ops());
		CHECK(store_load(&r2, &out) == 0 && same(&out, &c));
	}
}

/* power cut during the erase that happens when a page fills up */
static void test_power_loss_during_erase(void)
{
	blank();
	store_t s; store_data_t d, out;
	store_init(&s, ops());
	/* both pages full (the first switch goes to a blank page and needs no erase) */
	for (int i = 0; i < 16; i++) { d = sample(i); CHECK(store_save(&s, &d) == 0); }
	CHECK(store_save_needs_erase(&s) == 1);
	store_data_t last = d;
	M.erase_cut = 1;
	store_data_t nxt = sample(99);
	CHECK(store_save(&s, &nxt) != 0);
	store_t r; store_init(&r, ops());
	CHECK(store_load(&r, &out) == 0);
	CHECK(same(&out, &last)); /* the full page still holds the newest record */
	CHECK(store_save(&r, &nxt) == 0);
	store_t r2; store_init(&r2, ops());
	CHECK(store_load(&r2, &out) == 0 && same(&out, &nxt));
}

/* pre-erasing the spare page at a quiet moment keeps every later save erase-free */
static void test_spare_pre_erase(void)
{
	blank();
	store_t s; store_data_t d, out;
	store_init(&s, ops());
	/* blank flash: nothing to pre-erase, the first save needs no erase */
	CHECK(store_spare_needs_erase(&s) == 0 && store_save_needs_erase(&s) == 0);
	CHECK(store_erase_spare(&s) == 0 && M.erases[0] + M.erases[1] == 0);
	/* foreign data in page 0 and nothing valid: the first save must erase */
	memset(M.page[0] + 100, 0x12, 8);
	store_init(&s, ops());
	CHECK(store_save_needs_erase(&s) == 1);
	d = sample(0);
	CHECK(store_save(&s, &d) == 0 && M.erases[0] == 1);
	/* the fresh page 1 is blank: filling page 0 and switching needs no erase */
	for (int i = 1; i < 8; i++) { CHECK(store_save_needs_erase(&s) == 0); d = sample(i); CHECK(store_save(&s, &d) == 0); }
	CHECK(store_spare_needs_erase(&s) == 0);
	CHECK(store_save_needs_erase(&s) == 0); /* page 0 full, page 1 blank */
	d = sample(8);
	CHECK(store_save(&s, &d) == 0 && s.active_page == 1 && M.erases[1] == 0);
	/* now page 0 (old records) is the spare: it needs an erase, and so would the switch back */
	CHECK(store_spare_needs_erase(&s) == 1);
	for (int i = 9; i < 16; i++) { d = sample(i); CHECK(store_save(&s, &d) == 0); }
	CHECK(store_save_needs_erase(&s) == 1);
	/* pre-erase at a quiet moment: the newest record stays loadable, the switch is erase-free */
	uint32_t e0 = M.erases[0];
	CHECK(store_erase_spare(&s) == 0 && M.erases[0] == e0 + 1);
	CHECK(store_spare_needs_erase(&s) == 0 && store_save_needs_erase(&s) == 0);
	{ store_t r; store_init(&r, ops()); CHECK(store_load(&r, &out) == 0 && same(&out, &d)); }
	d = sample(16);
	CHECK(store_save(&s, &d) == 0 && s.active_page == 0 && M.erases[0] == e0 + 1);
	/* long run with a pre-erase after every save that left a dirty spare: saves never erase */
	uint32_t save_erases = 0;
	for (int i = 17; i < 600; i++) {
		if (store_spare_needs_erase(&s)) CHECK(store_erase_spare(&s) == 0);
		uint32_t before = M.erases[0] + M.erases[1];
		CHECK(store_save_needs_erase(&s) == 0);
		d = sample(i);
		CHECK(store_save(&s, &d) == 0);
		save_erases += (M.erases[0] + M.erases[1]) - before;
	}
	CHECK_MSG(save_erases == 0, "saves erased %u times", save_erases);
	{ store_t r; store_init(&r, ops()); CHECK(store_load(&r, &out) == 0 && same(&out, &d) && r.seq == 600); }
	/* the spare erase never touches the page with the newest record */
	int act = s.active_page;
	uint8_t snap[STORE_PAGE_BYTES];
	memcpy(snap, M.page[act], sizeof snap);
	CHECK(store_erase_spare(&s) == 0);
	CHECK(memcmp(snap, M.page[act], sizeof snap) == 0);
	/* power cut during a pre-erase: newest record intact, next saves still work */
	for (int i = 0; i < 8; i++) { d = sample(700 + i); CHECK(store_save(&s, &d) == 0); }
	M.erase_cut = 1;
	CHECK(store_erase_spare(&s) != 0);
	{ store_t r; store_init(&r, ops()); CHECK(store_load(&r, &out) == 0 && same(&out, &d));
	  CHECK(store_spare_needs_erase(&r) == 1);
	  store_data_t n2 = sample(999);
	  CHECK(store_save(&r, &n2) == 0);
	  store_t r2; store_init(&r2, ops()); CHECK(store_load(&r2, &out) == 0 && same(&out, &n2)); }
}

/* rc4: out_mode rides in a journal extension byte after the v1 payload; records without it
 * (all earlier firmware) read as headphones; the STATE payload (store_encode) is unchanged */
static void test_out_mode_extension(void)
{
	blank();
	store_t s; store_data_t d = sample(1), out;
	store_init(&s, ops());
	CHECK(store_save(&s, &d) == 0);
	{ store_t r; store_init(&r, ops()); CHECK(store_load(&r, &out) == 0 && same(&out, &d) && out.out_mode == 0); }
	d.out_mode = 1;
	CHECK(store_save(&s, &d) == 0);
	{ store_t r; store_init(&r, ops()); CHECK(store_load(&r, &out) == 0 && same(&out, &d) && out.out_mode == 1); }
	d.out_mode = 2;
	CHECK(store_save(&s, &d) == 0);
	{ store_t r; store_init(&r, ops()); CHECK(store_load(&r, &out) == 0 && out.out_mode == 2); }
	d.out_mode = 1;
	/* an old-format record (exactly the v1 payload, no extension) -> headphones */
	uint8_t pl[STORE_RECORD_BYTES];
	uint16_t n = store_encode(&d, pl, sizeof pl);
	CHECK(n == store_payload_len());
	store_data_t back;
	CHECK(store_decode(pl, n, STORE_LAYOUT_VERSION, &back) == 0 && back.out_mode == 0);
	/* hand-written old record in a fresh journal */
	blank();
	uint8_t rec[STORE_RECORD_BYTES];
	memset(rec, 0xFF, sizeof rec);
	uint32_t magic = 0x58463153u, seq = 5;
	memcpy(rec, &magic, 4);
	rec[4] = 1; rec[5] = 0; rec[6] = (uint8_t)n; rec[7] = (uint8_t)(n >> 8);
	memcpy(rec + 8, &seq, 4);
	memcpy(rec + 12, pl, n);
	uint32_t crc = sp1fx_crc32(rec, 12u + n);
	memcpy(rec + 12 + n, &crc, 4);
	memcpy(M.page[0], rec, sizeof rec);
	{ store_t r; store_init(&r, ops()); CHECK(store_load(&r, &out) == 0 && out.out_mode == 0 && same(&out, &d)); }
	/* defaults */
	store_data_default(&out);
	CHECK(out.out_mode == 0);
}

/* rc4: a save programmed a few bytes per control tick; a power cut anywhere in it keeps the
 * previous record; finishing it equals store_save */
static void test_save_job_steps(void)
{
	blank();
	store_t s; store_data_t a = sample(3), b = sample(4), out;
	store_init(&s, ops());
	CHECK(store_save(&s, &a) == 0);
	static store_job_t j;
	CHECK(store_job_begin(&s, &b, &j) == 0 && j.active && j.total <= STORE_RECORD_BYTES && (j.total & 3u) == 0);
	int steps = 0, r;
	while ((r = store_job_step(&s, &j, 32)) == 0) {
		steps++;
		/* "power cut" after every step: a fresh boot still loads the previous record */
		store_t q; store_init(&q, ops());
		CHECK(store_load(&q, &out) == 0 && same(&out, &a));
	}
	CHECK(r == 1 && steps >= (int)(j.total / 32u) - 1);
	{ store_t q; store_init(&q, ops()); CHECK(store_load(&q, &out) == 0 && same(&out, &b) && q.seq == 2); }
	/* an abandoned job: the next save skips the torn slot and works */
	CHECK(store_job_begin(&s, &a, &j) == 0);
	CHECK(store_job_step(&s, &j, 64) == 0);
	{
		store_t q; store_init(&q, ops());
		CHECK(store_load(&q, &out) == 0 && same(&out, &b));
		store_data_t c = sample(5);
		CHECK(store_save(&q, &c) == 0);
		store_t q2; store_init(&q2, ops());
		CHECK(store_load(&q2, &out) == 0 && same(&out, &c));
	}
	/* stepping a finished job is an error, never a write */
	store_t q3; store_init(&q3, ops());
	CHECK(store_job_begin(&q3, &a, &j) == 0);
	while (store_job_step(&q3, &j, 400) == 0) {}
	uint32_t w0 = M.writes;
	CHECK(store_job_step(&q3, &j, 32) == -3 && M.writes == w0);
}

static void test_corruption(void)
{
	blank();
	store_t s; store_data_t a = sample(1), b = sample(2), out;
	store_init(&s, ops());
	store_save(&s, &a);
	store_save(&s, &b);
	/* flip a bit in the newest record: falls back to the previous one */
	M.page[0][STORE_RECORD_BYTES + 40] ^= 0x10;
	store_t r; store_init(&r, ops());
	CHECK(store_load(&r, &out) == 0 && same(&out, &a));
	/* corrupt everything: defaults, and saving still works */
	for (int p = 0; p < 2; p++) for (int i = 0; i < (int)STORE_PAGE_BYTES; i += 3) M.page[p][i] = (uint8_t)(i * 7);
	store_t r2; store_init(&r2, ops());
	store_data_t def;
	CHECK(store_load(&r2, &out) == -1);
	store_data_default(&def);
	CHECK(same(&out, &def));
	CHECK(store_save(&r2, &b) == 0);
	store_t r3; store_init(&r3, ops());
	CHECK(store_load(&r3, &out) == 0 && same(&out, &b));
	/* record claiming an absurd length is rejected, not trusted */
	blank();
	store_t r4; store_init(&r4, ops());
	store_save(&r4, &a);
	M.page[0][6] = 0xFF; M.page[0][7] = 0x7F;
	store_t r5; store_init(&r5, ops());
	CHECK(store_load(&r5, &out) == -1);
}

static void test_write_failure_then_rescan(void)
{
	blank();
	store_t s; store_data_t a = sample(1), b = sample(2), out;
	store_init(&s, ops());
	store_save(&s, &a);
	M.fail_after_bytes = 100;
	CHECK(store_save(&s, &b) != 0);
	M.fail_after_bytes = -1;
	CHECK(store_save(&s, &b) == 0); /* same store_t, rescans internally and skips the torn slot */
	store_t r; store_init(&r, ops());
	CHECK(store_load(&r, &out) == 0 && same(&out, &b));
}

static void test_debounce(void)
{
	store_debounce_t d;
	store_debounce_init(&d);
	CHECK(!store_debounce_due(&d, 1000));          /* nothing changed */
	store_debounce_mark(&d, 1000);
	CHECK(!store_debounce_due(&d, 2000));          /* still within quiet period */
	store_debounce_mark(&d, 2400);                 /* more changes extend it */
	CHECK(!store_debounce_due(&d, 3500));
	CHECK(store_debounce_due(&d, 4000));           /* 1.6 s quiet: write */
	CHECK(!store_debounce_due(&d, 4100));          /* nothing new */
	store_debounce_mark(&d, 4200);
	CHECK(!store_debounce_due(&d, 6000));          /* quiet satisfied but < 5 s since last write */
	CHECK(store_debounce_due(&d, 9100));
	/* a drag of a fader (changes every 50 ms for 10 s) causes a single write at the end */
	store_debounce_init(&d);
	int writes = 0;
	for (uint32_t t = 0; t < 10000; t += 50) { store_debounce_mark(&d, t); writes += store_debounce_due(&d, t); }
	CHECK(writes == 0);
	writes += store_debounce_due(&d, 10000 + 1600);
	CHECK(writes == 1);
}

int main(void)
{
	RUN(test_crc32_vector);
	RUN(test_encode_decode_roundtrip);
	RUN(test_blank_flash_gives_defaults);
	RUN(test_save_load_across_reboots);
	RUN(test_page_rotation_and_wear);
	RUN(test_power_loss_during_write);
	RUN(test_power_loss_during_erase);
	RUN(test_spare_pre_erase);
	RUN(test_out_mode_extension);
	RUN(test_save_job_steps);
	RUN(test_corruption);
	RUN(test_write_failure_then_rescan);
	RUN(test_debounce);
	T_DONE();
}
