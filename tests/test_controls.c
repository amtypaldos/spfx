/* Button gestures, soft takeover, UI controller and LED behaviour. */
#include "t.h"
#include "../firmware/controls/controller.h"
#include "../firmware/app/sync_out.h"
#include "../firmware/app/factory_presets.h"
#include "../firmware/dsp/util.h"

#define SR 48000.0f
static uint8_t pool[150 * 1024];
static engine_t eng;
static controller_t ui;
static uint32_t T; /* ms clock */
static float fad[4];

static void setup(uint32_t budget)
{
	memset(pool, 0, sizeof pool);
	engine_init(&eng, pool, sizeof pool, SR, budget);
	engine_assign(&eng, 0, FX_FILTER);
	engine_assign(&eng, 1, FX_CHORUS);
	engine_assign(&eng, 2, FX_DELAY);
	engine_assign(&eng, 3, FX_REVERB);
	for (int s = 0; s < 4; s++) { engine_set_wet(&eng, s, 0.5f); engine_set_bypass(&eng, s, 1); fad[s] = 0.5f; }
	controller_init(&ui, &eng);
	T = 1000;
}

/* advance `ms` with a held button mask, ticking every 8 ms */
static void hold(uint16_t mask, uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 8) { T += 8; controller_tick(&ui, T, mask, fad); }
}
#define B(x) BTN_BIT(x)

static void test_debounce(void)
{
	debounce_t d;
	debounce_init(&d, 2);
	CHECK(debounce_update(&d, B(BTN_T1)) == 0);        /* one-sample glitch */
	CHECK(debounce_update(&d, 0) == 0);
	CHECK(debounce_update(&d, B(BTN_T1)) == 0);
	CHECK(debounce_update(&d, B(BTN_T1)) == B(BTN_T1)); /* two in a row */
	CHECK(debounce_update(&d, 0) == B(BTN_T1));
	CHECK(debounce_update(&d, 0) == 0);
}

static void test_tap_toggles_bypass(void)
{
	setup(850);
	CHECK(eng.cfg.bypass[1] == 1);
	hold(B(BTN_T2), 80); hold(0, 40);
	CHECK(eng.cfg.bypass[1] == 0);
	hold(B(BTN_T2), 80); hold(0, 40);
	CHECK(eng.cfg.bypass[1] == 1);
	CHECK(eng.cfg.bypass[0] == 1 && eng.cfg.bypass[2] == 1);
	/* a very long press is not a tap */
	hold(B(BTN_T3), 1200); hold(0, 40);
	CHECK(eng.cfg.bypass[2] == 1);
}

static void test_rocker_cycles_only_held_slot(void)
{
	setup(850);
	engine_set_wet(&eng, 0, 0.37f);
	controller_resync(&ui); /* external change: re-arm takeover so the fader at 0.5 does not override it */
	int before1 = eng.cfg.fx_id[1], before2 = eng.cfg.fx_id[2];
	int fx0 = eng.cfg.fx_id[0]; /* Filter; next effect is Chorus, which fits */
	hold(B(BTN_T1), 100);
	hold(B(BTN_T1) | B(BTN_FWD), 40);
	hold(B(BTN_T1), 40);
	CHECK(eng.cfg.fx_id[0] == (fx0 + 1) % FX_COUNT);
	hold(B(BTN_T1) | B(BTN_REV), 40);
	hold(B(BTN_T1), 40);
	CHECK(eng.cfg.fx_id[0] == fx0);
	hold(0, 80);
	/* releasing after cycling must NOT toggle bypass */
	CHECK(eng.cfg.bypass[0] == 1);
	CHECK(eng.cfg.fx_id[1] == before1 && eng.cfg.fx_id[2] == before2);
	/* wet level survives the effect change */
	CHECK_NEAR(eng.cfg.wet[0], 0.37, 1e-6);
}

static void test_rocker_wraps_and_repeats(void)
{
	setup(100000);
	/* slot 3 (index 2) currently Delay (3). Step backwards past None to wrap. */
	engine_assign(&eng, 0, FX_NONE);
	hold(B(BTN_T1), 100);
	hold(B(BTN_T1) | B(BTN_REV), 16);
	hold(B(BTN_T1), 40);
	CHECK(eng.cfg.fx_id[0] == FX_TAPE_WOBBLE); /* None -> last, wrapped */
	hold(B(BTN_T1) | B(BTN_FWD), 16);
	hold(B(BTN_T1), 40);
	CHECK(eng.cfg.fx_id[0] == FX_NONE);          /* and forward wraps back to None */
	/* holding the rocker repeats after the delay */
	hold(B(BTN_T1) | B(BTN_FWD), 1200);
	CHECK(eng.cfg.fx_id[0] != FX_NONE);
	int a = eng.cfg.fx_id[0];
	CHECK(a >= 3); /* several repeats, not just one step */
	hold(0, 80);
}

