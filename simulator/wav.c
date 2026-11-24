#include "wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

void wav_free(wav_audio *a)
{
	free(a->l);
	free(a->r);
	a->l = a->r = 0;
	a->frames = 0;
}

int wav_read(const char *path, wav_audio *out)
{
	memset(out, 0, sizeof *out);
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 44 || sz > 512L * 1024 * 1024) { fclose(f); return -2; }
	uint8_t *b = malloc((size_t)sz);
	if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return -3; }
	fclose(f);
	if (memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) { free(b); return -4; }

	int fmt = 0, ch = 0, bits = 0;
	uint32_t sr = 0, data_off = 0, data_len = 0;
	long p = 12;
	while (p + 8 <= sz) {
		uint32_t cl = rd32(b + p + 4);
		if (!memcmp(b + p, "fmt ", 4) && p + 8 + 16 <= sz) {
			fmt = rd16(b + p + 8);
			ch = rd16(b + p + 10);
			sr = rd32(b + p + 12);
			bits = rd16(b + p + 22);
			if (fmt == 0xFFFE && cl >= 26) fmt = rd16(b + p + 8 + 24); /* extensible */
		} else if (!memcmp(b + p, "data", 4)) {
			data_off = (uint32_t)p + 8;
			data_len = cl;
			if ((long)data_off + data_len > sz) data_len = (uint32_t)(sz - data_off);
			break;
		}
		p += 8 + (long)cl + (cl & 1);
	}
	int ok_fmt = (fmt == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32);
	if (!data_off || (ch != 1 && ch != 2) || !ok_fmt) { free(b); return -5; }

	uint32_t bps = (uint32_t)bits / 8;
	uint32_t frames = data_len / (bps * (uint32_t)ch);
	out->l = malloc(sizeof(float) * (frames ? frames : 1));
	out->r = malloc(sizeof(float) * (frames ? frames : 1));
	if (!out->l || !out->r) { free(b); wav_free(out); return -6; }
	const uint8_t *d = b + data_off;
	for (uint32_t i = 0; i < frames; i++) {
		float v[2] = {0, 0};
		for (int c = 0; c < ch; c++) {
			const uint8_t *s = d + ((size_t)i * (size_t)ch + (size_t)c) * bps;
			float x;
			if (fmt == 3) { uint32_t u = rd32(s); memcpy(&x, &u, 4); }
			else if (bits == 16) x = (float)(int16_t)rd16(s) / 32768.0f;
			else if (bits == 24) { int32_t u = (int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24); x = (float)(u >> 8) / 8388608.0f; }
			else x = (float)((int32_t)rd32(s)) / 2147483648.0f;
			v[c] = x;
		}
		out->l[i] = v[0];
		out->r[i] = ch == 2 ? v[1] : v[0];
	}
	out->frames = frames;
	out->sample_rate = sr;
	free(b);
	return 0;
}

static void w32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void w16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

int wav_write(const char *path, const wav_audio *a, int bits)
{
	if (bits != 16 && bits != 24 && bits != 32) return -1;
	uint32_t bps = (uint32_t)bits / 8, dl = a->frames * 2 * bps;
	uint8_t h[44];
	memcpy(h, "RIFF", 4); w32(h + 4, 36 + dl); memcpy(h + 8, "WAVEfmt ", 8);
	w32(h + 16, 16); w16(h + 20, bits == 32 ? 3 : 1); w16(h + 22, 2);
	w32(h + 24, a->sample_rate); w32(h + 28, a->sample_rate * 2 * bps);
	w16(h + 32, (uint16_t)(2 * bps)); w16(h + 34, (uint16_t)bits);
	memcpy(h + 36, "data", 4); w32(h + 40, dl);
	FILE *f = fopen(path, "wb");
	if (!f) return -2;
	fwrite(h, 1, 44, f);
	for (uint32_t i = 0; i < a->frames; i++) {
		for (int c = 0; c < 2; c++) {
			float x = c ? a->r[i] : a->l[i];
			uint8_t o[4];
			if (bits == 32) { memcpy(o, &x, 4); }
			else {
				if (x > 1.0f) x = 1.0f;
				if (x < -1.0f) x = -1.0f;
				if (bits == 16) w16(o, (uint16_t)(int16_t)(x * 32767.0f + (x >= 0 ? 0.5f : -0.5f)));
				else { int32_t v = (int32_t)(x * 8388607.0f + (x >= 0 ? 0.5f : -0.5f)); o[0] = v; o[1] = v >> 8; o[2] = v >> 16; }
			}
			fwrite(o, 1, bps, f);
		}
	}
	fclose(f);
	return 0;
}
