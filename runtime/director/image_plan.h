#ifndef DIRECTOR64_IMAGE_PLAN_H
#define DIRECTOR64_IMAGE_PLAN_H
#include "ink.h"

// Optional conversion-time conservative coverage bounds. Byte27 is 0xa0 |
// coordinate shift; bytes28..31 hold left/top/right/bottom in 2^shift units.
// The original virtual extent and registration never change. Missing/legacy
// hints use the existing decoded-pixel path.
static inline bool dg_image_coverage_hint(const uint8_t *data, uint32_t length,
    unsigned ink, bool follow_alpha, unsigned *left, unsigned *top,
    unsigned *right, unsigned *bottom) {
  if (!data || length < 32 || (data[27] & 0xf0u) != 0xa0u ||
      (data[27] & 15u) > 9u || dg_fdi_palette_brush(data) ||
      (dg_fdi_rich_text(data) && ink != 36) ||
      (dg_ink_whole_rect(ink) && !follow_alpha)) return false;
  unsigned width=dg_fdi_be16(data+8), height=dg_fdi_be16(data+10);
  if (!width || !height) return false;
  unsigned shift=data[27]&15u, step=1u<<shift;
  unsigned l=(unsigned)data[28]<<shift, t=(unsigned)data[29]<<shift;
  unsigned r=(unsigned)data[30]<<shift, b=(unsigned)data[31]<<shift;
  if (!l && !t && !r && !b) { *left=*top=*right=*bottom=0; return true; }
  if (r<=l || b<=t || l>=width || t>=height ||
      r>=width+step || b>=height+step) return false;
  *left=l; *top=t; *right=r<width?r:width; *bottom=b<height?b:height;
  return true;
}
#endif
