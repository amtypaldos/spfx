/* SP-1 FX — sync jack output scheduling (pure logic).
 *
 * The SP-1 sync jack is digital: TIP = Pocket Operator/Volca-style pulse,
 * RING = MIDI (31250 baud, via an inverting transistor). It cannot carry audio.
 * This module only decides WHEN ticks happen, counted in audio frames so the
 * clock is locked to the audio clock and never drifts against it. The driver
 * turns ticks into MIDI bytes / PO pulses. */
#ifndef SP1FX_SYNC_OUT_H
#define SP1FX_SYNC_OUT_H

#include <stdint.h>

#define SYNC_PPQN 24
#define SYNC_PO_DIV 12 /* 24 PPQN / 12 = 2 PPQN pulses (1/8 notes) */

typedef struct {
	double frames_per_tick;
	double acc;        /* frames until next tick */
	uint8_t running;
	uint8_t send_start, send_stop;
	uint32_t tick_count;
	float sr;
} sync_out_t;

typedef struct {
	uint32_t midi_ticks;  /* 0xF8 clocks to emit */
	uint32_t po_pulses;   /* PO sync pulses to emit */
	uint8_t start, stop;  /* 0xFA / 0xFC */
} sync_out_events_t;

void sync_out_init(sync_out_t *s, float sr);
void sync_out_set_bpm(sync_out_t *s, float bpm); /* clamped to 30..300 */
void sync_out_start(sync_out_t *s);
void sync_out_stop(sync_out_t *s);
/* Advance by n audio frames; returns events due in that span. */
sync_out_events_t sync_out_advance(sync_out_t *s, uint32_t n);

#endif
