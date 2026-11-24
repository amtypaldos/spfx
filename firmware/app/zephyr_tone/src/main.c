/*
 * SP-1 FX audio bring-up image: a QUIET test tone.
 *
 * NOT TESTED ON HARDWARE. What it is meant to do (docs/BRINGUP_TONE.md):
 *   - everything zephyr_min does: feed the bootloader's watchdog (< 5 s),
 *     heartbeat on track LED 1, FUNCTION held 1.5 s -> SYSTEM_OFF,
 *   - power the 3.072 MHz oscillator (P0.13), release the CS42L42 / TAS2505
 *     resets, configure both codecs over I2C (both outputs muted during setup),
 *   - stream 48 kHz stereo 16-bit I2S as a CLOCK SLAVE (oscillator drives SCK,
 *     CS42L42 drives LRCK) in 64-frame blocks from a static k_mem_slab:
 *     440 Hz on the LEFT channel, 660 Hz on the RIGHT, each -30 dBFS peak,
 *     faded in over 1.5 s,
 *   - headphones in (CS42L42 tip sense) -> speaker muted, headphones on;
 *     otherwise headphones muted, speaker on. Re-checked every 250 ms; every
 *     switch ramps the digital level to zero first (30 ms), swaps the mutes,
 *     then fades back in over 1.5 s,
 *   - LED 2 = CS42L42 answered with DEVID 0x42, LED 3 = TAS2505 ACKed,
 *     LED 4 = I2S stream running without errors in the last second,
 *   - USB CDC ACM status line once per second (non-blocking, lines dropped if
 *     the host is not reading). Optional: build with no_usb.conf.
 *
 * SP-1 bootloader rules ("BIG FIVE"): app at 0x20000 (check_image.py), WDT fed
 * < 5 s, no re-init of bootloader-owned clocks/peripherals beyond what Tape
 * Looper does (Zephyr clock driver, USBD, I2S, TWIM; SAADC/PWM untouched
 * except the guarded LED-pin release), SYSTEM_OFF as the way back to the
 * bootloader, RESETREAS cleared at boot and before SYSTEM_OFF.
 *
 * New code for SP-1 FX. Codec register sequences live in sp1_codecs.c
 * (adapted from chattock/sp1-tape-looper, MIT). The I2S configuration (slave,
 * 16-bit, I2S format, prime-then-START, drop/re-prime on error) follows Tape
 * Looper's audio_thread(); the watchdog/power-off code is zephyr_min's.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/irq.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_gpio.h>
#include <nrfx.h>
#include <math.h>
#include <string.h>

#include "sp1_codecs.h"

#ifdef CONFIG_SP1FX_USB_CONSOLE
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/usbd.h>
#include <sample_usbd.h>
#endif

#define LOOP_MS          10
#define BLINK_HALF_MS    500
#define FN_HOLD_OFF_MS   1500
#define FN_RELEASE_MS    60
#define HP_POLL_MS       250
#define STATUS_MS        1000

/* ---------------------------- audio constants ---------------------------- */
#define FS_HZ            48000
#define BLK_FRAMES       64                       /* 1.333 ms per block */
#define BLK_BYTES        (BLK_FRAMES * 2 * (int)sizeof(int16_t))
/* 4 queued (CONFIG_I2S_NRFX_TX_BLOCK_COUNT) + 2 latched in EasyDMA + 1 being
 * rendered + 1 spare for the re-prime after an error (Tape Looper's count). */
#define SLAB_BLOCKS      8
#define PRIME_BLOCKS     4
#define TONE_L_HZ        440.0f
#define TONE_R_HZ        660.0f
/* HEARING SAFETY: -30 dBFS peak = 10^(-30/20) = 0.0316 of full scale
 * (|sample| <= 1036 of 32767). */
#define TONE_LEVEL       0.0316228f
#define FADE_IN_S        1.5f     /* >= 1 s, every time an output turns on */
#define FADE_OUT_S       0.03f    /* ramp to zero before any mute change   */
#define CPU_HZ           64000000u

