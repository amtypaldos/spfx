/*
 * SP-1 audio codecs (CS42L42 headphone codec, TAS2505 speaker amp) over I2C.
 * See sp1_codecs.c for provenance (adapted from chattock/sp1-tape-looper,
 * MIT, Copyright (c) 2026 chattock).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SP1_CODECS_H
#define SP1_CODECS_H

#include <stdbool.h>
#include <stdint.h>

#define CS42L42_ADDR 0x48u
#define TAS2505_ADDR 0x18u

bool sp1_i2c_ready(void);

/* CS42L42: reads DEVID_AB/CD/E (0x1001..0x1003) into devid[3].
 * Returns true if DEVID_AB == 0x42 (genuine CS42L42). */
bool cs42_probe(uint8_t devid[3]);
/* Full init (I2S frame master, 48 kHz from the 3.072 MHz SCLK). Leaves the
 * headphone output MUTED. Returns true if every register write ACKed. */
bool cs42_configure(void);
/* Headphone output on (unmute + low mixer volume) / off (mute). */
bool cs42_hp_enable(bool on);
/* 1 = headphones in the jack, 0 = not, -1 = I2C read failed. */
int cs42_hp_present(void);
/* PLL / clock status byte read during configure (diagnostics). */
uint8_t cs42_clk_status(void);

/* TAS2505: true if it ACKs a page-select write. */
bool tas_probe(void);
/* Full init (PLL locked to BCLK). Leaves the speaker MUTED. */
bool tas_configure(void);
/* Speaker on (DAC unmuted, minimum non-zero driver gain) / off (muted). */
bool tas_spk_enable(bool on);

/* Before SYSTEM_OFF: mute both outputs, soft-reset the TAS2505. */
void sp1_codecs_shutdown(void);

#endif
