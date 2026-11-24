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

int main(void)
{
	RUN(test_save_sched_rules);
	RUN(test_save_sched_with_journal);
	RUN(test_conmode);
	RUN(test_separator_with_protocol_parser);
	RUN(test_quiet);
	RUN(test_ratelimit);
	T_DONE();
}
