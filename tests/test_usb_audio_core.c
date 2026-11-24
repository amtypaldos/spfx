/* USB audio clock-domain core (firmware/audio/usb_audio_core.c): ring, consumer
 * safeguards, feedback law, packet sizing, conversions, ramps, and a simulated
 * host/device clock drift for 10 minutes (explicit feedback = image A,
 * implicit feedback = image B). SIMULATED, not measured on hardware. */
#include "t.h"
#include "../firmware/audio/usb_audio_core.h"

/* Defaults mirrored from firmware/app/zephyr_usbaudio/Kconfig. */
#define RING_FRAMES 512u
#define TARGET      192u
#define LOW         96u
#define HIGH        320u
#define RET_TARGET  160u
#define RET_LOW     64u
#define RET_HIGH    352u
#define BLK         64u
#define FB_KP       8
#define FB_CLAMP    (1 << UA_FB_FRAC)   /* +-1 frame per ms */
#define FB_EMA      6
#define NOMINAL_Q14 (48 << UA_FB_FRAC)

static int16_t store[RING_FRAMES * 2];
static int16_t store2[RING_FRAMES * 2];

static void test_ring_basic_and_wrap(void)
{
	ua_ring_t r;
	int16_t in[2 * 300], out[2 * 300];

	CHECK(ua_ring_init(&r, store, 100) == -1);   /* not a power of two */
	CHECK(ua_ring_init(&r, store, RING_FRAMES) == 0);
	CHECK(ua_ring_fill(&r) == 0 && ua_ring_space(&r) == RING_FRAMES);
	int16_t v = 0;
	int16_t expect = 0;
	int ok = 1;
	for (int round = 0; round < 50; round++) {   /* many wraps, odd sizes */
		uint32_t n = 37u + (uint32_t)round * 5u;
		for (uint32_t i = 0; i < 2 * n; i++) in[i] = v++;
		CHECK(ua_ring_write(&r, in, n) == n);
		CHECK(ua_ring_read(&r, out, n) == n);
		for (uint32_t i = 0; i < 2 * n; i++) if (out[i] != expect++) ok = 0;
	}
	CHECK(ok);
	/* overfill: only the space is written */
	for (int i = 0; i < 600; i++) in[i] = (int16_t)i;
	CHECK(ua_ring_write(&r, in, 300) == 300);
	CHECK(ua_ring_write(&r, in, 300) == RING_FRAMES - 300);
	CHECK(ua_ring_fill(&r) == RING_FRAMES && ua_ring_space(&r) == 0);
	CHECK(ua_ring_read(&r, NULL, 10) == 10);
	ua_ring_flush(&r);
	CHECK(ua_ring_fill(&r) == 0);
	CHECK(ua_ring_read(&r, out, 5) == 0);
}

static void put_frames(ua_ring_t *r, uint32_t n, int16_t start)
{
	int16_t f[2 * 512];
	for (uint32_t i = 0; i < n; i++) {
		f[2 * i] = (int16_t)(start + (int16_t)i);
		f[2 * i + 1] = (int16_t)(-(start + (int16_t)i));
	}
	(void)ua_ring_write(r, f, n);
}

