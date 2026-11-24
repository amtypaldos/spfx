/*
 * SP-1 FX controls bring-up image. NOT TESTED ON HARDWARE.
 *
 * Runs the real SP-1 FX control logic (firmware/controls/controller.c) on the
 * SP-1's buttons, faders and LEDs, with NO audio:
 *   every 8 ms: SAADC scan (2 ladders + 4 faders) + FUNCTION GPIO
 *     -> ladder decode (firmware/drivers/ladder.c) -> controller_tick()
 *     -> controller_leds() -> PWM brightness on the 8 LEDs.
 * The engine is a real engine_t (factory-default config applied with
 * engine_apply_config, factory presets installed for FN + rocker), but
 * engine_process() is never called: nothing reads or writes audio.
 * A USB CDC ACM console prints raw values and every gesture
 * (docs/BRINGUP_CONTROLS.md); optional via CONFIG_SP1FX_USB_CONSOLE.
 *
 * SP-1 bootloader rules ("BIG FIVE", SP-1-dev wiki "Bootloader" page and
 * chattock/sp1-tape-looper main.c, MIT), as in zephyr_min:
 *   - app at 0x20000 (checked by firmware/scripts/check_image.py),
 *   - watchdog fed < 5 s (fed every 8 ms loop, and in every wait),
 *   - do not re-init bootloader-owned clocks/peripherals beyond what Tape
 *     Looper does (USBD + SAADC via Zephyr drivers, as Tape Looper; LEDs on
 *     PWM0/PWM1, PWM2/PWM3 only stopped if routed to an LED pin),
 *   - SYSTEM_OFF is the way back to the bootloader (no reset pin),
 *   - RESETREAS is cleared at boot and before SYSTEM_OFF.
 * FN held 1.5 s alone = power off: decided ONLY by the controller
 * (GE_POWER_OFF -> ui.power_off_request); this file does not time FN itself.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>
#include <nrfx.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef CONFIG_SP1FX_USB_CONSOLE
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/usbd.h>
#include <sample_usbd.h>
#endif

#include "../../../drivers/controls_hw.h"
#include "../../../drivers/ladder.h"
#include "../../../controls/controller.h"
#include "../../factory_presets.h"

#define TICK_MS       8
#define STATUS_MS     100   /* 10 Hz status line */
#define FN_RELEASE_MS 60
#define SAMPLE_RATE   48000.0f
/* Same size the host tests and docs/STATUS.md use (150 KB), so admission
 * (which effects fit) behaves exactly as in the real firmware. It is .bss
 * and never touched: only engine_process() writes the pool, and this image
 * never calls it. */
#define POOL_BYTES    (150u * 1024u)

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

volatile uint32_t sp1fx_boot_resetreas;

static uint8_t pool[POOL_BYTES] __aligned(8);
static engine_t eng;
static controller_t ui;
static ladder_t lad1, lad2;
static fader_t fad[4];

static void feed_wdt(void)
{
	for (int ch = 0; ch < 8; ch++) {
		NRF_WDT->RR[ch] = WDT_RR_RR_Reload;
	}
}

/* ------------------------------ USB console ------------------------------ */
/* Same mechanism as firmware/app/zephyr_tone: our own ring buffer drained by
 * the CDC ACM IRQ callback; a line that does not fit is dropped whole, and
 * nothing is queued while the host has not opened the port (DTR low). */
#ifdef CONFIG_SP1FX_USB_CONSOLE
static const struct device *const cdc = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
RING_BUF_DECLARE(con_rb, 2048);
static struct k_spinlock con_lock;
static bool usb_ok;
static int usb_rc = 1;

static void cdc_cb(const struct device *dev, void *user)
{
	ARG_UNUSED(user);

	if (!uart_irq_update(dev) || !uart_irq_tx_ready(dev)) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&con_lock);
	uint8_t *p;
	uint32_t n = ring_buf_get_claim(&con_rb, &p, 64);
	int w = (n > 0u) ? uart_fifo_fill(dev, p, (int)n) : 0;

	if (w < 0) {
		w = 0;
	}
	(void)ring_buf_get_finish(&con_rb, (uint32_t)w);
	bool empty = ring_buf_is_empty(&con_rb);

	k_spin_unlock(&con_lock, key);
	if (empty) {
		uart_irq_tx_disable(dev);
	}
}

static void usb_console_start(void)
{
	struct usbd_context *ctx;

	if (!device_is_ready(cdc)) {
		usb_rc = -100;
		return;
	}
	ctx = sample_usbd_init_device(NULL);
	if (ctx == NULL) {
		usb_rc = -101;
		return;
	}
	usb_rc = usbd_enable(ctx);
	if (usb_rc != 0) {
		return;
	}
	uart_irq_callback_user_data_set(cdc, cdc_cb, NULL);
	usb_ok = true;
}

