/* SP-1 FX -- physical controls driver (Zephyr only, nRF52840 / SP-1).
 *
 * Reads the two button ladders and the four faders on the SAADC, the
 * FUNCTION button on its own GPIO, switches the ladder/fader supply rail
 * (BTN_COM, P1.10) and drives the eight LEDs with hardware PWM, brightness
 * 0..255. Decoding raw values into buttons is NOT done here: see the
 * hardware-independent firmware/drivers/ladder.{h,c}.
 *
 * Devicetree contract (the app overlay provides it on /zephyr,user):
 *   io-channels = <&adc 0>..<&adc 5>  ladder1, ladder2, fader1..fader4 (in
 *                                     this order; channel id == index)
 *   fn-gpios      FUNCTION, active-low with pull-up
 *   btn-com-gpios BTN_COM rail, active-high
 *
 * NOT TESTED ON HARDWARE.
 * SPDX-License-Identifier: MIT
 */
#ifndef SP1FX_CONTROLS_HW_H
#define SP1FX_CONTROLS_HW_H

#include <stdbool.h>
#include <stdint.h>

#define CONTROLS_HW_NUM_FADERS 4

typedef struct {
	int16_t ladder1;   /* AIN0, P0.02: T1..T4 + PLAY  (raw 12-bit, -1 = ADC error) */
	int16_t ladder2;   /* AIN1, P0.03: VOL+/- + FWD/REV rocker */
	int16_t fader[CONTROLS_HW_NUM_FADERS]; /* AIN3, AIN6, AIN2, AIN7 */
} controls_raw_t;

/* Rail on, ADC channels set up, FN input, LED pins + PWM0/PWM1 started dark.
 * Returns 0, or a negative code (the app prints it; reads then return -1s). */
int controls_hw_init(void);

/* One scan of all six channels, two conversions averaged (as Tape Looper).
 * Blocking for roughly 0.3 ms. On error every field is -1. */
int controls_hw_read(controls_raw_t *out);

/* Non-blocking variant (needs CONFIG_ADC_ASYNC): never waits for the SAADC. Each call
 * collects a finished scan (if any), averages it with the previous one (as the two-scan
 * controls_hw_read) and starts the next. Returns 0 with fresh values, 1 when the last values
 * are repeated (scan still running), <0 on error (values -1). A scan that does not finish
 * within timeout_ms is counted (controls_hw_adc_timeouts) and NOT restarted: the controls
 * then hold their last values while everything else keeps running. */
int controls_hw_read_nb(controls_raw_t *out, uint32_t now_ms, uint32_t timeout_ms);
uint32_t controls_hw_adc_timeouts(void);
uint32_t controls_hw_adc_max_ms(void);   /* longest scan seen (start -> result collected) */
int controls_hw_adc_stuck(void);         /* 1: a scan never finished (controls frozen) */

bool controls_hw_fn_pressed(void);

/* Brightness 0..255 per LED (perceptual: squared before it becomes a duty). */
void controls_hw_leds(const uint8_t track[4], const uint8_t play[4]);

/* Stop both LED PWMs and force every LED pin low (before SYSTEM_OFF). */
void controls_hw_leds_off(void);

/* Ladder/fader supply rail on/off. */
void controls_hw_rail(bool on);

/* FN as SYSTEM_OFF wake source (sense LOW, pull-up). */
void controls_hw_arm_fn_wake(void);

/* Diagnostics: which bootloader PWM instances were found routed to LED pins
 * and released at init (bit n = PWMn), ADC channel setup result. */
uint32_t controls_hw_released_pwms(void);
int controls_hw_adc_status(void);

#endif
