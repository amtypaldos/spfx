/*
 * SP-1 FX effect CPU profiling image (docs/BRINGUP_FXPROF.md).
 *
 * NOT TESTED ON HARDWARE. What it does:
 *   - No audio input or output: the codecs are held in reset (silent), no I2S, no UAC2.
 *   - A bench thread (priority 10, below main) runs every case of
 *     firmware/audio/fxprof_core.c in turn through the real engine on a looping synthetic
 *     signal (noise + 220 Hz + 3.3 kHz, about -12 dBFS): Clean (engine only), each of the
 *     10 effects alone in slot 0 at default / worst-case params / one param swept every
 *     block, then the 6 factory presets.
 *   - Per case: CONFIG_SP1FX_FXPROF_WARMUP untimed blocks, then
 *     CONFIG_SP1FX_FXPROF_BLOCKS blocks, each engine_process() call timed with the DWT
 *     cycle counter (interrupts stay on: USB, timer and the main loop may add to "max";
 *     "min" is the least disturbed block). One console line per case:
 *       FXPROF case=10 id=4 Reverb var=def avg=..  min=.. max=.. cyc/frame cpu%=.. decl=..
 *   - A calibration line per round: the same small FP kernel executed from flash and from
 *     RAM (shows the flash wait-state / cache cost on this chip).
 *   - LED1 heartbeat, LED2 on while a case is being timed. FN held 1.5 s: power off.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/usbd.h>
#include <sample_usbd.h>
#include <hal/nrf_gpio.h>
#include <nrfx.h>
#include <string.h>

#include "dsp/engine.h"
#include "audio/fxprof_core.h"

#define FS_HZ          48000u
#define CPU_HZ         64000000u
#define BLK            64
#define LOOP_MS        10
#define BLINK_HALF_MS  500
#define FN_HOLD_OFF_MS 1500
#define FN_RELEASE_MS  60

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
#define ZU DT_PATH(zephyr_user)
static const struct gpio_dt_spec led_hb   = GPIO_DT_SPEC_GET(ZU, heartbeat_led_gpios);
static const struct gpio_dt_spec led_busy = GPIO_DT_SPEC_GET(ZU, busy_led_gpios);
static const struct gpio_dt_spec fn       = GPIO_DT_SPEC_GET(ZU, fn_gpios);
static const struct gpio_dt_spec osc_en   = GPIO_DT_SPEC_GET(ZU, osc_en_gpios);
static const struct gpio_dt_spec cs42_rst = GPIO_DT_SPEC_GET(ZU, cs42_rst_gpios);
static const struct gpio_dt_spec tas_rst  = GPIO_DT_SPEC_GET(ZU, tas_rst_gpios);

/* ------------------------------ USB console ------------------------------ */
static const struct device *const cdc = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
RING_BUF_DECLARE(con_rb, 4096);
static struct k_spinlock con_lock;
static bool usb_ok;
static int usb_rc = 1;
static volatile uint32_t con_drops;

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

static void usb_start(void)
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

static void con_write(const char *s, int len)
{
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
}

#define CON_PRINTF(...) do {						\
		char _l[256];						\
		int _n = snprintk(_l, sizeof(_l), __VA_ARGS__);		\
		if (_n > 0) {						\
			con_write(_l, MIN(_n, (int)sizeof(_l) - 1));	\
		}							\
	} while (0)

/* ------------------------------ wdt / power ------------------------------ */
static void feed_wdt(void)
{
	for (int ch = 0; ch < 8; ch++) {
		NRF_WDT->RR[ch] = WDT_RR_RR_Reload;
	}
}

/* GUESS (untested), as zephyr_min/tone/usbaudio: release a bootloader PWM claim. */
static void release_pwm_claim(uint32_t abs_pin)
{
	NRF_PWM_Type *const pwms[] = { NRF_PWM0, NRF_PWM1, NRF_PWM2, NRF_PWM3 };

	for (size_t i = 0; i < ARRAY_SIZE(pwms); i++) {
		NRF_PWM_Type *p = pwms[i];
		bool routed = false;

		if (p->ENABLE == 0u) {
			continue;
		}
		for (int c = 0; c < 4; c++) {
			uint32_t psel = p->PSEL.OUT[c];
			bool connected = (psel & PWM_PSEL_OUT_CONNECT_Msk) == 0u;
			uint32_t pin = psel & (PWM_PSEL_OUT_PIN_Msk | PWM_PSEL_OUT_PORT_Msk);

			if (connected && pin == abs_pin) {
				routed = true;
			}
		}
		if (!routed) {
			continue;
		}
		p->EVENTS_STOPPED = 0u;
		p->TASKS_STOP = 1u;
		for (int t = 0; t < 1000 && p->EVENTS_STOPPED == 0u; t++) {
			k_busy_wait(1);
		}
		p->ENABLE = 0u;
	}
}