static bool con_host_open(void)
{
	uint32_t dtr = 0;

	if (!usb_ok) {
		return false;
	}
	(void)uart_line_ctrl_get(cdc, UART_LINE_CTRL_DTR, &dtr);
	return dtr != 0u;
}
#endif

static uint32_t con_drops;

static __maybe_unused void con_write(const char *s, int len)
{
#ifdef CONFIG_SP1FX_USB_CONSOLE
	if (len <= 0 || !con_host_open()) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&con_lock);
	bool fits = ring_buf_space_get(&con_rb) >= (uint32_t)len;

	if (fits) {
		(void)ring_buf_put(&con_rb, (const uint8_t *)s, (uint32_t)len);
	}
	k_spin_unlock(&con_lock, key);
	if (fits) {
		uart_irq_tx_enable(cdc);
	} else {
		con_drops++;
	}
#else
	ARG_UNUSED(s);
	ARG_UNUSED(len);
#endif
}

static void con_printf(const char *fmt, ...)
{
#ifdef CONFIG_SP1FX_USB_CONSOLE
	char buf[200];
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);

	va_end(ap);
	if (n < 0) {
		return;
	}
	if (n >= (int)sizeof(buf)) {
		n = (int)sizeof(buf) - 1;
		buf[n - 2] = '\r';
		buf[n - 1] = '\n';
	}
	con_write(buf, n);
#else
	ARG_UNUSED(fmt);
#endif
}

/* ------------------------------ presets ---------------------------------- */
/* Same adapter as tests/test_controls.c fill_factory(). */
static int fill_factory(void *ctx, int idx, store_data_t *out)
{
	ARG_UNUSED(ctx);
	if (idx < 0 || idx >= FACTORY_PRESET_COUNT) {
		return -1;
	}
	factory_preset_fill(&factory_presets[idx], out);
	return 0;
}

/* ------------------------------ diagnostics ------------------------------ */
static const char *fx_name(int id)
{
	const fx_desc *d = fx_get(id);

	return (d && d->name) ? d->name : "?";
}

static int pct(float v)
{
	return (int)(v * 100.0f + 0.5f);
}

/* Shadow gesture recogniser: the same pure functions with the same input as
 * the controller (debounce 2, then gesture_update), used only to print every
 * gesture as it happens. It does not influence the controller. */
static debounce_t sh_deb;
static gesture_t sh_gest;

static void log_gestures(uint32_t now, uint16_t buttons)
{
	gesture_event_t ev[GESTURE_MAX_EVENTS];
	uint16_t st = debounce_update(&sh_deb, buttons);
	int n = gesture_update(&sh_gest, st, now, ev);

	for (int i = 0; i < n; i++) {
		const gesture_event_t *e = &ev[i];

		switch (e->type) {
		case GE_CHAN_DOWN:
			con_printf("G %u T%d down\r\n", now, e->arg + 1);
			break;
		case GE_CHAN_UP:
			con_printf("G %u T%d up\r\n", now, e->arg + 1);
			break;
		case GE_TOGGLE_BYPASS:
			con_printf("G %u TAP T%d -> toggle bypass slot %d\r\n", now, e->arg + 1, e->arg + 1);
			break;
		case GE_CYCLE_FX:
			con_printf("G %u T%d + %s -> cycle effect slot %d %+d\r\n", now, e->arg + 1,
				   e->arg2 > 0 ? "FWD" : "REV", e->arg + 1, e->arg2);
			break;
		case GE_ENTER_EDIT:
			con_printf("G %u FN + T%d -> EDIT slot %d\r\n", now, e->arg + 1, e->arg + 1);
			break;
		case GE_FN_TAP:
			con_printf("G %u FN tap (leaves edit mode)\r\n", now);
			break;
		case GE_GLOBAL_BYPASS:
			con_printf("G %u PLAY tap -> toggle global bypass\r\n", now);
			break;
		case GE_VOL:
			con_printf("G %u VOL%c\r\n", now, e->arg > 0 ? '+' : '-');
			break;
		case GE_POWER_OFF:
			con_printf("G %u FN held 1.5 s -> POWER OFF\r\n", now);
			break;
		case GE_CYCLE_PRESET:
			con_printf("G %u FN + %s -> preset %+d\r\n", now, e->arg > 0 ? "FWD" : "REV", e->arg);
			break;
		default:
			con_printf("G %u type %d arg %d %d\r\n", now, e->type, e->arg, e->arg2);
			break;
		}
	}
}

/* Controller-side results (what actually changed, incl. admission errors).
 * Wet/param changes (one per fader tick) are not printed; the status line
 * shows the values. */
