/* Minimal WAV reader/writer for the simulator: PCM 16/24/32 and float32 in,
 * PCM16/24 or float32 out. Mono input is duplicated to stereo. */
#ifndef SP1FX_WAV_H
#define SP1FX_WAV_H

#include <stdint.h>

typedef struct {
	float *l, *r;
	uint32_t frames;
	uint32_t sample_rate;
} wav_audio;

int wav_read(const char *path, wav_audio *out);            /* 0 ok */
int wav_write(const char *path, const wav_audio *a, int bits); /* bits: 16, 24 or 32 (float) */
void wav_free(wav_audio *a);

#endif
