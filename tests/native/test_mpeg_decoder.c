// Handwritten MPEG syntax, no game media. Bounded damaged-slice recovery.
#include <assert.h>
#include <stdio.h>
#define PL_MPEG_IMPLEMENTATION
#include "../../third_party/pl_mpeg/pl_mpeg.h"
static uint8_t bytes[1024];
static size_t bit;
static void bits(unsigned value, unsigned count) {
  for (unsigned i = count; i; i--) {
    if ((value >> (i - 1)) & 1)
      bytes[bit / 8] |= 1u << (7 - bit % 8);
    bit++;
  }
}
static void start(unsigned code) {
  bit = (bit + 7) & ~(size_t)7;
  bits(1, 24);
  bits(code, 8);
}
static void picture(unsigned number, int damage) {
  start(0);
  bits(number, 10);
  bits(1, 3);
  bits(65535, 16);
  bits(0, 1);
  start(1);
  bits(8, 5);
  bits(0, 1);
  if (damage) {
    bits(0, 2);
    return;
  }
  bits(1, 1);
  bits(1, 1); // address increment1, intra macroblock
  for (unsigned i = 0; i < 6; i++) {
    bits(i < 4 ? 4 : 0, i < 4 ? 3 : 2);
    bits(2, 2);
  }
}
static unsigned decode(int damage) {
  memset(bytes, 0, sizeof(bytes));
  bit = 0;
  start(0xb3);
  bits(16, 12);
  bits(16, 12);
  bits(1, 4);
  bits(3, 4);
  bits(1, 18);
  bits(1, 1);
  bits(0, 10);
  bits(0, 1);
  bits(0, 1);
  bits(0, 1);
  // User data also satisfies the upstream maximum sequence-header lookahead.
  start(0xb2);
  for (unsigned i = 0; i < 160; i++)
    bits(0x42, 8);
  picture(0, damage);
  picture(1, 0);
  picture(2, 0);
  start(0xb7);
  bits(0, 8);
  size_t length = (bit + 7) / 8;
  uint8_t original[1024];
  memcpy(original, bytes, length);
  plm_video_t *v = plm_video_create_with_buffer(
      plm_buffer_create_with_memory(bytes, length, 0), 1);
  unsigned count = 0;
  plm_frame_t *f;
  while ((f = plm_video_decode(v))) {
    assert(f->width == 16 && f->height == 16);
    count++;
  }
  assert(count == 3);
  assert(plm_video_get_corrupt_slices(v) == (unsigned)damage);
  assert(memcmp(original, bytes, length) ==
         0); // fixed memory input stays intact
  v->mb_col = 0;
  v->mb_row = 0;
  v->buffer->read_error = 0;
  plm_video_process_macroblock(v, v->frame_forward.y.data,
                               v->frame_current.y.data, 0, 1, 16, 0);
  assert(v->buffer->read_error); // half-pel needs a neighbor beyond final row
  unsigned errors = plm_video_get_corrupt_slices(v);
  plm_video_destroy(v);
  return errors;
}
int main(void) {
  assert(decode(0) == 0);
  assert(decode(1) == 1);
  puts("MPEG: synthetic frames, slice boundary recovery, fixed memory EOF and "
       "half-pel bounds passed");
}
