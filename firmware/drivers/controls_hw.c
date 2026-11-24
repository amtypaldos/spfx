/* SP-1 FX -- physical controls driver (Zephyr, nRF52840 / SP-1).
 * NOT TESTED ON HARDWARE. See controls_hw.h and docs/BRINGUP_CONTROLS.md.
 *
 * Ported from chattock/sp1-tape-looper @0359372e (MIT, (c) chattock),
 * tools/capacity422/runtime/src/main.c -- adapted, not copied verbatim:
 *   - ladder_scan(): one adc_read of every channel, twice, averaged
 *     (ADCSCAN-710); here 6 channels (no battery channel),
 *   - controls_init(): BTN_COM (P1.10) driven high before sampling; here via
 *     a devicetree gpio spec instead of raw NRF_P1 registers,
 *   - led_pwm_one()/LED_PWM_POL: register-level PWM, 16 MHz, COUNTERTOP for a
 *     1 kHz frame, a 4-value Individual-load sequence re-read every period;
 *     here on PWM0 (track row) and PWM1 (play row) instead of the
 *     bootloader-started PWM2/PWM3, brightness 0..255 squared (status_level()).
 *   - LED pin map (leds[] / track_leds[]).
 *
 * SPDX-License-Identifier: MIT
 */
#include "controls_hw.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>
#include <hal/nrf_gpio.h>
#include <nrfx.h>

#define ZU DT_PATH(zephyr_user)

/* zephyr_sp1fx disables the devicetree adc node and scans the SAADC itself
 * (drivers/saadc_scan.c); then the Zephyr-ADC parts below are left out. */
#define CHW_ADC DT_NODE_HAS_STATUS(DT_NODELABEL(adc), okay)

#if CHW_ADC
static const struct adc_dt_spec adc_ch[6] = {
	ADC_DT_SPEC_GET_BY_IDX(ZU, 0), /* AIN0 P0.02: ladder 1, T1..T4 + PLAY */
	ADC_DT_SPEC_GET_BY_IDX(ZU, 1), /* AIN1 P0.03: ladder 2, VOL+/- + FWD/REV */
	ADC_DT_SPEC_GET_BY_IDX(ZU, 2), /* AIN3 P0.05: fader 1 */
	ADC_DT_SPEC_GET_BY_IDX(ZU, 3), /* AIN6 P0.30: fader 2 */
	ADC_DT_SPEC_GET_BY_IDX(ZU, 4), /* AIN2 P0.04: fader 3 */
	ADC_DT_SPEC_GET_BY_IDX(ZU, 5), /* AIN7 P0.31: fader 4 */
};
#endif
static const struct gpio_dt_spec fn = GPIO_DT_SPEC_GET(ZU, fn_gpios);
static const struct gpio_dt_spec btn_com = GPIO_DT_SPEC_GET(ZU, btn_com_gpios);

/* GUESS (untested): time for the ladders/fader dividers to settle after the
 * rail goes high. Tape Looper raises the rail once at boot and never waits
 * explicitly (USB bring-up follows); 5 ms is generous for any RC here. */
#define RAIL_SETTLE_MS 5

/* LED pins, Tape Looper's verified map. Track row = LED next to each fader
 * (directly above buttons 1-4); play row = the four LEDs between PLAY and
 * FUNCTION. Active-high. */
static const uint32_t track_pins[4] = {
	NRF_GPIO_PIN_MAP(0, 29), NRF_GPIO_PIN_MAP(0, 26),
	NRF_GPIO_PIN_MAP(1, 15), NRF_GPIO_PIN_MAP(1, 14),
};
static const uint32_t play_pins[4] = {
	NRF_GPIO_PIN_MAP(1, 13), NRF_GPIO_PIN_MAP(0, 0),
	NRF_GPIO_PIN_MAP(1, 12), NRF_GPIO_PIN_MAP(0, 1),
};