static uint32_t abs_pin(const struct gpio_dt_spec *g)
{
	const uint32_t port = (g->port == DEVICE_DT_GET(DT_NODELABEL(gpio1))) ? 1u : 0u;

	return NRF_GPIO_PIN_MAP(port, g->pin);
}

static bool fn_pressed(void)
{
	return gpio_pin_get_dt(&fn) > 0;
}

static volatile bool g_bench_stop;

static FUNC_NORETURN void power_off(void)
{
	g_bench_stop = true;
	(void)gpio_pin_set_dt(&led_hb, 0);
	(void)gpio_pin_set_dt(&led_busy, 0);
	while (fn_pressed()) {
		feed_wdt();
		k_msleep(20);
	}
	feed_wdt();
	k_msleep(FN_RELEASE_MS);
	nrf_gpio_cfg_sense_input(NRF_GPIO_PIN_MAP(0, fn.pin), NRF_GPIO_PIN_PULLUP,
				 NRF_GPIO_PIN_SENSE_LOW);
	(void)irq_lock();
	feed_wdt();
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;
	__DSB();
	NRF_POWER->SYSTEMOFF = 1u;
	__DSB();
	for (;;) {
	}
}

static void dwt_enable(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0u;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* ------------------------------ calibration ------------------------------ */
/* The same FP kernel twice: one copy in flash (cached), one in RAM. 4 x 64 multiply-adds
 * with loads and stores, like a small filter; the ratio shows the instruction-fetch cost. */
#define CAL_KERNEL(NAME, ATTR)							\
	ATTR __noinline void NAME(float *x, int n)				\
	{									\
		float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;			\
		for (int i = 0; i < n; i++) {					\
			float v = x[i];						\
			a = a * 0.5f + v;					\
			b = b * 0.25f + a;					\
			c = c * 0.125f + b;					\
			d = d * 0.0625f + c;					\
			x[i] = d;						\
		}								\
	}
CAL_KERNEL(cal_flash, )
CAL_KERNEL(cal_ram, __ramfunc)

static float cal_buf[BLK];

static uint32_t cal_run(void (*fn_)(float *, int))
{
	uint32_t best = UINT32_MAX;

	for (int rep = 0; rep < 50; rep++) {
		for (int i = 0; i < BLK; i++) {
			cal_buf[i] = (float)(i & 7) * 0.01f;
		}
		const unsigned int key = irq_lock();   /* < 1 k cycles: 16 us with IRQs off */
		const uint32_t t0 = DWT->CYCCNT;

		fn_(cal_buf, BLK);
		const uint32_t dt = DWT->CYCCNT - t0;

		irq_unlock(key);
		if (dt < best) {
			best = dt;
		}
	}
	return best;
}

/* ------------------------------ bench thread ------------------------------ */
static uint8_t engine_pool[CONFIG_SP1FX_ENGINE_POOL_KIB * 1024] __aligned(8);
static engine_t eng;
static float in_l[BLK], in_r[BLK], out_l[BLK], out_r[BLK];
static volatile float sink;
static volatile bool g_busy;
static volatile uint32_t g_round;

/* x10 fixed point of cycles per frame and of % of the 1333.3 cycles a frame has */
static void per_frame(uint64_t cyc, uint32_t frames, unsigned int *f10, unsigned int *p10)
{
	*f10 = (unsigned int)((cyc * 10u + frames / 2u) / frames);
	*p10 = (unsigned int)((cyc * FS_HZ * 1000u + (uint64_t)frames * CPU_HZ / 2u) /
			      ((uint64_t)frames * CPU_HZ));
}

static void run_case(int c)
{
	engine_config_t cfg;
	fxprof_sig_t sig;
	const fxprof_case_t *k = fxprof_case(c);

	engine_init(&eng, engine_pool, sizeof(engine_pool), (float)FS_HZ, UINT32_MAX);
	eng.overload_n = 0;   /* measure, never shed */
	(void)fxprof_case_config(c, &cfg);
	const int rej = engine_post_config(&eng, &cfg);

	engine_pump(&eng);    /* effect init (memory clear) outside the timing */
	fxprof_sig_init(&sig);

	const uint32_t warm = CONFIG_SP1FX_FXPROF_WARMUP, nb = CONFIG_SP1FX_FXPROF_BLOCKS;
	uint64_t sum = 0;
	uint32_t mn = UINT32_MAX, mx = 0;

	g_busy = true;
	for (uint32_t b = 0; b < warm + nb && !g_bench_stop; b++) {
		fxprof_case_tick(&eng, c, b);
		fxprof_sig_block(&sig, in_l, in_r, BLK);
		const uint32_t t0 = DWT->CYCCNT;

		engine_process(&eng, in_l, in_r, out_l, out_r, BLK);
		const uint32_t dt = DWT->CYCCNT - t0;

		sink = out_l[0] + out_r[BLK - 1];
		if (b < warm) {
			continue;
		}
		sum += dt;
		mn = MIN(mn, dt);
		mx = MAX(mx, dt);
	}
	g_busy = false;

	unsigned int af, ap, nf, np, xf, xp;

	per_frame(sum, nb * BLK, &af, &ap);
	per_frame(mn, BLK, &nf, &np);
	per_frame(mx, BLK, &xf, &xp);
	const char *v = fxprof_kind_name(k->kind);
	const unsigned int decl = fxprof_case_declared(c);

	if (k->kind == FXP_PRESET) {
		CON_PRINTF("FXPROF case=%d preset=%u \"%s\" avg=%u.%u min=%u.%u max=%u.%u cyc/frame "
			   "cpu%%=%u.%u decl=%u rej=%d nan=%u blocks=%u\r\n",
			   c, k->preset + 1u, fxprof_case_name(c), af / 10, af % 10, nf / 10, nf % 10,
			   xf / 10, xf % 10, ap / 10, ap % 10, decl, rej,
			   (unsigned int)eng.diag.nan_events, (unsigned int)nb);
	} else {
		CON_PRINTF("FXPROF case=%d id=%u %s var=%s avg=%u.%u min=%u.%u max=%u.%u cyc/frame "
			   "cpu%%=%u.%u decl=%u nan=%u blocks=%u\r\n",
			   c, k->fx_id, fxprof_case_name(c), v, af / 10, af % 10, nf / 10, nf % 10,
			   xf / 10, xf % 10, ap / 10, ap % 10, decl,
			   (unsigned int)eng.diag.nan_events, (unsigned int)nb);
	}
}

#define BENCH_STACK 6144
static K_THREAD_STACK_DEFINE(bench_stack, BENCH_STACK);
static struct k_thread bench_tcb;

static void bench_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	for (;;) {
		/* wait for a terminal, so a round always starts at case 0 on screen */
		while (!con_host_open() && !g_bench_stop) {
			k_msleep(200);
		}
		if (g_bench_stop) {
			k_sleep(K_FOREVER);
		}
		k_msleep(500);
		g_round++;
		const uint32_t cf = cal_run(cal_flash), cr = cal_run(cal_ram);

		CON_PRINTF("FXPROF round=%u cases=%d blocks/case=%u warmup=%u image=fxprof-1"
			   " | cal kernel (64 x 4 MAC): flash=%u ram=%u cycles (con_drop=%u)\r\n",
			   (unsigned int)g_round, FXPROF_CASES, CONFIG_SP1FX_FXPROF_BLOCKS,
			   CONFIG_SP1FX_FXPROF_WARMUP, (unsigned int)cf, (unsigned int)cr,
			   (unsigned int)con_drops);
		for (int i = 0; i < FXPROF_CASES && !g_bench_stop; i++) {
			run_case(i);
			k_msleep(CONFIG_SP1FX_FXPROF_PAUSE_MS);
		}
		CON_PRINTF("FXPROF round=%u done\r\n", (unsigned int)g_round);
	}
}