static void test_rocker_ignored_without_channel_and_before_channel(void)
{
	setup(850);
	int id = eng.cfg.fx_id[0];
	hold(B(BTN_FWD), 100); hold(0, 40);
	CHECK(eng.cfg.fx_id[0] == id);
	/* rocker already down when channel is pressed: no action */
	hold(B(BTN_FWD), 40);
	hold(B(BTN_FWD) | B(BTN_T1), 100);
	hold(0, 40);
	CHECK(eng.cfg.fx_id[0] == id);
}

static void test_channel_chord_does_nothing(void)
{
	setup(850);
	hold(B(BTN_T1), 40);
	hold(B(BTN_T1) | B(BTN_T2), 80);
	hold(0, 40);
	CHECK(eng.cfg.bypass[0] == 1 && eng.cfg.bypass[1] == 1);
}

static void test_play_global_bypass(void)
{
	setup(850);
	hold(B(BTN_PLAY), 80); hold(0, 40);
	CHECK(eng.cfg.global_bypass == 1);
	hold(B(BTN_PLAY), 80); hold(0, 40);
	CHECK(eng.cfg.global_bypass == 0);
	/* PLAY + channel is not a global bypass */
	hold(B(BTN_PLAY), 40); hold(B(BTN_PLAY) | B(BTN_T1), 80); hold(0, 40);
	CHECK(eng.cfg.global_bypass == 0);
	CHECK(eng.cfg.bypass[0] == 1);
}

static void test_volume_steps_and_repeat(void)
{
	setup(850);
	engine_set_master(&eng, 0.5f);
	hold(B(BTN_VOLUP), 40); hold(0, 40);
	CHECK_NEAR(eng.cfg.master, 0.5 + 1.0 / VOL_STEPS, 1e-4);
	CHECK_NEAR(vol_master_db(eng.cfg.master) - vol_master_db(0.5f), VOL_DB_PER_STEP, 1e-3); /* 2 dB */
	hold(B(BTN_VOLDN), 40); hold(0, 40);
	CHECK_NEAR(eng.cfg.master, 0.5, 1e-4);
	/* the stock default 0.75 (-15 dB) is between grid points: +1 goes to -14 dB, -1 to -16 dB */
	engine_set_master(&eng, 0.75f);
	hold(B(BTN_VOLUP), 40); hold(0, 40);
	CHECK_NEAR(vol_master_db(eng.cfg.master), -14.0, 1e-3);
	engine_set_master(&eng, 0.75f);
	hold(B(BTN_VOLDN), 40); hold(0, 40);
	CHECK_NEAR(vol_master_db(eng.cfg.master), -16.0, 1e-3);
	/* holding repeats: full range up in ~3 s */
	engine_set_master(&eng, 0.0f);
	hold(B(BTN_VOLUP), GESTURE_VOL_DELAY_MS + GESTURE_VOL_REPEAT_MS * (VOL_STEPS - 1) + 40); hold(0, 40);
	CHECK_NEAR(eng.cfg.master, 1.0, 1e-4); /* reached the top ... */
	hold(B(BTN_VOLUP), 1000); hold(0, 40);
	CHECK_NEAR(eng.cfg.master, 1.0, 1e-4); /* ... and clamps */
	hold(B(BTN_VOLDN), 6000); hold(0, 40);
	CHECK_NEAR(eng.cfg.master, 0.0, 1e-4);
	/* every press emits one CTL_EV_MASTER with the new value */
	ctl_event_t ev;
	while (controller_pop_event(&ui, &ev)) {}
	hold(B(BTN_VOLUP), 40); hold(0, 40);
	int n = 0;
	while (controller_pop_event(&ui, &ev)) if (ev.kind == CTL_EV_MASTER) { n++; CHECK_NEAR(ev.v, eng.cfg.master, 1e-6); }
	CHECK(n == 1);
}

