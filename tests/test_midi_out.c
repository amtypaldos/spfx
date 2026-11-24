/* Tests for firmware/drivers/midi_out (bit-level MIDI + PO sync core) and its
 * integration with firmware/app/sync_out (audio-frame-counted scheduler).
 * Simulated only: one midi_out_tick() call = 32 us of simulated time. */
#include "t.h"
#include "../firmware/drivers/midi_out.h"
#include "../firmware/app/sync_out.h"

/* ---- software UART receiver over the (un-inverted) line ---------------- */
typedef struct {
	int state;        /* 0 idle, 1..9 receiving bit index */
	uint16_t shift;
	uint8_t bytes[8192];
	uint32_t n;
	uint32_t framing_errors;
	uint32_t first_start_tick[8192];
	uint32_t cur_start;
} rx_t;

static void rx_feed(rx_t *r, int line, uint32_t tick)
{
	if (r->state == 0) {
		if (line == 0) { r->state = 1; r->shift = 0; r->cur_start = tick; }
		return;
	}
	if (r->state <= 8) {
		r->shift |= (uint16_t)((line ? 1u : 0u) << (r->state - 1));
		r->state++;
		return;
	}
	/* stop bit */
	if (line != 1) r->framing_errors++;
	if (r->n < sizeof(r->bytes)) {
		r->first_start_tick[r->n] = r->cur_start;
		r->bytes[r->n++] = (uint8_t)r->shift;
	}
	r->state = 0;
}

static rx_t rx; /* large: static */

static void test_bit_pattern(void)
{
	midi_out_t m;
	midi_out_init(&m, 0, 5, 1);
	/* idle before anything queued */
	midi_out_levels_t l = midi_out_tick(&m);
	CHECK(l.midi == 1);
	CHECK(midi_out_send(&m, 0xF8) == 0);
	/* 0xF8 = 1111 1000: start 0, data LSB first 0 0 0 1 1 1 1 1, stop 1 */
	const uint8_t want[10] = { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1 };
	for (int i = 0; i < 10; i++) {
		l = midi_out_tick(&m);
		CHECK_MSG(l.midi == want[i], "bit %d = %d want %d", i, l.midi, want[i]);
	}
	CHECK(midi_out_tick(&m).midi == 1); /* back to idle */
	CHECK(m.bytes_sent == 1 && m.clocks_sent == 1);

	/* 0xFA = 1111 1010 -> data bits 0 1 0 1 1 1 1 1 */
	const uint8_t wfa[10] = { 0, 0, 1, 0, 1, 1, 1, 1, 1, 1 };
	midi_out_send(&m, 0xFA);
	for (int i = 0; i < 10; i++) CHECK(midi_out_tick(&m).midi == wfa[i]);
	/* 0xFC = 1111 1100 -> 0 0 1 1 1 1 1 1 ; 0xFB = 1111 1011 -> 1 1 0 1 1 1 1 1 */
	const uint8_t wfc[10] = { 0, 0, 0, 1, 1, 1, 1, 1, 1, 1 };
	const uint8_t wfb[10] = { 0, 1, 1, 0, 1, 1, 1, 1, 1, 1 };
	midi_out_send(&m, 0xFC);
	midi_out_send(&m, 0xFB);
	for (int i = 0; i < 10; i++) CHECK(midi_out_tick(&m).midi == wfc[i]);
	for (int i = 0; i < 10; i++) CHECK(midi_out_tick(&m).midi == wfb[i]); /* no gap */
	CHECK(midi_out_tick(&m).midi == 1);
	CHECK(m.bytes_sent == 4);
}

static void test_inversion(void)
{
	midi_out_t a, b;
	midi_out_init(&a, 0, 5, 1);
	midi_out_init(&b, 1, 5, 1);
	CHECK(midi_out_idle_levels(&a).midi == 1);
	CHECK(midi_out_idle_levels(&b).midi == 0);
	midi_out_send(&a, 0x5A);
	midi_out_send(&b, 0x5A);
	for (int i = 0; i < 14; i++) {
		midi_out_levels_t la = midi_out_tick(&a), lb = midi_out_tick(&b);
		CHECK(la.midi == !lb.midi);
		CHECK(la.po == lb.po);
	}
	/* PO polarity flag */
	midi_out_t p;
	midi_out_init(&p, 0, 5, 0);
	CHECK(midi_out_idle_levels(&p).po == 1);
	midi_out_po_pulse(&p);
	CHECK(midi_out_tick(&p).po == 0);
}

