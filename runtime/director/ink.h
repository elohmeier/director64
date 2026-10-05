#ifndef DIRECTOR64_INK_H
#define DIRECTOR64_INK_H
#include "bitmap_tiles.h"
#include "family.h"
#include <stdbool.h>
#include <stdint.h>

// FDI1 stores RGBA5551; FDI2 stores RGBA8888; FDIA stores RGB555 plus A8.
// Byte16 bit0 preserves coverage (Director7+ FollowAlpha or flattened film
// loops). Byte17 retains alphaThreshold.
static inline bool dg_fdi_follow_alpha(const uint8_t header[32]) {
  return (header[16] & 1u) != 0;
}
static inline uint32_t dg_fdi_be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
         (uint32_t)p[2] << 8 | p[3];
}
static inline unsigned dg_fdi_be16(const uint8_t *p) {
  return (unsigned)p[0] << 8 | p[1];
}
#if DG_D10
// The authored 800x600 stage prescales its converted images to the 640x480
// target (4/5). Geometry everywhere stays authored; texture dimensions map
// through this rule, mirrored exactly by the converter (prescaleImage).
// Cursor-sized images (<=16x16) keep their exact pixels.
static inline void dg_prescale_dims(unsigned width, unsigned height,
                                    unsigned *scaled_width,
                                    unsigned *scaled_height) {
  if (width <= 16 && height <= 16) {
    *scaled_width = width;
    *scaled_height = height;
    return;
  }
  *scaled_width = width ? (width * 4u + 2u) / 5u : 0u;
  if (width && !*scaled_width) *scaled_width = 1;
  *scaled_height = height ? (height * 4u + 2u) / 5u : 0u;
  if (height && !*scaled_height) *scaled_height = 1;
}
// Authored-space coordinate onto the 640x480 screen, flooring negatives.
static inline int dg_prescale_coord(int v) {
  return (v * 4 - (v < 0 ? 4 : 0)) / 5;
}
#endif
// Both planes hold whatever the stored layout pads them to, so a tile-packed
// image's colour plane ends where its tiles do and the coverage plane starts
// there. Tile counts are multiples of 1024, so the eight-byte alignment the
// linear case rounds up to is already satisfied.
static inline uint32_t dg_fdia_size(unsigned width, unsigned height,
                                   uint32_t *alpha_offset) {
  if (!width || !height || width > UINT16_MAX || height > UINT16_MAX)
    return 0;
  uint64_t pixels = bitmap_plane_pixels(width, height);
  if (pixels > (UINT32_MAX - 39u) / 3u)
    return 0;
  uint32_t offset = (uint32_t)((32u + pixels * 2u + 7u) & ~UINT64_C(7));
  *alpha_offset = offset;
  return offset + (uint32_t)pixels;
}
static inline bool dg_fdia_valid(const uint8_t *header, uint32_t length,
                                 unsigned width, unsigned height,
                                 uint32_t *alpha_offset) {
  uint32_t expected = dg_fdia_size(width, height, alpha_offset);
  return length >= 32 && expected && length == expected &&
         header[0] == 'F' && header[1] == 'D' && header[2] == 'I' &&
         header[3] == 'A' && dg_fdi_be32(header + 4) == expected &&
         dg_fdi_be16(header + 8) == width && dg_fdi_be16(header + 10) == height &&
         dg_fdi_follow_alpha(header) &&
         dg_fdi_be32(header + 20) == *alpha_offset;
}
// FDIC stores a colour-indexed plane over a palette of RGBA5551 words: the
// header is FDI1's with byte 18 the bits per index (4 or 8) and bytes 20..21
// the colour count. The palette follows the header and the index plane
// follows the palette at the next eight-byte boundary; the plane holds the
// stored layout's pixels (tiles past the texture limit, as every plane), two
// per byte with the left pixel in the high nibble for CI4, whose linear rows
// are ceil(width / 2) bytes, as the RDP reads them. The packer writes it for
// an image with at most 256 distinct words (full_assets.index_colors), which
// on the console halves or quarters what a scene entry decompresses and
// lets one palette carry an ink's alpha for every pixel of that colour.
static inline unsigned dg_fdic_bits(const uint8_t *header) { return header[18]; }
static inline unsigned dg_fdic_colors(const uint8_t *header) {
  return dg_fdi_be16(header + 20);
}
static inline uint32_t dg_fdic_index_offset(unsigned colors) {
  return (32u + colors * 2u + 7u) & ~7u;
}
static inline unsigned dg_fdic_row_bytes(unsigned width, unsigned bits) {
  return bits == 4 ? (width + 1) / 2 : width;
}
static inline uint64_t dg_fdic_index_bytes(unsigned width, unsigned height,
                                           unsigned bits) {
  if (bitmap_needs_tiles(width, height))
    return bitmap_tile_pixels(width, height) * bits / 8;
  return (uint64_t)dg_fdic_row_bytes(width, bits) * height;
}
static inline uint32_t dg_fdic_size(unsigned width, unsigned height,
                                    unsigned bits, unsigned colors) {
  if (!width || !height || width > UINT16_MAX || height > UINT16_MAX)
    return 0;
  if ((bits != 4 && bits != 8) || !colors || colors > (bits == 4 ? 16u : 256u))
    return 0;
  uint64_t total = dg_fdic_index_offset(colors) + dg_fdic_index_bytes(width, height, bits);
  return total > UINT32_MAX ? 0 : (uint32_t)total;
}
static inline bool dg_fdic_valid(const uint8_t *header, uint32_t length,
                                 unsigned width, unsigned height,
                                 unsigned *bits, unsigned *colors,
                                 uint32_t *index_offset) {
  *bits = dg_fdic_bits(header);
  *colors = dg_fdic_colors(header);
  *index_offset = dg_fdic_index_offset(*colors);
  uint32_t expected = dg_fdic_size(width, height, *bits, *colors);
  return length >= 32 && expected && length == expected &&
         header[0] == 'F' && header[1] == 'D' && header[2] == 'I' &&
         header[3] == 'C' && dg_fdi_be32(header + 4) == expected &&
         dg_fdi_be16(header + 8) == width && dg_fdi_be16(header + 10) == height;
}
// The RGBA5551 word of pixel (x, y) of an FDIC image.
static inline uint16_t dg_fdic_texel(const uint8_t *data, unsigned bits,
                                     uint32_t index_offset, unsigned width,
                                     unsigned height, unsigned x, unsigned y) {
  const uint8_t *plane = data + index_offset;
  unsigned index;
  if (bitmap_needs_tiles(width, height)) {
    unsigned pixel = bitmap_tile_index(width, x, y);
    index = bits == 4 ? ((pixel & 1) ? plane[pixel / 2] & 15u : plane[pixel / 2] >> 4)
                      : plane[pixel];
  } else if (bits == 4) {
    uint8_t byte = plane[(uint64_t)y * dg_fdic_row_bytes(width, 4) + x / 2];
    index = (x & 1) ? byte & 15u : byte >> 4;
  } else {
    index = plane[(uint64_t)y * width + x];
  }
  return dg_fdi_be16(data + 32 + index * 2);
}
static inline uint8_t dg_bitmap_alpha16(uint16_t color, uint8_t alpha,
                                        unsigned ink) {
  return ink == 36 && (color & 65534u) == 65534u ? 0 : alpha;
}
static inline uint32_t dg_bitmap_ink32(uint32_t pixel, unsigned ink,
                                       bool follow_alpha) {
  if (ink == 36 && (pixel >> 8) == 0xffffffu)
    return pixel & 0xffffff00u;
  if ((ink == 0 || ink == 32) && !follow_alpha)
    return pixel | 255u;
  return pixel;
}
static inline uint16_t dg_bitmap_ink16(uint16_t pixel, unsigned ink,
                                       bool follow_alpha) {
  if (ink == 36 && (pixel & 65534u) == 65534u)
    return pixel & 65534u;
  if ((ink == 0 || ink == 32) && !follow_alpha)
    return pixel | 1u;
  return pixel;
}

