#ifndef DIRECTOR64_FILM_BOUNDS_H
#define DIRECTOR64_FILM_BOUNDS_H
#include "film_render.h"
#include "ink.h"
#if DG_D10
typedef struct {
  unsigned left, top, width, height;
} dg_film_bounds_t;

// Bound only providers whose rendering is clipped to the sprite rectangle or
// quad. Flash fields/display lists may extend beyond their declared member,
// so they deliberately keep the complete intermediate. The resolved frame
// already contains live channel edits and independent nested-film poses.
static inline dg_film_bounds_t dg_film_bounds(dg_runtime_t *d, unsigned channel,
                                              const dg_member_t *member,
                                              const dg_film_frame_t *frame,
                                              unsigned full_width,
                                              unsigned full_height) {
  dg_film_bounds_t full = {0, 0, full_width, full_height};
  if (!full_width || !full_height)
    return (dg_film_bounds_t){0};
  if (!member || !frame || member->source_xtra || member->flash_field_count ||
      member->flash_clip_count)
    return full;
  int64_t left = full_width, top = full_height, right = 0, bottom = 0;
  for (unsigned i = 0; i < frame->count; i++) {
    const dg_film_layer_t *layer = &frame->layers[i];
    if (layer->affine || (layer->name && *layer->name))
      return full;
    if (!layer->value.type)
      continue;
    dg_sprite_t child;
    if (!dg_film_layer_sprite(d, channel, layer, &child)) {
      if (d->values->failed)
        return full;
      continue; // The renderer also skips an unresolved cast slot.
    }
    const dg_member_t *source = dg_member(d, child.value.member);
    bool direct = layer->asset && *layer->asset;
    if (!child.visible || (!dg_opacity(&child) &&
        (direct || !dg_film_container_effects(source))))
      continue;
    if (!direct && source) {
      if (source->type == 4 || source->type == 6 || source->type == 11 ||
          source->type == 14)
        continue;
      if (source->source_xtra || source->flash_field_count ||
          source->flash_clip_count)
        return full;
      switch (source->type) {
      case 1:
      case 2:
      case 3:
      case 5:
      case 7:
      case 8:
      case 9:
      case 10:
      case 12:
        break;
      default:
        return full;
      }
    } else if (!direct && !source && child.value.type >= 16)
      continue; // Empty native template channel.
    int l, t, r, b;
    dg_sprite_bounds(d, &child, &l, &t, &r, &b);
    if (r <= l || b <= t)
      continue;
    // Integer translation before prescaling matches draw_scene_frame. A
    // transformed bound gets one extra physical pixel for float rounding
    // when the renderer translates its quad into the intermediate's space.
    int pad = child.custom_quad || child.value.rotation || child.value.skew;
    int64_t cl = (int64_t)(l - member->film_left) - pad;
    int64_t ct = (int64_t)(t - member->film_top) - pad;
    int64_t cr = (int64_t)(r - member->film_left) + pad;
    int64_t cb = (int64_t)(b - member->film_top) + pad;
    if (cl < 0)
      cl = 0;
    if (ct < 0)
      ct = 0;
    if (cr > full_width)
      cr = full_width;
    if (cb > full_height)
      cb = full_height;
    if (cr <= cl || cb <= ct)
      continue;
    if (cl < left)
      left = cl;
    if (ct < top)
      top = ct;
    if (cr > right)
      right = cr;
    if (cb > bottom)
      bottom = cb;
  }
  if (right <= left || bottom <= top)
    return (dg_film_bounds_t){0};
  return (dg_film_bounds_t){(unsigned)left, (unsigned)top,
                            (unsigned)(right - left), (unsigned)(bottom - top)};
}
#endif
#endif
