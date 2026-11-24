/* SP-1 FX -- register-level SAADC scanner for the controls (zephyr_sp1fx). NOT TESTED ON
 * HARDWARE.
 *
 * Why not Zephyr's ADC driver: on hardware, rc1's blocking adc_read() never returned at
 * random moments (watchdog resets with every thread blocked) and rc2's async read had one
 * scan that never completed in every boot (adc_to=1, controls frozen). The SP-1-dev wiki notes
 * that the TE bootloader starts the SAADC before handing over; nothing in our images cleaned
 * that up. This driver owns the SAADC alone (the devicetree adc node is disabled), resets it
 * at init (STOP, disable, events and interrupts cleared, any PPI channel aimed at a SAADC task
 * disconnected), never uses interrupts and never waits: a scan is started in one control tick
 * and collected in a later one (app_logic.h scanfsm_t); a scan that does not end within
 * SCAN_TIMEOUT_MS is stopped and restarted.
 *
 * Channel setup is the one the hardware-verified controls image used through Zephyr (gain 1/6,
 * internal 0.6 V reference, 20 us acquisition, 12-bit, single-ended), so the ladder thresholds
 * are unchanged. Result order = channel order:
 *   0 AIN0 P0.02 ladder 1 (T1-T4, PLAY)   1 AIN1 P0.03 ladder 2 (VOL, FWD/REV)
 *   2 AIN3 P0.05 fader 1   3 AIN6 P0.30 fader 2   4 AIN2 P0.04 fader 3   5 AIN7 P0.31 fader 4
 *   6 AIN4 P0.28 battery voltage divider (SP-1-dev wiki "Battery charger"; Tape Looper reads
 *     it the same way)
 * SPDX-License-Identifier: MIT */
#ifndef SP1FX_SAADC_SCAN_H
#define SP1FX_SAADC_SCAN_H

#include <stdint.h>

#define SAADC_SCAN_CH 7

typedef struct {
	uint32_t ok, timeouts, start_fail, stop_fail, ppi_cleared, was_enabled;
	uint32_t age_ms;     /* since the last good scan (0xFFFFFFFF none yet) */
} saadc_scan_stats_t;

void saadc_scan_init(void);
/* Call every control tick. Returns 1 when out[] holds a new result (two scans averaged),
 * 0 when not (out[] untouched). Never blocks for more than a few microseconds. */
int saadc_scan_poll(uint32_t now_ms, int16_t out[SAADC_SCAN_CH]);
void saadc_scan_stats(saadc_scan_stats_t *s, uint32_t now_ms);
/* stop and disable (before SYSTEM_OFF) */
void saadc_scan_off(void);

#endif