static void log_ctl_events(uint32_t now)
{
	ctl_event_t e;

	while (controller_pop_event(&ui, &e)) {
		switch (e.kind) {
		case CTL_EV_BYPASS:
			con_printf("C %u slot %d bypass=%d\r\n", now, e.slot + 1, e.a);
			break;
		case CTL_EV_SLOT_FX:
			con_printf("C %u slot %d effect -> %s (flash)\r\n", now, e.slot + 1, fx_name(e.a));
			break;
		case CTL_EV_ADMISSION:
			con_printf("C %u slot %d REJECTED %s status=%d (error pattern, old effect kept)\r\n",
				   now, e.slot + 1, fx_name(e.a), e.b);
			break;
		case CTL_EV_GLOBAL_BYPASS:
			con_printf("C %u global bypass=%d\r\n", now, e.a);
			break;
		case CTL_EV_MASTER:
			con_printf("C %u master=%d%%\r\n", now, pct(e.v));
			break;
		case CTL_EV_PRESET:
			con_printf("C %u preset %d/%d '%s' loaded\r\n", now, e.a + 1, FACTORY_PRESET_COUNT,
				   (e.a < FACTORY_PRESET_COUNT) ? factory_presets[e.a].name : "?");
			break;
		default:
			break;
		}
	}
}

static void log_status(uint32_t now, const controls_raw_t *r, uint16_t buttons,
		       const float fv[4], uint32_t loop_max_us)
{
	const engine_config_t *c = &eng.cfg;
	char names[48];
	char mode[12];
	int v[4];

	ladder_mask_names(buttons, names, sizeof(names));
	if (ui.mode == UI_EDIT) {
		int id = c->fx_id[ui.edit_slot];

		snprintf(mode, sizeof(mode), "EDIT%c", (char)('1' + (ui.edit_slot & 3)));
		for (int i = 0; i < 4; i++) {
			v[i] = pct(c->params[ui.edit_slot][id][i]);
		}
	} else {
		snprintf(mode, sizeof(mode), "PERF");
		for (int i = 0; i < 4; i++) {
			v[i] = pct(c->wet[i]);
		}
	}
	con_printf("S %u L1=%4d L2=%4d F=%4d %4d %4d %4d f=%3d %3d %3d %3d btn=0x%03x %s "
		   "%s %s=%d %d %d %d need=%d%d%d%d byp=%d%d%d%d gb=%d m=%d pre=%d lt=%uus dr=%u\r\n",
		   now, r->ladder1, r->ladder2, r->fader[0], r->fader[1], r->fader[2], r->fader[3],
		   pct(fv[0]), pct(fv[1]), pct(fv[2]), pct(fv[3]), buttons, names, mode,
		   ui.mode == UI_EDIT ? "par" : "wet", v[0], v[1], v[2], v[3],
		   ui.to[0].picked ? 0 : ui.to[0].need, ui.to[1].picked ? 0 : ui.to[1].need,
		   ui.to[2].picked ? 0 : ui.to[2].need, ui.to[3].picked ? 0 : ui.to[3].need,
		   c->bypass[0], c->bypass[1], c->bypass[2], c->bypass[3], c->global_bypass,
		   pct(c->master), ui.preset_idx + 1, loop_max_us, con_drops);
}

/* ------------------------------ power off -------------------------------- */
static void wait_fed(int ms)
{
	while (ms > 0) {
		feed_wdt();
		k_msleep(ms > 20 ? 20 : ms);
		ms -= 20;
	}
	feed_wdt();
}

static FUNC_NORETURN void power_off(void)
{
	controls_hw_leds_off();
	con_printf("POWER OFF: release FUNCTION; next FUNCTION press wakes into the bootloader\r\n");
	wait_fed(50); /* let the console line go out (best effort) */

	/* Wait for release first, or the level sense armed below would wake
	 * the chip immediately. Keep feeding the watchdog meanwhile. */
	while (controls_hw_fn_pressed()) {
		feed_wdt();
		k_msleep(20);
	}
	wait_fed(FN_RELEASE_MS);
	controls_hw_leds_off();
	/* Ladder/fader rail off: SYSTEM_OFF retains GPIO levels, and a powered
	 * rail would keep current flowing through the fader pots. */
	controls_hw_rail(false);
	controls_hw_arm_fn_wake();

	(void)irq_lock();
	feed_wdt();
	NRF_POWER->RESETREAS = 0xFFFFFFFFu; /* write-1-to-clear, before SYSTEM_OFF */
	__DSB();
	NRF_POWER->SYSTEMOFF = 1u;
	__DSB();
	for (;;) {
		/* CPU is off; FUNCTION press wakes via reset into the bootloader */
	}
}

