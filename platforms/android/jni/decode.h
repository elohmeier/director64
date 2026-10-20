#ifndef DIRECTOR64_ANDROID_DECODE_H
#define DIRECTOR64_ANDROID_DECODE_H
// Decoded sounds: 16-bit PCM, interleaved, one or two channels.
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  int16_t *pcm;
  uint32_t frames;
  unsigned channels, rate;
  atomic_int references;
} sound_t;

sound_t *sound_retain(sound_t *);
void sound_release(sound_t *);
size_t sound_bytes(const sound_t *);
sound_t *sound_source_bounds(sound_t *, const uint8_t *, size_t);
bool is_wav(const uint8_t *data, size_t length);
// A PCM RIFF WAVE file, 8 or 16 bits, mono or stereo, as the converter
// writes them (platforms/web/site/audio.js decodeWav). NULL when malformed.
sound_t *decode_wav(const uint8_t *data, size_t length);
// A WAVE (or Ogg Opus) file's length in seconds without decoding it; 0 when
// malformed.
double wav_duration(const uint8_t *data, size_t length);
// Anything else the platform decodes (MP3 playFile streams), from `length`
// bytes at `offset` in an open file. Blocks while it decodes.
sound_t *decode_media(int fd, uint64_t offset, uint64_t length, char *error,
                      size_t error_capacity);
#endif
