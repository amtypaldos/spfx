/* SP-1 FX — master volume steps (VOL+ / VOL-) and the dB mapping. Pure C, host-tested.
 *
 * The engine's master value m (0..1, engine_config_t.master, protocol u16n) maps to
 *     engine gain = -60 dB + 60 dB * m        (m < 0.005: muted; dsp/engine.c master_gain)
 * The buttons move m on a grid of VOL_STEPS steps, i.e. VOL_DB_PER_STEP = 2 dB per press
 * (holding repeats: gestures GESTURE_VOL_DELAY_MS, then every GESTURE_VOL_REPEAT_MS).
 *
 * After the engine (whose soft limiter keeps |x| <= 1.0) the firmware applies a fixed output
 * trim (zephyr_sp1fx: CONFIG_SP1FX_OUT_TRIM_DB, -6 dB), so the loudest setting (m = 1) puts
 * at most -6 dBFS into the codecs. The codecs then keep their own hearing-safety settings
 * (CS42L42 headphone mixer -30 dB, TAS2505 speaker +6 dB gain / -6 dB attenuation;
 * firmware/app/zephyr_tone/src/sp1_codecs.c). See docs/BRINGUP_SP1FX.md for the table. */
#ifndef SP1FX_VOLUME_H
#define SP1FX_VOLUME_H

#define VOL_STEPS 30            /* grid over 0..1 */
#define VOL_DB_RANGE 60.0f      /* engine master: -60 dB .. 0 dB */
#define VOL_DB_PER_STEP 2.0f    /* VOL_DB_RANGE / VOL_STEPS */
#define VOL_MUTE_BELOW 0.005f   /* engine: master below this is silence */

/* One button step from master value m (dir +1 / -1): snapped to the grid, clamped to 0..1.
 * A value between grid points first moves to the next grid point in that direction. */
float vol_step(float m, int dir);

/* Engine master gain in dB for master value m (-INFINITY-like -200 for muted). */
float vol_master_db(float m);

/* Level at the codec input in dBFS for a full-scale signal: vol_master_db(m) + trim_db. */
float vol_out_dbfs(float m, float trim_db);

/* ---- headphone-jack output level (rc4 line out, rc5 three levels) ----
 * Ordered by loudness; the protocol value is the mode number (SET_OUTPUT_MODE).
 *   Headphones (0, default, EVERY start): CS42L42 mixer -30 dB, trim -6 dB (as all images).
 *   Loud       (2): mixer -15 dB, trim -6 dB: +15 dB over Headphones.
 *   Line out   (1): mixer 0 dB, trim 0 dB: +36 dB (the level Tape Looper ships with).
 * CALCULATED jack level for a 0 dBFS signal at master 100 %, from Cirrus' "35 mW into 30 ohm"
 * (~1.02 Vrms full scale): Headphones ~16 mVrms, Loud ~91 mVrms, Line out ~1.0 Vrms (+2 dBu).
 * Not measured. The speaker is never affected. */
#define OUT_MODE_HP 0
#define OUT_MODE_LINE 1
#define OUT_MODE_LOUD 2
#define OUT_MODE_COUNT 3
#define OUT_HP_MIX_ATT_DB 30
#define OUT_HP_TRIM_DB (-6)
#define OUT_LOUD_MIX_ATT_DB 15
#define OUT_LOUD_TRIM_DB (-6)
#define OUT_LINE_MIX_ATT_DB 0
#define OUT_LINE_TRIM_DB 0
#define OUT_SPK_TRIM_DB (-6)
typedef struct {
	int mix_att_db;  /* CS42L42 mixer attenuation (headphone jack only) */
	int trim_db;     /* digital trim after the engine limiter */
} out_levels_t;
out_levels_t vol_out_levels(int mode, int on_headphone_jack);
/* dB a mode is louder than Headphones at the jack (0, 15, 36) */
int vol_mode_extra_db(int mode);
int vol_line_extra_db(void);   /* = vol_mode_extra_db(OUT_MODE_LINE) */
/* Master value after a mode switch so the jack level never jumps up: to a louder mode the
 * master is lowered by the difference (rounded down to the VOL grid, clamped at mute); to a
 * quieter mode it is kept. */
float vol_master_for_mode_switch(float m, int from_mode, int to_mode);

#endif
