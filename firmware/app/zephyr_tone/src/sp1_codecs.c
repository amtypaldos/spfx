/*
 * SP-1 audio codecs over I2C: CS42L42 headphone codec (0x48) and TAS2505
 * speaker amp (0x18).
 *
 * MIT License
 *
 * Copyright (c) 2026 chattock
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 *
 * PROVENANCE
 *   Adapted from chattock/sp1-tape-looper commit 0359372e11f61d74a85e2e4449178a5c62010664,
 *   tools/capacity422/runtime/src/main.c: tas_wr()/tas_page()/tas_set_speaker(),
 *   tas2505_configure(), tpw()/tpr() paged CS42L42 access, hp_detect_connected()
 *   and the HP_TIM_TEST branch of hp_codec_init() (which itself is Tim Knapen's
 *   SP-1-dev wiki I2C.md sequence, MIT, Copyright (c) 2026 Tim Knapen).
 *   Register addresses and values are kept as in Tape Looper EXCEPT the
 *   hearing-safety / pop changes marked "SP1FX:" below:
 *     - both outputs are configured MUTED and only unmuted later by the app,
 *       after the I2S stream runs and with the digital signal at zero,
 *     - CS42L42 mixer volume -30 dB instead of 0 dB,
 *     - TAS2505 class-D gain +6 dB (minimum non-mute step) instead of +18 dB
 *       and speaker attenuation -6 dB instead of 0 dB,
 *     - mute/unmute helpers for headphone-detect switching.
 *   Rewritten as small functions without Tape Looper's globals.
 */
#include "sp1_codecs.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>

static const struct device *const i2c_bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));

/* ---- volume choices (SP1FX: hearing safety) ----------------------------------
 * CS42L42 MIXER_CHA_VOL / MIXER_CHB_VOL (0x2301 / 0x2303), 6-bit attenuation.
 * GUESS from the Linux cs42l42 driver's TLV (from memory, not re-verified
 * against the datasheet): 0x00 = 0 dB (the loudest setting, which Tape Looper
 * and Tim's wiki use), 1 dB per step, 0x3F = mute.
 * We use 0x1E = -30 dB: 30 dB below the register maximum. With the -30 dBFS
 * tone the headphone signal is ~-60 dB below the codec's full-scale output
 * (roughly 1 Vrms class for this codec family) -> about 1 mVrms, i.e. well
 * under 0.1 uW into 16-32 ohm phones: quiet but audible next to the ear even
 * with sensitive in-ear monitors. If 0x3F is NOT mute on this part the
 * analog HP mute in HP_CTL still silences the output. */
#define CS42_HP_MIX_ATT   0x1Eu   /* -30 dB (register max loudness is 0x00) */
#define CS42_MIX_MUTE     0x3Fu
#define CS42_HP_CTL_MUTE  0x0Du   /* Tim's wiki "HP Control: mute all" */
#define CS42_HP_CTL_ON    0x01u   /* Tim's wiki "unmute headphones"    */

/* TAS2505 page 1 reg 0x30 (48) class-D driver gain, bits D6-D4:
 * 000 = mute, 001 = +6 dB ... 100 = +24 dB (Tape Looper comment; TI SLAU472C).
 * +6 dB is the smallest non-mute step. Tape Looper ships +18 dB.
 * Page 1 reg 0x2E (46) speaker volume: 0x00 = 0 dB, ~0.5 dB/step (GUESS from
 * SLAU472C table, not re-verified): 0x0C = -6 dB. Tape Looper uses 0x00. */
#define TAS_SPK_GAIN_ON   0x10u   /* +6 dB  */
#define TAS_SPK_GAIN_MUTE 0x00u
#define TAS_SPK_ATT       0x0Cu   /* -6 dB  */
/* Page 0 reg 0x40 (64) DAC mute: Tape Looper writes 0x04 ("not muted": left
 * channel unmuted, right muted). 0x0C sets both mute bits. */
#define TAS_DAC_UNMUTE    0x04u
#define TAS_DAC_MUTE      0x0Cu

bool sp1_i2c_ready(void)
{
	return device_is_ready(i2c_bus);
}

