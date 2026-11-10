/* PCM16 RIFF/WAVE parsing and 48 kHz stereo conversion. */

#include "ra_wav.h"

#include <stdlib.h>
#include <string.h>

static uint16_t read_u16(const uint8_t *p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int ra_wav_take_pcm48k(RaWav *wav, int16_t **samples, size_t *sample_count) {
  if (wav == NULL || samples == NULL || sample_count == NULL)
    return 0;
  if (wav->sample_rate == 48000u && wav->channels == 2 &&
      wav->samples != NULL && wav->frames > 0) {
    *samples = wav->samples;
    *sample_count = (size_t)wav->frames * 2u;
    wav->samples = NULL;
    wav->frames = 0;
    return 1;
  }
  return ra_wav_to_stereo_48k(wav, samples, sample_count);
}

void ra_wav_free(RaWav *wav) {
  if (wav == NULL)
    return;
  free(wav->samples);
  memset(wav, 0, sizeof(*wav));
}

int ra_wav_parse(const uint8_t *data, size_t size, RaWav *wav) {
  uint32_t offset;
  uint16_t format = 0;
  uint16_t channels = 0;
  uint16_t bits = 0;
  uint32_t sample_rate = 0;
  const uint8_t *pcm = NULL;
  uint32_t pcm_size = 0;
  uint32_t frames;

  if (data == NULL || wav == NULL)
    return 0;
  memset(wav, 0, sizeof(*wav));
  if (size < 12 || memcmp(data, "RIFF", 4) != 0 ||
      memcmp(data + 8, "WAVE", 4) != 0)
    return 0;

  offset = 12;
  while (offset + 8 <= size) {
    const uint8_t *chunk = data + offset;
    uint32_t chunk_size = read_u32(chunk + 4);
    const uint8_t *payload = chunk + 8;

    if (chunk_size > size - offset - 8)
      return 0;
    if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
      format = read_u16(payload);
      channels = read_u16(payload + 2);
      sample_rate = read_u32(payload + 4);
      bits = read_u16(payload + 14);
    } else if (memcmp(chunk, "data", 4) == 0) {
      pcm = payload;
      pcm_size = chunk_size;
    }
    offset += 8 + chunk_size + (chunk_size & 1u);
  }

  if (format != 1 || (channels != 1 && channels != 2) ||
      bits != 16 || sample_rate < 8000 || sample_rate > 192000 ||
      pcm == NULL || pcm_size < channels * 2)
    return 0;

  frames = pcm_size / (uint32_t)(channels * 2);
  if ((size_t)frames > ((size_t)-1) / (size_t)(channels * 2))
    return 0;
  wav->samples = (int16_t *)malloc((size_t)frames * channels * 2);
  if (wav->samples == NULL)
    return 0;
  memcpy(wav->samples, pcm, (size_t)frames * channels * 2);
  wav->frames = frames;
  wav->channels = (uint8_t)channels;
  wav->sample_rate = sample_rate;
  return 1;
}

int ra_wav_to_stereo_48k(const RaWav *wav, int16_t **samples,
                         size_t *sample_count) {
  uint32_t out_frames;
  int16_t *output;
  uint32_t i;

  if (wav == NULL || samples == NULL || sample_count == NULL ||
      wav->samples == NULL || wav->frames == 0 || wav->channels == 0 ||
      wav->sample_rate == 0)
    return 0;

  out_frames = (uint32_t)(((uint64_t)wav->frames * 48000u) /
                          wav->sample_rate);
  if (out_frames == 0)
    out_frames = 1;
  output = (int16_t *)malloc((size_t)out_frames * 2 * sizeof(int16_t));
  if (output == NULL)
    return 0;

  for (i = 0; i < out_frames; ++i) {
    double source = ((double)i * wav->sample_rate) / 48000.0;
    uint32_t index = (uint32_t)source;
    double frac = source - index;
    int16_t left;
    int16_t right;

    if (index >= wav->frames)
      index = wav->frames - 1;
    if (wav->channels == 1) {
      int16_t a = wav->samples[index];
      int16_t b = wav->samples[index + 1 < wav->frames ? index + 1 : index];
      left = right = (int16_t)(a + (b - a) * frac);
    } else {
      uint32_t next = index + 1 < wav->frames ? index + 1 : index;
      int16_t l0 = wav->samples[index * 2];
      int16_t r0 = wav->samples[index * 2 + 1];
      int16_t l1 = wav->samples[next * 2];
      int16_t r1 = wav->samples[next * 2 + 1];
      left = (int16_t)(l0 + (l1 - l0) * frac);
      right = (int16_t)(r0 + (r1 - r0) * frac);
    }
    output[i * 2] = left;
    output[i * 2 + 1] = right;
  }

  *samples = output;
  *sample_count = (size_t)out_frames * 2;
  return 1;
}
