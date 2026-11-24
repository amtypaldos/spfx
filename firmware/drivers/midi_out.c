/* SP-1 FX — bit-level MIDI + PO sync transmitter. See midi_out.h.
 *
 * Framing idea (10-bit frame shifted out LSB first, one bit per 32 us timer
 * event: bit0 = start 0, bits1..8 = data LSB first, bit9 = stop 1) follows
 * chattock/sp1-tape-looper tools/capacity422/runtime/src/main.c @0359372e
 * (midi_send / midi_timer_isr, MIT, (c) chattock); rewritten as a
 * hardware-independent, queued, host-testable module for SP-1 FX.
 *
 * SPDX-License-Identifier: MIT
 */
#include "midi_out.h"

#include <string.h>

uint32_t midi_out_ms_to_ticks(uint32_t ms)
{
	return (ms * 1000u + MIDI_OUT_BIT_US / 2u) / MIDI_OUT_BIT_US;
}

uint32_t midi_out_byte_us(void)
{
	return MIDI_OUT_BITS_PER_BYTE * MIDI_OUT_BIT_US;
}

void midi_out_init(midi_out_t *m, int invert, uint32_t po_width_ms, int po_active_high)
{
	memset(m, 0, sizeof(*m));
	m->invert = invert ? 1u : 0u;
	m->po_active_high = po_active_high ? 1u : 0u;
	m->po_width_ticks = midi_out_ms_to_ticks(po_width_ms);
	if (m->po_width_ticks == 0u) {
		m->po_width_ticks = 1u;
	}
}

static uint8_t midi_pin(const midi_out_t *m, uint8_t line)
{
	return (uint8_t)(m->invert ? !line : line);
}

static uint8_t po_pin(const midi_out_t *m, int active)
{
	return (uint8_t)(m->po_active_high ? (active ? 1 : 0) : (active ? 0 : 1));
}

midi_out_levels_t midi_out_idle_levels(const midi_out_t *m)
{
	midi_out_levels_t l = { midi_pin(m, 1u), po_pin(m, 0) };
	return l;
}

int midi_out_send(midi_out_t *m, uint8_t byte)
{
	if (byte >= 0xF8u) {
		uint32_t h = m->rt_head;
		if (h - m->rt_tail >= MIDI_OUT_RT_QLEN) {
			m->bytes_dropped++;
			return -1;
		}
		m->rt_q[h & (MIDI_OUT_RT_QLEN - 1u)] = byte;
		m->rt_head = h + 1u;   /* publish after the data is written */
	} else {
		uint32_t h = m->msg_head;
		if (h - m->msg_tail >= MIDI_OUT_MSG_QLEN) {
			m->bytes_dropped++;
			return -1;
		}
		m->msg_q[h & (MIDI_OUT_MSG_QLEN - 1u)] = byte;
		m->msg_head = h + 1u;
	}
	return 0;
}

int midi_out_po_pulse(midi_out_t *m)
{
	if (m->po_req - m->po_done >= MIDI_OUT_PO_MAX_PENDING) {
		m->pulses_dropped++;
		return -1;
	}
	m->po_req = m->po_req + 1u;
	return 0;
}

uint32_t midi_out_pending(const midi_out_t *m)
{
	return (m->rt_head - m->rt_tail) + (m->msg_head - m->msg_tail);
}

int midi_out_busy(const midi_out_t *m)
{
	return m->bits_left != 0u;
}

static int next_byte(midi_out_t *m, uint8_t *b)
{
	uint32_t t = m->rt_tail;
	if (m->rt_head != t) {
		*b = m->rt_q[t & (MIDI_OUT_RT_QLEN - 1u)];
		m->rt_tail = t + 1u;
		return 1;
	}
	t = m->msg_tail;
	if (m->msg_head != t) {
		*b = m->msg_q[t & (MIDI_OUT_MSG_QLEN - 1u)];
		m->msg_tail = t + 1u;
		return 1;
	}
	return 0;
}

midi_out_levels_t midi_out_tick(midi_out_t *m)
{
	midi_out_levels_t out;
	uint8_t line = 1u; /* mark / idle */
	uint8_t b;

	m->ticks++;

	if (m->bits_left == 0u && next_byte(m, &b)) {
		/* bit0 = start (0), bits1..8 = data LSB first, bit9 = stop (1) */
		m->frame = (uint16_t)(((uint16_t)b << 1) | (1u << 9));
		m->bits_left = (uint8_t)MIDI_OUT_BITS_PER_BYTE;
		if (b == MIDI_CLOCK) {
			m->clocks_sent++;   /* counted at start; bytes_sent at stop */
		}
	}
	if (m->bits_left != 0u) {
		line = (uint8_t)(m->frame & 1u);
		m->frame >>= 1;
		m->bits_left--;
		if (m->bits_left == 0u) {
			m->bytes_sent++;   /* stop bit is being driven now */
		}
	}
	out.midi = midi_pin(m, line);

	/* PO pulse: exactly po_width_ticks active, then >= po_width_ticks idle. */
	if (m->po_remaining == 0u && m->po_gap == 0u && m->po_req != m->po_done) {
		m->po_done = m->po_done + 1u;
		m->po_remaining = m->po_width_ticks;
		m->pulses_sent++;
	}
	if (m->po_remaining != 0u) {
		out.po = po_pin(m, 1);
		if (--m->po_remaining == 0u) {
			m->po_gap = m->po_width_ticks;
		}
	} else {
		out.po = po_pin(m, 0);
		if (m->po_gap != 0u) {
			m->po_gap--;
		}
	}
	return out;
}