K_MEM_SLAB_DEFINE_STATIC(tx_slab, BLK_BYTES, SLAB_BLOCKS, 4);

static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));
static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

#define ZU DT_PATH(zephyr_user)
static const struct gpio_dt_spec led_hb   = GPIO_DT_SPEC_GET(ZU, heartbeat_led_gpios);
static const struct gpio_dt_spec led_cs42 = GPIO_DT_SPEC_GET(ZU, cs42_led_gpios);
static const struct gpio_dt_spec led_tas  = GPIO_DT_SPEC_GET(ZU, tas_led_gpios);
static const struct gpio_dt_spec led_i2s  = GPIO_DT_SPEC_GET(ZU, i2s_led_gpios);
static const struct gpio_dt_spec fn       = GPIO_DT_SPEC_GET(ZU, fn_gpios);
static const struct gpio_dt_spec osc_en   = GPIO_DT_SPEC_GET(ZU, osc_en_gpios);
static const struct gpio_dt_spec cs42_rst = GPIO_DT_SPEC_GET(ZU, cs42_rst_gpios);
static const struct gpio_dt_spec tas_rst  = GPIO_DT_SPEC_GET(ZU, tas_rst_gpios);

/* Kept in RAM for a debugger; also printed on the console. */
volatile uint32_t sp1fx_boot_resetreas;

/* ------------------------ audio thread shared state ---------------------- */
static volatile bool     g_out_enable;     /* main -> audio: ramp target 1 or 0 */
static volatile bool     g_audio_stop;     /* main -> audio: park (power-off)   */
static volatile bool     g_gain_zero = true; /* audio -> main: level is exactly 0 */
static volatile int      g_i2s_cfg_rc = 1;
static volatile int      g_i2s_start_rc = 1;
static volatile uint32_t g_blocks;         /* successful i2s_write() calls      */
static volatile uint32_t g_err;            /* i2s_write() errors (-EIO etc.)    */
static volatile uint32_t g_tmo;            /* i2s_write() timeouts (-EAGAIN)    */
static volatile uint32_t g_recover;        /* DROP + re-prime + START cycles    */
static volatile uint32_t g_gen_us_max;     /* max render time of one block      */
static volatile uint32_t g_period_us_max;  /* max time between two good writes  */

struct osc {
	float c, s, cw, sw;
};

static void osc_init(struct osc *o, float hz)
{
	const float w = 2.0f * 3.14159265358979f * hz / (float)FS_HZ;

	o->c = 1.0f;
	o->s = 0.0f;
	o->cw = cosf(w);
	o->sw = sinf(w);
}

/* Quadrature (rotation) oscillator: exact sine without a table. The repo's
 * fx_sin01() (firmware/dsp/util.h) is a ~3 % shape-error LFO approximation and
 * would add audible harmonics to a test tone, so it is not used here. */
static inline float osc_step(struct osc *o)
{
	const float c = o->c * o->cw - o->s * o->sw;
	const float s = o->s * o->cw + o->c * o->sw;

	o->c = c;
	o->s = s;
	return s;
}

static inline void osc_renorm(struct osc *o)
{
	const float g = 1.5f - 0.5f * (o->c * o->c + o->s * o->s);

	o->c *= g;
	o->s *= g;
}

static struct osc osc_l, osc_r;
static float g_gain;   /* audio thread only */

