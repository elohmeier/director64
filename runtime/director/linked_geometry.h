#ifndef DIRECTOR64_LINKED_GEOMETRY_H
#define DIRECTOR64_LINKED_GEOMETRY_H
#include "director.h"
#if DG_D10
// Macromedia Using Director MX2004, p203: Scale fits the sprite rectangle;
// Crop retains the movie's default dimensions, optionally centered in that
// rectangle. The on-disk flags (noCrop=2, center=1) are format facts checked
// against clean ScummVM 41ac2b31847622d0662d22c03fe6979e3b43cfbc.
// Both painting and input start in normalized sprite-quad coordinates.
static inline bool dg_linked_uv(const dg_member_t *member, const dg_sprite_t *sprite,
                                float *u, float *v) {
  if (!(member->linked_flags & 2)) {
    float width = sprite->stretch ? sprite->value.width : member->width;
    float height = sprite->stretch ? sprite->value.height : member->height;
    if (!member->width || !member->height || width <= 0 || height <= 0) return false;
    float left = member->linked_flags & 1 ? (width - member->width) / 2 : 0;
    float top = member->linked_flags & 1 ? (height - member->height) / 2 : 0;
    *u = (*u * width - left) / member->width;
    *v = (*v * height - top) / member->height;
  }
  return *u >= 0 && *u < 1 && *v >= 0 && *v < 1;
}
#endif
#endif