static void test_consumer_states(void)
{
	ua_ring_t r;
	ua_cons_t c;
	int16_t out[2 * (BLK + 1)];

	ua_ring_init(&r, store, RING_FRAMES);
	ua_cons_init(&c, &r, TARGET, LOW, HIGH);
	put_frames(&r, TARGET - 1, 0);
	CHECK(ua_cons_read(&c, out, BLK) == UA_BLK_SILENT);        /* still prebuffering */
	put_frames(&r, 1, (int16_t)(TARGET - 1));
	int f = ua_cons_read(&c, out, BLK);
	CHECK(f == (UA_BLK_AUDIO | UA_BLK_STARTED));
	CHECK(out[0] == 0 && out[2 * 63] == 63 && out[2 * 63 + 1] == -63);
	CHECK(ua_ring_fill(&r) == TARGET - BLK);                    /* 128 left */
	/* 128 >= LOW: normal block */
	CHECK(ua_cons_read(&c, out, BLK) == UA_BLK_AUDIO);
	/* 64 < LOW: duplicate -> consumes 63, last frame repeated */
	f = ua_cons_read(&c, out, BLK);
	CHECK(f == (UA_BLK_AUDIO | UA_BLK_DUP));
	CHECK(out[2 * 63] == out[2 * 62] && out[2 * 63 + 1] == out[2 * 62 + 1]);
	CHECK(ua_ring_fill(&r) == 1 && c.dups == 1);
	/* 1 frame: underrun, back to prebuffer */
	f = ua_cons_read(&c, out, BLK);
	CHECK(f == (UA_BLK_SILENT | UA_BLK_UNDERRUN));
	CHECK(c.underruns == 1 && c.state == UA_PREBUF);
	/* above HIGH: drop one */
	ua_cons_restart(&c);
	put_frames(&r, HIGH + 10, 1000);
	f = ua_cons_read(&c, out, BLK);
	CHECK(f == (UA_BLK_AUDIO | UA_BLK_STARTED | UA_BLK_DROP));
	CHECK(ua_ring_fill(&r) == HIGH + 10 - BLK - 1 && c.drops == 1);
	CHECK(c.fill_max == HIGH + 10 && c.fill_min == 1);
	ua_cons_stats_window_reset(&c);
	CHECK(c.fill_n == 0 && c.fill_max == 0);
}

static void test_feedback_law(void)
{
	ua_fb_t fb;
	ua_fb_init(&fb, NOMINAL_Q14, TARGET, FB_KP, FB_CLAMP, FB_EMA);
	CHECK(ua_fb_update(&fb, TARGET) == NOMINAL_Q14);               /* on target */
	ua_fb_reset(&fb);
	CHECK(ua_fb_update(&fb, TARGET + 10) == NOMINAL_Q14 - 10 * FB_KP);     /* too full: ask less */
	ua_fb_reset(&fb);
	CHECK(ua_fb_update(&fb, TARGET - 10) == NOMINAL_Q14 + 10 * FB_KP);     /* too empty: ask more */
	ua_fb_reset(&fb);
	CHECK(ua_fb_update(&fb, 100000u) == NOMINAL_Q14 - FB_CLAMP);   /* clamped */
	/* EMA: a single spike moves the value by ~1/64 of the step */
	ua_fb_reset(&fb);
	(void)ua_fb_update(&fb, TARGET);
	int32_t v = ua_fb_update(&fb, TARGET + 64);
	CHECK(v == NOMINAL_Q14 - FB_KP);
}

static void test_packet_sizes(void)
{
	uint32_t acc = 0, tot = 0;
	int n47 = 0, n49 = 0, other = 0;
	/* 48.0005 frames per ms for 10 s: 480005 frames, sizes only 48/49 */
	int32_t rate = (int32_t)(48.0005 * 16384.0 + 0.5);
	for (int i = 0; i < 10000; i++) {
		uint32_t n = ua_pkt_frames(&acc, rate);
		tot += n;
		if (n == 49) n49++;
		else if (n == 47) n47++;
		else if (n != 48) other++;
	}
	CHECK(other == 0 && n47 == 0 && n49 > 0);
	CHECK_NEAR((double)tot, 10000.0 * rate / 16384.0, 1.0);
	acc = 0;
	CHECK(ua_pkt_frames(&acc, NOMINAL_Q14 - 1) == 47);  /* first packet rounds down */
	CHECK(ua_pkt_frames(&acc, NOMINAL_Q14 - 1) == 48);
}