#define LED_PWM_TRACK NRF_PWM0
#define LED_PWM_PLAY  NRF_PWM1
#define LED_TOP       16000u   /* 16 MHz / 16000 = 1 kHz frame (Tape Looper) */
#define LED_POL       0x8000u  /* Tape Looper LED_PWM_POL: high for `duty` ticks */
/* Full-scale duty for brightness 255. Tape Looper's default ("dim") mode runs
 * the LEDs at ~5 % duty because full duty is very bright; 25 % keeps the
 * 0..255 range comfortably visible while still showing the dim end.
 * Change here if it is too bright/dim on hardware. */
#define LED_FULL_TICKS 4000u

static uint16_t track_seq[4] __aligned(4); /* EasyDMA: must live in RAM */
static uint16_t play_seq[4] __aligned(4);
static uint32_t released_pwms;
static int adc_status = 1;
static __maybe_unused int16_t scan_buf[6];

static void pwm_stop(NRF_PWM_Type *p)
{
	if (p->ENABLE == 0u) {
		return;
	}
	p->EVENTS_STOPPED = 0u;
	p->TASKS_STOP = 1u;
	for (int t = 0; t < 1000 && p->EVENTS_STOPPED == 0u; t++) {
		k_busy_wait(1);
	}
	p->ENABLE = 0u;
}

/* The TE bootloader starts PWM2/PWM3 (SP-1-dev wiki "Bootloader"/"PWM").
 * If an enabled instance other than ours is routed to one of our LED pins,
 * stop and disable it so it cannot fight PWM0/PWM1 for the pin. Instances
 * routed elsewhere are left alone. (Tape Looper instead reprograms
 * PWM2/PWM3 itself; this image keeps its hands off them unless they collide.) */
static void release_foreign_pwms(void)
{
	NRF_PWM_Type *const others[] = { NRF_PWM2, NRF_PWM3 };

	for (size_t i = 0; i < ARRAY_SIZE(others); i++) {
		NRF_PWM_Type *p = others[i];
		bool hit = false;

		if (p->ENABLE == 0u) {
			continue;
		}
		for (int c = 0; c < 4; c++) {
			uint32_t psel = p->PSEL.OUT[c];
			uint32_t pin = psel & (PWM_PSEL_OUT_PIN_Msk | PWM_PSEL_OUT_PORT_Msk);

			if ((psel & PWM_PSEL_OUT_CONNECT_Msk) != 0u) {
				continue; /* disconnected */
			}
			for (int k = 0; k < 4; k++) {
				if (pin == track_pins[k] || pin == play_pins[k]) {
					hit = true;
				}
			}
		}
		if (hit) {
			pwm_stop(p);
			for (int c = 0; c < 4; c++) {
				p->PSEL.OUT[c] = PWM_PSEL_OUT_CONNECT_Msk; /* disconnect */
			}
			released_pwms |= 1u << (2u + i);
		}
	}
}

static void led_pwm_start(NRF_PWM_Type *p, const uint32_t pins[4], uint16_t *seq)
{
	pwm_stop(p);
	for (int i = 0; i < 4; i++) {
		nrf_gpio_pin_clear(pins[i]);
		nrf_gpio_cfg_output(pins[i]); /* idle level while PWM is off: low = dark */
		seq[i] = LED_POL | 0u;
		p->PSEL.OUT[i] = pins[i]; /* PIN | PORT bits, CONNECT = 0 */
	}
	p->MODE = PWM_MODE_UPDOWN_Up << PWM_MODE_UPDOWN_Pos;
	p->PRESCALER = PWM_PRESCALER_PRESCALER_DIV_1 << PWM_PRESCALER_PRESCALER_Pos;
	p->COUNTERTOP = LED_TOP;
	p->LOOP = 1u;
	p->DECODER = (PWM_DECODER_LOAD_Individual << PWM_DECODER_LOAD_Pos) |
		     (PWM_DECODER_MODE_RefreshCount << PWM_DECODER_MODE_Pos);
	p->SEQ[0].PTR = (uint32_t)seq;
	p->SEQ[0].CNT = 4u;
	p->SEQ[0].REFRESH = 0u;
	p->SEQ[0].ENDDELAY = 0u;
	p->SEQ[1].PTR = (uint32_t)seq;
	p->SEQ[1].CNT = 4u;
	p->SEQ[1].REFRESH = 0u;
	p->SEQ[1].ENDDELAY = 0u;
	p->SHORTS = PWM_SHORTS_LOOPSDONE_SEQSTART0_Msk; /* forever; buffer re-read each period */
	p->ENABLE = 1u;
	p->TASKS_SEQSTART[0] = 1u;
}

