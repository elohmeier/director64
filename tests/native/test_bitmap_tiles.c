#include "bitmap_tiles.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

// The stored order for an image past the RDP's texture limit. The converter
// writes planes in exactly this order (compiler/src/convert/fdi.rs), so what
// this checks is that every logical pixel has one slot, that the slots run in
// the order the RDP reads each 32x32 surface, and that the padding a partial
// edge tile leaves belongs to no pixel at all.
static void layout(unsigned width, unsigned height) {
  size_t pixels = bitmap_tile_pixels(width, height);
  uint8_t *seen = calloc(pixels, 1);
  assert(seen);
  size_t base = 0;
  for (unsigned top = 0; top < height; top += 32)
    for (unsigned left = 0; left < width; left += 32, base += 1024) {
      // Every tile base is eight-byte aligned at one, two or four bytes per
      // pixel, which is what lets the RDP load a tile without a copy.
      assert((base * 4) % 8 == 0);
      for (unsigned v = 0; v < 32; v++)
        for (unsigned u = 0; u < 32; u++) {
          unsigned x = left + u, y = top + v;
          size_t pixel = base + v * 32 + u;
          assert(pixel < pixels);
          if (x < width && y < height) {
            assert(bitmap_tile_index(width, x, y) == pixel);
            assert(!seen[pixel]++); // one slot per pixel, and only one
          }
        }
    }
  assert(base == pixels);
  for (size_t i = 0; i < pixels; i++)
    assert(seen[i] <= 1);
  free(seen);
}

static void visible_tiles(void) {
  // Every returned interval must contain precisely the tiles with a positive
  // destination overlap, across scrolling, scaling and partial edge tiles.
  const unsigned widths[] = {1, 31, 32, 33, 640, 1182, 1920};
  for (unsigned w = 0; w < sizeof(widths) / sizeof(*widths); w++) {
    unsigned width = widths[w];
    for (int left = -2000; left <= 700; left += 37)
      for (int span = 1; span <= 2500; span += 113) {
        unsigned first, limit;
        bitmap_visible_tiles(width, left, left + span, 640, &first, &limit);
        for (unsigned x = 0; x < width; x += 32) {
          unsigned right = x + 32 < width ? x + 32 : width;
          bool overlap = (int64_t)left * width + (int64_t)right * span > 0 &&
                         (int64_t)left * width + (int64_t)x * span < 640 * (int64_t)width;
          assert((x >= first && x < limit) == overlap);
        }
      }
  }
  unsigned first, limit;
  bitmap_visible_tiles(1920, 0, 1920, 640, &first, &limit);
  assert(first == 0 && limit == 640); // BR: 200 paired uploads, previously 600.
  bitmap_visible_tiles(32, 20, 20, 640, &first, &limit);
  assert(first == limit);
}
int main(void) {
  visible_tiles();
  assert(!bitmap_needs_tiles(1024, 1024));
  assert(bitmap_needs_tiles(1025, 1));
  assert(bitmap_needs_tiles(1, 1025));
  assert(bitmap_tile_pixels(1182, 127) == 151552);
  assert(bitmap_tile_pixels(65535, 65535) == UINT64_C(4294967296));
  // An image within the limit is stored exactly as authored; only one past it
  // pays for padding, and every reader sizes its plane through this.
  assert(bitmap_plane_pixels(640, 480) == 307200);
  assert(bitmap_plane_pixels(1024, 1024) == 1048576);
  assert(bitmap_plane_pixels(1182, 127) == 151552);
  assert(bitmap_plane_pixels(1786, 149) == 286720); // TS.DXR ts_st6
  layout(1182, 127); // TS.DXR member 35, ts_st4: reported train crash.
  layout(1025, 33);
  layout(33, 1025);
  layout(1786, 149);
  puts("bitmap tiles: passed");
}
