#ifndef DIRECTOR64_AUDIO_BOUNDS_H
#define DIRECTOR64_AUDIO_BOUNDS_H
#include <stdbool.h>

// audioconv64 ULC2 rounds the logical waveform down to an eight-byte
// boundary (up to three mono frames). Source loop endpoints remain unrounded.
static inline bool audio_loop_bounds(unsigned start, unsigned end,
                                     unsigned decoded_frames, unsigned *length,
                                     unsigned *stop) {
  if (end > decoded_frames)
    end = decoded_frames;
  if (start >= end)
    return false;
  *length = end - start;
  *stop = end;
  return true;
}
#endif
