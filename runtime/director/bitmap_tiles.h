#ifndef DIRECTOR64_BITMAP_TILES_H
#define DIRECTOR64_BITMAP_TILES_H

#include <stdbool.h>
#include <stdint.h>

enum { BITMAP_TILE_SIZE = 32, BITMAP_TEXTURE_LIMIT = 1024 };

// Source tile interval intersecting an axis-aligned destination and viewport.
// Keep complete edge tiles: their original UVs and the RDP scissor do the crop.
static inline void bitmap_visible_tiles(unsigned size, int start, int end,
                                        unsigned viewport, unsigned *first,
                                        unsigned *limit) {
  *first = *limit = 0;
  if (!size || end <= start || end <= 0 || start >= (int)viewport) return;
  uint64_t span = (int64_t)end - start;
  if (start < 0)
    *first = ((uint64_t)(-(int64_t)start) * size / span) / 32 * 32;
  *limit = size;
  if (end > (int)viewport) {
    uint64_t edge = ((int64_t)viewport - start) * size;
    uint64_t tile_span = span * 32;
    unsigned last = (unsigned)((edge + tile_span - 1) / tile_span) * 32;
    if (last < size) *limit = last;
  }
}

static inline bool bitmap_needs_tiles(unsigned width, unsigned height) {
  return width > BITMAP_TEXTURE_LIMIT || height > BITMAP_TEXTURE_LIMIT;
}

// Tile-major storage keeps every upload's dimensions, stride and UVs within
// RDP limits. Partial edge tiles are padded; all tile bases are 8-byte aligned.
// The converter stores images past the limit in this order, so the console
// never rearranges one: repacking a station pavement in place cost about
// 460 ms of a frame, and the cache reloads it each time the scene scrolls
// back to it.
static inline uint64_t bitmap_tile_pixels(unsigned width, unsigned height) {
  return ((uint64_t)width + 31) / 32 * (((uint64_t)height + 31) / 32) * 1024;
}

// Pixels a stored plane holds, padding included. This is the one place that
// decides a stored image's extent; the converter mirrors it exactly.
static inline uint64_t bitmap_plane_pixels(unsigned width, unsigned height) {
  return bitmap_needs_tiles(width, height) ? bitmap_tile_pixels(width, height)
                                           : (uint64_t)width * height;
}

static inline unsigned bitmap_tile_index(unsigned width, unsigned x, unsigned y) {
  return ((y / 32) * ((width + 31) / 32) + x / 32) * 1024 +
         (y % 32) * 32 + x % 32;
}

#endif