// Director's arithmetic Darken ink chooses each RGB minimum. An enabled
// sprite blend takes precedence over arithmetic ink (ScummVM graphics.cpp),
// and the recovered bitmap matte still excludes transparent source pixels.
static inline uint16_t dg_dark_rgba16(uint16_t source, uint16_t destination,
                                      unsigned opacity) {
  if (!(source & 1) || !opacity)
    return destination;
  uint16_t result = 1;
  for (unsigned component = 0; component < 3; component++) {
    unsigned shift = 11 - component * 5;
    unsigned s = (source >> shift) & 31, d = (destination >> shift) & 31;
    unsigned value = opacity < 100 ? (s * opacity + d * (100 - opacity)) / 100
                                   : (s < d ? s : d);
    result |= (uint16_t)(value << shift);
  }
  return result;
}
static inline uint16_t dg_dark_rgba32(uint32_t source, uint16_t destination,
                                      unsigned opacity) {
  unsigned alpha = (source & 255u) * opacity;
  if (!alpha) return destination;
  uint16_t result = 1;
  for (unsigned c = 0; c < 3; c++) {
    unsigned shift = 11 - c * 5;
    unsigned s = (source >> (24 - c * 8)) & 255u;
    unsigned d = ((destination >> shift) & 31u) * 255u / 31u;
    // Sprite blend takes precedence over arithmetic ink, as with RGBA16.
    unsigned ink = opacity < 100 ? s : (s < d ? s : d);
    unsigned mixed = (ink * alpha + d * (25500u - alpha)) / 25500u;
    result |= (uint16_t)((mixed >> 3) << shift);
  }
  return result;
}
static inline uint16_t dg_dark_alpha16(uint16_t source, uint8_t source_alpha,
                                       uint16_t destination, unsigned opacity) {
  unsigned alpha = source_alpha * opacity;
  if (!alpha)
    return destination;
  uint16_t result = 1;
  for (unsigned c = 0; c < 3; c++) {
    unsigned shift = 11 - c * 5;
    unsigned s = (source >> shift) & 31u, d = (destination >> shift) & 31u;
    s = (s << 3) | (s >> 2);
    d = (d << 3) | (d >> 2);
    unsigned ink = opacity < 100 ? s : (s < d ? s : d);
    unsigned mixed = (ink * alpha + d * (25500u - alpha)) / 25500u;
    result |= (uint16_t)((mixed >> 3) << shift);
  }
  return result;
}
#endif
