/* SP-1 FX -- register-level SAADC scanner. See saadc_scan.h. NOT TESTED ON HARDWARE.
 * SPDX-License-Identifier: MIT */
#include "saadc_scan.h"
#include "../app/app_logic.h"

#include <zephyr/kernel.h>
#include <nrfx.h>
#include <string.h>

static const uint8_t ain[SAADC_SCAN_CH] = { 0, 1, 3, 6, 2, 7, 4 };
static volatile int16_t buf[SAADC_SCAN_CH] __aligned(4);   /* EasyDMA target: RAM */
static int16_t prev[SAADC_SCAN_CH];
static int have_prev;
static scanfsm_t fsm;
static uint32_t start_fail, stop_fail, ppi_cleared, was_enabled;

static int wait_event(volatile uint32_t *ev, int us)
{
	for (int i = 0; i < us; i++) {
		if (*ev) {
			return 1;
		}
		k_busy_wait(1);
	}
	return *ev != 0u;
}

static void hw_stop(void)
{
	NRF_SAADC->EVENTS_STOPPED = 0u;
	NRF_SAADC->TASKS_STOP = 1u;
	if (!wait_event(&NRF_SAADC->EVENTS_STOPPED, 200)) {
		stop_fail++;
	}
	NRF_SAADC->EVENTS_STOPPED = 0u;
	NRF_SAADC->EVENTS_STARTED = 0u;
	NRF_SAADC->EVENTS_END = 0u;
	NRF_SAADC->EVENTS_DONE = 0u;
	NRF_SAADC->EVENTS_RESULTDONE = 0u;
}

static void hw_start(void)
{
	NRF_SAADC->RESULT.PTR = (uint32_t)buf;
	NRF_SAADC->RESULT.MAXCNT = SAADC_SCAN_CH;
	NRF_SAADC->EVENTS_STARTED = 0u;
	NRF_SAADC->EVENTS_END = 0u;
	NRF_SAADC->TASKS_START = 1u;
	if (!wait_event(&NRF_SAADC->EVENTS_STARTED, 50)) {
		start_fail++;   /* the timeout path stops and restarts it */
		return;
	}
	NRF_SAADC->EVENTS_STARTED = 0u;
	NRF_SAADC->TASKS_SAMPLE = 1u;   /* scan mode: one SAMPLE converts every enabled channel */
}

/* Disconnect any PPI channel (or fork) whose task endpoint is a SAADC task: a leftover from
 * the bootloader could START/SAMPLE/STOP the SAADC behind our back. */
static void clear_saadc_ppi(void)
{
	const uint32_t lo = (uint32_t)NRF_SAADC;
	const uint32_t hi = lo + 0x100u;   /* tasks live at the start of the peripheral */

	for (int c = 0; c < 20; c++) {
		const uint32_t tep = NRF_PPI->CH[c].TEP, fork = NRF_PPI->FORK[c].TEP;

		if ((tep >= lo && tep < hi) || (fork >= lo && fork < hi)) {
			NRF_PPI->CHENCLR = 1u << c;
			NRF_PPI->CH[c].TEP = 0u;
			NRF_PPI->FORK[c].TEP = 0u;
			ppi_cleared++;
		}
	}
}

void saadc_scan_init(void)
{
	NVIC_DisableIRQ(SAADC_IRQn);
	NRF_SAADC->INTENCLR = 0xFFFFFFFFu;
	was_enabled = NRF_SAADC->ENABLE;
	if (was_enabled) {
		hw_stop();
	}
	NRF_SAADC->ENABLE = 0u;
	clear_saadc_ppi();
	NVIC_ClearPendingIRQ(SAADC_IRQn);

	for (int i = 0; i < 8; i++) {
		NRF_SAADC->CH[i].PSELP = 0u;   /* NC: channel off */
		NRF_SAADC->CH[i].PSELN = 0u;
	}
	for (int i = 0; i < SAADC_SCAN_CH; i++) {
		NRF_SAADC->CH[i].CONFIG =
			(SAADC_CH_CONFIG_RESP_Bypass << SAADC_CH_CONFIG_RESP_Pos) |
			(SAADC_CH_CONFIG_RESN_Bypass << SAADC_CH_CONFIG_RESN_Pos) |
			(SAADC_CH_CONFIG_GAIN_Gain1_6 << SAADC_CH_CONFIG_GAIN_Pos) |
			(SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
			(SAADC_CH_CONFIG_TACQ_20us << SAADC_CH_CONFIG_TACQ_Pos) |
			(SAADC_CH_CONFIG_MODE_SE << SAADC_CH_CONFIG_MODE_Pos) |
			(SAADC_CH_CONFIG_BURST_Disabled << SAADC_CH_CONFIG_BURST_Pos);
		NRF_SAADC->CH[i].PSELN = 0u;
		NRF_SAADC->CH[i].PSELP = (uint32_t)(SAADC_CH_PSELP_PSELP_AnalogInput0 + ain[i]);
	}
	NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_12bit;
	NRF_SAADC->OVERSAMPLE = SAADC_OVERSAMPLE_OVERSAMPLE_Bypass;
	NRF_SAADC->SAMPLERATE = SAADC_SAMPLERATE_MODE_Task << SAADC_SAMPLERATE_MODE_Pos;
	NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled;
	scanfsm_init(&fsm);
	have_prev = 0;
}

static int16_t clamp12(int32_t v)
{
	return (int16_t)(v < 0 ? 0 : (v > 4095 ? 4095 : v));
}

int saadc_scan_poll(uint32_t now_ms, int16_t out[SAADC_SCAN_CH])
{
	const int ended = NRF_SAADC->EVENTS_END != 0u;
	const scan_act_t a = scanfsm_step(&fsm, now_ms, ended);
	int fresh = 0;

	switch (a) {
	case SCAN_ACT_START:
		hw_start();
		break;
	case SCAN_ACT_COLLECT_START: {
		int16_t cur[SAADC_SCAN_CH];

		NRF_SAADC->EVENTS_END = 0u;
		for (int i = 0; i < SAADC_SCAN_CH; i++) {
			cur[i] = buf[i];
		}
		for (int i = 0; i < SAADC_SCAN_CH; i++) {
			const int32_t b = have_prev ? prev[i] : cur[i];

			out[i] = clamp12(((int32_t)cur[i] + b) / 2);
			prev[i] = cur[i];
		}
		have_prev = 1;
		fresh = 1;
		hw_start();
		break;
	}
	case SCAN_ACT_ABORT_START:
		hw_stop();
		hw_start();
		break;
	default:
		break;
	}
	return fresh;
}

void saadc_scan_stats(saadc_scan_stats_t *s, uint32_t now_ms)
{
	s->ok = fsm.ok;
	s->timeouts = fsm.timeouts;
	s->start_fail = start_fail;
	s->stop_fail = stop_fail;
	s->ppi_cleared = ppi_cleared;
	s->was_enabled = was_enabled;
	s->age_ms = scanfsm_age(&fsm, now_ms);
}

void saadc_scan_off(void)
{
	hw_stop();
	NRF_SAADC->ENABLE = 0u;
}