static void test_conversions(void)
{
	int16_t in[8] = { 0, 32767, -32768, 16384, 1, -1, 100, -100 };
	float l[4], r[4];
	ua_s16_to_f32(in, l, r, 4);
	CHECK(l[0] == 0.0f && r[0] == 32767.0f / 32768.0f);
	CHECK(l[1] == -1.0f && r[1] == 0.5f);
	int16_t out[8];
	CHECK(ua_f32_to_s16(l, r, out, 4, 1.0f) == 0);
	CHECK(memcmp(in, out, sizeof in) == 0);                  /* exact round trip */
	float big[4] = { 2.0f, -2.0f, NAN, INFINITY };
	float sm[4] = { 0.25f, -0.25f, 1.0f / 65536.0f * 1.01f, -1.0f / 65536.0f * 1.01f };
	CHECK(ua_f32_to_s16(big, sm, out, 4, 1.0f) == 4);
	CHECK(out[0] == 32767 && out[2] == -32768 && out[4] == 0 && out[6] == 0);
	CHECK(out[1] == 8192 && out[3] == -8192 && out[5] == 1 && out[7] == -1); /* rounding */
	/* -12 dB trim (0.2512): full-scale input lands at ~8231, never clips */
	float one[1] = { 1.0f }, mone[1] = { -1.0f };
	int16_t o2[2];
	CHECK(ua_f32_to_s16(one, mone, o2, 1, 0.25118864f) == 0);
	CHECK(o2[0] == 8231 && o2[1] == -8231);
}

static void test_ramps(void)
{
	ua_ramp_t rp;
	float l[64], r[64];
	ua_ramp_init(&rp, 48000.0f, 0.2f, 0.005f);
	/* 200 ms fade-in = 9600 samples = 150 blocks of 64 */
	int blocks = 0;
	float g = 0.0f;
	while (g < 1.0f && blocks < 1000) {
		for (int i = 0; i < 64; i++) l[i] = r[i] = 1.0f;
		g = ua_ramp_apply(&rp, 1.0f, l, r, 64);
		blocks++;
		CHECK(l[63] <= 1.0f && l[0] >= 0.0f);
	}
	CHECK(blocks >= 149 && blocks <= 151);
	for (int i = 0; i < 64; i++) l[i] = r[i] = 1.0f;
	ua_ramp_apply(&rp, 1.0f, l, r, 64);
	CHECK(l[10] == 1.0f);
	/* 5 ms down = 240 samples: 4 blocks */
	blocks = 0;
	do {
		for (int i = 0; i < 64; i++) l[i] = r[i] = 1.0f;
		g = ua_ramp_apply(&rp, 0.0f, l, r, 64);
		blocks++;
	} while (g > 0.0f && blocks < 100);
	CHECK(blocks == 4 && l[63] == 0.0f);
	ua_decay(0.5f, -0.5f, l, r, 64);
	CHECK(l[0] < 0.5f && l[0] > 0.49f && r[0] > -0.5f && l[63] == 0.0f);
	for (int i = 1; i < 64; i++) CHECK(l[i] < l[i - 1]);
}

/* ---------------------------------------------------------- drift simulation
 * Event simulation in host time (the host's 1 ms SOF is the reference).
 *  - SOF k at k ms: device sof_cb updates the feedback from the ring fill
 *    (A) or also sizes and sends the IN packet (B).
 *  - The OUT packet of SOF k lands in the ring at k + 0.6 ms (data_recv_cb).
 *  - The host applies the feedback / IN size it saw HOST_LAG SOFs ago.
 *  - The device consumes BLK frames every BLK / dev_hz seconds (I2S). */
#define HOST_LAG 3

typedef struct {
	uint32_t under, dups, drops, overrun_frames;
	uint32_t ret_under, ret_dups, ret_drops, ret_overrun;
	uint32_t fill_min, fill_max;          /* after settling, before each read */
	uint32_t ret_fill_min, ret_fill_max;
	double fb_mean_last60;                /* Q10.14 LSBs */
	double expected_q14;
} sim_res;