void controls_hw_rail(bool on)
{
	(void)gpio_pin_set_dt(&btn_com, on ? 1 : 0);
}

int controls_hw_init(void)
{
	(void)gpio_pin_configure_dt(&fn, GPIO_INPUT);
	(void)gpio_pin_configure_dt(&btn_com, GPIO_OUTPUT_ACTIVE); /* rail on */

	release_foreign_pwms();
	led_pwm_start(LED_PWM_TRACK, track_pins, track_seq);
	led_pwm_start(LED_PWM_PLAY, play_pins, play_seq);

#if CHW_ADC
	adc_status = 0;
	for (int i = 0; i < 6; i++) {
		if (!adc_is_ready_dt(&adc_ch[i])) {
			adc_status = -100;
			break;
		}
		if (adc_ch[i].channel_id != (uint8_t)i || adc_ch[i].dev != adc_ch[0].dev) {
			adc_status = -101; /* buffer order would not be our order */
			break;
		}
		int rc = adc_channel_setup_dt(&adc_ch[i]);
		if (rc < 0) {
			adc_status = rc;
			break;
		}
	}
#else
	adc_status = 1;   /* not used: the app scans the SAADC itself */
#endif
	k_msleep(RAIL_SETTLE_MS);
	return CHW_ADC ? adc_status : 0;
}

#if CHW_ADC

static int scan_once(void)
{
	struct adc_sequence seq = {
		.buffer = scan_buf,
		.buffer_size = sizeof(scan_buf),
	};
	int rc = adc_sequence_init_dt(&adc_ch[0], &seq);

	if (rc < 0) {
		return rc;
	}
	seq.channels = 0u;
	for (int i = 0; i < 6; i++) {
		seq.channels |= BIT(adc_ch[i].channel_id);
	}
	return adc_read_dt(&adc_ch[0], &seq);
}

static int16_t clamp_raw(int32_t v)
{
	/* single-ended SAADC can report small negatives near 0 V */
	return (int16_t)(v < 0 ? 0 : (v > 4095 ? 4095 : v));
}

int controls_hw_read(controls_raw_t *out)
{
	int16_t first[6];
	int rc = adc_status;

	if (rc == 0) {
		rc = scan_once();
	}
	if (rc == 0) {
		memcpy(first, scan_buf, sizeof(first));
		rc = scan_once();
	}
	if (rc != 0) {
		out->ladder1 = out->ladder2 = -1;
		for (int i = 0; i < CONTROLS_HW_NUM_FADERS; i++) {
			out->fader[i] = -1;
		}
		return rc;
	}
	int16_t v[6];

	for (int i = 0; i < 6; i++) {
		v[i] = clamp_raw(((int32_t)first[i] + (int32_t)scan_buf[i]) / 2);
	}
	out->ladder1 = v[0];
	out->ladder2 = v[1];
	for (int i = 0; i < CONTROLS_HW_NUM_FADERS; i++) {
		out->fader[i] = v[2 + i];
	}
	return 0;
}

#ifdef CONFIG_ADC_ASYNC
static struct k_poll_signal nb_sig = K_POLL_SIGNAL_INITIALIZER(nb_sig);
static struct adc_sequence nb_seq;
static int16_t nb_buf[6];
static int16_t nb_prev[6];
static bool nb_running, nb_have_prev, nb_stuck;
static uint32_t nb_t0, nb_timeouts, nb_max_ms;
static controls_raw_t nb_last = { -1, -1, { -1, -1, -1, -1 } };