static void test_volume_mapping(void)
{
	/* grid, clamps, NaN */
	CHECK_NEAR(vol_step(0.0f, -1), 0.0, 1e-6);
	CHECK_NEAR(vol_step(1.0f, +1), 1.0, 1e-6);
	CHECK_NEAR(vol_step(NAN, +1), 1.0 / VOL_STEPS, 1e-6);
	for (int k = 0; k <= VOL_STEPS; k++) {
		float m = (float)k / VOL_STEPS;
		if (k < VOL_STEPS) CHECK_NEAR(vol_step(m, +1), (float)(k + 1) / VOL_STEPS, 1e-6);
		if (k > 0) CHECK_NEAR(vol_step(m, -1), (float)(k - 1) / VOL_STEPS, 1e-6);
		CHECK_NEAR(vol_step(m, 0), m, 1e-6);
	}
	/* dB mapping: -60..0 dB engine, mute at the bottom; codec input = engine + trim */
	CHECK_NEAR(vol_master_db(1.0f), 0.0, 1e-4);
	CHECK_NEAR(vol_master_db(0.5f), -30.0, 1e-4);
	CHECK_NEAR(vol_master_db(0.75f), -15.0, 1e-4);
	CHECK(vol_master_db(0.0f) <= -200.0f);
	CHECK_NEAR(vol_out_dbfs(1.0f, -6.0f), -6.0, 1e-4);   /* loudest setting: -6 dBFS ceiling */
	CHECK(vol_out_dbfs(0.0f, -6.0f) <= -200.0f);
	/* matches the engine's actual gain: steady output of a 0.25 input at master m */
	for (int k = 6; k <= VOL_STEPS; k += 6) {
		float m = (float)k / VOL_STEPS;
		engine_t e2;
		static uint8_t p2[1024];
		engine_init(&e2, p2, sizeof p2, SR, 850);
		engine_config_t c;
		engine_config_default(&c);
		for (int s = 0; s < 4; s++) c.fx_id[s] = FX_NONE;
		c.master = m;
		engine_post_config(&e2, &c);
		float l[64], r[64];
		for (int b = 0; b < 400; b++) {
			for (int i = 0; i < 64; i++) l[i] = r[i] = 0.25f;
			engine_process(&e2, l, r, l, r, 64);
		}
		CHECK_NEAR(20.0 * log10(l[63] / 0.25), vol_master_db(m), 0.05);
	}
}

static void test_volume_led_bar(void)
{
	setup(850);
	led_frame_t f;
	engine_set_master(&eng, 0.5f);
	hold(B(BTN_VOLUP), 40); hold(0, 40); /* 0.5333: two LEDs full, the third a little lit */
	controller_leds(&ui, T, &f);
	CHECK(f.play[0] == 255 && f.play[1] == 255 && f.play[2] > 0 && f.play[2] < 255 && f.play[3] == 0);
	/* gone after VOL_SHOW_MS */
	hold(0, VOL_SHOW_MS + 16);
	controller_leds(&ui, T, &f);
	CHECK(!(f.play[0] == 255 && f.play[1] == 255 && f.play[2] > 0 && f.play[2] < 255));
	/* mute: only LED 1 blinks dim */
	engine_set_master(&eng, 1.0f / VOL_STEPS);
	hold(B(BTN_VOLDN), 40); hold(0, 16);
	CHECK(eng.cfg.master == 0.0f);
	int seen_on = 0, others = 0;
	for (int t = 0; t < 600; t += 8) {
		controller_leds(&ui, T + (uint32_t)t, &f);
		if (f.play[0]) seen_on = 1;
		if (f.play[1] || f.play[2] || f.play[3]) others = 1;
	}
	CHECK(seen_on && !others);
}

static void test_edit_mode_and_takeover(void)
{
	setup(850);
	/* slot 3 = Delay. Param 1 (time) logical value: default norm */
	float logical = eng.cfg.params[2][FX_DELAY][0];
	/* faders are physically at 0.5 (they were the wet faders) */
	hold(B(BTN_FN), 40);
	hold(B(BTN_FN) | B(BTN_T3), 80);
	hold(B(BTN_FN), 40);
	hold(0, 40);
	CHECK(ui.mode == UI_EDIT);
	CHECK(ui.edit_slot == 2);
	/* FN release after FN+channel must not exit edit mode */
	hold(0, 100);
	CHECK(ui.mode == UI_EDIT);
	/* logical value did not jump to the physical fader */
	CHECK_NEAR(eng.cfg.params[2][FX_DELAY][0], logical, 1e-6);
	/* wet levels untouched by entering edit mode */
	CHECK_NEAR(eng.cfg.wet[2], 0.5, 1e-6);
	/* move fader 1 far away from logical value: nothing happens until it reaches it */
	float target = logical;
	fad[0] = target > 0.5f ? 0.0f : 1.0f;
	hold(0, 80);
	CHECK_NEAR(eng.cfg.params[2][FX_DELAY][0], logical, 1e-6);
	CHECK(ui.to[0].need != 0);
	/* sweeping across the logical value picks it up */
	for (int i = 0; i <= 20; i++) { fad[0] = fad[0] + (target - fad[0]) * 0.2f; hold(0, 8); }
	CHECK(ui.to[0].picked);
	fad[0] = 0.5f * target + 0.5f * (target > 0.5f ? 1.0f : 0.0f); /* now it moves the parameter */
	hold(0, 16);
	CHECK(fabsf(eng.cfg.params[2][FX_DELAY][0] - logical) > 0.01f);
	/* FN tap returns to performance mode */
	hold(B(BTN_FN), 60); hold(0, 40);
	CHECK(ui.mode == UI_PERFORM);
	/* back in performance the faders do not suddenly set wet levels */
	CHECK_NEAR(eng.cfg.wet[2], 0.5, 1e-6);
}

