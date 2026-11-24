/* SP-1 FX — sync jack hardware driver (Zephyr, nRF52840). NOT TESTED ON HARDWARE.
 *
 * Drives the SYNC jack exactly the way chattock/sp1-tape-looper @0359372e
 * describes it (tools/capacity422/runtime/src/main.c, MIT):
 *   P0.23 BC807_BASE -> PNP -> SYNC_RING  (MIDI; the PNP inverts)
 *   P0.20 PO_A       -> SYNC_TIP          (PO / Volca sync pulse)
 *   P0.17 PO_B       -> SYNC_TIP          (paralleled with PO_A; always
 *                                          driven to the SAME level)
 * All three are push-pull outputs, standard drive (S0S1), as Tape Looper
 * configures them. The pure-C core (midi_out.c) decides the levels; this file
 * only owns the pins and a 32 us hardware timer whose ISR calls
 * midi_out_tick() and writes the result to the pins.
 *
 * Timer: NRF_TIMER2, direct register access (same instance as Tape Looper).
 * See sync_hw.c for why.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SP1FX_SYNC_HW_H
#define SP1FX_SYNC_HW_H

#include <stdint.h>
#include "midi_out.h"

#define SYNC_HW_PIN_RING  23u  /* P0.23, through the inverting PNP stage */
#define SYNC_HW_PIN_TIP_A 20u  /* P0.20 */
#define SYNC_HW_PIN_TIP_B 17u  /* P0.17, paralleled with P0.20 */

typedef struct {
	int midi_invert;      /* 1: compensate the PNP inversion (Tape Looper default) */
	int swap_tip_ring;    /* 0: MIDI on ring (P0.23), PO on tip (P0.20+P0.17)
	                       * 1: MIDI on tip, PO on ring (for a TRS-B style jack) */
	int po_active_high;   /* 1: PO pulse drives the pin high */
	uint32_t po_pulse_ms; /* 5 in Tape Looper */
	int midi_enable;      /* 0: never queue MIDI bytes (line held idle) */
	int po_enable;        /* 0: never queue PO pulses (line held idle) */
	int start_stopped;    /* 1: configure pins + timer but leave the 32 us timer (and its
	                       * 31,250 interrupts/s) OFF until sync_hw_run(1) */
} sync_hw_cfg_t;

typedef struct {
	uint32_t isr_count;    /* timer interrupts serviced */
	uint32_t late;         /* ISRs that ran >= 16 us (half a bit) after the compare */
	uint32_t max_lat_us;   /* worst ISR entry latency seen (reset by reader) */
	uint32_t hfclk_xtal;   /* 1 if HFCLK runs from the crystal (HFXO) */
} sync_hw_stats_t;

/* Configure pins (idle levels first, then outputs) and start the timer. */
int sync_hw_init(const sync_hw_cfg_t *cfg);

/* Thread-side helpers (single producer). Return 0 or -1 (dropped/disabled). */
int sync_hw_midi(uint8_t byte);
int sync_hw_po_pulse(void);
void sync_hw_enable(int midi_enable, int po_enable);

const midi_out_t *sync_hw_core(void);
void sync_hw_stats(sync_hw_stats_t *out, int reset_max);

/* Start (1) / stop (0) the 32 us bit timer. Stopping drives the idle levels; call it only
 * when sync_hw_drained() (or anything still queued is cut). Thread context. */
void sync_hw_run(int on);
int sync_hw_running(void);
/* 1 when no MIDI byte is queued or on the wire and no PO pulse is pending or active. */
int sync_hw_drained(void);

/* Stop the timer and return all three pins to their reset state (input,
 * disconnected) — used before SYSTEM_OFF so nothing keeps the PNP biased. */
void sync_hw_shutdown(void);

#endif
