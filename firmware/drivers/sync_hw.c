/* SP-1 FX — sync jack hardware driver (Zephyr, nRF52840). NOT TESTED ON HARDWARE.
 *
 * Timer choice: NRF_TIMER2 by direct register access + IRQ_CONNECT.
 *   - It is the instance chattock/sp1-tape-looper @0359372e uses for the same
 *     job (midi_timer_init / midi_timer_isr, MIT; adapted here: the ISR now
 *     runs continuously and calls the queued pure-C core, and also clocks the
 *     PO pulse; plus a latency/overrun counter).
 *   - TIMER0 is avoided: it belongs to the Bluetooth controller / MPSL in
 *     configs that have one (none here, but the final firmware might).
 *     TIMER1 is used by some radio/802.15.4 and nrfx samples. The system
 *     clock on nRF is RTC1, not a TIMER. The board devicetree binds no TIMER
 *     node and this image enables no Zephyr counter driver, so no Zephyr
 *     driver touches TIMER2.
 *   - The TE bootloader is documented to start LFCLK/HFCLK, PWM2, PWM3 and
 *     SAADC ("BIG FIVE" notes in firmware/app/zephyr_min/src/main.c); no
 *     TIMER is listed. GUESS: if it left TIMER2 running, the init below stops
 *     and fully reprograms it (no PPI channels are touched).
 *   - A direct register driver (rather than the Zephyr counter API) keeps the
 *     ISR to a few register writes: a 31.25 kHz interrupt must be tiny.
 *
 * The timer runs continuously at 16 MHz / 16 = 1 MHz, CC[0] = 32, cleared on
 * compare: one interrupt every 32 us. Cost: 31250 IRQs/s of roughly 1-2 us
 * each (estimate, unmeasured) = a few % of the CPU. Bit edges are written by
 * the CPU in the ISR, so each edge lags the compare by the ISR entry latency
 * (a few hundred ns nominal; more while another ISR of equal/higher priority
 * or an irq_lock() section runs). The ISR measures that latency with
 * CAPTURE[1] and counts ISRs that start >= 16 us late (half a MIDI bit:
 * a receiver sampling mid-bit could misread) as `late`.
 *
 * Accuracy: TIMER runs from HFCLK. With HFXO (crystal, +/-40 ppm) MIDI timing
 * is exact; on HFINT (RC) it is only ~1 % class (see the nRF52840 PS). USB
 * requests HFXO, so with the USB console enabled HFXO should be on; the
 * status line reports which source is active.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sync_hw.h"

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <nrfx.h>

#define SYNC_TIMER      NRF_TIMER2
#define SYNC_TIMER_IRQn TIMER2_IRQn
#define SYNC_TIMER_PRIO 1
#define LATE_US         (MIDI_OUT_BIT_US / 2u)

static midi_out_t core;
static uint32_t midi_mask, po_mask, all_mask;
static volatile int midi_on, po_on;
static volatile uint32_t isr_count, late_count, max_lat;
static int timer_running;

static void sync_timer_isr(const void *arg)
{
	ARG_UNUSED(arg);
	SYNC_TIMER->TASKS_CAPTURE[1] = 1u;
	uint32_t lat = SYNC_TIMER->CC[1];

	SYNC_TIMER->EVENTS_COMPARE[0] = 0u;
	(void)SYNC_TIMER->EVENTS_COMPARE[0];   /* flush the clear (as Tape Looper) */

	midi_out_levels_t l = midi_out_tick(&core);
	uint32_t set = (l.midi ? midi_mask : 0u) | (l.po ? po_mask : 0u);

	NRF_P0->OUTSET = set;
	NRF_P0->OUTCLR = all_mask & ~set;

	isr_count++;
	if (lat >= LATE_US) {
		late_count++;
	}
	if (lat > max_lat) {
		max_lat = lat;
	}
}