static void simulate(double dev_hz, double seconds, int mode /*0 = explicit, 1 = implicit, 2 = no feedback*/,
		     sim_res *res)
{
	ua_ring_t ring, ret;
	ua_cons_t cons, rcons;
	ua_fb_t fb;
	int32_t hist[HOST_LAG + 1];
	uint32_t hist_n[HOST_LAG + 1];
	int16_t pkt[2 * 64], blk[2 * 64];
	uint32_t host_acc = 0, dev_acc = 0;
	const double settle = 30.0;

	memset(res, 0, sizeof *res);
	res->fill_min = res->ret_fill_min = 0xFFFFFFFFu;
	ua_ring_init(&ring, store, RING_FRAMES);
	ua_ring_init(&ret, store2, RING_FRAMES);
	ua_cons_init(&cons, &ring, TARGET, LOW, HIGH);
	ua_cons_init(&rcons, &ret, RET_TARGET, RET_LOW, RET_HIGH);
	ua_fb_init(&fb, NOMINAL_Q14, TARGET, FB_KP, FB_CLAMP, FB_EMA);
	for (int i = 0; i <= HOST_LAG; i++) { hist[i] = NOMINAL_Q14; hist_n[i] = 48; }
	memset(pkt, 0, sizeof pkt);

	const double blk_dt = (double)BLK / dev_hz;
	const long n_sof = (long)(seconds * 1000.0);
	long sof = 0;
	double t_sof = 0.0, t_rx = 1e30, t_blk = blk_dt;
	double fb_sum = 0.0;
	long fb_cnt = 0;
	uint32_t pending = 0;          /* OUT frames of the last SOF, in flight */

	while (sof < n_sof) {
		if (t_sof <= t_rx && t_sof <= t_blk) {
			/* SOF: device sof_cb, then the host sends its OUT packet */
			int32_t v = ua_fb_update(&fb, ua_ring_fill(&ring));
			uint32_t host_n;
			for (int i = HOST_LAG; i > 0; i--) { hist[i] = hist[i - 1]; hist_n[i] = hist_n[i - 1]; }
			hist[0] = v;
			if (mode == 1) {
				uint32_t n = ua_pkt_frames(&dev_acc, v);   /* device-chosen IN size */
				int16_t inpkt[2 * 64];
				if (t_sof > settle) {
					uint32_t rf = ua_ring_fill(&ret);
					if (rf < res->ret_fill_min) res->ret_fill_min = rf;
					if (rf > res->ret_fill_max) res->ret_fill_max = rf;
				}
				int f = ua_cons_read(&rcons, inpkt, n);
				if (f & UA_BLK_UNDERRUN) res->ret_under++;
				hist_n[0] = n;
				host_n = hist_n[HOST_LAG];                 /* host mirrors IN sizes */
			} else if (mode == 0) {
				host_n = ua_pkt_frames(&host_acc, hist[HOST_LAG]);
			} else {
				host_n = 48;
			}
			if (t_sof > seconds - 60.0) { fb_sum += v; fb_cnt++; }
			pending = host_n;
			t_rx = t_sof + 0.6e-3;
			sof++;
			t_sof = (double)sof * 1e-3;
		} else if (t_rx <= t_blk) {
			res->overrun_frames += pending - ua_ring_write(&ring, pkt, pending);
			t_rx = 1e30;
		} else {
			uint32_t f0 = ua_ring_fill(&ring);
			int f = ua_cons_read(&cons, blk, BLK);
			if ((f & UA_BLK_AUDIO) && t_blk > settle) {
				if (f0 < res->fill_min) res->fill_min = f0;
				if (f0 > res->fill_max) res->fill_max = f0;
			}
			if (mode == 1 && (f & UA_BLK_AUDIO)) res->ret_overrun += BLK - ua_ring_write(&ret, blk, BLK);
			t_blk += blk_dt;
		}
	}
	res->under = cons.underruns;
	res->dups = cons.dups;
	res->drops = cons.drops;
	res->ret_dups = rcons.dups;
	res->ret_drops = rcons.drops;
	res->fb_mean_last60 = fb_cnt ? fb_sum / (double)fb_cnt : 0.0;
	res->expected_q14 = dev_hz / 1000.0 * 16384.0;
}

