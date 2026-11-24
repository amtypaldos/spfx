/* SP-1 FX — bit-level MIDI + Pocket Operator sync transmitter (pure C).
 *
 * Hardware-independent core of the sync-jack driver. A caller invokes
 * midi_out_tick() once per MIDI bit time (32 us = 31250 baud); each call
 * returns the logical levels to drive on the MIDI line and the PO-sync line
 * for the next 32 us. On the device the caller is a hardware-timer ISR
 * (firmware/drivers/sync_hw.c); in tests it is a simulated clock.
 *
 * MIDI framing: 8N1, 10 ticks per byte = 320 us: start bit (space = 0), 8 data
 * bits LSB first, stop bit (mark = 1). Line idles at mark. Back-to-back bytes
 * are sent with no gap (next start bit directly after the stop bit).
 *
 * Output levels are PIN levels: `invert` flips the MIDI line (the SP-1 drives
 * the ring through a PNP stage that inverts, so the device default is
 * invert = 1, as in chattock/sp1-tape-looper); `po_active_high` selects the
 * PO pulse polarity (idle = the opposite level).
 *
 * Queues: real-time bytes (0xF8..0xFF: clock, start, continue, stop) go into
 * their own FIFO which is always served before the normal FIFO, so they jump
 * ahead of any other (non-real-time) message bytes. A byte never interrupts a
 * byte already on the wire. A full queue DROPS THE NEW BYTE and counts it.
 *
 * Concurrency: single producer (thread: midi_out_send / midi_out_po_pulse)
 * and single consumer (ISR: midi_out_tick); lock-free ring indices, each
 * written by one side only. No allocation, no floating point in tick().
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SP1FX_MIDI_OUT_H
#define SP1FX_MIDI_OUT_H

#include <stdint.h>

#define MIDI_OUT_BAUD          31250u
#define MIDI_OUT_BIT_US        32u      /* 1e6 / 31250 */
#define MIDI_OUT_BITS_PER_BYTE 10u      /* start + 8 data + stop */
#define MIDI_OUT_TICKS_PER_S   31250u

#define MIDI_OUT_RT_QLEN   16u   /* power of two */
#define MIDI_OUT_MSG_QLEN  32u   /* power of two */
#define MIDI_OUT_PO_MAX_PENDING 4u

#define MIDI_CLOCK    0xF8u
#define MIDI_START    0xFAu
#define MIDI_CONTINUE 0xFBu
#define MIDI_STOP     0xFCu

typedef struct {
	/* config (set by midi_out_init; do not change while ticking) */
	uint8_t invert;          /* 1: pin = !line (PNP stage) */
	uint8_t po_active_high;  /* 1: pulse drives the PO pin high */
	uint32_t po_width_ticks; /* pulse width in 32 us ticks */

	/* queues: head written by producer only, tail by consumer only */
	uint8_t rt_q[MIDI_OUT_RT_QLEN];
	volatile uint32_t rt_head, rt_tail;
	uint8_t msg_q[MIDI_OUT_MSG_QLEN];
	volatile uint32_t msg_head, msg_tail;

	/* transmitter (consumer side) */
	uint16_t frame;          /* remaining bits, LSB = next bit out */
	uint8_t bits_left;

	/* PO pulse generator */
	volatile uint32_t po_req;    /* producer: pulses requested */
	volatile uint32_t po_done;   /* consumer: pulses started */
	uint32_t po_remaining;       /* ticks left in the active pulse */
	uint32_t po_gap;             /* ticks of enforced idle after a pulse */

	/* statistics (written by the side named) */
	volatile uint32_t ticks;          /* consumer */
	volatile uint32_t bytes_sent;     /* consumer: bytes whose stop bit was sent */
	volatile uint32_t clocks_sent;    /* consumer: 0xF8 among them */
	volatile uint32_t pulses_sent;    /* consumer */
	volatile uint32_t bytes_dropped;  /* producer: queue full */
	volatile uint32_t pulses_dropped; /* producer: too many pending */
} midi_out_t;

typedef struct {
	uint8_t midi; /* pin level for the MIDI line (after inversion) */
	uint8_t po;   /* pin level for the PO line (after polarity) */
} midi_out_levels_t;

/* po_width_ms: pulse width, rounded to whole 32 us ticks (5 ms -> 156 ticks
 * = 4.992 ms). After a pulse the PO line stays idle at least as long again. */
void midi_out_init(midi_out_t *m, int invert, uint32_t po_width_ms, int po_active_high);

/* Idle levels (what to drive before the first tick / after stopping). */
midi_out_levels_t midi_out_idle_levels(const midi_out_t *m);

/* Producer side. Return 0 if queued, -1 if dropped (queue full). */
int midi_out_send(midi_out_t *m, uint8_t byte);
int midi_out_po_pulse(midi_out_t *m);

/* Bytes waiting (not counting one on the wire); 1 if transmitting. */
uint32_t midi_out_pending(const midi_out_t *m);
int midi_out_busy(const midi_out_t *m);

/* Consumer side: call exactly once per 32 us. */
midi_out_levels_t midi_out_tick(midi_out_t *m);

/* Helpers (pure arithmetic). */
uint32_t midi_out_ms_to_ticks(uint32_t ms);
uint32_t midi_out_byte_us(void); /* 320 */

#endif
