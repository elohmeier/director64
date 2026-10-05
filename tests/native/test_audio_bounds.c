#include "audio_bounds.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  unsigned length = 0, stop = 0;
  for (unsigned tail = 0; tail < 4; tail++) {
    assert(audio_loop_bounds(0, 233472 + tail, 233472, &length, &stop));
    assert(length == 233472 && stop == 233472);
  }
  assert(audio_loop_bounds(100, 200, 300, &length, &stop));
  assert(length == 100 && stop == 200);
  assert(!audio_loop_bounds(300, 400, 300, &length, &stop));
  assert(!audio_loop_bounds(200, 100, 300, &length, &stop));
  assert(!audio_loop_bounds(0, 1, 0, &length, &stop));
  puts("audio loops: codec-rounded endpoints and invalid bounds OK");
  return 0;
}