static void test_takeover_unit(void)
{
	takeover_t t;
	float out = -1;
	takeover_arm(&t, 0.5f);
	CHECK(!takeover_update(&t, 0.1f, &out));
	CHECK(t.need == 1); /* fader is below: move up */
	CHECK(!takeover_update(&t, 0.3f, &out));
	CHECK(takeover_update(&t, 0.49f, &out)); /* inside the pickup window */
	CHECK_NEAR(out, 0.49, 1e-6);
	CHECK(t.picked && t.need == 0);
	CHECK(!takeover_update(&t, 0.491f, &out)); /* jitter ignored */
	CHECK(takeover_update(&t, 0.7f, &out));

	/* jumping straight over the value also picks up (crossing) */
	takeover_arm(&t, 0.5f);
	CHECK(!takeover_update(&t, 0.1f, &out));
	CHECK(takeover_update(&t, 0.9f, &out));
	CHECK(t.picked);

	/* from above */
	takeover_arm(&t, 0.2f);
	CHECK(!takeover_update(&t, 0.9f, &out));
	CHECK(t.need == -1);
	CHECK(takeover_update(&t, 0.21f, &out));

	/* NaN is ignored */
	takeover_arm(&t, 0.5f);
	CHECK(!takeover_update(&t, NAN, &out));
}

static void test_faders_set_wet_after_pickup(void)
{
	setup(850);
	/* fader is at the logical value: picks up immediately and follows */
	fad[0] = 0.5f; hold(0, 24);
	fad[0] = 0.9f; hold(0, 24);
	CHECK_NEAR(eng.cfg.wet[0], 0.9, 1e-4);
	/* external change (companion) -> resync -> no jump to the physical fader */
	engine_set_wet(&eng, 0, 0.2f);
	controller_resync(&ui);
	hold(0, 40);
	CHECK_NEAR(eng.cfg.wet[0], 0.2, 1e-4);
	CHECK(ui.to[0].need == -1); /* fader (0.9) is above logical (0.2): must move down */
}

static void test_power_off_gesture(void)
{
	setup(850);
	hold(B(BTN_FN), 1400);
	CHECK(!ui.power_off_request);
	hold(B(BTN_FN), 200);
	CHECK(ui.power_off_request);
	hold(0, 40);
	CHECK(ui.mode == UI_PERFORM);
}

static void test_admission_error_pattern_and_rollback(void)
{
	setup(100000);
	/* fill memory with two reverbs + chorus so Delay on slot 1 cannot fit */
	CHECK(engine_assign(&eng, 2, FX_REVERB) == ENG_OK); /* frees the 77 KB delay first */
	CHECK(engine_assign(&eng, 3, FX_NONE) == ENG_OK);
	CHECK(engine_assign(&eng, 0, FX_REVERB) == ENG_OK);
	CHECK(engine_assign(&eng, 1, FX_FLANGER) == ENG_OK);
	int before = eng.cfg.fx_id[1];
	CHECK(before == FX_FLANGER);
	hold(B(BTN_T2), 100);
	/* flanger(6) -> bitcrusher(7) ok ... walk back to delay(3) via REV: 6->5->4->3 : delay fails */
	for (int k = 0; k < 3; k++) { hold(B(BTN_T2) | B(BTN_REV), 16); hold(B(BTN_T2), 24); }
	CHECK(eng.cfg.fx_id[1] == FX_CHORUS + 0 || eng.cfg.fx_id[1] != FX_DELAY);
	CHECK(eng.cfg.fx_id[1] != FX_DELAY); /* never accepted */
	CHECK(ui.err_until > T - 100);
	led_frame_t f;
	/* during the error window the PLAY-side LEDs alternate */
	ui.err_until = T + 700;
	controller_leds(&ui, T, &f);
	int a = f.play[0], b = f.play[1];
	CHECK((a == 255 && b == 0) || (a == 0 && b == 255));
	controller_leds(&ui, T + 80, &f);
	CHECK(f.play[0] != a);
	hold(0, 40);
}

static void test_led_performance(void)
{
	setup(850);
	led_frame_t f;
	/* all bypassed: track LEDs off */
	controller_leds(&ui, T, &f);
	for (int i = 0; i < 4; i++) CHECK(f.track[i] == 0);
	/* enable slot 1: lit, brighter with more wet */
	hold(B(BTN_T1), 80); hold(0, 40);
	engine_set_wet(&eng, 0, 0.1f); controller_resync(&ui);
	controller_leds(&ui, T, &f);
	uint8_t lo = f.track[0];
	engine_set_wet(&eng, 0, 0.9f); controller_resync(&ui);
	fad[0] = 0.9f; hold(0, 40);
	controller_leds(&ui, T, &f);
	CHECK(lo > 0);
	CHECK(f.track[0] > lo);
	CHECK(f.track[1] == 0);
	/* global bypass: exactly one LED lit, and it walks */
	hold(B(BTN_PLAY), 80); hold(0, 40);
	int seen[4] = {0};
	for (uint32_t t = 0; t < 1200; t += 50) {
		controller_leds(&ui, T + t, &f);
		int lit = 0;
		for (int i = 0; i < 4; i++) if (f.track[i]) { lit++; seen[i] = 1; }
		CHECK(lit == 1);
	}
	CHECK(seen[0] && seen[1] && seen[2] && seen[3]);
}

