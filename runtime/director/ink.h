#ifndef DIRECTOR64_INK_H
#define DIRECTOR64_INK_H
#include "bitmap_tiles.h"
#include "family.h"
#include <stdbool.h>
#include <stddef.h>
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
#if defined(DIRECTOR64_NATIVE_STAGE) && DIRECTOR64_NATIVE_STAGE
// The browser and Android TV draw the authored 800x600 stage as it is:
// images keep their converted size and geometry is not scaled.
static inline void dg_prescale_dims(unsigned width, unsigned height,
                                    unsigned *scaled_width,
                                    unsigned *scaled_height) {
  *scaled_width = width;
  *scaled_height = height;
}
static inline int dg_prescale_coord(int v) { return v; }
// A text size or metric on the target stage.
static inline int dg_prescale_size(int v) { return v; }
// A target-screen coordinate in authored stage space.
static inline int dg_stage_coord(int v) { return v; }
#else
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
static inline int dg_prescale_size(int v) { return (v * 4 + 2) / 5; }
static inline int dg_stage_coord(int v) { return v * 5 / 4; }
#endif
// Generated offscreen planes are already rasterized in target pixels. Unlike
// imported cursor-sized images, their small dimensions must still prescale.
static inline void dg_prescale_plane_dims(unsigned width, unsigned height,
                                          unsigned *scaled_width, unsigned *scaled_height) {
#if defined(DIRECTOR64_NATIVE_STAGE) && DIRECTOR64_NATIVE_STAGE
  *scaled_width = width; *scaled_height = height;
#else
  *scaled_width = width ? (width * 4u + 2u) / 5u : 0;
  *scaled_height = height ? (height * 4u + 2u) / 5u : 0;
  if (width && !*scaled_width) *scaled_width = 1;
  if (height && !*scaled_height) *scaled_height = 1;
#endif
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
// are ceil(width / 2) bytes. The packer writes it for an image with at most
// 256 distinct words (full_assets.index_colors), which halves or quarters
// what a scene entry decompresses and
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
// The palette index of pixel (x, y) of an FDIC image.
static inline unsigned dg_fdic_index(const uint8_t *data, unsigned bits,
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
  return index;
}
static inline uint16_t dg_fdic_texel(const uint8_t *data, unsigned bits,
                                     uint32_t offset, unsigned width,
                                     unsigned height, unsigned x, unsigned y) {
  return dg_fdi_be16(data + 32 + dg_fdic_index(data,bits,offset,width,height,x,y)*2);
}
// Brushes preserve palette slots even on true-color stages.
static inline bool dg_fdi_palette_brush(const uint8_t *data) {
  return data[0]=='F' && data[1]=='D' && data[2]=='I' && data[3]=='C' && (data[16]&4u);
}
// Mask ink takes the cast member after the sprite's as its mask (ScummVM
// channel.cpp getMask) when that member is one. The corpus names masks after
// their image, "<name>_mask", "<name>mask" or "<name>_Mask", which may be a
// few pixels larger; a sibling-named mask ("highscore_pict_old" is followed
// by "highscore_pict_mask") is one only at the image's size, since
// neighbouring animation frames precede other sprites' masks. Any other
// neighbour ("Stand" before "Move left") is no mask, and the image then
// draws as it is, as the original does.
static inline bool dg_mask_pair(const char *image, const char *mask, int width_difference,
                                int height_difference) {
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  // Director's Mask ink takes the next cast member, whatever its name: the
  // Findus 8.5 masks are unnamed, "<image> mask" or another layer's name,
  // trimmed to their own bounds (the compositor places them by
  // registration point).
  (void)width_difference;
  (void)height_difference;
  (void)image;
  (void)mask;
  return true;
#endif
  if (!image || !mask)
    return false;
  size_t length = 0;
  while (image[length] && (image[length] | 32) == (mask[length] | 32))
    length++;
  const char *rest = image[length] ? NULL : mask + length;
  if (rest && *rest == '_')
    rest++;
  if (rest && (rest[0] | 32) == 'm' && (rest[1] | 32) == 'a' && (rest[2] | 32) == 's' &&
      (rest[3] | 32) == 'k' && !rest[4])
    return true;
  bool named = false;
  for (const char *p = mask; *p && !named; p++)
    named = (p[0] | 32) == 'm' && (p[1] | 32) == 'a' && (p[2] | 32) == 's' && (p[3] | 32) == 'k';
  return named && width_difference >= -3 && width_difference <= 3 && height_difference >= -3 &&
         height_difference <= 3;
}
// The Birthday reference runs DIRAPI/IML32 8.5.1r104 inside the MX9 projector
// wrapper. Its stage preserves the exterior matte for every ink except Copy.
// Keep older profiles' reference behavior until their projectors are checked.
static inline bool dg_ink_whole_rect(unsigned ink) {
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  return ink == 0;
#else
  return ink == 0 || ink == 32 || ink == 1 || ink == 2 || ink == 3 || ink == 5 || ink == 35;
#endif
}
static inline bool dg_blended_copy_matte(unsigned opacity) {
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  (void)opacity;
  return false;
#else
  return opacity < 100;
#endif
}
static inline uint8_t dg_bitmap_alpha16(uint16_t color, uint8_t alpha,
                                        unsigned ink) {
  return ink == 36 && (color & 65534u) == 65534u ? 0 : alpha;
}
static inline uint32_t dg_bitmap_ink32(uint32_t pixel, unsigned ink,
                                       bool follow_alpha) {
  if (ink == 36 && (pixel >> 8) == 0xffffffu)
    return pixel & 0xffffff00u;
  if (dg_ink_whole_rect(ink) && !follow_alpha)
    return pixel | 255u;
  return pixel;
}
static inline uint16_t dg_bitmap_ink16(uint16_t pixel, unsigned ink,
                                       bool follow_alpha) {
  if (ink == 36 && (pixel & 65534u) == 65534u)
    return pixel & 65534u;
  if (dg_ink_whole_rect(ink) && !follow_alpha)
    return pixel | 1u;
  return pixel;
}

// Channel arithmetic in true-color mode. The RGB order is deliberately
// independent of the framebuffer byte order; each renderer supplies bytes.
// Behavioral reference: ScummVM 41ac2b31847622d0662d22c03fe6979e3b43cfbc,
// engines/director/graphics.cpp (format/behavior only).
static inline bool dg_ink_arithmetic(unsigned ink) {
  return (ink >= 1 && ink <= 7) || (ink >= 33 && ink <= 41 && ink != 36);
}
static inline unsigned dg_ink_channel(unsigned s, unsigned d, unsigned ink) {
  switch (ink) {
  case 1: return s & d;
  case 2: return d ^ (255u - s);
  case 3: return d | (255u - s);
  case 4: return 255u - s;
  case 5: return d & (255u - s);
  case 6: return d ^ s;
  case 7: return d | s;
  case 33: return s + d > 255 ? 255 : s + d;
  case 34: return (s + d) & 255u;
  // Original DIRAPI/IML32 8.5.1r104 under Wine8, 2026-10-18: source60 over
  // destination100 with copyPixels ink35 returns40 (ScummVM returns39).
  case 35: return d > s ? d - s : 0;
  case 37: return s > d ? s : d;
  case 38: return (d - s) & 255u;
  case 39: return s < d ? s : d;
  default: return s;
  }
}
// Darken41 and Lighten40 change the source through foreground/background
// colors; they do not select destination minima/maxima. Independently
// recovered from original DIRAPI/IML32 8.5.1r104 under Wine8, 2026-10-18:
// All 608 gray/color-control observations match these integer rules.
// Background 255 is an identity scale; smaller controls use 8-bit fixed point.
static inline unsigned dg_ink_tint_channel(unsigned source, unsigned foreground,
                                           unsigned background) {
  unsigned value = foreground + ((source * (background + (background == 255))) >> 8);
  return value > 255 ? 255 : value;
}
static inline unsigned dg_ink_lighten_channel(unsigned source, unsigned foreground,
                                               unsigned background) {
  unsigned scale = background + (background == 255);
  unsigned value = foreground + (source * scale >> 8) + 255 - (255 * scale >> 8);
  return value > 255 ? 255 : value;
}
static inline unsigned dg_ink_colorize_channel(unsigned source, unsigned foreground,
                                                unsigned background) {
  return (source & background) | ((255 - source) & foreground);
}
// Sprite blend uses a 256-step mix after reducing percent to an 8-bit weight.
// Original Birthday engine observation: red255 over blue255 at50% is126/0/128.
static inline unsigned dg_sprite_blend_channel(unsigned source, unsigned destination,
                                                unsigned opacity) {
  if (opacity >= 100) return source;
  unsigned weight = (opacity * 255 + 49) / 100;
  return (source * weight + destination * (256 - weight)) >> 8;
}
static inline unsigned dg_sprite_ink_channel(unsigned source, unsigned destination,
    unsigned ink, unsigned opacity, unsigned foreground, unsigned background) {
  if (opacity < 100) {
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
    if (ink != 40 && ink != 41)
      source = dg_ink_colorize_channel(source, foreground, background);
    return dg_sprite_blend_channel(source, destination, opacity);
#else
    return (source * opacity + destination * (100 - opacity)) / 100;
#endif
  }
  if (ink == 40) return dg_ink_lighten_channel(source, foreground, background);
  if (ink == 41) return dg_ink_tint_channel(source, foreground, background);
  if (dg_ink_arithmetic(ink)) return dg_ink_channel(source, destination, ink);
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  return dg_ink_colorize_channel(source, foreground, background);
#else
  return source;
#endif
}
// Shapes use a one-bit pattern before foreground/background substitution.
// They do not apply the RGB bitmap Boolean inks to an already colored pixel.
// Independent original DIRAPI/IML32 8.5.1r104 observations: 960 combinations
// from tools/reference/shape-arithmetic.lingo and shape-background.lingo.
static inline bool dg_shape_ink_visible(bool foreground_pixel, unsigned ink,
    unsigned opacity, uint32_t foreground, uint32_t background) {
  if (!opacity) return false;
  if (opacity >= 100) {
    if (foreground_pixel && (ink == 5 || ink == 6 || ink == 7)) return false;
    if (!foreground_pixel && (ink == 1 || ink == 2 || ink == 3)) return false;
  }
  if (ink != 36) return true;
  uint32_t source = foreground_pixel ? foreground : background;
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  if (opacity < 100)
    source = (source & background) | ((~source) & foreground);
#endif
  return (source & 0xffffffu) != (background & 0xffffffu);
}
static inline unsigned dg_shape_ink_channel(bool foreground_pixel,
    unsigned destination, unsigned ink, unsigned opacity,
    unsigned foreground, unsigned background) {
  unsigned source = foreground_pixel ? foreground : background;
  if (opacity < 100) {
    if (ink == 40 || ink == 41) source = foreground_pixel ? 0 : 255;
    return dg_sprite_ink_channel(source, destination, ink, opacity, foreground, background);
  }
  switch (ink) {
  case 0: case 8: case 9:
#if DG_FAMILY_D7 || (defined(DIRECTOR64_D85) && DIRECTOR64_D85)
    // Original7.0.2 and8.5.1 shape Copy foreground passes through RGB555.
    // D85 pattern background keeps fullRGB; D7 background is unmeasured.
    if (foreground_pixel) return (foreground & 248) | (foreground >> 5);
#endif
    return source;
  case 1: return foreground_pixel ? foreground : destination;
  case 2: return foreground_pixel ? 255 - destination : destination;
  case 3: return foreground_pixel ? background : destination;
  case 4: return foreground_pixel ? background : foreground;
  case 5: return foreground_pixel ? destination : foreground;
  case 6: return foreground_pixel ? destination : 255 - destination;
  case 7: return foreground_pixel ? destination : background;
  case 32:
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
    return dg_ink_colorize_channel(source, foreground, background);
#else
    return source;
#endif
  case 40: return dg_ink_lighten_channel(foreground_pixel ? 0 : 255, foreground, background);
  case 41: return dg_ink_tint_channel(foreground_pixel ? 0 : 255, foreground, background);
  default: return dg_ink_channel(source, destination, ink);
  }
}
// A full-color tile retains its own colors under Copy, independently of the
// shape's two monochrome pattern colors (original pattern58 tint probe).
static inline unsigned dg_shape_tile_channel(unsigned source, unsigned destination,
    unsigned ink, unsigned opacity, unsigned foreground, unsigned background) {
  if (opacity < 100)
    return dg_sprite_ink_channel(source, destination, ink, opacity, 0, 255);
  if (ink == 40) return dg_ink_lighten_channel(source, foreground, background);
  if (ink == 41) return dg_ink_tint_channel(source, foreground, background);
  return dg_ink_channel(source, destination, ink);
}
static inline unsigned dg_sprite_coverage_channel(unsigned source, unsigned destination,
                                                   unsigned coverage, unsigned ink) {
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  // Original 8.5.1r104 Mask9: gray128 mask over red on blue gives126/0/128,
  // using a 256-step coverage mix. A black mask remains fully opaque.
  if (ink == 9) {
    unsigned weight = coverage + (coverage == 255);
    return (source * weight + destination * (256 - weight)) >> 8;
  }
#else
  (void)ink;
#endif
  return (source * coverage + destination * (255 - coverage)) / 255;
}
// Straight RGBA surfaces (red in the low byte). Transparent film planes need
// coverage independent of RGB until the final RGB555 display quantization.
static inline uint32_t dg_rgba_blend(uint32_t destination,uint32_t source,unsigned alpha) {
  if (!alpha) return destination;
  if (alpha==255) return source|0xff000000u;
  unsigned remaining=(destination>>24)*(255-alpha);
  unsigned output_alpha=alpha+(remaining+127)/255;
  if (!output_alpha) return destination;
  uint32_t result=output_alpha<<24;
  for(unsigned shift=0;shift<24;shift+=8)
    result|=(((source>>shift&255)*alpha+((destination>>shift&255)*remaining+127)/255+
              output_alpha/2)/output_alpha)<<shift;
  return result;
}
static inline uint32_t dg_rgba_ink(uint32_t destination,uint32_t source,unsigned ink,
    unsigned coverage,unsigned opacity,uint32_t foreground,uint32_t background) {
  if (!coverage || !opacity) return destination;
  uint32_t result=0xff000000u;
  bool transparent=(destination>>24)!=255;
  for(unsigned shift=0;shift<24;shift+=8) {
    unsigned dc=destination>>shift&255;
    unsigned value=dg_sprite_ink_channel(source>>shift&255,dc,ink,transparent?100:opacity,
                                        foreground>>shift&255,background>>shift&255);
    result|=(transparent?value:dg_sprite_coverage_channel(value,dc,coverage,ink))<<shift;
  }
  return transparent?dg_rgba_blend(destination,result,coverage*opacity/100):result;
}
static inline uint32_t dg_rgba_sprite(uint32_t destination,uint32_t source,unsigned ink,
    unsigned coverage,unsigned opacity,uint32_t foreground,uint32_t background) {
  bool arithmetic=dg_ink_arithmetic(ink);
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  arithmetic=arithmetic||opacity<100||(foreground&0xffffffu)!=0||
             (background&0xffffffu)!=0xffffffu||ink==9;
#endif
  return arithmetic?dg_rgba_ink(destination,source,ink,coverage,opacity,foreground,background):
      dg_rgba_blend(destination,source,coverage*opacity/100);
}
static inline uint32_t dg_rgba_shape(uint32_t destination,uint32_t sample,bool tile,bool bit,
    unsigned ink,unsigned opacity,uint32_t foreground,uint32_t background) {
  uint32_t color=0xff000000u;
  bool transparent=(destination>>24)!=255;
  for(unsigned shift=0;shift<24;shift+=8) {
    unsigned fore=foreground>>shift&255,back=background>>shift&255,value;
    if(tile) value=transparent&&opacity<100?sample>>shift&255:
        dg_shape_tile_channel(sample>>shift&255,destination>>shift&255,ink,opacity,fore,back);
    else if(transparent&&opacity<100) {
      value=bit?fore:back;
      if(ink==40||ink==41)value=bit?0:255;
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
      else value=dg_ink_colorize_channel(value,fore,back);
#endif
    } else value=dg_shape_ink_channel(bit,destination>>shift&255,ink,opacity,fore,back);
    color|=value<<shift;
  }
  return transparent||(tile&&(sample>>24)<255)
      ?dg_rgba_blend(destination,color,(tile?sample>>24:255)*opacity/100):color;
}
static inline unsigned dg_fdi_depth(const uint8_t header[32]) {
  return header[19]; // 0: older conversion did not retain original bit depth.
}
static inline bool dg_fdi_rich_text(const uint8_t header[32]) {
  return (header[16] & 2u) != 0;
}
// A decoded mask pixel uses RGBA with red in the low byte. Director's mask
// darkness replaces the recovered matte, or multiplies explicit alpha.
static inline unsigned dg_mask_alpha(uint32_t mask, unsigned source_alpha, bool follow_alpha) {
  unsigned luminance = (mask & 255) * 77 + (mask >> 8 & 255) * 151 + (mask >> 16 & 255) * 28;
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  unsigned coverage = (mask >> 24) ? 255 - luminance / 256 : 0;
  return (follow_alpha ? source_alpha : 255) * coverage / 255;
#else
  return luminance >= 32768 ? 0 : follow_alpha ? source_alpha : 255;
#endif
}
// RTE2 rich text retains glyph coverage for background-transparent ink36.
// Other inks first draw that coverage over the authored text background.
// Pixel uses the on-disk RRGGBBAA order.
static inline uint32_t dg_rich_pixel32(uint32_t pixel, const uint8_t header[32], unsigned ink) {
  if (ink == 36) return pixel;
  unsigned alpha = pixel & 255;
  uint32_t out = 255;
  for (unsigned component = 0; component < 3; component++) {
    unsigned shift = 24 - component * 8;
    unsigned color = pixel >> shift & 255;
    unsigned background = header[24 + component];
    out |= ((color * alpha + background * (255 - alpha) + 127) / 255) << shift;
  }
  return out;
}

// Director's arithmetic Darkest ink chooses each RGB minimum. An enabled
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