static void test_timing_arith(void)
{
	CHECK(MIDI_OUT_BIT_US * MIDI_OUT_BAUD == 1000000u);
	CHECK(midi_out_byte_us() == 320u);
	CHECK(midi_out_ms_to_ticks(5) == 156u);           /* 4.992 ms */
	CHECK(midi_out_ms_to_ticks(1) == 31u);            /* 0.992 ms */
	/* one byte occupies exactly 10 ticks, i.e. 320 us */
	midi_out_t m;
	midi_out_init(&m, 1, 5, 1);
	midi_out_send(&m, 0xF8);
	uint32_t t = 0;
	while (m.bytes_sent == 0) { midi_out_tick(&m); t++; }
	CHECK(t == 10);
	CHECK(t * MIDI_OUT_BIT_US == 320u);
	/* PO pulse is exactly 156 ticks active, then idle >= 156 ticks */
	midi_out_t p;
	midi_out_init(&p, 0, 5, 1);
	midi_out_po_pulse(&p);
	midi_out_po_pulse(&p);
	uint32_t hi = 0, first_lo = 0, i;
	for (i = 0; i < 2000; i++) {
		midi_out_levels_t l = midi_out_tick(&p);
		if (l.po) { if (first_lo == 0) hi++; }
		else if (hi && first_lo == 0) first_lo = i;
		if (first_lo && l.po) break;
	}
	CHECK(hi == 156);
	CHECK(i - first_lo == 156);    /* second pulse starts after the gap */
	CHECK(p.pulses_sent == 2);
}

static void test_queue_overflow(void)
{
	midi_out_t m;
	midi_out_init(&m, 0, 5, 1);
	int ok = 0, drop = 0;
	for (int i = 0; i < 40; i++) {
		if (midi_out_send(&m, 0xF8) == 0) ok++; else drop++;
	}
	CHECK(ok == (int)MIDI_OUT_RT_QLEN);
	CHECK(drop == 40 - (int)MIDI_OUT_RT_QLEN);
	CHECK(m.bytes_dropped == (uint32_t)drop);
	/* normal queue is independent */
	CHECK(midi_out_send(&m, 0x90) == 0);
	/* after draining one byte there is room again */
	for (int i = 0; i < 10; i++) midi_out_tick(&m);
	CHECK(midi_out_send(&m, 0xFC) == 0);
	/* the queued bytes still go out intact, in order, rt before 0x90 */
	memset(&rx, 0, sizeof(rx));
	/* first byte already sent above; decode the rest */
	for (uint32_t t = 0; t < 30 * 10 + 20; t++) rx_feed(&rx, midi_out_tick(&m).midi, t);
	CHECK(rx.framing_errors == 0);
	CHECK(rx.n == MIDI_OUT_RT_QLEN + 1);
	CHECK(rx.bytes[MIDI_OUT_RT_QLEN - 2] == 0xF8);
	CHECK(rx.bytes[MIDI_OUT_RT_QLEN - 1] == 0xFC);   /* rt queue first ... */
	CHECK(rx.bytes[MIDI_OUT_RT_QLEN] == 0x90);       /* ... then the message */
	/* PO pending limit */
	midi_out_t p;
	midi_out_init(&p, 0, 5, 1);
	for (int i = 0; i < 6; i++) midi_out_po_pulse(&p);
	CHECK(p.pulses_dropped == 6 - MIDI_OUT_PO_MAX_PENDING);
}

static void test_realtime_jumps_queue(void)
{
	midi_out_t m;
	midi_out_init(&m, 0, 5, 1);
	midi_out_send(&m, 0x90); midi_out_send(&m, 0x3C); midi_out_send(&m, 0x7F);
	midi_out_tick(&m);              /* 0x90 now on the wire (not interrupted) */
	midi_out_send(&m, 0xF8);
	memset(&rx, 0, sizeof(rx));
	rx_feed(&rx, 0, 0);             /* start bit of 0x90 already sent */
	for (uint32_t t = 1; t < 60; t++) rx_feed(&rx, midi_out_tick(&m).midi, t);
	CHECK(rx.n == 4);
	CHECK(rx.bytes[0] == 0x90 && rx.bytes[1] == 0xF8 && rx.bytes[2] == 0x3C && rx.bytes[3] == 0x7F);
}

/* 24 PPQN at 300 BPM = 120 clocks/s; one per 31250/120 = 260.4 ticks. */
static void test_300bpm_no_overrun(void)
{
	midi_out_t m;
	midi_out_init(&m, 1, 5, 1);
	uint32_t max_pending = 0, sent = 0;
	double next = 0.0;
	const double period = (double)MIDI_OUT_TICKS_PER_S / 120.0;
	for (uint32_t t = 0; t < 10u * MIDI_OUT_TICKS_PER_S; t++) {
		if ((double)t >= next) { midi_out_send(&m, 0xF8); sent++; next += period; }
		uint32_t p = midi_out_pending(&m) + (uint32_t)midi_out_busy(&m);
		if (p > max_pending) max_pending = p;
		midi_out_tick(&m);
	}
	CHECK(sent == 1200);
	CHECK(m.bytes_dropped == 0);
	CHECK(max_pending <= 1);          /* line is idle before the next clock */
	CHECK(m.bytes_sent == 1200);
	/* line utilisation 120 * 320 us = 3.84 % */
	CHECK_NEAR(120.0 * 320e-6, 0.0384, 1e-9);
}

