/*
 * SP-1 FX sync-jack bring-up image: MIDI clock + Pocket Operator sync.
 *
 * NOT TESTED ON HARDWARE. What it is meant to do (docs/BRINGUP_SYNC.md):
 *   - everything zephyr_min does: feed the bootloader's watchdog (< 5 s),
 *     FUNCTION held 1.5 s -> SYSTEM_OFF (next press -> bootloader),
 *   - send MIDI Start + 24 PPQN MIDI clock (0xF8) on the SYNC jack ring and
 *     2 PPQN 5 ms pulses on the tip, at a fixed CONFIG_SP1FX_SYNC_BPM (120),
 *   - FUNCTION tap cycles: 1 MIDI+PO -> 2 MIDI only -> 3 PO only -> 4 stopped
 *     -> 1 ... (MIDI Stop 0xFC when MIDI turns off; Start 0xFA when it
 *     turns on / the clock restarts),
 *   - LEDs: for ~1 s after a change, LEDs 1..N lit steady for mode N;
 *     otherwise LED 1 flashes on every beat (mode 4: a short blip every 2 s),
 *   - USB CDC ACM status line once per second (non-blocking).
 *
 * Clock source: the audio clock does not exist in this image. sync_out (the
 * same scheduler the final firmware uses) is advanced by a frame count derived
 * from the kernel's monotonic uptime (k_uptime_ticks, RTC-based) as if it were
 * a 48 kHz audio stream. In the final firmware the argument to
 * sync_out_advance() is the real audio frame counter (frames per I2S block),
 * so the clock is locked to the audio clock. Here it is polled every ~1 ms, so
 * each clock byte can be up to ~1 ms late (jitter), plus <= 32 us bit-grid
 * alignment; bit timing itself comes from the 32 us hardware timer (sync_hw.c).
 *
 * Tempo: fixed. The FWD/RWD rocker sits on the SAADC button ladder
 * (P0.03/AIN1), which this image does not read (SAADC is bootloader-owned and
 * ladder decoding is being written elsewhere; not duplicated here).
 *
 * Saved on/off (docs/PROTOCOL.md SET_SYNC): in the final firmware the sync
 * on/off state is a saved device setting restored at power-up (the clock
 * starts itself if it was on). NOT implemented here: no persistence, no
 * storage writes; this image always boots into mode 1.
 *
 * SP-1 bootloader rules ("BIG FIVE"): app at 0x20000 (check_image.py), WDT fed
 * < 5 s, no re-init of bootloader-owned clocks/peripherals (USBD as Tape
 * Looper; TIMER2 for the bit clock; SAADC/PWM untouched except the guarded
 * LED-pin release), SYSTEM_OFF as the way back, RESETREAS cleared at boot and
 * before SYSTEM_OFF. Watchdog/power-off/PWM-release code is zephyr_min's; the
 * USB console is zephyr_tone's pattern.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/irq.h>
#include <zephyr/sys/printk.h>
#include <hal/nrf_gpio.h>
#include <nrfx.h>

#include "sync_out.h"
#include "sync_hw.h"
#include "midi_out.h"

#ifdef CONFIG_SP1FX_USB_CONSOLE
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/usbd.h>
#include <sample_usbd.h>
#endif

#define LOOP_MS          1
#define FN_HOLD_OFF_MS   1500
#define FN_TAP_MIN_MS    30
#define FN_RELEASE_MS    60
#define MODE_SHOW_MS     1000
#define BEAT_FLASH_MS    60
#define IDLE_BLIP_EVERY  2000
#define IDLE_BLIP_MS     30
#define STATUS_MS        1000
#define FS_HZ            48000u
#define MAX_ADVANCE      4800u   /* clamp per call (100 ms) after a stall */

enum { MODE_ALL = 1, MODE_MIDI = 2, MODE_PO = 3, MODE_STOP = 4 };
static const char *const mode_name[] = { "?", "midi+po", "midi", "po", "stopped" };

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

#define ZU DT_PATH(zephyr_user)
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(ZU, led1_gpios);
static const struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(ZU, led2_gpios);
static const struct gpio_dt_spec led3 = GPIO_DT_SPEC_GET(ZU, led3_gpios);
static const struct gpio_dt_spec led4 = GPIO_DT_SPEC_GET(ZU, led4_gpios);
static const struct gpio_dt_spec fn = GPIO_DT_SPEC_GET(ZU, fn_gpios);
static const struct gpio_dt_spec *const leds[] = { &led1, &led2, &led3, &led4 };

