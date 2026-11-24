/* SP-1 FX -- resistor-ladder button decode and fader normalisation.
 *
 * Pure C, no hardware access: the Zephyr driver (controls_hw.c) hands in raw
 * 12-bit SAADC codes, this module turns them into the button bitmask of
 * firmware/controls/controls.h (BTN_T1..BTN_REV) and into 0..1 fader values.
 * Host-tested in tests/test_ladder.c.
 *
 * ---- threshold source (source finding, NOT verified on our firmware) ----
 * All raw thresholds/nominals below come from chattock/sp1-tape-looper
 * @0359372e (MIT), tools/capacity422/runtime/src/main.c: decode_tracks(),
 * decode_vol(), lad_press() (PLAY + track nominals "W113"), and the fader
 * scaling `fv * 256 / 3700`. They are raw codes under the board ADC setup
 * (gain 1/6, internal 0.6 V reference, 20 us acquisition, 12-bit; full scale
 * 3.6 V = 4095). Tape Looper calibrated them on real SP-1 units; this firmware
 * has never run on hardware. To recalibrate, read the raw values printed on
 * the USB console (docs/BRINGUP_CONTROLS.md) and edit the LADDER*_ macros
 * here (or pass your own ladder_cfg_t to ladder_init()).
 *
 * ---- what the hardware can / cannot distinguish (source finding) ----
 * Each ladder is ONE analog value, so only one "state" per ladder is
 * readable at a time:
 *   - Ladder 1 (AIN0): T1 ~213, T2 ~403, T3 ~733, T4 ~1220, PLAY ~1807-1823.
 *     PLAY + Tn chords read slightly above bare PLAY (1861/1910/2004/2159),
 *     but bare PLAY on a unit reading ~2-3 % high lands on the PLAY + T1
 *     level (Tape Looper bug LADREL-937); Tape Looper needed a per-press
 *     gain-learning scheme to tell them apart. Default here: everything at or
 *     above the PLAY threshold is PLAY alone (LADDER1_DECODE_PLAY_CHORDS 0),
 *     because the controller gives PLAY + channel no function and a
 *     mis-decode the other way would fire a spurious channel tap.
 *     Two TRACK buttons together have no measured level in Tape Looper; they
 *     will read as some other single band (typically a higher track or
 *     PLAY). The controller's "two channels = reserved chord" logic
 *     therefore cannot be reached reliably on this hardware.
 *   - Ladder 2 (AIN1): REV ~404, VOL- ~729, FWD ~1220, VOL+ ~1820, and
 *     VOL+ and VOL- together ~1998 (measured, Tape Looper "W115") which is
 *     decoded to BTN_VOLUP|BTN_VOLDN. A rocker + volume chord is on the same
 *     ladder and is NOT decodable.
 *   - FUNCTION is its own GPIO, so FN + anything works.
 *   - Channel + rocker and channel + volume are on DIFFERENT ladders, so they
 *     decode independently and work (channel+rocker = effect cycle).
 *
 * ---- noise handling ----
 * Each ladder: band hysteresis of `hyst` counts around every threshold, then
 * a new state must be seen on `stable_n` consecutive reads before it is
 * reported (rejects the transit through lower bands while a contact settles;
 * Tape Looper uses an equivalent sticky 3-sample commit). The controller adds
 * its own 2-tick debounce on top. An ADC error (raw < 0) holds the last state.
 */
#ifndef SP1FX_LADDER_H
#define SP1FX_LADDER_H

#include <stdint.h>

/* ---- ladder 1: T1..T4 + PLAY (raw codes, Tape Looper decode_tracks) ---- */
#define LADDER1_IDLE_MAX      110   /* below: nothing pressed */
#define LADDER1_T1_MAX        300   /* T1 ~213 */
#define LADDER1_T2_MAX        560   /* T2 ~403 */
#define LADDER1_T3_MAX        950   /* T3 ~733 */
#define LADDER1_T4_MAX       1500   /* T4 ~1220; at/above: PLAY ~1807-1823 */
/* Optional PLAY + Tn chord split (off by default, see above). Midpoints of
 * Tape Looper's nominals PLAY 1807 | +T1 1861 | +T2 1910 | +T3 2004 | +T4 2159. */