int main(void)
{
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

	const struct gpio_dt_spec *leds[] = { &led_hb, &led_busy };

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		release_pwm_claim(abs_pin(leds[i]));
		(void)gpio_pin_configure_dt(leds[i], GPIO_OUTPUT_INACTIVE);
	}
	(void)gpio_pin_configure_dt(&fn, GPIO_INPUT);
	/* audio silent by construction: oscillator off, both codecs held in reset */
	(void)gpio_pin_configure_dt(&cs42_rst, GPIO_OUTPUT_ACTIVE);
	(void)gpio_pin_configure_dt(&tas_rst, GPIO_OUTPUT_ACTIVE);
	(void)gpio_pin_configure_dt(&osc_en, GPIO_OUTPUT_INACTIVE);
	dwt_enable();

	usb_start();
	feed_wdt();

	k_thread_create(&bench_tcb, bench_stack, K_THREAD_STACK_SIZEOF(bench_stack),
			bench_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(10), K_FP_REGS, K_NO_WAIT);
	k_thread_name_set(&bench_tcb, "bench");

	bool fn_armed = !fn_pressed(), led_on = false;
	int64_t fn_down_at = -1;
	int64_t next_toggle = k_uptime_get() + BLINK_HALF_MS;

	for (;;) {
		feed_wdt();
		const int64_t now = k_uptime_get();

		if (now >= next_toggle) {
			led_on = !led_on;
			(void)gpio_pin_set_dt(&led_hb, led_on ? 1 : 0);
			next_toggle += BLINK_HALF_MS;
		}
		(void)gpio_pin_set_dt(&led_busy, g_busy ? 1 : 0);
		if (fn_pressed()) {
			if (fn_armed) {
				if (fn_down_at < 0) {
					fn_down_at = now;
				} else if (now - fn_down_at >= FN_HOLD_OFF_MS) {
					power_off();
				}
			}
		} else {
			fn_armed = true;
			fn_down_at = -1;
		}
		ARG_UNUSED(usb_rc);
		k_msleep(LOOP_MS);
	}
	return 0;
}
