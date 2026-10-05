#ifndef DIRECTOR64_CURSOR_H
#define DIRECTOR64_CURSOR_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Values, never pointers into Lingo's moving heap or unloadable movie overlays.
// An image identifies a bitmap cursor; otherwise resource is a built-in ID.
typedef struct {
  int32_t resource;
  uint32_t image, mask;
} dg_cursor_t;
static inline bool dg_cursor_equal(dg_cursor_t a, dg_cursor_t b) {
  return a.resource == b.resource && a.image == b.image && a.mask == b.mask;
}
static inline bool dg_cursor_active(dg_cursor_t c) {
  return c.image || c.resource; // Sprite 0 clears an override; -1 forces arrow.
}

enum { DG_CURSOR_SIZE = 16 };
typedef struct {
  uint16_t black[DG_CURSOR_SIZE], opaque[DG_CURSOR_SIZE];
  uint8_t hot_x, hot_y;
} dg_cursor_bitmap_t;

static inline void dg_cursor_hotspot(dg_cursor_bitmap_t *out, int x, int y) {
  if (x < 0 || x >= DG_CURSOR_SIZE || y < 0 || y >= DG_CURSOR_SIZE)
    x = y = 8;
  out->hot_x = (uint8_t)x;
  out->hot_y = (uint8_t)y;
}

// Inputs are 16x16 cropped RGBA5551 samples in host byte order. Ignore their
// ordinary sprite alpha: a cursor's explicit black mask defines coverage.
// A missing mask gives an opaque rectangle, clipped to the source dimensions.
static inline bool dg_cursor_compose(dg_cursor_bitmap_t *out,
    const uint16_t *image, unsigned width, unsigned height,
    const uint16_t *mask, unsigned mask_width, unsigned mask_height,
    int hot_x, int hot_y) {
  memset(out, 0, sizeof(*out));
  dg_cursor_hotspot(out, hot_x, hot_y);
  for (unsigned y = 0; y < DG_CURSOR_SIZE && y < height; y++)
    for (unsigned x = 0; x < DG_CURSOR_SIZE && x < width; x++) {
      if (mask && (x >= mask_width || y >= mask_height)) continue;
      unsigned i = y * DG_CURSOR_SIZE + x;
      uint16_t color = image[i] & 0xfffe;
      uint16_t coverage = mask ? mask[i] & 0xfffe : 0;
      // This service implements classic monochrome cast cursors. Resource
      // and color Cursor Xtra formats need their own recovered contracts.
      if ((color && color != 0xfffe) || (coverage && coverage != 0xfffe))
        return false;
      uint16_t bit = (uint16_t)(0x8000u >> x);
      if (!coverage) out->opaque[y] |= bit;
      if (!color) out->black[y] |= bit;
    }
  return true;
}

// RGBA5551 with the coverage bit set, for tinting a cursor's ink.
static inline uint16_t dg_cursor_ink(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | 1);
}
// The ink replaces the glyph's black body; the white contrast pixels never
// tint, so a coloured cursor still reads against the art underneath it.
static inline void dg_cursor_pixels_ink(const dg_cursor_bitmap_t *bitmap,
                                        uint16_t pixels[256], uint16_t ink) {
  if (!ink) ink = 1;
  for (unsigned y = 0; y < DG_CURSOR_SIZE; y++)
    for (unsigned x = 0; x < DG_CURSOR_SIZE; x++) {
      unsigned bit = 0x8000u >> x;
      pixels[y * DG_CURSOR_SIZE + x] = !(bitmap->opaque[y] & bit) ? 0 :
          (bitmap->black[y] & bit) ? ink : 0xffff;
    }
}
static inline void dg_cursor_pixels(const dg_cursor_bitmap_t *bitmap,
                                     uint16_t pixels[256]) {
  dg_cursor_pixels_ink(bitmap, pixels, 1);
}
static inline uint32_t dg_cursor_bitmap_hash(const dg_cursor_bitmap_t *bitmap) {
  uint32_t hash = 2166136261u;
  for (unsigned y = 0; y < 16; y++) {
    uint16_t black = bitmap->black[y] & bitmap->opaque[y];
    const unsigned bytes[] = {black >> 8, black & 255,
                              bitmap->opaque[y] >> 8, bitmap->opaque[y] & 255};
    for (unsigned i = 0; i < 4; i++) hash = (hash ^ bytes[i]) * 16777619u;
  }
  hash = (hash ^ bitmap->hot_x) * 16777619u;
  return (hash ^ bitmap->hot_y) * 16777619u;
}

// Independently drawn target glyphs for Director's standard cursor roles.
// Original game artwork is loaded from the user's recovered cast assets.
static inline bool dg_cursor_builtin(dg_cursor_bitmap_t *out, int resource) {
  static const uint16_t arrow[16] = {
    0x8000,0xc000,0xe000,0xf000,0xf800,0xfc00,0xfe00,0xff00,
    0xff80,0xfc00,0xdc00,0x8e00,0x0e00,0x0700,0x0700,0x0200};
  static const uint16_t beam[16] = {
    0,0,0x3c00,0x1800,0x1800,0x1800,0x1800,0x1800,
    0x1800,0x1800,0x1800,0x1800,0x3c00,0,0,0};
  static const uint16_t cross[16] = {
    0,0x0100,0x0100,0x0100,0x0100,0,0x0100,0x7dfc,
    0x0100,0,0x0100,0x0100,0x0100,0x0100,0,0};
  static const uint16_t plus[16] = {
    0,0,0,0x0180,0x0180,0x0180,0x0180,0x1ff8,
    0x1ff8,0x0180,0x0180,0x0180,0x0180,0,0,0};
  static const uint16_t watch[16] = {
    0x07e0,0x07e0,0x1ff8,0x2004,0x4182,0x8181,0x8181,0x81f1,
    0x8001,0x8001,0x4002,0x2004,0x1ff8,0x07e0,0x07e0,0};
  const uint16_t *shape;
  memset(out, 0, sizeof(*out));
  switch (resource) {
  case -1: case 0: shape = arrow; break;
  case 1: shape = beam; out->hot_x = 4; out->hot_y = 7; break;
  case 2: shape = cross; out->hot_x = out->hot_y = 7; break;
  case 3: shape = plus; out->hot_x = out->hot_y = 7; break;
  case 4: shape = watch; out->hot_x = out->hot_y = 7; break;
  case 200: return true;
  default: return false;
  }
  memcpy(out->black, shape, sizeof(out->black));
  // One white pixel of contrast around the glyph, plus the watch's face.
  for (unsigned y = 0; y < DG_CURSOR_SIZE; y++) {
    uint16_t row = shape[y];
    if (y) row |= shape[y - 1];
    if (y + 1 < DG_CURSOR_SIZE) row |= shape[y + 1];
    out->opaque[y] = (uint16_t)(row | row << 1 | row >> 1);
    if (resource == 4 && y >= 4 && y <= 11) out->opaque[y] |= 0x7ffe;
  }
  return true;
}
#endif