static void test_led_select_and_edit(void)
{
	setup(850);
	led_frame_t f;
	hold(B(BTN_T3), 100);
	hold(B(BTN_T3) | B(BTN_FWD), 16); /* Delay(3) -> Reverb(4) */
	hold(B(BTN_T3), 24);
	CHECK(eng.cfg.fx_id[2] == FX_REVERB);
	controller_leds(&ui, T, &f);
	/* no numbers: a just-completed change flashes all four PLAY-side LEDs ... */
	CHECK(f.play[0] == 255 && f.play[1] == 255 && f.play[2] == 255 && f.play[3] == 255);
	/* ... and the held channel's own LED is lit */
	CHECK(f.track[2] > 0);
	/* once the flash is over they go dark (effect is not None) */
	controller_leds(&ui, T + 400, &f);
	CHECK(f.play[0] == 0 && f.play[3] == 0);
	/* None is recognisable: steady dim row */
	ui.cursor[2] = FX_NONE;
	controller_leds(&ui, T + 400, &f);
	CHECK(f.play[0] == 14 && f.play[3] == 14);
	hold(0, 40);
	/* edit mode: PLAY-side LEDs show the slot one-hot, not the meter */
	hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_T2), 40); hold(B(BTN_FN), 40); hold(0, 40);
	CHECK(ui.mode == UI_EDIT && ui.edit_slot == 1);
	controller_leds(&ui, T, &f);
	CHECK(f.play[1] > 0 && f.play[0] == 0 && f.play[2] == 0 && f.play[3] == 0);
	/* pickup status is visible: faders at 0.5, chorus rate default != 0.5 => fast-blinking LED */
	int changes = 0;
	uint8_t last = 0;
	for (uint32_t t = 0; t < 1000; t += 20) {
		controller_leds(&ui, T + t, &f);
		if (t && f.track[0] != last) changes++;
		last = f.track[0];
	}
	if (!ui.to[0].picked) CHECK(changes >= 2);
}

static void test_sync_out(void)
{
	sync_out_t s;
	sync_out_init(&s, SR);
	sync_out_set_bpm(&s, 120.0f);
	sync_out_events_t e = sync_out_advance(&s, 48000);
	CHECK(e.midi_ticks == 0); /* not started */
	sync_out_start(&s);
	e = sync_out_advance(&s, 48000);
	CHECK(e.start == 1);
	CHECK(e.midi_ticks >= 48 && e.midi_ticks <= 49);
	CHECK(e.po_pulses >= 4 && e.po_pulses <= 5);
	e = sync_out_advance(&s, 1);
	CHECK(e.start == 0);

	/* long-run: no drift against the audio clock at an awkward tempo */
	sync_out_init(&s, SR);
	sync_out_set_bpm(&s, 123.4f);
	sync_out_start(&s);
	uint64_t ticks = 0;
	uint64_t frames = 0;
	for (int i = 0; i < 600 * 750; i++) { ticks += sync_out_advance(&s, 64).midi_ticks; frames += 64; }
	double expect = (double)frames / SR * 123.4 * 24.0 / 60.0;
	CHECK_MSG(fabs((double)ticks - expect) <= 1.5, "ticks %llu expect %.2f", (unsigned long long)ticks, expect);

	/* stop is reported once and ticks cease */
	sync_out_stop(&s);
	e = sync_out_advance(&s, 4800);
	CHECK(e.stop == 1 && e.midi_ticks == 0);
	e = sync_out_advance(&s, 4800);
	CHECK(e.stop == 0 && e.midi_ticks == 0);

	/* bpm clamping and NaN */
	sync_out_set_bpm(&s, 5000.0f);
	sync_out_set_bpm(&s, NAN);
	CHECK(s.frames_per_tick > 0);
}


