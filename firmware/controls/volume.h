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

#endif