/* ------------------------------- TAS2505 --------------------------------- */

static int tas_wr(uint8_t reg, uint8_t val)
{
	return i2c_reg_write_byte(i2c_bus, TAS2505_ADDR, reg, val);
}

static int tas_page(uint8_t p)
{
	return tas_wr(0x00, p);
}

bool tas_probe(void)
{
	return tas_page(0x00) == 0;
}

/* TAS2505 speaker bring-up (Tape Looper tas2505_configure(), after TI
 * SLAU472C section 5.1). PLL_CLKIN = BCLK = the 3.072 MHz oscillator,
 * J = 32 -> 98.304 MHz, NDAC 2 x MDAC 8 x DOSR 128 -> DAC_FS 48 kHz.
 * BCLK must be running (oscillator on) for the PLL to lock. */
bool tas_configure(void)
{
	int rc = 0;

	rc |= tas_page(0x00);
	rc |= tas_wr(0x01, 0x01);          /* software reset */
	k_msleep(5);

	rc |= tas_page(0x01);
	rc |= tas_wr(0x02, 0x00);          /* LDO 1.8 V, level shifters up */

	rc |= tas_page(0x00);
	rc |= tas_wr(0x04, 0x07);          /* PLL_CLKIN = BCLK, CODEC_CLKIN = PLL */
	rc |= tas_wr(0x05, 0x91);          /* PLL powered, P=1, R=1 */
	rc |= tas_wr(0x06, 0x20);          /* J = 32 */
	rc |= tas_wr(0x07, 0x00);          /* D MSB */
	rc |= tas_wr(0x08, 0x00);          /* D LSB */
	k_msleep(15);                      /* PLL lock */
	rc |= tas_wr(0x0B, 0x82);          /* NDAC = 2, powered */
	rc |= tas_wr(0x0C, 0x88);          /* MDAC = 8, powered */
	rc |= tas_wr(0x0D, 0x00);          /* DOSR MSB */
	rc |= tas_wr(0x0E, 0x80);          /* DOSR = 128 */
	rc |= tas_wr(0x1B, 0x00);          /* I2S, 16-bit, BCLK/WCLK inputs */
	rc |= tas_wr(0x1C, 0x00);          /* data slot offset 0 */
	rc |= tas_wr(0x3C, 0x02);          /* PRB_P2 */

	/* SP1FX: DAC muted BEFORE it is powered (Tape Looper unmutes here). */
	rc |= tas_wr(0x40, TAS_DAC_MUTE);
	rc |= tas_wr(0x3F, 0x90);          /* DAC powered, left data -> DAC, soft-step */
	rc |= tas_wr(0x41, 0x00);          /* DAC digital gain 0 dB */

	rc |= tas_page(0x01);
	rc |= tas_wr(0x01, 0x10);          /* master analog reference on */
	rc |= tas_wr(0x0A, 0x00);          /* common mode 0.9 V */
	rc |= tas_wr(0x0C, 0x04);          /* DAC routed to the output mixer */
	rc |= tas_wr(0x16, 0x00);          /* (TAS HP) volume 0 dB */
	rc |= tas_wr(0x18, 0x00);          /* AINL volume / mixer */
	rc |= tas_wr(0x09, 0x20);          /* (TAS HP) driver power, as Tape Looper */
	rc |= tas_wr(0x10, 0x00);          /* (TAS HP) unmute 0 dB, as Tape Looper */
	/* SP1FX: speaker attenuation -6 dB and the driver gain MUTED before the
	 * driver is powered, so power-up cannot pop through an open gain stage. */
	rc |= tas_wr(0x2E, TAS_SPK_ATT);
	rc |= tas_wr(0x30, TAS_SPK_GAIN_MUTE);
	rc |= tas_wr(0x2D, 0x02);          /* speaker driver powered up */

	rc |= tas_page(0x00);
	k_msleep(10);
	return rc == 0;
}

/* Speaker on/off. Called only while the digital signal is ramped to zero.
 * Order: on = DAC unmute then driver gain; off = driver gain mute then DAC. */
