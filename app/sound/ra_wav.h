/*
 * ra_wav.h -- PCM16 RIFF/WAVE decoding.
 */

#ifndef RA_WAV_H
#define RA_WAV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int16_t *samples;
  uint32_t frames;
  uint8_t channels;
  uint32_t sample_rate;
} RaWav;

/* Parse a PCM16 RIFF/WAVE buffer. Returns 1 on success. */
int ra_wav_parse(const uint8_t *data, size_t size, RaWav *wav);

/* Convert to interleaved stereo at 48 kHz. Returns 1 on success; caller owns
 * the returned buffer via free(). */
int ra_wav_to_stereo_48k(const RaWav *wav, int16_t **samples,
                         size_t *sample_count);

/* Playback helper: if the file is already 48 kHz stereo, transfers its buffer
 * to the caller without copying (wav->samples becomes NULL); otherwise falls
 * back to ra_wav_to_stereo_48k(). Caller owns *samples via free(). */
int ra_wav_take_pcm48k(RaWav *wav, int16_t **samples, size_t *sample_count);

void ra_wav_free(RaWav *wav);

#ifdef __cplusplus
}
#endif

#endif /* RA_WAV_H */