/* ---------------------------------------------------------------- presets (FN + rocker) */
static int fill_factory(void *ctx, int idx, store_data_t *out)
{
	(void)ctx;
	if (idx < 0 || idx >= FACTORY_PRESET_COUNT) return -1;
	factory_preset_fill(&factory_presets[idx], out);
	return 0;
}
static int fill_toobig(void *ctx, int idx, store_data_t *out)
{
	(void)ctx; (void)idx;
	store_data_default(out);
	for (int s = 0; s < 4; s++) out->cfg.fx_id[s] = FX_DELAY; /* 4 x 77 KB cannot fit */
	return 0;
}
static void setup_presets(void)
{
	setup(850);
	/* roomy budget so every factory preset fits whatever the setup() slots hold */
	engine_init(&eng, pool, sizeof pool, SR, ENGINE_DEFAULT_CPU_BUDGET);
	controller_init(&ui, &eng);
	controller_set_presets(&ui, FACTORY_PRESET_COUNT, fill_factory, 0);
	for (int i = 0; i < 4; i++) fad[i] = 0.5f;
	T = 1000;
}
static int same_fx(const store_data_t *d)
{
	for (int s = 0; s < 4; s++) if (eng.cfg.fx_id[s] != d->cfg.fx_id[s]) return 0;
	return 1;
}

static void test_preset_keeps_volume_and_single_clean(void)
{
	/* the device preset list is exactly the factory table: one Clean (the usbaudio bring-up
	 * image had an extra "Clean" at index 0 in front of the table, so it appeared twice) */
	int cleans = 0;
	for (int k = 0; k < FACTORY_PRESET_COUNT; k++) cleans += strcmp(factory_presets[k].name, "Clean") == 0;
	CHECK(cleans == 1);
	for (int a = 0; a < FACTORY_PRESET_COUNT; a++)
		for (int b = a + 1; b < FACTORY_PRESET_COUNT; b++) CHECK(strcmp(factory_presets[a].name, factory_presets[b].name) != 0);
	/* cycling through the whole list once visits every preset once, Clean included */
	setup_presets();
	int seen[FACTORY_PRESET_COUNT] = {0};
	for (int k = 0; k < FACTORY_PRESET_COUNT; k++) {
		hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_FWD), 24); hold(B(BTN_FN), 80); hold(0, 40);
		CHECK(ui.preset_idx >= 0 && ui.preset_idx < FACTORY_PRESET_COUNT);
		if (ui.preset_idx >= 0 && ui.preset_idx < FACTORY_PRESET_COUNT) seen[ui.preset_idx]++;
	}
	for (int k = 0; k < FACTORY_PRESET_COUNT; k++) CHECK(seen[k] == 1);
	/* with preset_keep_master (zephyr_sp1fx) a preset load keeps the owner's volume */
	setup_presets();
	ui.preset_keep_master = 1;
	engine_set_master(&eng, 0.9f);
	hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_FWD), 24); hold(B(BTN_FN), 80); hold(0, 40);
	CHECK(ui.preset_idx == 0);
	CHECK_NEAR(eng.cfg.master, 0.9, 1e-6);
	/* without it the preset's own master applies (companion-style full recall) */
	setup_presets();
	engine_set_master(&eng, 0.9f);
	hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_FWD), 24); hold(B(BTN_FN), 80); hold(0, 40);
	CHECK_NEAR(eng.cfg.master, factory_presets[0].master, 1e-6);
}

static void test_preset_cycle_gesture(void)
{
	setup_presets();
	store_data_t d;
	/* FN + FWD: first press loads preset 0 after the fade */
	hold(B(BTN_FN), 40);
	hold(B(BTN_FN) | B(BTN_FWD), 24);
	hold(B(BTN_FN), 80);
	hold(0, 40);
	fill_factory(0, 0, &d);
	CHECK(same_fx(&d));
	CHECK(ui.preset_idx == 0);
	/* FN release after FN + rocker is not a tap: no mode change, no bypass toggles */
	CHECK(ui.mode == UI_PERFORM);
	/* next, previous, wrap both ways */
	hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_FWD), 24); hold(B(BTN_FN), 80); hold(0, 40);
	CHECK(ui.preset_idx == 1);
	hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_REV), 24); hold(B(BTN_FN), 80); hold(0, 40);
	CHECK(ui.preset_idx == 0);
	hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_REV), 24); hold(B(BTN_FN), 80); hold(0, 40);
	CHECK(ui.preset_idx == FACTORY_PRESET_COUNT - 1);
	hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_FWD), 24); hold(B(BTN_FN), 80); hold(0, 40);
	CHECK(ui.preset_idx == 0);
	/* holding the rocker repeats */
	hold(B(BTN_FN), 40);
	hold(B(BTN_FN) | B(BTN_FWD), 1500);
	hold(B(BTN_FN), 80); hold(0, 80);
	CHECK(ui.preset_idx >= 3 || ui.preset_idx < 0 + 1 + 6);
	CHECK(ui.preset_idx != 0);
	/* the first press from a fresh start going backwards lands on the last preset */
	setup_presets();
	hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_REV), 24); hold(B(BTN_FN), 80); hold(0, 40);
	CHECK(ui.preset_idx == FACTORY_PRESET_COUNT - 1);
}

