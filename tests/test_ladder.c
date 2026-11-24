/* Resistor-ladder decode + fader normaliser (firmware/drivers/ladder.c).
 * Nominal raw codes are Tape Looper's (chattock/sp1-tape-looper @0359372e, MIT,
 * main.c decode_tracks/decode_vol comments and lad_press nominals). Synthetic. */
#include "t.h"
#include "../firmware/drivers/ladder.h"
#include "../firmware/controls/controls.h"

#define B(x) BTN_BIT(BTN_##x)

/* feed the same raw value n times, return the final mask */
static uint16_t feed(ladder_t *l, int raw, int n)
{
	uint16_t m = 0;
	for (int i = 0; i < n; i++) m = ladder_update(l, raw);
	return m;
}

static void test_ladder1_nominals(void)
{
	ladder_cfg_t c;
	ladder_cfg_ladder1(&c, 0);
	/* Tape Looper nominal levels */
	CHECK(ladder_decode_raw(&c, 0) == 0);
	CHECK(ladder_decode_raw(&c, 60) == 0);
	CHECK(ladder_decode_raw(&c, 213) == B(T1));
	CHECK(ladder_decode_raw(&c, 403) == B(T2));
	CHECK(ladder_decode_raw(&c, 733) == B(T3));
	CHECK(ladder_decode_raw(&c, 1220) == B(T4));
	CHECK(ladder_decode_raw(&c, 1807) == B(PLAY));
	CHECK(ladder_decode_raw(&c, 1823) == B(PLAY));
	/* PLAY + Tn chords (W113) read as PLAY by default */
	CHECK(ladder_decode_raw(&c, 1861) == B(PLAY));
	CHECK(ladder_decode_raw(&c, 2159) == B(PLAY));
	/* exact thresholds: raw < max -> lower band */
	CHECK(ladder_decode_raw(&c, 109) == 0);
	CHECK(ladder_decode_raw(&c, 110) == B(T1));
	CHECK(ladder_decode_raw(&c, 1499) == B(T4));
	CHECK(ladder_decode_raw(&c, 1500) == B(PLAY));
	CHECK(ladder_decode_raw(&c, -1) == 0);
	/* +-8 % unit gain error (Tape Looper LADREL-937 clamp) still decodes singles */
	static const int nom[5] = { 213, 403, 733, 1220, 1807 };
	static const uint16_t want[5] = { B(T1), B(T2), B(T3), B(T4), B(PLAY) };
	for (int i = 0; i < 5; i++) {
		CHECK(ladder_decode_raw(&c, nom[i] * 92 / 100) == want[i]);
		CHECK(ladder_decode_raw(&c, nom[i] * 108 / 100) == want[i]);
	}
}

static void test_ladder1_play_chords_option(void)
{
	ladder_cfg_t c;
	ladder_cfg_ladder1(&c, 1);
	CHECK(ladder_decode_raw(&c, 1807) == B(PLAY));
	CHECK(ladder_decode_raw(&c, 1861) == (B(PLAY) | B(T1)));
	CHECK(ladder_decode_raw(&c, 1910) == (B(PLAY) | B(T2)));
	CHECK(ladder_decode_raw(&c, 2004) == (B(PLAY) | B(T3)));
	CHECK(ladder_decode_raw(&c, 2159) == (B(PLAY) | B(T4)));
	CHECK(ladder_decode_raw(&c, 1220) == B(T4));
	/* the documented hazard: bare PLAY on a unit reading 2.5 % high is
	 * mistaken for PLAY + T1 -- why the option is off by default */
	CHECK(ladder_decode_raw(&c, 1807 * 1025 / 1000) == (B(PLAY) | B(T1)));
}

