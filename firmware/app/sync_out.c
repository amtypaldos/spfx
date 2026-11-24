#include "sync_out.h"

void sync_out_init(sync_out_t *s, float sr)
{
	s->sr = sr;
	s->running = 0;
	s->send_start = s->send_stop = 0;
	s->tick_count = 0;
	s->acc = 0.0;
	sync_out_set_bpm(s, 120.0f);
}

void sync_out_set_bpm(sync_out_t *s, float bpm)
{
	if (!(bpm == bpm)) return;
	if (bpm < 30.0f) bpm = 30.0f;
	if (bpm > 300.0f) bpm = 300.0f;
	s->frames_per_tick = (double)s->sr * 60.0 / ((double)bpm * SYNC_PPQN);
}

void sync_out_start(sync_out_t *s)
{
	if (s->running) return;
	s->running = 1;
	s->send_start = 1;
	s->tick_count = 0;
	s->acc = 0.0; /* first tick is due immediately after Start */
}

void sync_out_stop(sync_out_t *s)
{
	if (!s->running) return;
	s->running = 0;
	s->send_stop = 1;
}

sync_out_events_t sync_out_advance(sync_out_t *s, uint32_t n)
{
	sync_out_events_t ev = {0, 0, 0, 0};
	if (s->send_start) { ev.start = 1; s->send_start = 0; }
	if (s->running) {
		double remaining = (double)n;
		while (s->acc <= remaining) {
			remaining -= s->acc;
			s->acc = s->frames_per_tick;
			ev.midi_ticks++;
			if (s->tick_count % SYNC_PO_DIV == 0) ev.po_pulses++;
			s->tick_count++;
		}
		s->acc -= remaining;
	}
	if (s->send_stop) { ev.stop = 1; s->send_stop = 0; }
	return ev;
}
