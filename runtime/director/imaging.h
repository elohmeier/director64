#ifndef DIRECTOR64_IMAGING_H
#define DIRECTOR64_IMAGING_H
#include <stdbool.h>
#include <stdint.h>

// Owned, unscaled image in authored pixels. RGBA8888 has red in the low byte,
// matching the software compositor. Alpha is conventional coverage (255 opaque).
// A host read_image callback returns malloc'd pixels; the runtime owns them.
typedef struct {
  unsigned width, height, depth;
  int reg_x, reg_y;
  uint32_t *pixels;
  bool use_alpha, grayscale;
} dg_pixel_image_t;
typedef struct dg_imaging dg_imaging_t;
#endif