/* --------------------------------- main ---------------------------------- */
int main(void)
{
	sp1fx_boot_resetreas = NRF_POWER->RESETREAS;
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;
	feed_wdt();

	if (device_is_ready(wdt)) {
		struct wdt_timeout_cfg cfg = {
			.window.min = 0U,
			.window.max = 4000U,
			.callback = NULL,
			.flags = WDT_FLAG_RESET_SOC,
		};
		(void)wdt_install_timeout(wdt, &cfg);
		(void)wdt_setup(wdt, 0);
	}
	feed_wdt();

	int hw_rc = controls_hw_init();

	feed_wdt();

	/* Engine: factory-default config (all four slots bypassed: Filter,
	 * Chorus, Delay, Reverb), presets for FN + rocker. No audio processing. */
	engine_config_t def;

	engine_init(&eng, pool, sizeof(pool), SAMPLE_RATE, ENGINE_DEFAULT_CPU_BUDGET);
	engine_config_default(&def);
	int rejected = engine_apply_config(&eng, &def);

	controller_init(&ui, &eng);
	controller_set_presets(&ui, FACTORY_PRESET_COUNT, fill_factory, NULL);
	debounce_init(&sh_deb, 2); /* same as controller_init() */
	gesture_init(&sh_gest);

	ladder_cfg_t c1, c2;

	ladder_cfg_ladder1(&c1, LADDER1_DECODE_PLAY_CHORDS);
	ladder_cfg_ladder2(&c2);
	ladder_init(&lad1, &c1);
	ladder_init(&lad2, &c2);
	for (int i = 0; i < 4; i++) {
		fader_init(&fad[i], FADER_RAW_LO_DEFAULT, FADER_RAW_HI_DEFAULT);
	}

#ifdef CONFIG_SP1FX_USB_CONSOLE
	usb_console_start();
	feed_wdt();
#endif

	/* If FUNCTION is still held from the wake-up press, hide it from the
	 * controller until it has been released once, so that press cannot
	 * become a 1.5 s power-off hold. */
	bool fn_armed = !controls_hw_fn_pressed();
	int64_t next = k_uptime_get();
	uint32_t next_status = 0;
	uint32_t banner_at = 0;
	uint32_t loop_max_us = 0;
	controls_raw_t raw;
	float fv[4] = { 0 };

	for (;;) {
		feed_wdt();
		uint32_t t0 = k_cycle_get_32();
		uint32_t now = k_uptime_get_32();

		(void)controls_hw_read(&raw);
		uint16_t buttons = (uint16_t)(ladder_update(&lad1, raw.ladder1) |
					      ladder_update(&lad2, raw.ladder2));
		bool fn = controls_hw_fn_pressed();

		if (!fn) {
			fn_armed = true;
		}
		if (fn && fn_armed) {
			buttons |= BTN_BIT(BTN_FN);
		}
		for (int i = 0; i < 4; i++) {
			fv[i] = fader_update(&fad[i], raw.fader[i]);
		}

		log_gestures(now, buttons);
		controller_tick(&ui, now, buttons, fv);
		log_ctl_events(now);

		if (ui.power_off_request) {
			power_off();
		}

		led_frame_t f;

		controller_leds(&ui, now, &f);
		controls_hw_leds(f.track, f.play);

		/* Info banner every 10 s, so a `screen` opened late still sees it. */
		if ((int32_t)(now - banner_at) >= 0) {
			banner_at = now + 10000u;
			con_printf("I SP-1 FX controls bring-up (no audio). resetreas=0x%08x hw=%d "
				   "adc=%d pwm_released=0x%x rejected=%d presets=%d play_chords=%d\r\n",
				   sp1fx_boot_resetreas, hw_rc, controls_hw_adc_status(),
				   controls_hw_released_pwms(), rejected, FACTORY_PRESET_COUNT,
				   LADDER1_DECODE_PLAY_CHORDS);
			con_printf("I slots: 1=%s 2=%s 3=%s 4=%s\r\n", fx_name(eng.cfg.fx_id[0]),
				   fx_name(eng.cfg.fx_id[1]), fx_name(eng.cfg.fx_id[2]),
				   fx_name(eng.cfg.fx_id[3]));
		}
		if ((int32_t)(now - next_status) >= 0) {
			next_status = now + STATUS_MS;
			log_status(now, &raw, buttons, fv, loop_max_us);
			loop_max_us = 0;
		}

		uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);

		if (us > loop_max_us) {
			loop_max_us = us;
		}

		next += TICK_MS;
		int64_t t = k_uptime_get();

		if (next <= t) {
			next = t + 1; /* overran: do not try to catch up */
		}
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
	return 0;
}
