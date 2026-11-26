/* SP-1 FX — physical controls: debounce, gesture state machine, soft takeover.
 * Pure C, no hardware access. The driver layer feeds in a button bitmask and
 * normalised fader positions every ~8 ms and passes a millisecond clock. */
#ifndef SP1FX_CONTROLS_H
#define SP1FX_CONTROLS_H

#include <stdint.h>

enum {
	BTN_T1 = 0, BTN_T2, BTN_T3, BTN_T4,
	BTN_PLAY, BTN_FN, BTN_VOLUP, BTN_VOLDN, BTN_FWD, BTN_REV,
	BTN_COUNT
};
#define BTN_BIT(b) ((uint16_t)(1u << (b)))

/* ---- debounce ---------------------------------------------------------- */
typedef struct {
	uint16_t stable;
	uint8_t n_required;
	uint8_t cnt[BTN_COUNT];
} debounce_t;

void debounce_init(debounce_t *d, uint8_t n_required);
uint16_t debounce_update(debounce_t *d, uint16_t raw);

/* ---- gestures ---------------------------------------------------------- */
typedef enum {
	GE_NONE = 0,
	GE_TOGGLE_BYPASS, /* arg = slot */
	GE_CYCLE_FX,      /* arg = slot, arg2 = +1/-1 */
	GE_ENTER_EDIT,    /* arg = slot (FN + channel) */
	GE_FN_TAP,        /* FN pressed and released alone */
	GE_GLOBAL_BYPASS, /* PLAY tap */
	GE_VOL,           /* arg = +1/-1 (with auto-repeat) */
	GE_POWER_OFF,     /* FN held alone GESTURE_FN_LONG_MS: return to bootloader */
	GE_CHAN_DOWN,     /* arg = slot: a channel button went down (for UI) */
	GE_CHAN_UP,       /* arg = slot */
	GE_CYCLE_PRESET,  /* arg = +1/-1: FN + rocker (repeats while held) */
	GE_OUT_LEVEL      /* VOL+ and VOL- held together GESTURE_VOL_BOTH_MS: next headphone-jack level */
} gesture_type;

typedef struct {
	uint8_t type;
	int8_t arg, arg2;
} gesture_event_t;

#define GESTURE_TAP_MAX_MS 600u
/* FN held alone this long = power off (owner decision for rc3: 4 s; 1.5 s was hit by accident
 * while cycling presets with FN + rocker). Any other button pressed during the same FN press
 * cancels it for that press. The controller shows an 8-LED fill, one LED per 0.5 s. */
#define GESTURE_FN_LONG_MS 4000u
#define GESTURE_FN_STEP_MS 500u
#define GESTURE_ROCKER_DELAY_MS 450u
#define GESTURE_ROCKER_REPEAT_MS 200u
#define GESTURE_VOL_DELAY_MS 400u
#define GESTURE_VOL_REPEAT_MS 90u
/* VOL+ and VOL- held together this long = next output level (owner request, 2026.11.25.2).
 * While both are down the volume does not repeat; one event per hold. */
#define GESTURE_VOL_BOTH_MS 3000u
#define GESTURE_MAX_EVENTS 8

typedef struct {
	uint16_t prev;
	uint8_t chan_down[4], chan_consumed[4];
	uint32_t chan_t[4];
	int8_t last_chan;
	uint8_t fn_down, fn_consumed, fn_long_sent;
	uint32_t fn_t;
	uint8_t play_down, play_consumed;
	uint32_t play_t;
	int8_t rk_dir, rk_slot;
	uint32_t rk_next;
	int8_t vol_dir;
	uint32_t vol_next;
	uint8_t vol_both, vol_both_sent;
	uint32_t vol_both_t;
} gesture_t;

void gesture_init(gesture_t *g);
/* Returns number of events written to ev (max GESTURE_MAX_EVENTS). */
int gesture_update(gesture_t *g, uint16_t mask, uint32_t now_ms, gesture_event_t *ev);

/* ---- soft takeover ------------------------------------------------------ */
#define TAKEOVER_WINDOW 0.025f   /* close enough to pick up */
#define TAKEOVER_DEADBAND 0.004f /* ignore ADC jitter once picked up */

typedef struct {
	float value;      /* logical value (what the DSP uses) */
	float last_phys;
	float prev_diff;
	int8_t picked;
	int8_t have_prev;
	int8_t need;      /* +1: move the fader up, -1: move it down, 0: picked up */
} takeover_t;

void takeover_arm(takeover_t *t, float logical_value);
/* Returns 1 if the logical value changed (written to *value_out). */
int takeover_update(takeover_t *t, float phys, float *value_out);

#endif