bool tas_spk_enable(bool on)
{
	int rc = 0;

	if (on) {
		rc |= tas_page(0x00);
		rc |= tas_wr(0x40, TAS_DAC_UNMUTE);
		rc |= tas_page(0x01);
		rc |= tas_wr(0x30, TAS_SPK_GAIN_ON);
	} else {
		rc |= tas_page(0x01);
		rc |= tas_wr(0x30, TAS_SPK_GAIN_MUTE);
		rc |= tas_page(0x00);
		rc |= tas_wr(0x40, TAS_DAC_MUTE);
	}
	rc |= tas_page(0x00);
	return rc == 0;
}

/* ------------------------------- CS42L42 --------------------------------- */

static uint8_t cs42_clk;

/* paged write: page register 0x00 first, then the 8-bit offset */
static bool tpw(uint16_t reg, uint8_t val)
{
	uint8_t p[2] = { 0x00, (uint8_t)(reg >> 8) };
	uint8_t b[2] = { (uint8_t)reg, val };

	if (i2c_write(i2c_bus, p, 2, CS42L42_ADDR) != 0) {
		return false;
	}
	return i2c_write(i2c_bus, b, 2, CS42L42_ADDR) == 0;
}

/* paged read */
static bool tpr(uint16_t reg, uint8_t *val)
{
	uint8_t p[2] = { 0x00, (uint8_t)(reg >> 8) };
	uint8_t o = (uint8_t)reg;

	if (i2c_write(i2c_bus, p, 2, CS42L42_ADDR) != 0) {
		return false;
	}
	return i2c_write_read(i2c_bus, CS42L42_ADDR, &o, 1, val, 1) == 0;
}

bool cs42_probe(uint8_t devid[3])
{
	devid[0] = devid[1] = devid[2] = 0;
	(void)tpr(0x1001, &devid[0]);   /* DEVID_AB (0x42 = CS42L42) */
	(void)tpr(0x1002, &devid[1]);   /* DEVID_CD */
	(void)tpr(0x1003, &devid[2]);   /* DEVID_E  */
	return devid[0] == 0x42;
}

uint8_t cs42_clk_status(void)
{
	return cs42_clk;
}

/* Tim Knapen's SP-1-dev wiki sequence as used by Tape Looper (HP_TIM_TEST
 * branch of hp_codec_init()): CS42L42 is the I2S frame master, PLL from the
 * oscillator-driven 3.072 MHz SCLK, LRCK = 48 kHz (64 SCLK per frame).
 * Requires the oscillator on and the reset released (done by the caller). */
