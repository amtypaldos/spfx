/* SP-1 FX — UI controller: maps gestures and faders onto the engine and renders
 * the monochrome LEDs. Hardware-independent; tested on the host. */
#ifndef SP1FX_CONTROLLER_H
#define SP1FX_CONTROLLER_H

#include "controls.h"
#include "../dsp/engine.h"

typedef enum { UI_PERFORM = 0, UI_EDIT = 1 } ui_mode;

/* 8 single-colour LEDs, 0..255 brightness each. */
typedef struct {
	uint8_t track[4]; /* LED next to each fader */
	uint8_t play[4];  /* four LEDs between PLAY and FUNCTION */
} led_frame_t;

/* Device-originated change events (additive API, consumed by the companion protocol).
 * Filled wherever the controller changes state on its own; host-originated changes made
 * directly through engine_* never appear here. */
typedef enum {
	CTL_EV_WET = 1,           /* slot, v */
	CTL_EV_PARAM,             /* slot, a = param idx, v */
	CTL_EV_BYPASS,            /* slot, a = on */
	CTL_EV_SLOT_FX,           /* slot, a = fx id */
	CTL_EV_GLOBAL_BYPASS,     /* a = on */
	CTL_EV_MASTER,            /* v */
	CTL_EV_ADMISSION          /* slot, a = rejected fx id, b = engine_status */
} ctl_event_kind;

typedef struct {
	uint8_t kind, slot, a, b;
	float v;
} ctl_event_t;

#define CTL_EVENT_CAP 32 /* ring; when full the oldest event is dropped and ev_dropped counts it */

typedef struct {
	engine_t *eng;
	debounce_t deb;
	gesture_t gest;
	takeover_t to[4];
	ui_mode mode;
	int edit_slot;

	int held_slot;       /* channel currently held, -1 if none */
	uint32_t held_since;
	int cursor[4];       /* effect index being browsed per slot while a channel is held */
	int selecting;       /* rocker used during current hold */
	uint32_t err_until;  /* admission-error LED pattern end time */
	uint32_t flash_until;/* effect-change confirmation flash */
	int last_master_step;

	int power_off_request;
	uint32_t change_seq; /* increments on any user-visible state change (companion / persistence) */
	uint32_t last_change_ms;

	ctl_event_t ev[CTL_EVENT_CAP];
	uint8_t ev_head, ev_count;
	uint32_t ev_dropped;
} controller_t;

void controller_init(controller_t *c, engine_t *e);
/* Call every ~8 ms. buttons = raw bitmask, fader[i] in 0..1. */
void controller_tick(controller_t *c, uint32_t now_ms, uint16_t buttons, const float fader[4]);
/* Re-arm soft takeover after the state was changed from elsewhere (companion, preset load). */
void controller_resync(controller_t *c);
/* Pop the oldest device-originated event; returns 0 if none. */
int controller_pop_event(controller_t *c, ctl_event_t *out);
void controller_leds(const controller_t *c, uint32_t now_ms, led_frame_t *out);

#define MASTER_STEPS 16

#endif