static void pin_output(uint32_t pin, int level)
{
	if (level) {
		NRF_P0->OUTSET = 1u << pin;
	} else {
		NRF_P0->OUTCLR = 1u << pin;
	}
	/* push-pull, standard drive S0S1, input buffer disconnected
	 * (Tape Looper: DIR_Output | DRIVE_S0S1) */
	NRF_P0->PIN_CNF[pin] =
		(GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
		(GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos) |
		(GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos);
}

int sync_hw_init(const sync_hw_cfg_t *cfg)
{
	const uint32_t ring = 1u << SYNC_HW_PIN_RING;
	const uint32_t tip = (1u << SYNC_HW_PIN_TIP_A) | (1u << SYNC_HW_PIN_TIP_B);

	midi_out_init(&core, cfg->midi_invert, cfg->po_pulse_ms, cfg->po_active_high);
	midi_mask = cfg->swap_tip_ring ? tip : ring;
	po_mask = cfg->swap_tip_ring ? ring : tip;
	all_mask = ring | tip;
	midi_on = cfg->midi_enable;
	po_on = cfg->po_enable;

	/* stop and reset the timer in case anything left it running */
	SYNC_TIMER->TASKS_STOP = 1u;
	SYNC_TIMER->INTENCLR = 0xFFFFFFFFu;
	SYNC_TIMER->SHORTS = 0u;
	for (int i = 0; i < 6; i++) {
		SYNC_TIMER->EVENTS_COMPARE[i] = 0u;
	}
	NVIC_ClearPendingIRQ(SYNC_TIMER_IRQn);

	/* idle levels before the pins become outputs */
	midi_out_levels_t idle = midi_out_idle_levels(&core);

	pin_output(SYNC_HW_PIN_RING, (cfg->swap_tip_ring ? idle.po : idle.midi));
	pin_output(SYNC_HW_PIN_TIP_A, (cfg->swap_tip_ring ? idle.midi : idle.po));
	pin_output(SYNC_HW_PIN_TIP_B, (cfg->swap_tip_ring ? idle.midi : idle.po));

	SYNC_TIMER->MODE = TIMER_MODE_MODE_Timer;
	SYNC_TIMER->BITMODE = TIMER_BITMODE_BITMODE_16Bit;
	SYNC_TIMER->PRESCALER = 4u;                 /* 16 MHz / 16 = 1 us */
	SYNC_TIMER->CC[0] = MIDI_OUT_BIT_US;        /* 32 us = 1 MIDI bit */
	SYNC_TIMER->SHORTS = TIMER_SHORTS_COMPARE0_CLEAR_Msk;
	SYNC_TIMER->INTENSET = TIMER_INTENSET_COMPARE0_Msk;
	IRQ_CONNECT(TIMER2_IRQn, SYNC_TIMER_PRIO, sync_timer_isr, NULL, 0);
	if (cfg->start_stopped) {
		timer_running = 0;
		return 0;
	}
	irq_enable(SYNC_TIMER_IRQn);
	SYNC_TIMER->TASKS_CLEAR = 1u;
	SYNC_TIMER->TASKS_START = 1u;
	timer_running = 1;
	return 0;
}

void sync_hw_run(int on)
{
	if (on && !timer_running) {
		SYNC_TIMER->EVENTS_COMPARE[0] = 0u;
		NVIC_ClearPendingIRQ(SYNC_TIMER_IRQn);
		irq_enable(SYNC_TIMER_IRQn);
		SYNC_TIMER->TASKS_CLEAR = 1u;
		SYNC_TIMER->TASKS_START = 1u;
		timer_running = 1;
	} else if (!on && timer_running) {
		SYNC_TIMER->TASKS_STOP = 1u;
		irq_disable(SYNC_TIMER_IRQn);
		SYNC_TIMER->EVENTS_COMPARE[0] = 0u;
		NVIC_ClearPendingIRQ(SYNC_TIMER_IRQn);
		/* idle levels on both lines */
		midi_out_levels_t idle = midi_out_idle_levels(&core);
		uint32_t set = (idle.midi ? midi_mask : 0u) | (idle.po ? po_mask : 0u);

		NRF_P0->OUTSET = set;
		NRF_P0->OUTCLR = all_mask & ~set;
		timer_running = 0;
	}
}

int sync_hw_running(void)
{
	return timer_running;
}

int sync_hw_drained(void)
{
	return midi_out_pending(&core) == 0u && !midi_out_busy(&core) &&
	       core.po_req == core.po_done && core.po_remaining == 0u;
}

int sync_hw_midi(uint8_t byte)
{
	return midi_on ? midi_out_send(&core, byte) : -1;
}

int sync_hw_po_pulse(void)
{
	return po_on ? midi_out_po_pulse(&core) : -1;
}

void sync_hw_enable(int midi_enable, int po_enable)
{
	midi_on = midi_enable;
	po_on = po_enable;
}

const midi_out_t *sync_hw_core(void)
{
	return &core;
}

void sync_hw_stats(sync_hw_stats_t *out, int reset_max)
{
	out->isr_count = isr_count;
	out->late = late_count;
	out->max_lat_us = max_lat;
	if (reset_max) {
		max_lat = 0u;
	}
	out->hfclk_xtal = (NRF_CLOCK->HFCLKSTAT & CLOCK_HFCLKSTAT_SRC_Msk) ==
			  (CLOCK_HFCLKSTAT_SRC_Xtal << CLOCK_HFCLKSTAT_SRC_Pos);
}

void sync_hw_shutdown(void)
{
	timer_running = 0;
	irq_disable(SYNC_TIMER_IRQn);
	SYNC_TIMER->TASKS_STOP = 1u;
	SYNC_TIMER->INTENCLR = 0xFFFFFFFFu;
	SYNC_TIMER->SHORTS = 0u;
	/* reset value of PIN_CNF: input, input buffer disconnected, no pull */
	NRF_P0->PIN_CNF[SYNC_HW_PIN_RING] = 0x00000002u;
	NRF_P0->PIN_CNF[SYNC_HW_PIN_TIP_A] = 0x00000002u;
	NRF_P0->PIN_CNF[SYNC_HW_PIN_TIP_B] = 0x00000002u;
}