static int nb_start(uint32_t now_ms)
{
	int rc = adc_sequence_init_dt(&adc_ch[0], &nb_seq);

	if (rc < 0) {
		return rc;
	}
	nb_seq.buffer = nb_buf;
	nb_seq.buffer_size = sizeof(nb_buf);
	nb_seq.channels = 0u;
	for (int i = 0; i < 6; i++) {
		nb_seq.channels |= BIT(adc_ch[i].channel_id);
	}
	k_poll_signal_reset(&nb_sig);
	rc = adc_read_async(adc_ch[0].dev, &nb_seq, &nb_sig);
	if (rc == 0) {
		nb_running = true;
		nb_t0 = now_ms;
	}
	return rc;
}

int controls_hw_read_nb(controls_raw_t *out, uint32_t now_ms, uint32_t timeout_ms)
{
	int fresh = 1;

	if (adc_status != 0) {
		*out = nb_last;
		return adc_status;
	}
	if (nb_running) {
		unsigned int signaled = 0;
		int result = 0;

		k_poll_signal_check(&nb_sig, &signaled, &result);
		if (!signaled) {
			if (!nb_stuck && now_ms - nb_t0 > timeout_ms) {
				nb_stuck = true;     /* never restart: the driver still owns the context */
				nb_timeouts++;
			}
			*out = nb_last;
			return 1;
		}
		nb_running = false;
		nb_stuck = false;
		nb_max_ms = MAX(nb_max_ms, now_ms - nb_t0);
		if (result == 0) {
			int16_t v[6];

			for (int i = 0; i < 6; i++) {
				const int32_t a = nb_buf[i];
				const int32_t b = nb_have_prev ? nb_prev[i] : nb_buf[i];

				v[i] = clamp_raw((a + b) / 2);
				nb_prev[i] = nb_buf[i];
			}
			nb_have_prev = true;
			nb_last.ladder1 = v[0];
			nb_last.ladder2 = v[1];
			for (int i = 0; i < CONTROLS_HW_NUM_FADERS; i++) {
				nb_last.fader[i] = v[2 + i];
			}
			fresh = 0;
		}
	}
	(void)nb_start(now_ms);
	*out = nb_last;
	return fresh;
}

uint32_t controls_hw_adc_timeouts(void)
{
	return nb_timeouts;
}

uint32_t controls_hw_adc_max_ms(void)
{
	return nb_max_ms;
}

int controls_hw_adc_stuck(void)
{
	return nb_stuck ? 1 : 0;
}
#endif

#endif /* CHW_ADC */

bool controls_hw_fn_pressed(void)
{
	return gpio_pin_get_dt(&fn) > 0;
}

static uint16_t duty(uint8_t b)
{
	uint32_t d = ((uint32_t)LED_FULL_TICKS * b * b) / (255u * 255u);

	if (b > 0u && d < 1u) {
		d = 1u;
	}
	return (uint16_t)(LED_POL | d);
}

void controls_hw_leds(const uint8_t track[4], const uint8_t play[4])
{
	for (int i = 0; i < 4; i++) {
		track_seq[i] = duty(track[i]);
		play_seq[i] = duty(play[i]);
	}
}

void controls_hw_leds_off(void)
{
	for (int i = 0; i < 4; i++) {
		track_seq[i] = LED_POL;
		play_seq[i] = LED_POL;
	}
	pwm_stop(LED_PWM_TRACK);
	pwm_stop(LED_PWM_PLAY);
	for (int i = 0; i < 4; i++) {
		nrf_gpio_pin_clear(track_pins[i]);
		nrf_gpio_pin_clear(play_pins[i]);
	}
}

void controls_hw_arm_fn_wake(void)
{
	nrf_gpio_cfg_sense_input(NRF_GPIO_PIN_MAP(0, fn.pin), NRF_GPIO_PIN_PULLUP,
				 NRF_GPIO_PIN_SENSE_LOW);
}

uint32_t controls_hw_released_pwms(void)
{
	return released_pwms;
}

int controls_hw_adc_status(void)
{
	return adc_status;
}