static void render_block(int16_t *buf)
{
	const float up = 1.0f / (FADE_IN_S * (float)FS_HZ);
	const float down = 1.0f / (FADE_OUT_S * (float)FS_HZ);
	const float target = g_out_enable ? 1.0f : 0.0f;
	const float amp = TONE_LEVEL * 32767.0f;
	float g = g_gain;

	for (int i = 0; i < BLK_FRAMES; i++) {
		if (g < target) {
			g += up;
			if (g > target) {
				g = target;
			}
		} else if (g > target) {
			g -= down;
			if (g < target) {
				g = target;
			}
		}
		const float a = amp * g;
		/* nRF I2S, 16-bit stereo: each 32-bit word = left (low half-word)
		 * then right (high half-word) -> int16 order L, R. */
		buf[2 * i]     = (int16_t)(a * osc_step(&osc_l));
		buf[2 * i + 1] = (int16_t)(a * osc_step(&osc_r));
	}
	osc_renorm(&osc_l);
	osc_renorm(&osc_r);
	g_gain = g;
	g_gain_zero = (g <= 0.0f);
}

static void prime_and_start(void)
{
	for (int i = 0; i < PRIME_BLOCKS; i++) {
		void *pb;

		if (k_mem_slab_alloc(&tx_slab, &pb, K_NO_WAIT) != 0) {
			break;
		}
		memset(pb, 0, BLK_BYTES);
		if (i2s_write(i2s_dev, pb, BLK_BYTES) != 0) {
			k_mem_slab_free(&tx_slab, pb);
		}
	}
	g_i2s_start_rc = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
}

static K_THREAD_STACK_DEFINE(audio_stack, 1024);
static struct k_thread audio_tcb;

static void audio_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	/* Same configuration as Tape Looper's audio_thread(): both clocks are
	 * inputs (SCK from the oscillator, LRCK from the CS42L42), 16-bit words,
	 * Philips I2S format. With a 64-SCLK frame the nRF shifts the 16 MSBs of
	 * each 32-SCLK half-frame (Tape Looper comment; the codecs are set for
	 * 16-bit (TAS2505) and 24-bit (CS42L42) MSB-first slots). */
	struct i2s_config cfg = {
		.word_size      = 16,
		.channels       = 2,
		.format         = I2S_FMT_DATA_FORMAT_I2S,
		.options        = I2S_OPT_FRAME_CLK_SLAVE | I2S_OPT_BIT_CLK_SLAVE,
		.frame_clk_freq = FS_HZ,
		.mem_slab       = &tx_slab,
		.block_size     = BLK_BYTES,
		/* i2s_write() waits at most this long for queue space; with no
		 * LRCK yet it times out and is counted, never blocks forever. */
		.timeout        = 100,
	};

	osc_init(&osc_l, TONE_L_HZ);
	osc_init(&osc_r, TONE_R_HZ);

	if (!device_is_ready(i2s_dev)) {
		g_i2s_cfg_rc = -100;
		return;
	}
	g_i2s_cfg_rc = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
	if (g_i2s_cfg_rc != 0) {
		return;
	}
	prime_and_start();

	uint32_t last_ok = DWT->CYCCNT;

	for (;;) {
		void *blk;

		if (g_audio_stop) {
			k_sleep(K_FOREVER);
		}
		if (k_mem_slab_alloc(&tx_slab, &blk, K_MSEC(100)) != 0) {
			g_tmo++;
			continue;
		}

		const uint32_t t0 = DWT->CYCCNT;

		render_block(blk);

		const uint32_t gen_us = (DWT->CYCCNT - t0) / (CPU_HZ / 1000000u);

		if (gen_us > g_gen_us_max) {
			g_gen_us_max = gen_us;
		}

		const int rc = i2s_write(i2s_dev, blk, BLK_BYTES);

		if (rc == 0) {
			const uint32_t now = DWT->CYCCNT;
			const uint32_t per = (now - last_ok) / (CPU_HZ / 1000000u);

			if (per > g_period_us_max) {
				g_period_us_max = per;
			}
			last_ok = now;
			g_blocks++;
			continue;
		}
		k_mem_slab_free(&tx_slab, blk);
		if (rc == -EAGAIN) {
			g_tmo++;             /* queue full and not draining: no clocks? */
			continue;
		}
		/* -EIO: the driver went to its ERROR state (e.g. "next buffers not
		 * supplied on time" = underrun) and refuses writes until restarted.
		 * Same failsafe as Tape Looper: drop, re-prime with silence, start. */
		g_err++;
		if (g_audio_stop) {
			continue;
		}
		(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
		prime_and_start();
		g_recover++;
		last_ok = DWT->CYCCNT;
	}
}

