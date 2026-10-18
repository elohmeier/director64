#ifndef DIRECTOR64_ANDROID_AUDIO_POSITION_H
#define DIRECTOR64_ANDROID_AUDIO_POSITION_H
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

typedef struct {
  double position, loop_start, loop_end;
  bool looping;
} audio_start_position_t;

// Resolve an asynchronously decoded sound at its current active playback
// time. A release while decoding first wraps at the release instant, then
// advances through the remaining tail without wrapping again.
static inline audio_start_position_t audio_start_position(
    uint32_t frames, uint32_t rate, bool looping, uint32_t start, uint32_t end,
    double offset, double release_offset) {
  audio_start_position_t out = {.position=offset * rate, .loop_end=frames, .looping=looping};
  if (!looping) return out;
  if (end > frames) end = frames;
  if (start < end) { out.loop_start=start; out.loop_end=end; }
  if (release_offset >= 0) out.position=release_offset * rate;
  if (out.position >= out.loop_end && out.loop_end > out.loop_start)
    out.position=out.loop_start + fmod(out.position - out.loop_start, out.loop_end - out.loop_start);
  if (release_offset >= 0) {
    out.position += fmax(0, offset - release_offset) * rate;
    out.looping=false;
  }
  return out;
}
#endif
