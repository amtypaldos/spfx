/*
 * SP-1 FX minimal bring-up image.
 *
 * Purpose: prove the toolchain, linking at 0x20000 and the flash-safety
 * checks. NOT TESTED ON HARDWARE. It does only three things:
 *   1. feeds the watchdog the TE bootloader arms (must be fed < 5 s),
 *   2. blinks track LED 1 (P0.29) as a heartbeat (0.5 s on / 0.5 s off),
 *   3. FUNCTION held >= 1.5 s -> LED off, wait for release, SYSTEM_OFF, so
 *      the next FUNCTION press wakes the chip into the TE bootloader.
 *
 * SP-1 bootloader rules ("BIG FIVE", SP-1-dev wiki "Bootloader" page and
 * chattock/sp1-tape-looper main.c, MIT):
 *   - app at 0x20000 (board devicetree slot0_partition, checked by
 *     firmware/scripts/check_image.py),
 *   - watchdog fed < 5 s,
 *   - do not re-init bootloader-owned clocks/peripherals (app.overlay
 *     disables USB/I2S/I2C/SAADC; no PWM/clock code here except the
 *     guarded pin release below),
 *   - SYSTEM_OFF is the way back to the bootloader (no reset pin),
 *   - RESETREAS is cleared at boot and before SYSTEM_OFF.
 *
 * Watchdog and power-off sequences mirror Tape Looper's main.c
 * (feed_wdt(), power_off(), main() prologue); written fresh, not copied.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/irq.h>
#include <hal/nrf_gpio.h>
#include <nrfx.h>

#define LOOP_MS          10
#define BLINK_HALF_MS    500
#define FN_HOLD_OFF_MS   1500
#define FN_RELEASE_MS    60

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static const struct gpio_dt_spec led =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), heartbeat_led_gpios);
static const struct gpio_dt_spec fn =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), fn_gpios);

/* Kept in RAM for a debugger; never written anywhere persistent. */
volatile uint32_t sp1fx_boot_resetreas;

/* Reload every channel: the bootloader may have enabled more than one
 * RREN channel (Tape Looper feeds all 8 the same way). Writing the reload
 * value to a channel that is not enabled has no effect. */
static void feed_wdt(void)
{
	for (int ch = 0; ch < 8; ch++) {
		NRF_WDT->RR[ch] = WDT_RR_RR_Reload;
	}
}

/* GUESS (untested): the bootloader starts PWM2/PWM3, which drive the LEDs in
 * the stock firmware. A running PWM instance whose PSEL selects the LED pin
 * overrides the GPIO OUT value, so a GPIO heartbeat would be invisible.
 * Only if an enabled PWM instance is routed to our pin do we stop and disable
 * it (Tape Looper likewise disables and re-programs PWM2/PWM3 for its LEDs).
 * If no PWM is routed to the pin, nothing is touched. */
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

static bool fn_pressed(void)
{
	return gpio_pin_get_dt(&fn) > 0;
}

static FUNC_NORETURN void power_off(void)
{
	(void)gpio_pin_set_dt(&led, 0);

	/* Wait for release first, or the level sense armed below would wake
	 * the chip immediately. Keep feeding the watchdog meanwhile. */
	while (fn_pressed()) {
		feed_wdt();
		k_msleep(20);
	}
	feed_wdt();
	k_msleep(FN_RELEASE_MS);
	(void)gpio_pin_set_dt(&led, 0);

	/* Wake source for SYSTEM_OFF: FUNCTION pin, sense LOW, pull-up. */
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

int main(void)
{
	/* BIG FIVE: capture then clear RESETREAS at boot. */
	sp1fx_boot_resetreas = NRF_POWER->RESETREAS;
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;
	feed_wdt();

	/* Same calls as Tape Looper: the WDT is already running (started by
	 * the bootloader), so CRV cannot change; this registers the channel
	 * with Zephyr's driver. The register-level feed above/below is what
	 * actually keeps the device alive. */
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

	release_pwm_claim(NRF_GPIO_PIN_MAP(0, led.pin));   /* both pins are on gpio0 */
	(void)gpio_pin_configure_dt(&fn, GPIO_INPUT);
	(void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	/* If FUNCTION is still held from the wake-up press, require one
	 * release before a hold can power off again. */
	bool fn_armed = !fn_pressed();
	int64_t fn_down_at = -1;
	int64_t next_toggle = k_uptime_get() + BLINK_HALF_MS;
	bool led_on = false;

	for (;;) {
		feed_wdt();

		int64_t now = k_uptime_get();

		if (now >= next_toggle) {
			led_on = !led_on;
			(void)gpio_pin_set_dt(&led, led_on ? 1 : 0);
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

		k_msleep(LOOP_MS);
	}
	return 0;
}
