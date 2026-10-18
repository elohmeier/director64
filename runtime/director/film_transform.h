#ifndef DIRECTOR64_FILM_TRANSFORM_H
#define DIRECTOR64_FILM_TRANSFORM_H
#include "film_render.h"
#if DG_D10
typedef struct {
  float quad[8], left, top, width, height;
  bool flip_h, flip_v;
} dg_film_transform_t;

// Each quad is expressed in its containing film's raster coordinates. Inverse
// mapping through every ancestor preserves nonaffine nested quads; mapping
// just the four child corners would replace that composition with a different
// bilinear surface.
static inline void dg_film_transform_init(dg_runtime_t *d,
                                          const dg_sprite_t *sprite,
                                          const dg_member_t *member, int ox,
                                          int oy, float scale,
                                          dg_film_transform_t *out) {
  dg_sprite_quad(d, sprite, out->quad);
  for (unsigned point = 0; point < 8; point += 2) {
    out->quad[point] = (out->quad[point] + ox) * scale;
    out->quad[point + 1] = (out->quad[point + 1] + oy) * scale;
  }
  out->left = member->film_left * scale;
  out->top = member->film_top * scale;
  out->width = member->width * scale;
  out->height = member->height * scale;
  out->flip_h = sprite->flip_h;
  out->flip_v = sprite->flip_v;
}
static inline bool dg_film_transform_point(const dg_film_transform_t *stack,
                                           unsigned count, float x, float y,
                                           float *local_x, float *local_y) {
  for (unsigned i = 0; i < count; i++) {
    const dg_film_transform_t *t = &stack[i];
    float u, v;
    if (!dg_quad_uv(t->quad, x, y, &u, &v))
      return false;
    if (t->flip_h)
      u = 1 - u;
    if (t->flip_v)
      v = 1 - v;
    x = t->left + u * t->width;
    y = t->top + v * t->height;
  }
  *local_x = x;
  *local_y = y;
  return true;
}
static inline void dg_film_transform_bounds(const dg_film_transform_t *outer,
                                            int *left, int *top, int *right,
                                            int *bottom) {
  float l = outer->quad[0], t = outer->quad[1], r = l, b = t;
  for (unsigned i = 2; i < 8; i += 2) {
    if (outer->quad[i] < l)
      l = outer->quad[i];
    if (outer->quad[i] > r)
      r = outer->quad[i];
    if (outer->quad[i + 1] < t)
      t = outer->quad[i + 1];
    if (outer->quad[i + 1] > b)
      b = outer->quad[i + 1];
  }
  *left = (int)floorf(l);
  *top = (int)floorf(t);
  *right = (int)ceilf(r);
  *bottom = (int)ceilf(b);
}
static inline bool dg_film_transform_full_identity(const dg_film_transform_t *t,
                                                   unsigned width,
                                                   unsigned height) {
  return !t->flip_h && !t->flip_v && t->left == 0 && t->top == 0 &&
         t->width == width && t->height == height && t->quad[0] == 0 &&
         t->quad[1] == 0 && t->quad[2] == width && t->quad[3] == 0 &&
         t->quad[4] == width && t->quad[5] == height && t->quad[6] == 0 &&
         t->quad[7] == height;
}

// Preserve the original raster lattice as well as its colors. Scaling or
// rotating an already rasterized child changes edge sampling.
static inline bool
dg_film_transform_raster_aligned(const dg_film_transform_t *t) {
  return !t->flip_h && !t->flip_v && floorf(t->left) == t->left &&
         floorf(t->top) == t->top && floorf(t->width) == t->width &&
         floorf(t->height) == t->height && floorf(t->quad[0]) == t->quad[0] &&
         floorf(t->quad[1]) == t->quad[1] &&
         t->quad[2] == t->quad[0] + t->width && t->quad[3] == t->quad[1] &&
         t->quad[4] == t->quad[2] && t->quad[5] == t->quad[1] + t->height &&
         t->quad[6] == t->quad[0] && t->quad[7] == t->quad[5];
}

// Only destination-independent, fully opaque operations may be flattened.
// Partial blend and soft masks use Director's destination-dependent integer
// rounding and must retain their position in the stage's composition order.
static inline bool dg_film_independent_ink(unsigned ink, unsigned opacity) {
  return opacity == 100 && (ink == 0 || ink == 4 || ink == 8 || ink == 9 ||
                            ink == 32 || ink == 36 || ink == 40 || ink == 41);
}
typedef bool (*dg_film_leaf_safe_t)(void *, dg_runtime_t *, const dg_sprite_t *,
                                    const dg_member_t *,
                                    const dg_film_layer_t *);
static inline bool dg_film_isolated_safe(dg_runtime_t *d, unsigned parent,
                                         const dg_member_t *member,
                                         const dg_film_frame_t *frame,
                                         dg_film_leaf_safe_t leaf,
                                         void *context, unsigned depth) {
  if (depth >= 8)
    return false;
  for (unsigned i = 0; i < frame->count; i++) {
    const dg_film_layer_t *layer = &frame->layers[i];
    dg_sprite_t child;
    if (!dg_film_layer_sprite(d, parent, layer, &child))
      continue;
    if (!child.visible || !child.value.type)
      continue;
    const dg_member_t *source = dg_member(d, child.value.member);
    bool direct = layer->asset && *layer->asset;
    if (!direct && !source && layer->name && *layer->name)
      return false;
    if (!direct && dg_film_container_effects(source) &&
        (source->film_tracks || source->film_frames)) {
      dg_film_frame_t nested;
      dg_film_layer_t *owned =
          dg_film_frame_resolve(d, DG_SPRITES, &child, source, &nested);
      if (!owned)
        return false;
      bool safe = dg_film_isolated_safe(d, DG_SPRITES, source, &nested, leaf,
                                        context, depth + 1);
      free(owned);
      if (!safe)
        return false;
      continue;
    }
    if (!direct && dg_film_container_effects(source))
      dg_film_container_sprite(&child, &child);
    if (!dg_opacity(&child))
      continue;
    if (!direct && !source && child.value.type >= 16)
      continue;
    if (!dg_film_independent_ink(child.value.ink, dg_opacity(&child)) ||
        !leaf(context, d, &child, source, layer))
      return false;
  }
  (void)member;
  return !d->values->failed;
}
#endif
#endif
