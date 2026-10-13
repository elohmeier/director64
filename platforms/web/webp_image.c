#include "webp_image.h"
#include "bitmap_tiles.h"
#include "src/webp/decode.h"
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

bool dg_webp_packed(const uint8_t *data, uint32_t length) {
  return length > 40 && !memcmp(data, "D64W", 4);
}

uint8_t *dg_webp_fdi(const uint8_t *data, uint32_t length, uint32_t *out_length) {
  *out_length = 0;
  if (!dg_webp_packed(data, length))
    return NULL;
  const uint8_t *header = data + 8;
  uint32_t stored = be32(data + 4);
  unsigned width = be16(header + 8), height = be16(header + 10);
  bool wide = !memcmp(header, "FDI2", 4), planes = !memcmp(header, "FDIA", 4);
  if (stored < 32 || be32(header + 4) != stored || !width || !height ||
      !(wide || planes || !memcmp(header, "FDI1", 4)))
    return NULL;
  bool tiled = bitmap_needs_tiles(width, height);
  uint64_t plane = bitmap_plane_pixels(width, height);
  uint32_t alpha_offset = planes ? be32(header + 20) : 0;
  if (32u + plane * (wide ? 4u : 2u) > stored || (planes && alpha_offset + plane > stored))
    return NULL;
  int w = 0, h = 0;
  uint8_t *rgba = WebPDecodeRGBA(data + 40, length - 40, &w, &h);
  if (!rgba || (unsigned)w != width || (unsigned)h != height) {
    WebPFree(rgba);
    return NULL;
  }
  uint8_t *out = calloc(stored, 1);
  if (!out) {
    WebPFree(rgba);
    return NULL;
  }
  memcpy(out, header, 32);
  for (unsigned y = 0; y < height; y++)
    for (unsigned x = 0; x < width; x++) {
      const uint8_t *p = rgba + ((size_t)y * width + x) * 4;
      uint64_t slot = tiled ? bitmap_tile_index(width, x, y) : (uint64_t)y * width + x;
      if (wide) {
        memcpy(out + 32 + slot * 4, p, 4);
        continue;
      }
      unsigned word = (unsigned)(p[0] >> 3) << 11 | (unsigned)(p[1] >> 3) << 6 |
                      (unsigned)(p[2] >> 3) << 1 | (p[3] > 0);
      out[32 + slot * 2] = (uint8_t)(word >> 8);
      out[33 + slot * 2] = (uint8_t)word;
      if (planes)
        out[alpha_offset + slot] = p[3];
    }
  WebPFree(rgba);
  *out_length = stored;
  return out;
}