static void test_ladder2_nominals(void)
{
	ladder_cfg_t c;
	ladder_cfg_ladder2(&c);
	CHECK(ladder_decode_raw(&c, 0) == 0);
	CHECK(ladder_decode_raw(&c, 150) == 0);
	CHECK(ladder_decode_raw(&c, 404) == B(REV));
	CHECK(ladder_decode_raw(&c, 729) == B(VOLDN));
	CHECK(ladder_decode_raw(&c, 1220) == B(FWD));
	CHECK(ladder_decode_raw(&c, 1820) == B(VOLUP));
	CHECK(ladder_decode_raw(&c, 1998) == (B(VOLUP) | B(VOLDN)));
	CHECK(ladder_decode_raw(&c, 199) == 0);
	CHECK(ladder_decode_raw(&c, 200) == B(REV));
	CHECK(ladder_decode_raw(&c, 1909) == B(VOLUP));
	CHECK(ladder_decode_raw(&c, 1910) == (B(VOLUP) | B(VOLDN)));
}

static void test_stability_and_transit(void)
{
	ladder_cfg_t c;
	ladder_cfg_ladder1(&c, 0);
	ladder_t l;
	ladder_init(&l, &c);
	/* one read is not enough, two consecutive are */
	CHECK(ladder_update(&l, 1807) == 0);
	CHECK(ladder_update(&l, 1807) == B(PLAY));
	/* release through the lower bands: one transit sample each, never reported */
	CHECK(ladder_update(&l, 1220) == B(PLAY));
	CHECK(ladder_update(&l, 403) == B(PLAY));
	CHECK(ladder_update(&l, 0) == B(PLAY));
	CHECK(ladder_update(&l, 0) == 0);
	/* press of T3 with a bounce to idle in between: needs two in a row */
	CHECK(ladder_update(&l, 733) == 0);
	CHECK(ladder_update(&l, 20) == 0);
	CHECK(ladder_update(&l, 733) == 0);
	CHECK(ladder_update(&l, 733) == B(T3));
	/* ADC error holds the state */
	CHECK(ladder_update(&l, -1) == B(T3));
	CHECK(ladder_update(&l, -5) == B(T3));
	CHECK(feed(&l, 0, 2) == 0);
}

static void test_hysteresis(void)
{
	ladder_cfg_t c;
	ladder_cfg_ladder1(&c, 0);
	ladder_t l;
	ladder_init(&l, &c);
	/* a T4 reading hovering just below the PLAY threshold (1500) */
	CHECK(feed(&l, 1490, 3) == B(T4));
	/* noise across the threshold by less than the hysteresis does not flip */
	CHECK(feed(&l, 1510, 4) == B(T4));
	CHECK(feed(&l, 1519, 4) == B(T4));
	/* clearly above (>= 1500 + 20) does */
	CHECK(feed(&l, 1520, 2) == B(PLAY));
	/* and back down needs < 1500 - 20 */
	CHECK(feed(&l, 1490, 4) == B(PLAY));
	CHECK(feed(&l, 1481, 4) == B(PLAY));
	CHECK(feed(&l, 1479, 2) == B(T4));
	/* idle threshold (110): 100..125 noise from idle stays idle */
	CHECK(feed(&l, 0, 2) == 0);
	int noise[] = { 100, 125, 118, 104, 129, 112 };
	for (int i = 0; i < 6; i++) CHECK(ladder_update(&l, noise[i]) == 0);
	/* alternating noise between two bands never commits a third */
	ladder_cfg_t c2;
	ladder_cfg_ladder2(&c2);
	ladder_t v;
	ladder_init(&v, &c2);
	CHECK(feed(&v, 1820, 2) == B(VOLUP));
	for (int i = 0; i < 20; i++) CHECK(ladder_update(&v, (i & 1) ? 1900 : 1925) == B(VOLUP));
	CHECK(feed(&v, 1998, 2) == (B(VOLUP) | B(VOLDN)));
	CHECK(feed(&v, 0, 2) == 0);
}