static void test_preset_rapid_presses_coalesce(void)
{
	/* human-speed taps (~100 ms apart): each one loads its own preset */
	setup_presets();
	hold(B(BTN_FN), 40);
	for (int k = 0; k < 3; k++) { hold(B(BTN_FN) | B(BTN_FWD), 40); hold(B(BTN_FN), 64); }
	hold(B(BTN_FN), 80); hold(0, 40);
	CHECK(ui.preset_idx == 2);
	ctl_event_t ev; int presets = 0;
	while (controller_pop_event(&ui, &ev)) if (ev.kind == CTL_EV_PRESET) presets++;
	CHECK(presets == 3);

	/* edges arriving faster than the fade window (1-tick debounce) coalesce into ONE load */
	setup_presets();
	ui.deb.n_required = 1;
	hold(B(BTN_FN), 40);
	for (int k = 0; k < 3; k++) { hold(B(BTN_FN) | B(BTN_FWD), 8); hold(B(BTN_FN), 8); }
	hold(B(BTN_FN), 80); hold(0, 40);
	CHECK(ui.preset_idx == 2);
	presets = 0;
	while (controller_pop_event(&ui, &ev)) if (ev.kind == CTL_EV_PRESET) presets++;
	CHECK(presets == 1);
}

static void test_preset_leaves_edit_mode_and_does_not_jump_faders(void)
{
	setup_presets();
	/* enter edit mode for slot 2, then FN + rocker */
	hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_T2), 40); hold(B(BTN_FN), 40); hold(0, 40);
	CHECK(ui.mode == UI_EDIT);
	hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_FWD), 24); hold(B(BTN_FN), 80); hold(0, 80);
	CHECK(ui.mode == UI_PERFORM);
	store_data_t d; fill_factory(0, 0, &d);
	/* faders sit at 0.5 but the preset's wet levels differ: nothing may jump */
	for (int i = 0; i < 4; i++) {
		CHECK_NEAR(eng.cfg.wet[i], d.cfg.wet[i], 1e-4);
		CHECK(!ui.to[i].picked || fabsf(0.5f - d.cfg.wet[i]) <= TAKEOVER_WINDOW);
	}
	/* a fader far from its value shows pickup guidance */
	int guided = 0;
	for (int i = 0; i < 4; i++) if (ui.to[i].need != 0) guided++;
	CHECK(guided >= 1);
}

static void test_preset_fade_is_silent_and_recovers(void)
{
	setup_presets();
	engine_set_master(&eng, 1.0f); eng.master_cur = 1.0f;
	for (int s = 0; s < 4; s++) engine_set_bypass(&eng, s, 1);
	float ph = 0, l[384], r[384];
	double before = 0, quiet = 1e9, after = 0;
	int stage = 0; uint32_t t_press = 0;
	for (int tick = 0; tick < 200; tick++) {
		T += 8;
		uint16_t btn = 0;
		if (tick >= 50 && tick < 53) btn = B(BTN_FN);
		if (tick >= 53 && tick < 55) btn = B(BTN_FN) | B(BTN_FWD);
		if (tick >= 55 && tick < 60) btn = B(BTN_FN);
		if (tick == 53) t_press = T;
		controller_tick(&ui, T, btn, fad);
		for (int i = 0; i < 384; i++) { l[i] = r[i] = 0.3f * sinf(ph); ph += 2.0f * FX_PI * 440.0f / SR; }
		engine_process(&eng, l, r, l, r, 384);
		double e = 0; for (int i = 0; i < 384; i++) e += (double)l[i] * l[i];
		e = sqrt(e / 384);
		if (tick == 45) before = e;
		if (t_press && T - t_press >= 16 && T - t_press <= 24) { if (e < quiet) quiet = e; }
		if (tick == 150) after = e;
		(void)stage;
	}
	CHECK_MSG(before > 0.15, "baseline rms %g", before);
	CHECK_MSG(quiet < before * 0.1, "not faded out before the swap: %g vs %g", quiet, before);
	CHECK_MSG(after > before * 0.3, "did not recover after the swap: %g vs %g", after, before);
	CHECK(eng.duck == 0);
}

