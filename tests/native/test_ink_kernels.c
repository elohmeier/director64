// Specialized ink kernels must equal the general per-channel ink exactly.
// Every Mask9 coverage and opacity, with pseudo-random colors and the channel
// extremes, over opaque destinations (the only ones the kernel accepts).
#include "ink.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static uint32_t state = 0x9e3779b9u;
static uint32_t next(void) {
  state ^= state << 13; state ^= state >> 17; state ^= state << 5;
  return state;
}
static uint32_t color(unsigned i) {
  static const uint32_t edges[] = {0, 0xffffffu, 0xff00ffu, 0x00ff00u, 0x010101u, 0xfefefeu};
  return i < sizeof(edges) / sizeof(*edges) ? edges[i] : next() & 0xffffffu;
}
int main(void) {
  unsigned long long checked = 0;
  for (unsigned coverage = 1; coverage <= 255; coverage++)
    for (unsigned opacity = 1; opacity <= 100; opacity++)
      for (unsigned i = 0; i < 12; i++) {
        uint32_t source = color(i) | (next() & 0xff000000u);
        uint32_t destination = color((i + 3) % 12) | 0xff000000u;
        uint32_t fore = i & 1 ? color(i / 2) : 0, back = i & 2 ? color(i / 3) : 0xffffffu;
        uint32_t expected = dg_rgba_ink(destination, source, 9, coverage, opacity, fore, back);
        uint32_t actual = dg_rgba_mask_ink(destination, source, coverage, opacity, fore, back);
        if (actual != expected)
          fprintf(stderr, "mask cov%u op%u src%08x dst%08x fg%06x bg%06x: %08x != %08x\n",
                  coverage, opacity, source, destination, fore, back, actual, expected);
        assert(actual == expected);
        checked++;
      }
  printf("ink kernels: Mask9 equals the general ink for %llu coverage/opacity/color cases\n",
         checked);
}