static void test_independent_ladders_combine(void)
{
	/* channel + rocker: different ladders, both decode -> controller sees the chord */
	ladder_cfg_t c1, c2;
	ladder_cfg_ladder1(&c1, 0);
	ladder_cfg_ladder2(&c2);
	ladder_t a, b;
	ladder_init(&a, &c1);
	ladder_init(&b, &c2);
	uint16_t m = 0;
	for (int i = 0; i < 2; i++) m = (uint16_t)(ladder_update(&a, 403) | ladder_update(&b, 1220));
	CHECK(m == (B(T2) | B(FWD)));
}

static void test_mask_names(void)
{
	char buf[64];
	ladder_mask_names(0, buf, sizeof buf);
	CHECK(strcmp(buf, "-") == 0);
	ladder_mask_names((uint16_t)(B(T1) | B(PLAY) | B(FN) | B(REV)), buf, sizeof buf);
	CHECK(strcmp(buf, "T1+PLAY+FN+REV") == 0);
	ladder_mask_names(0x3FF, buf, 8); /* truncates safely */
	CHECK(strlen(buf) == 7);
}

static void test_fader(void)
{
	fader_t f;
	fader_init(&f, FADER_RAW_LO_DEFAULT, FADER_RAW_HI_DEFAULT);
	/* end stops */
	CHECK(fader_update(&f, 0) == 0.0f);
	for (int i = 0; i < 40; i++) fader_update(&f, 3700);
	CHECK(f.out == 1.0f);
	for (int i = 0; i < 40; i++) fader_update(&f, 4095); /* past the calibrated top */
	CHECK(f.out == 1.0f);
	for (int i = 0; i < 40; i++) fader_update(&f, 20);   /* within the 1 % snap */
	CHECK(f.out == 0.0f);
	/* middle, Tape Looper scaling raw/3700 */
	for (int i = 0; i < 40; i++) fader_update(&f, 1850);
	CHECK_NEAR(f.out, 0.5, 0.003);
	/* jitter of +-4 counts does not move the output (hysteresis 6) */
	for (int i = 0; i < 10; i++) fader_update(&f, 1850 + ((i & 1) ? 4 : -4));
	float held = f.out;
	for (int i = 0; i < 50; i++) fader_update(&f, 1850 + ((i & 1) ? 4 : -4));
	CHECK(f.out == held);
	/* a real move does, within a few reads */
	fader_update(&f, 2220);
	fader_update(&f, 2220);
	fader_update(&f, 2220);
	CHECK(f.out > held);
	for (int i = 0; i < 40; i++) fader_update(&f, 2220);
	CHECK_NEAR(f.out, 2220.0 / 3700.0, 0.003);
	/* ADC error holds */
	float o = f.out;
	CHECK(fader_update(&f, -1) == o);
	/* monotonic sweep end to end stays within 0..1 and is non-decreasing */
	fader_t g;
	fader_init(&g, 0, 3700);
	float last = -1.0f;
	int mono = 1, inrange = 1;
	for (int r = 0; r <= 4095; r += 15) {
		float x = fader_update(&g, r);
		if (x < last) mono = 0;
		if (x < 0.0f || x > 1.0f) inrange = 0;
		last = x;
	}
	CHECK(mono);
	CHECK(inrange);
	for (int i = 0; i < 20; i++) fader_update(&g, 4095);
	CHECK(g.out == 1.0f);
	/* custom end-stop calibration */
	fader_t h;
	fader_init(&h, 200, 3400);
	for (int i = 0; i < 40; i++) fader_update(&h, 1800);
	CHECK_NEAR(h.out, 0.5, 0.003);
	for (int i = 0; i < 40; i++) fader_update(&h, 150);
	CHECK(h.out == 0.0f);
}

int main(void)
{
	RUN(test_ladder1_nominals);
	RUN(test_ladder1_play_chords_option);
	RUN(test_ladder2_nominals);
	RUN(test_stability_and_transit);
	RUN(test_hysteresis);
	RUN(test_independent_ladders_combine);
	RUN(test_mask_names);
	RUN(test_fader);
	T_DONE();
}