/* ------------------------------ USB console ------------------------------ */
#ifdef CONFIG_SP1FX_USB_CONSOLE
static const struct device *const cdc = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
RING_BUF_DECLARE(con_rb, 1024);
static struct k_spinlock con_lock;
static bool usb_ok;
static int usb_rc = 1;

/* Runs in the CDC ACM work-queue context (the only context allowed to call
 * uart_fifo_fill on this stack). Moves bytes from our ring into the CDC
 * ring; never waits. */
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

/* Non-blocking: whole line or nothing. */
static void con_write(const char *s, int len)
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

/* ------------------------- watchdog / LEDs / power ----------------------- */

/* Reload every channel: the bootloader may have enabled more than one RREN
 * channel (Tape Looper feeds all 8 the same way). */
static void feed_wdt(void)
{
	for (int ch = 0; ch < 8; ch++) {
		NRF_WDT->RR[ch] = WDT_RR_RR_Reload;
	}
}

/* GUESS (untested), as in zephyr_min: if an enabled PWM instance (the
 * bootloader starts PWM2/PWM3) is routed to one of our LED pins, stop and
 * disable it so GPIO can drive the pin. Otherwise nothing is touched. */
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

/* Sleep in small steps, feeding the watchdog. */
static void wait_fed(int ms)
{
	while (ms > 0) {
		feed_wdt();
		k_msleep(ms > 20 ? 20 : ms);
		ms -= 20;
	}
	feed_wdt();
}

/* Wait until the audio thread has ramped to exactly zero, plus the queued
 * blocks (<= 6 x 1.33 ms) have played out. Bounded at ~150 ms. */
static void wait_silent(void)
{
	for (int t = 0; t < 10 && !g_gain_zero; t++) {
		wait_fed(10);
	}
	wait_fed(15);
}

static bool cs42_ok, tas_ok, cs42_cfg_ok, tas_cfg_ok;