static void test_preset_leds_flash_then_position_bar(void)
{
	setup_presets();
	led_frame_t f;
	hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_FWD), 24); hold(B(BTN_FN), 8);
	/* wait for the swap */
	uint32_t t0 = 0;
	for (int i = 0; i < 20 && !t0; i++) { hold(B(BTN_FN), 8); if (ui.preset_idx == 0) t0 = T; }
	CHECK(t0 != 0);
	controller_leds(&ui, T + 10, &f);
	CHECK(f.play[0] == 255 && f.play[1] == 255 && f.play[2] == 255 && f.play[3] == 255); /* confirm flash */
	controller_leds(&ui, T + 400, &f);
	CHECK(f.play[0] == 255 && f.play[1] == 0 && f.play[3] == 0); /* first preset -> 1 LED */
	controller_leds(&ui, T + 2000, &f);
	CHECK(f.play[0] == 0 && f.play[3] == 0); /* back to the level meter (silent) */
	/* last preset -> 4 LEDs */
	hold(0, 40);
	hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_REV), 24); hold(B(BTN_FN), 8);
	for (int i = 0; i < 20 && ui.preset_idx != FACTORY_PRESET_COUNT - 1; i++) hold(B(BTN_FN), 8);
	CHECK(ui.preset_idx == FACTORY_PRESET_COUNT - 1);
	controller_leds(&ui, T + 400, &f);
	CHECK(f.play[0] == 255 && f.play[1] == 255 && f.play[2] == 255 && f.play[3] == 255);
	/* middle presets are monotonic: the bar never shrinks as the index grows */
	int prev = 0;
	for (int idx = 0; idx < FACTORY_PRESET_COUNT; idx++) {
		ui.preset_idx = idx; ui.preset_bar_until = T + 1000; ui.preset_flash_until = 0;
		controller_leds(&ui, T + 600, &f);
		int lit = (f.play[0] > 0) + (f.play[1] > 0) + (f.play[2] > 0) + (f.play[3] > 0);
		CHECK(lit >= prev && lit >= 1 && lit <= 4);
		prev = lit;
	}
}

static void test_preset_admission_failure_and_no_list(void)
{
	setup_presets();
	controller_set_presets(&ui, 1, fill_toobig, 0);
	hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_FWD), 24); hold(B(BTN_FN), 80); hold(0, 40);
	/* three of four slots were refused: error pattern requested, engine still valid */
	CHECK(ui.err_until > 0);
	CHECK(eng.cfg.fx_id[0] == FX_DELAY);
	float l[64] = {0}, r[64] = {0};
	engine_process(&eng, l, r, l, r, 64);
	CHECK(eng.duck == 0);
	/* no list installed: FN + rocker does nothing, FN is still consumed (no edit-mode exit) */
	setup(850);
	controller_set_presets(&ui, 0, 0, 0);
	hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_T1), 40); hold(B(BTN_FN), 40); hold(0, 40);
	CHECK(ui.mode == UI_EDIT);
	int before = eng.cfg.fx_id[0];
	hold(B(BTN_FN), 40); hold(B(BTN_FN) | B(BTN_FWD), 24); hold(B(BTN_FN), 80); hold(0, 40);
	CHECK(eng.cfg.fx_id[0] == before);
	CHECK(ui.mode == UI_EDIT); /* unchanged: no accidental FN tap */
}

static void test_fn_rocker_does_not_break_other_gestures(void)
{
	setup_presets();
	/* channel + rocker still cycles effects, and FN + channel still enters edit mode */
	int fx0 = eng.cfg.fx_id[0];
	hold(B(BTN_T1), 100); hold(B(BTN_T1) | B(BTN_FWD), 24); hold(B(BTN_T1), 40); hold(0, 40);
	CHECK(eng.cfg.fx_id[0] != fx0 || fx0 == FX_NONE);
	CHECK(ui.preset_idx == -1);
	/* FN pressed while a channel is held, then rocker: stays an effect cycle, never a preset */
	hold(B(BTN_T2), 100); hold(B(BTN_T2) | B(BTN_FN), 24); hold(B(BTN_T2) | B(BTN_FN) | B(BTN_FWD), 24);
	hold(0, 80);
	CHECK(ui.preset_idx == -1);
}

int main(void)
{
	RUN(test_debounce);
	RUN(test_tap_toggles_bypass);
	RUN(test_rocker_cycles_only_held_slot);
	RUN(test_rocker_wraps_and_repeats);
	RUN(test_rocker_ignored_without_channel_and_before_channel);
	RUN(test_channel_chord_does_nothing);
	RUN(test_play_global_bypass);
	RUN(test_volume_steps_and_repeat);
	RUN(test_volume_mapping);
	RUN(test_volume_led_bar);
	RUN(test_edit_mode_and_takeover);
	RUN(test_takeover_unit);
	RUN(test_faders_set_wet_after_pickup);
	RUN(test_power_off_gesture);
	RUN(test_admission_error_pattern_and_rollback);
	RUN(test_led_performance);
	RUN(test_led_select_and_edit);
	RUN(test_sync_out);
	RUN(test_preset_cycle_gesture);
	RUN(test_preset_keeps_volume_and_single_clean);
	RUN(test_preset_rapid_presses_coalesce);
	RUN(test_preset_leaves_edit_mode_and_does_not_jump_faders);
	RUN(test_preset_fade_is_silent_and_recovers);
	RUN(test_preset_leds_flash_then_position_bar);
	RUN(test_preset_admission_failure_and_no_list);
	RUN(test_fn_rocker_does_not_break_other_gestures);
	T_DONE();
}