/* Kept in RAM for a debugger; also printed on the console. */
volatile uint32_t sp1fx_boot_resetreas;

/* ------------------------------ USB console ------------------------------ */
#ifdef CONFIG_SP1FX_USB_CONSOLE
static const struct device *const cdc = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
RING_BUF_DECLARE(con_rb, 1024);
static struct k_spinlock con_lock;
static bool usb_ok;
static int usb_rc = 1;

/* CDC ACM callback context: moves bytes from our ring into the CDC FIFO;
 * never waits. */
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

static void leds_set(int bar, bool led1_on)
{
	for (int i = 0; i < 4; i++) {
		int on = (bar > 0) ? (i < bar) : (i == 0 && led1_on);

		(void)gpio_pin_set_dt(leds[i], on);
	}
}

static FUNC_NORETURN void power_off(bool midi_on)
{
	leds_set(0, false);

	/* Polite: tell a following device that the clock stopped. */
	if (midi_on) {
		(void)sync_hw_midi(MIDI_STOP);
	}

	while (fn_pressed()) {
		feed_wdt();
		k_msleep(20);
	}
	feed_wdt();
	k_msleep(FN_RELEASE_MS);   /* also lets the Stop byte (320 us) finish */

	/* Timer off, jack pins back to their reset state (input, disconnected)
	 * so SYSTEM_OFF does not hold the PNP stage biased. GUESS: matches how
	 * the pins are left before this image configured them. */
	sync_hw_shutdown();
	leds_set(0, false);

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

/* --------------------------------- modes --------------------------------- */

static bool mode_midi(int m)
{
	return m == MODE_ALL || m == MODE_MIDI;
}

static bool mode_po(int m)
{
	return m == MODE_ALL || m == MODE_PO;
}

static sync_out_t sync;
static uint32_t clk_index;      /* 0xF8 ticks since the last Start */
static uint32_t ticks_total;    /* sync ticks scheduled since boot */
static uint32_t pulses_total;   /* PO pulses scheduled since boot */

static void apply_mode(int old, int now)
{
	if (mode_midi(old) && !mode_midi(now)) {
		(void)sync_hw_midi(MIDI_STOP);
	}
	sync_hw_enable(mode_midi(now), mode_po(now));
	if (now == MODE_STOP) {
		sync_out_stop(&sync);
	} else if (old == MODE_STOP || old == 0) {
		sync_out_start(&sync);   /* next advance() yields Start */
		clk_index = 0;
	} else if (!mode_midi(old) && mode_midi(now)) {
		(void)sync_hw_midi(MIDI_START);   /* not reached by the 1..4 cycle */
	}
}

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

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		release_pwm_claim(abs_pin(leds[i]));
		(void)gpio_pin_configure_dt(leds[i], GPIO_OUTPUT_INACTIVE);
	}
	(void)gpio_pin_configure_dt(&fn, GPIO_INPUT);

#ifdef CONFIG_SP1FX_USB_CONSOLE
	usb_console_start();
	feed_wdt();