/* sync_out (48 kHz frame counted) -> midi_out over 60 s simulated. The
 * scheduler is advanced once per 1 ms (48 frames), like the bring-up app;
 * between advances, 31.25 bit ticks run (fractional carry). */
static void run_integration(float bpm, uint32_t *f8, uint32_t *pulses, uint32_t *ticks_ev,
			    uint32_t *po_ev)
{
	sync_out_t s;
	midi_out_t m;
	sync_out_init(&s, 48000.0f);
	sync_out_set_bpm(&s, bpm);
	midi_out_init(&m, 1, 5, 1);
	memset(&rx, 0, sizeof(rx));
	sync_out_start(&s);

	uint64_t bit_ticks = 0;
	uint32_t ev_ticks = 0, ev_po = 0, rising = 0;
	int prev_po = 0;
	for (uint32_t ms = 0; ms < 60000u; ms++) {
		sync_out_events_t ev = sync_out_advance(&s, 48);
		if (ms == 59999u) {
			sync_out_stop(&s);
			sync_out_events_t e2 = sync_out_advance(&s, 0);
			ev.stop = e2.stop;
		}
		if (ev.start) midi_out_send(&m, MIDI_START);
		for (uint32_t i = 0; i < ev.midi_ticks; i++) midi_out_send(&m, MIDI_CLOCK);
		for (uint32_t i = 0; i < ev.po_pulses; i++) midi_out_po_pulse(&m);
		if (ev.stop) midi_out_send(&m, MIDI_STOP);
		ev_ticks += ev.midi_ticks;
		ev_po += ev.po_pulses;
		uint64_t until = ((uint64_t)(ms + 1u) * 1000u) / MIDI_OUT_BIT_US;
		for (; bit_ticks < until; bit_ticks++) {
			midi_out_levels_t l = midi_out_tick(&m);
			rx_feed(&rx, !l.midi, (uint32_t)bit_ticks);   /* undo the PNP inversion */
			if (l.po && !prev_po) rising++;
			prev_po = l.po;
		}
	}
	for (int i = 0; i < 400; i++) {   /* drain */
		midi_out_levels_t l = midi_out_tick(&m);
		rx_feed(&rx, !l.midi, (uint32_t)bit_ticks++);
		if (l.po && !prev_po) rising++;
		prev_po = l.po;
	}
	uint32_t n_f8 = 0;
	for (uint32_t i = 0; i < rx.n; i++) n_f8 += rx.bytes[i] == 0xF8;
	*f8 = n_f8;
	*pulses = rising;
	*ticks_ev = ev_ticks;
	*po_ev = ev_po;
	CHECK(rx.framing_errors == 0);
	CHECK(m.bytes_dropped == 0 && m.pulses_dropped == 0);
	CHECK(rx.n >= 2 && rx.bytes[0] == 0xFA);           /* Start precedes the first clock */
	CHECK(rx.bytes[rx.n - 1] == 0xFC);                  /* Stop is last */
	CHECK(rx.n == n_f8 + 2);
}

static void test_integration_60s(void)
{
	uint32_t f8, pulses, te, pe;
	run_integration(120.0f, &f8, &pulses, &te, &pe);
	/* 120 BPM * 24 PPQN = 48/s -> 2880 in 60 s (+1: the tick at t=0) */
	CHECK_MSG(f8 == 2880u || f8 == 2881u, "f8=%u", f8);
	CHECK(f8 == te);
	/* 2 PPQN = 4 pulses/s -> 240 (+1) */
	CHECK_MSG(pulses == 240u || pulses == 241u, "pulses=%u", pulses);
	CHECK(pulses == pe);

	run_integration(300.0f, &f8, &pulses, &te, &pe);
	CHECK_MSG(f8 == 7200u || f8 == 7201u, "f8=%u", f8);
	CHECK(f8 == te);
	CHECK_MSG(pulses == 600u || pulses == 601u, "pulses=%u", pulses);
}

int main(void)
{
	RUN(test_bit_pattern);
	RUN(test_inversion);
	RUN(test_timing_arith);
	RUN(test_queue_overflow);
	RUN(test_realtime_jumps_queue);
	RUN(test_300bpm_no_overrun);
	RUN(test_integration_60s);
	T_DONE();
}