static void test_drift_explicit_10min(void)
{
	sim_res s;
	const double rates[] = { 48000.5, 47999.5, 48024.0 /* +500 ppm */, 47976.0 };
	for (unsigned k = 0; k < sizeof rates / sizeof rates[0]; k++) {
		simulate(rates[k], 600.0, 0, &s);
		printf("  explicit dev=%.1f Hz: fill %u..%u (limits %u..%u) fb_mean=%.2f expect=%.2f "
		       "under=%u dup=%u drop=%u ovr=%u\n", rates[k], s.fill_min, s.fill_max, LOW, HIGH,
		       s.fb_mean_last60, s.expected_q14, s.under, s.dups, s.drops, s.overrun_frames);
		CHECK(s.under == 0 && s.overrun_frames == 0);
		CHECK(s.dups == 0 && s.drops == 0);
		CHECK(s.fill_min >= LOW && s.fill_max <= HIGH);
		CHECK_NEAR(s.fb_mean_last60, s.expected_q14, 1.0);   /* 1 LSB = 1.3 ppm */
	}
}

static void test_drift_implicit_10min(void)
{
	sim_res s;
	const double rates[] = { 48000.5, 47999.5, 48024.0 };
	for (unsigned k = 0; k < sizeof rates / sizeof rates[0]; k++) {
		simulate(rates[k], 600.0, 1, &s);
		printf("  implicit dev=%.1f Hz: fill %u..%u ret %u..%u fb_mean=%.2f expect=%.2f "
		       "under=%u dup=%u drop=%u | ret under=%u dup=%u drop=%u ovr=%u\n", rates[k],
		       s.fill_min, s.fill_max, s.ret_fill_min, s.ret_fill_max, s.fb_mean_last60,
		       s.expected_q14, s.under, s.dups, s.drops, s.ret_under, s.ret_dups, s.ret_drops,
		       s.ret_overrun);
		CHECK(s.under == 0 && s.dups == 0 && s.drops == 0 && s.overrun_frames == 0);
		CHECK(s.fill_min >= LOW && s.fill_max <= HIGH);
		CHECK(s.ret_under == 0 && s.ret_overrun == 0);
		CHECK(s.ret_dups == 0 && s.ret_drops == 0);
		CHECK_NEAR(s.fb_mean_last60, s.expected_q14, 1.0);
	}
}

/* A host that ignores feedback (e.g. implicit-feedback image with the return
 * stream closed): the drop/dup safeguard must hold the ring without underrun. */
static void test_no_feedback_safeguard(void)
{
	sim_res s;
	simulate(48000.5, 600.0, 2, &s);
	printf("  no-fb dev=48000.5: dup=%u drop=%u under=%u ovr=%u fill %u..%u\n",
	       s.dups, s.drops, s.under, s.overrun_frames, s.fill_min, s.fill_max);
	/* 0.5 frame/s for 600 s = 300 frames; the first (TARGET-LOW..) are absorbed */
	CHECK(s.under == 0 && s.overrun_frames == 0 && s.drops == 0);
	CHECK(s.dups > 100 && s.dups < 300);
	simulate(47999.5, 600.0, 2, &s);
	printf("  no-fb dev=47999.5: dup=%u drop=%u under=%u ovr=%u fill %u..%u\n",
	       s.dups, s.drops, s.under, s.overrun_frames, s.fill_min, s.fill_max);
	CHECK(s.under == 0 && s.overrun_frames == 0 && s.dups == 0);
	CHECK(s.drops > 50 && s.drops < 300);
}

int main(void)
{
	RUN(test_ring_basic_and_wrap);
	RUN(test_consumer_states);
	RUN(test_feedback_law);
	RUN(test_packet_sizes);
	RUN(test_conversions);
	RUN(test_ramps);
	RUN(test_drift_explicit_10min);
	RUN(test_drift_implicit_10min);
	RUN(test_no_feedback_safeguard);
	T_DONE();
}
