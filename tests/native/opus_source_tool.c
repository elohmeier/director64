// Synthetic Ogg files only: verify exact source bounds after platform decode.
#include "../../platforms/android/jni/decode.c"
#include <assert.h>
int main(int argc, char **argv) {
  assert(argc == 2);
  FILE *file = fopen(argv[1], "rb"); assert(file);
  assert(!fseek(file, 0, SEEK_END)); long size = ftell(file); assert(size > 0);
  rewind(file); uint8_t *bytes = malloc((size_t)size); assert(bytes);
  assert(fread(bytes, 1, (size_t)size, file) == (size_t)size); fclose(file);
  uint32_t rate, frames; assert(opus_source(bytes, (size_t)size, &rate, &frames));
  sound_t *decoded = new_sound(2, 48000, (uint32_t)((uint64_t)frames * 48000 / rate + 1));
  assert(decoded);
  for (uint32_t i = 0; i < decoded->frames; i++) {
    decoded->pcm[(size_t)i * 2] = 1234;
    decoded->pcm[(size_t)i * 2 + 1] = -2345;
  }
  decoded = sound_source_bounds(decoded, bytes, (size_t)size);
  assert(decoded && decoded->rate == rate && decoded->frames == frames);
  for (uint32_t i = 0; i < frames; i++)
    assert(decoded->pcm[(size_t)i * 2] == 1234 && decoded->pcm[(size_t)i * 2 + 1] == -2345);
  assert(wav_duration(bytes, (size_t)size) == (double)frames / rate);
  // Truncation never yields trusted source sample metadata.
  for (size_t n = 0; n < 60 && n < (size_t)size; n++)
    assert(!opus_source(bytes, n, &rate, &frames));
  printf("{\"rate\":%u,\"frames\":%u}\n", decoded->rate, decoded->frames);
  sound_release(decoded); free(bytes);
}