#ifndef LADDER1_DECODE_PLAY_CHORDS
#define LADDER1_DECODE_PLAY_CHORDS 0
#endif
#define LADDER1_PLAY_MAX     1834
#define LADDER1_PT1_MAX      1885
#define LADDER1_PT2_MAX      1957
#define LADDER1_PT3_MAX      2081

/* ---- ladder 2: volume + rocker (raw codes, Tape Looper decode_vol) ---- */
#define LADDER2_IDLE_MAX      200   /* below: nothing pressed */
#define LADDER2_REV_MAX       560   /* REV  (VOL_TEMPO_DOWN) ~404 */
#define LADDER2_VOLDN_MAX     950   /* VOL- ~729 */
#define LADDER2_FWD_MAX      1500   /* FWD  (VOL_TEMPO_UP) ~1220 */
#define LADDER2_VOLUP_MAX    1910   /* VOL+ ~1820; at/above: VOL+ and VOL- ~1998 */

#define LADDER_HYST_DEFAULT     20  /* counts (~0.5 % of full scale) */
#define LADDER_STABLE_DEFAULT    2  /* consecutive reads (2 x 8 ms) */

/* ---- faders (Tape Looper: q8 = raw * 256 / 3700, clamped) ---- */
#define FADER_RAW_LO_DEFAULT     0  /* raw code at the bottom end stop */
#define FADER_RAW_HI_DEFAULT  3700  /* raw code at the top end stop */
#define FADER_SNAP_DEFAULT  0.01f   /* within 1 % of an end -> exactly 0 / 1 */
#define FADER_ALPHA_DEFAULT 0.5f    /* one-pole low-pass per 8 ms read */
#define FADER_HYST_DEFAULT  6.0f    /* raw counts the filtered value must move */

#define LADDER_MAX_BANDS 10

typedef struct {
	int nbands;                           /* thresholds used */
	int16_t max[LADDER_MAX_BANDS];        /* band i: raw < max[i] (ascending); last band open-ended */
	uint16_t mask[LADDER_MAX_BANDS + 1];  /* mask of band i; mask[nbands] = top band */
	int16_t hyst;
	uint8_t stable_n;
} ladder_cfg_t;

typedef struct {
	ladder_cfg_t cfg;
	int8_t band;       /* committed band (0 = idle) */
	int8_t cand;       /* candidate band */
	uint8_t cand_n;    /* consecutive reads of cand */
} ladder_t;

/* Default configurations from the thresholds above. */
/* play_chords: 0 = PLAY + Tn reads as PLAY (default, LADDER1_DECODE_PLAY_CHORDS);
 * 1 = split PLAY + Tn by the LADDER1_P*_MAX midpoints (fragile, see above). */
void ladder_cfg_ladder1(ladder_cfg_t *c, int play_chords);
void ladder_cfg_ladder2(ladder_cfg_t *c);

void ladder_init(ladder_t *l, const ladder_cfg_t *cfg);
/* Stateless decode of one raw value (no hysteresis, no stability). */
uint16_t ladder_decode_raw(const ladder_cfg_t *cfg, int raw);
/* Feed one raw reading (raw < 0 = ADC error: hold). Returns the committed mask. */
uint16_t ladder_update(ladder_t *l, int raw);
uint16_t ladder_mask(const ladder_t *l);

/* ---- fader normaliser ---- */
typedef struct {
	float raw_lo, raw_hi;   /* end-stop calibration */
	float snap;             /* end snap zone (fraction) */
	float alpha;            /* low-pass coefficient 0..1 (1 = no filtering) */
	float hyst;             /* raw counts */
	float filt;             /* filtered raw */
	float held;             /* filtered raw last reported */
	float out;              /* 0..1 */
	int primed;
} fader_t;

void fader_init(fader_t *f, float raw_lo, float raw_hi);
/* Feed one raw reading (raw < 0 = ADC error: hold). Returns 0..1. */
float fader_update(fader_t *f, int raw);

/* Writes names of the set bits ("T1+PLAY", "-" if none) into buf. */
void ladder_mask_names(uint16_t mask, char *buf, int len);

#endif