#endif

	const sync_hw_cfg_t hw = {
		.midi_invert = IS_ENABLED(CONFIG_SP1FX_SYNC_MIDI_INVERT),
		.swap_tip_ring = IS_ENABLED(CONFIG_SP1FX_SYNC_SWAP_TIP_RING),
		.po_active_high = !IS_ENABLED(CONFIG_SP1FX_SYNC_PO_ACTIVE_LOW),
		.po_pulse_ms = CONFIG_SP1FX_SYNC_PO_PULSE_MS,
		.midi_enable = 0,
		.po_enable = 0,
	};
	(void)sync_hw_init(&hw);

	sync_out_init(&sync, (float)FS_HZ);
	sync_out_set_bpm(&sync, (float)CONFIG_SP1FX_SYNC_BPM);

	int mode = MODE_ALL;
	apply_mode(0, mode);

	bool fn_armed = !fn_pressed();
	int64_t fn_down_at = -1;
	int64_t now = k_uptime_get();
	int64_t show_until = now + MODE_SHOW_MS;
	int64_t beat_off_at = 0;
	int64_t next_status = now + STATUS_MS;
	const int64_t t0 = k_uptime_ticks();
	uint64_t frames_done = 0;
	uint32_t prev_isr = 0;

	for (;;) {
		feed_wdt();
		now = k_uptime_get();

		/* ---- time -> 48 kHz frame count (final firmware: audio frames) */
		uint64_t frames = (uint64_t)(k_uptime_ticks() - t0) * FS_HZ /
				  (uint64_t)CONFIG_SYS_CLOCK_TICKS_PER_SEC;
		uint64_t n = frames - frames_done;

		if (n > MAX_ADVANCE) {
			n = MAX_ADVANCE;   /* after a stall: do not burst; drift is ok here */
			frames_done = frames - n;
		}
		frames_done += n;

		sync_out_events_t ev = sync_out_advance(&sync, (uint32_t)n);

		if (ev.start) {
			(void)sync_hw_midi(MIDI_START);
		}
		for (uint32_t i = 0; i < ev.midi_ticks; i++) {
			(void)sync_hw_midi(MIDI_CLOCK);
			if (clk_index % SYNC_PPQN == 0u) {
				beat_off_at = now + BEAT_FLASH_MS;
			}
			clk_index++;
		}
		for (uint32_t i = 0; i < ev.po_pulses; i++) {
			(void)sync_hw_po_pulse();
		}
		if (ev.stop) {
			(void)sync_hw_midi(MIDI_STOP);
		}
		ticks_total += ev.midi_ticks;
		pulses_total += ev.po_pulses;

		/* ---- FUNCTION: tap = next mode, hold 1.5 s = power off */
		if (fn_pressed()) {
			if (fn_armed) {
				if (fn_down_at < 0) {
					fn_down_at = now;
				} else if (now - fn_down_at >= FN_HOLD_OFF_MS) {
					power_off(mode_midi(mode));
				}
			}
		} else {
			if (fn_armed && fn_down_at >= 0 && now - fn_down_at >= FN_TAP_MIN_MS) {
				int next = (mode % 4) + 1;

				apply_mode(mode, next);
				mode = next;
				show_until = now + MODE_SHOW_MS;
			}
			fn_armed = true;
			fn_down_at = -1;
		}

		/* ---- LEDs */
		if (now < show_until) {
			leds_set(mode, false);
		} else if (mode == MODE_STOP) {
			leds_set(0, (now % IDLE_BLIP_EVERY) < IDLE_BLIP_MS);
		} else {
			leds_set(0, now < beat_off_at);
		}

		/* ---- status line */
		if (now >= next_status) {
			next_status += STATUS_MS;
			sync_hw_stats_t st;
			const midi_out_t *c = sync_hw_core();

			sync_hw_stats(&st, 1);
			char line[220];
			int len = snprintk(line, sizeof(line),
				"t=%u mode=%d(%s) bpm=%d midi_bytes=%u f8=%u ticks=%u "
				"pulses=%u/%u drop=%u/%u isr/s=%u late=%u maxlat_us=%u "
				"hfxo=%u inv=%d swap=%d con_drop=%u rr=%x\r\n",
				(unsigned int)(now / 1000), mode, mode_name[mode],
				CONFIG_SP1FX_SYNC_BPM,
				(unsigned int)c->bytes_sent, (unsigned int)c->clocks_sent,
				(unsigned int)ticks_total,
				(unsigned int)c->pulses_sent, (unsigned int)pulses_total,
				(unsigned int)c->bytes_dropped, (unsigned int)c->pulses_dropped,
				(unsigned int)(st.isr_count - prev_isr),
				(unsigned int)st.late, (unsigned int)st.max_lat_us,
				(unsigned int)st.hfclk_xtal,
				(int)IS_ENABLED(CONFIG_SP1FX_SYNC_MIDI_INVERT),
				(int)IS_ENABLED(CONFIG_SP1FX_SYNC_SWAP_TIP_RING),
				(unsigned int)con_drops,
				(unsigned int)sp1fx_boot_resetreas);

			prev_isr = st.isr_count;
			if (len > 0) {
				con_write(line, MIN(len, (int)sizeof(line) - 1));
			}
		}

		k_msleep(LOOP_MS);
	}
	return 0;
}