bool cs42_configure(void)
{
	bool ok = true;

	/* SP1FX: analog mute + mixer mute FIRST, before anything is powered. */
	ok &= tpw(0x2001, CS42_HP_CTL_MUTE);
	ok &= tpw(0x2301, CS42_MIX_MUTE);
	ok &= tpw(0x2303, CS42_MIX_MUTE);

	ok &= tpw(0x1508, 0x10);   /* PLL Control 3 */
	ok &= tpw(0x1504, 0x80);   /* PLL Division Fractional Byte 2 */
	ok &= tpw(0x1505, 0x3E);   /* PLL Division Integer */
	ok &= tpw(0x150A, 0x7D);   /* PLL Calibration Ratio */
	ok &= tpw(0x1009, 0x00);   /* MCLK Control */
	ok &= tpw(0x1201, 0x01);   /* MCLK Source Select */
	ok &= tpw(0x120A, 0x01);   /* Input ASRC Clock Select */
	ok &= tpw(0x120B, 0x01);   /* Output ASRC Clock Select */
	ok &= tpw(0x1501, 0x01);   /* PLL Control 1: start */
	ok &= tpw(0x1107, 0x01);   /* Oscillator Switch (SCLK present) */
	for (int t = 0; t < 10; t++) {   /* wait for "SCLK selected" (0x02) */
		k_msleep(1);
		if (tpr(0x1109, &cs42_clk) && cs42_clk == 0x02) {
			break;
		}
	}
	ok &= tpw(0x1007, 0x13);   /* Serial Port SRC Control */
	ok &= tpw(0x1203, 0x1F);   /* FSYNC Pulse Width Lower (64-SCLK frame) */
	ok &= tpw(0x1205, 0x3F);   /* FSYNC Period Lower */
	ok &= tpw(0x1207, 0x34);   /* ASP Clock Config: MASTER */
	ok &= tpw(0x1208, 0x1A);   /* ASP Frame Configuration */
	ok &= tpw(0x2A02, 0x02);   /* Channel 1: 24-bit */
	ok &= tpw(0x2A05, 0x42);   /* Channel 2: phase + 24-bit */
	ok &= tpw(0x2601, 0x4C);   /* SRC Input Sample Rate */
	ok &= tpw(0x2609, 0x4C);   /* SRC Output Sample Rate */
	ok &= tpw(0x2A01, 0x0C);   /* ASP Receive Enable */
	ok &= tpw(0x240E, 0x01);   /* Equalizer Input Mute Control */
	/* SP1FX: Tim/Tape Looper write 0x00 (0 dB) here; keep the mixer muted. */
	ok &= tpw(0x2301, CS42_MIX_MUTE);
	ok &= tpw(0x2303, CS42_MIX_MUTE);
	ok &= tpw(0x1101, 0x96);   /* power up the codec */
	k_msleep(10);              /* HP amp operational after 10 ms */
	ok &= tpw(0x1121, 0x41);   /* Headset switch control */
	ok &= tpw(0x1B74, 0x03);   /* Miscellaneous detect control */
	ok &= tpw(0x1129, 0x01);   /* Headset clamp disable */
	ok &= tpw(0x2001, CS42_HP_CTL_MUTE);   /* HP Control: mute all */
	ok &= tpw(0x1F06, 0x84);   /* DAC Control 2 */
	ok &= tpw(0x2301, CS42_MIX_MUTE);      /* SP1FX: muted (was 0x00) */
	ok &= tpw(0x2303, CS42_MIX_MUTE);
	ok &= tpw(0x1B73, 0xC2);   /* Tip Sense Control */
	ok &= tpw(0x1B75, 0x9F);   /* Mic detect control 1 */
	/* SP1FX: Tim/Tape Looper unmute here (0x2001 = 0x01); we stay muted until
	 * the app has the stream running and the digital level at zero. */
	return ok;
}

/* Called only while the digital signal is ramped to zero.
 * Order: on = mixer volume then analog unmute; off = analog mute then mixer. */
static uint8_t hp_att = CS42_HP_MIX_ATT;

bool cs42_hp_set_att(uint8_t att_db, bool hp_on)
{
	hp_att = att_db > 0x3Eu ? 0x3Eu : att_db;
	if (!hp_on) {
		return true;
	}
	bool ok = tpw(0x2301, hp_att);

	ok &= tpw(0x2303, hp_att);
	return ok;
}

bool cs42_hp_enable(bool on)
{
	bool ok = true;

	if (on) {
		ok &= tpw(0x2301, hp_att);
		ok &= tpw(0x2303, hp_att);
		ok &= tpw(0x2001, CS42_HP_CTL_ON);
	} else {
		ok &= tpw(0x2001, CS42_HP_CTL_MUTE);
		ok &= tpw(0x2301, CS42_MIX_MUTE);
		ok &= tpw(0x2303, CS42_MIX_MUTE);
	}
	return ok;
}

/* DET_STATUS1 (page 0x1B reg 0x77) bit 7, per Tim's wiki "request headphone
 * status" (datasheet 7.9.8). */
int cs42_hp_present(void)
{
	uint8_t st;

	if (!tpr(0x1B77, &st)) {
		return -1;
	}
	return (st >> 7) & 1;
}

void sp1_codecs_shutdown(void)
{
	(void)tas_page(0x01);
	(void)tas_wr(0x30, TAS_SPK_GAIN_MUTE);   /* class-D driver mute (as Tape Looper) */
	(void)tas_page(0x00);
	(void)tas_wr(0x40, TAS_DAC_MUTE);
	(void)tpw(0x2001, CS42_HP_CTL_MUTE);
	k_msleep(30);
	(void)tas_wr(0x01, 0x01);                /* TAS2505 software reset */
}