static FUNC_NORETURN void power_off(void)
{
	/* Silence first (digital ramp), then mute and park everything. */
	g_out_enable = false;
	wait_silent();
	if (tas_ok || cs42_ok) {
		sp1_codecs_shutdown();
	}
	g_audio_stop = true;
	(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
	(void)gpio_pin_set_dt(&tas_rst, 1);    /* assert resets (active-low pins) */
	(void)gpio_pin_set_dt(&cs42_rst, 1);
	(void)gpio_pin_set_dt(&osc_en, 0);     /* oscillator off (battery drain) */

	(void)gpio_pin_set_dt(&led_hb, 0);
	(void)gpio_pin_set_dt(&led_cs42, 0);
	(void)gpio_pin_set_dt(&led_tas, 0);
	(void)gpio_pin_set_dt(&led_i2s, 0);

	/* Wait for release first, or the level sense armed below would wake
	 * the chip immediately. Keep feeding the watchdog meanwhile. */
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
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;  /* write-1-to-clear, before SYSTEM_OFF */
	__DSB();
	NRF_POWER->SYSTEMOFF = 1u;
	__DSB();
	for (;;) {
		/* CPU is off; FUNCTION press wakes via reset into the bootloader */
	}
}

/* ------------------------------ output select ---------------------------- */
enum out { OUT_NONE, OUT_SPK, OUT_HP };
static const char *const out_name[] = { "none", "spk", "hp" };
static enum out cur_out = OUT_NONE;
static int hp_in = -1;          /* debounced: 1 in, 0 out, -1 unknown */
static int hp_raw = -1;         /* last raw read */

static enum out wanted_out(void)
{
	if (hp_in == 1 && cs42_cfg_ok) {
		return OUT_HP;
	}
	if (tas_cfg_ok) {
		return OUT_SPK;
	}
	return OUT_NONE;
}

/* Must be called with the digital level at zero (wait_silent()). */
static void apply_out(enum out o)
{
	/* mute the old output first, then unmute the new one */
	if (cur_out == OUT_SPK || o != OUT_SPK) {
		if (tas_cfg_ok) {
			(void)tas_spk_enable(false);
		}
	}
	if (cur_out == OUT_HP || o != OUT_HP) {
		if (cs42_cfg_ok) {
			(void)cs42_hp_enable(false);
		}
	}
	if (o == OUT_SPK) {
		(void)tas_spk_enable(true);
	} else if (o == OUT_HP) {
		(void)cs42_hp_enable(true);
	}
	cur_out = o;
	g_out_enable = (o != OUT_NONE);   /* fade back in over FADE_IN_S */
}

static void dwt_enable(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0u;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

int main(void)
{
	/* BIG FIVE: capture then clear RESETREAS at boot. */
	sp1fx_boot_resetreas = NRF_POWER->RESETREAS;
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;
	feed_wdt();

	/* Same calls as Tape Looper / zephyr_min: the WDT is already running,
	 * the register-level feed is what keeps the device alive. */
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

	const struct gpio_dt_spec *leds[] = { &led_hb, &led_cs42, &led_tas, &led_i2s };

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		release_pwm_claim(abs_pin(leds[i]));
		(void)gpio_pin_configure_dt(leds[i], GPIO_OUTPUT_INACTIVE);
	}
	(void)gpio_pin_configure_dt(&fn, GPIO_INPUT);

	/* Codec power-up order (Tim Knapen's wiki, CS42L42 datasheet 5.1):
	 * hold RESET, start SCLK (oscillator), release RESET, wait >= 2.5 ms.
	 * TAS2505: no register writes within 1 ms of reset release. */
	(void)gpio_pin_configure_dt(&cs42_rst, GPIO_OUTPUT_ACTIVE);  /* in reset */
	(void)gpio_pin_configure_dt(&tas_rst, GPIO_OUTPUT_ACTIVE);   /* in reset */
	(void)gpio_pin_configure_dt(&osc_en, GPIO_OUTPUT_INACTIVE);
	dwt_enable();

#ifdef CONFIG_SP1FX_USB_CONSOLE
	usb_console_start();
	feed_wdt();
#endif

	k_msleep(2);
	(void)gpio_pin_set_dt(&osc_en, 1);
	k_msleep(2);
	(void)gpio_pin_set_dt(&cs42_rst, 0);
	(void)gpio_pin_set_dt(&tas_rst, 0);
	wait_fed(10);

	uint8_t devid[3] = { 0 };

	if (sp1_i2c_ready()) {
		cs42_ok = cs42_probe(devid);
		tas_ok = tas_probe();
	}
	(void)gpio_pin_set_dt(&led_cs42, cs42_ok ? 1 : 0);
	(void)gpio_pin_set_dt(&led_tas, tas_ok ? 1 : 0);

	/* I2S first (slave: it waits for clocks), then TAS2505 (PLL locks to the
	 * oscillator BCLK), then CS42L42 (starts LRCK). Same order as Tape
	 * Looper: audio_init() -> tas2505_configure() -> hp_init(). */
	k_thread_create(&audio_tcb, audio_stack, K_THREAD_STACK_SIZEOF(audio_stack),
			audio_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(0), K_FP_REGS,
			K_NO_WAIT);
	k_thread_name_set(&audio_tcb, "tone");
	wait_fed(5);

	if (tas_ok) {
		tas_cfg_ok = tas_configure();
	}
	feed_wdt();
	if (cs42_ok) {
		cs42_cfg_ok = cs42_configure();
	}
	feed_wdt();

	/* Give LRCK time to appear and the stream to run (<= 500 ms). */
	for (int t = 0; t < 50 && g_blocks < 100u; t++) {
		wait_fed(10);
	}

	if (cs42_cfg_ok) {
		int votes = 0, reads = 0;

		for (int i = 0; i < 3; i++) {
			int c = cs42_hp_present();

			if (c >= 0) {
				reads++;
				votes += c;
			}
			hp_raw = c;
			wait_fed(8);
		}
		hp_in = (reads > 0) ? ((votes * 2 > reads) ? 1 : 0) : -1;
	}
	apply_out(wanted_out());   /* level is still 0: g_out_enable was false */

	bool fn_armed = !fn_pressed();
	int64_t fn_down_at = -1;
	int64_t now = k_uptime_get();
	int64_t next_toggle = now + BLINK_HALF_MS;
	int64_t next_hp = now + HP_POLL_MS;
	int64_t next_status = now + STATUS_MS;
	bool led_on = false;
	int hp_cand = -1, hp_cnt = 0;
	uint32_t prev_blocks = g_blocks, prev_err = g_err, prev_tmo = g_tmo;

	for (;;) {
		feed_wdt();
		now = k_uptime_get();

		if (now >= next_toggle) {
			led_on = !led_on;
			(void)gpio_pin_set_dt(&led_hb, led_on ? 1 : 0);
			next_toggle += BLINK_HALF_MS;
		}

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

		if (now >= next_hp) {
			next_hp += HP_POLL_MS;
			if (cs42_cfg_ok) {
				int c = cs42_hp_present();

				hp_raw = c;
				if (c >= 0) {   /* failed reads hold the last state */
					if (c == hp_cand) {
						hp_cnt++;
					} else {
						hp_cand = c;
						hp_cnt = 1;
					}
					if (hp_cnt >= 2) {
						hp_in = c;
					}
				}
			}
			enum out w = wanted_out();

			if (w != cur_out) {
				/* ramp to zero, swap mutes, fade back in */
				g_out_enable = false;
				wait_silent();
				apply_out(w);
			}
		}

		if (now >= next_status) {
			next_status += STATUS_MS;

			const uint32_t b = g_blocks, e = g_err, tm = g_tmo;
			const uint32_t db = b - prev_blocks;
			const bool running = db >= 700u && e == prev_err && tm == prev_tmo;
			const uint32_t gen = g_gen_us_max, per = g_period_us_max;

			g_gen_us_max = 0u;
			g_period_us_max = 0u;
			prev_blocks = b;
			prev_err = e;
			prev_tmo = tm;
			(void)gpio_pin_set_dt(&led_i2s, running ? 1 : 0);

			char line[200];
			int n = snprintk(line, sizeof(line),
				"t=%u cs42=%02x%02x%02x%s clk=%02x tas=%s%s hp=%d out=%s "
				"i2s=%s cfg=%d/%d blk/s=%u err=%u tmo=%u rec=%u "
				"gen_us=%u per_us=%u drop=%u rr=%x\r\n",
				(unsigned int)(now / 1000), devid[0], devid[1], devid[2],
				cs42_cfg_ok ? "+" : (cs42_ok ? "!" : "-"),
				cs42_clk_status(),
				tas_ok ? "ack" : "nak", tas_cfg_ok ? "+" : "-",
				hp_raw, out_name[cur_out],
				running ? "run" : "STALL", g_i2s_cfg_rc, g_i2s_start_rc,
				(unsigned int)db, (unsigned int)e, (unsigned int)tm,
				(unsigned int)g_recover, (unsigned int)gen,
				(unsigned int)per, (unsigned int)con_drops,
				(unsigned int)sp1fx_boot_resetreas);

			if (n > 0) {
				con_write(line, MIN(n, (int)sizeof(line) - 1));
			}
		}

		k_msleep(LOOP_MS);
	}
	return 0;
}
