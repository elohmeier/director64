#ifndef DIRECTOR64_FILM_RENDER_H
#define DIRECTOR64_FILM_RENDER_H
#include "director.h"
#include "film_channels.h"
#include <math.h>
#include <stdlib.h>

#if DG_D10
static inline bool dg_film_container_effects(const dg_member_t *member) {
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  // Original Birthday DIRAPI/IML32 8.5.1r104: native film children remain
  // visible when their containing sprite has blend0. The container supplies
  // geometry; the children's own inks/blends supply their picture effects.
  // Flash Xtras keep their separate authored compositing behavior.
  return member && member->type == 2 && !member->source_xtra;
#else
  (void)member;
  return false;
#endif
}
static inline void dg_film_container_sprite(dg_sprite_t *out,
                                             const dg_sprite_t *source) {
  *out = *source;
  out->value.ink = 0;
  out->value.blend = 100;
  out->value.thickness |= DG_HAS_BLEND;
  out->value.fore_rgb = 0;
  out->value.back_rgb = 0xffffff;
  out->value.flags |= 48;
}
static inline bool dg_film_name_equal(const char *a, const char *b) {
  if (!a || !b) return false;
  while (*a && *b && ((*a | 32) == (*b | 32))) { a++; b++; }
  return !*a && !*b;
}
static inline float dg_film_number(lv_runtime_t *r, lv_t bag, const char *name, float fallback) {
  lv_t value = lv_get(r, NULL, name, bag);
  return lv_type(value) == LV_NUMBER ? (float)lv_numeric(value) : fallback;
}
// Adobe ActionScript2 MovieClip reference: _x/_y locate the registration
// point in the parent's coordinates; size and rotation transform around it.
// https://open-flash.github.io/mirrors/as2-language-reference/MovieClip.html
static inline bool dg_film_layer_sprite(dg_runtime_t *d, unsigned parent,
    const dg_film_layer_t *layer, dg_sprite_t *sprite) {
  if (layer->value.member && !dg_member(d, layer->value.member)) return false;
  *sprite = (dg_sprite_t){.value = layer->value, .visible = true,
      .stretch = layer->value.stretch, .film_frame = layer->film_frame};
  // Direct planes have top-left crop coordinates, whereas a memberless
  // Director sprite rectangle normally registers at its center.
  if (layer->asset && *layer->asset) {
    sprite->value.x = sprite->value.y = 0;
    sprite->custom_quad = true;
    const float left = layer->value.x, top = layer->value.y;
    const float right = left + layer->value.width, bottom = top + layer->value.height;
    const float q[] = {left, top, right, top, right, bottom, left, bottom};
    for (unsigned i = 0; i < 8; i++) sprite->quad[i] = q[i];
  }
  if (!layer->name || !*layer->name) {
    sprite->film_instance=(uint16_t)layer->placement_generation;
    return true;
  }
  lv_t bag = {0};
  for (unsigned i = 0; i < d->flash_object_count; i++)
    if (parent < DG_SPRITES && d->flash_objects[i].sprite == parent &&
        d->flash_objects[i].member == d->sprites[parent].value.member &&
        dg_film_name_equal(d->flash_objects[i].name, layer->name)) {
      bag = d->values->roots[DG_FLASH_ROOT + i]; break;
    }
  if (lv_type(bag) != LV_PROPLIST) return true;
  lv_runtime_t *r = d->values;
  lv_t visible = lv_get(r, NULL, "_visible", bag);
  if (lv_type(visible) != LV_VOID && !lv_truth(r, visible)) return false;
  float origin_x = layer->origin_x / 65536.0f, origin_y = layer->origin_y / 65536.0f;
  float x = dg_film_number(r, bag, "_x", origin_x), y = dg_film_number(r, bag, "_y", origin_y);
  float w = dg_film_number(r, bag, "_width", layer->value.width);
  float h = dg_film_number(r, bag, "_height", layer->value.height);
  float angle = dg_film_number(r, bag, "_rotation", 0);
  float alpha = dg_film_number(r, bag, "_alpha", 100);
  if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) ||
      !isfinite(angle) || !isfinite(alpha) || fabsf(x) > 1000000 || fabsf(y) > 1000000 ||
      fabsf(w) > 1000000 || fabsf(h) > 1000000) {
    lv_fail(r, "invalid Flash child geometry"); return false;
  }
  if (!layer->value.width || !layer->value.height || !w || !h || alpha <= 0) return false;
  if (alpha > 100) alpha = 100;
  sprite->value.blend = (uint8_t)(sprite->value.blend * alpha / 100);
  sprite->value.thickness |= DG_HAS_BLEND;
  float sx = w / layer->value.width, sy = h / layer->value.height;
  float left = layer->value.x - origin_x, top = layer->value.y - origin_y;
  const float px[] = {left, left + layer->value.width, left + layer->value.width, left};
  const float py[] = {top, top, top + layer->value.height, top + layer->value.height};
  float radians = angle * (3.14159265358979323846f / 180.0f);
  float cs = cosf(radians), sn = sinf(radians);
  sprite->value.x = sprite->value.y = 0;
  sprite->custom_quad = true;
  for (unsigned i = 0; i < 4; i++) {
    sprite->quad[i * 2] = x + px[i] * sx * cs - py[i] * sy * sn;
    sprite->quad[i * 2 + 1] = y + px[i] * sx * sn + py[i] * sy * cs;
  }
  return true;
}
// A live Flash field is laid out before its authored placement transform.
// TextField width/height reflow the local plane; scale/rotation transform it.
typedef struct {
  dg_sprite_t sprite;
  unsigned width, height;
} dg_flash_plane_t;
static inline bool dg_flash_field_plane(dg_runtime_t *d, unsigned parent,
    const dg_member_t *member, const dg_flash_field_t *field,
    const dg_film_layer_t *layer, dg_flash_plane_t *out) {
  float left = field->local_bounds[0] / 65536.0f;
  float top = field->local_bounds[1] / 65536.0f;
  float width = field->local_bounds[2] / 65536.0f;
  float height = field->local_bounds[3] / 65536.0f;
  float a = layer->matrix[0] / 65536.0f, b = layer->matrix[1] / 65536.0f;
  float c = layer->matrix[2] / 65536.0f, e = layer->matrix[3] / 65536.0f;
  float x = layer->matrix[4] / 65536.0f, y = layer->matrix[5] / 65536.0f;
  lv_t bag = {0}; unsigned changed = 0;
  for (unsigned i = 0; i < d->flash_object_count; i++)
    if (d->flash_objects[i].sprite == parent && d->flash_objects[i].member == member->id &&
        dg_flash_field_find(member, d->flash_objects[i].name) == field) {
      bag = d->values->roots[DG_FLASH_ROOT + i];
      changed = d->flash_objects[i].overridden; break;
    }
  float alpha = 100;
  if (lv_type(bag) == LV_PROPLIST) {
    lv_runtime_t *r = d->values;
    if (!lv_truth(r, lv_get(r, NULL, "_visible", bag))) return false;
    if (changed & 1) x = dg_film_number(r, bag, "_x", x);
    if (changed & 2) y = dg_film_number(r, bag, "_y", y);
    if (changed & 4) width = dg_film_number(r, bag, "_width", width);
    if (changed & 8) height = dg_film_number(r, bag, "_height", height);
    float original_angle = atan2f(b, a);
    if (changed & 128) {
      float length = hypotf(a, b), scale = dg_film_number(r, bag, "_xscale", length * 100) / 100;
      if (length) { a *= scale / length; b *= scale / length; }
    }
    if (changed & 256) {
      float length = hypotf(c, e), scale = dg_film_number(r, bag, "_yscale", length * 100) / 100;
      if (length) { c *= scale / length; e *= scale / length; }
    }
    if (changed & 64) {
      float angle = dg_film_number(r, bag, "_rotation", original_angle * 180 / 3.14159265358979323846f);
      float delta = angle * 3.14159265358979323846f / 180 - original_angle;
      float cs = cosf(delta), sn = sinf(delta), aa = a, cc = c;
      a = aa * cs - b * sn; b = aa * sn + b * cs;
      c = cc * cs - e * sn; e = cc * sn + e * cs;
    }
    alpha = dg_film_number(r, bag, "_alpha", 100);
  }
  if (!isfinite(width) || !isfinite(height) || !isfinite(x) || !isfinite(y) ||
      !isfinite(a) || !isfinite(b) || !isfinite(c) || !isfinite(e) || !isfinite(alpha) ||
      width > 16384 || height > 16384 || fabsf(x) > 1000000 || fabsf(y) > 1000000) {
    lv_fail(d->values, "invalid Flash field transform"); return false;
  }
  if (width <= 0 || height <= 0 || alpha <= 0 || fabsf(a * e - b * c) < 0.0000001f) return false;
  out->width = (unsigned)ceilf(width); out->height = (unsigned)ceilf(height);
  if ((size_t)out->width * out->height > 16u * 1024u * 1024u) {
    lv_fail(d->values, "Flash field raster exceeds budget"); return false;
  }
  out->sprite = (dg_sprite_t){.value = {.type = 16, .width = out->width, .height = out->height,
      .ink = 8, .blend = (uint8_t)(alpha > 100 ? 100 : alpha), .thickness = DG_HAS_BLEND,
      .flags = 48, .back_rgb = 0xffffff}, .visible = true, .stretch = true, .custom_quad = true};
  const float px[] = {left, left + width, left + width, left};
  const float py[] = {top, top, top + height, top + height};
  for (unsigned i = 0; i < 4; i++) {
    out->sprite.quad[i * 2] = x + a * px[i] + c * py[i];
    out->sprite.quad[i * 2 + 1] = y + b * px[i] + e * py[i];
  }
  return true;
}
// Script edits merge into the selected recorded pose. Channels untouched by
// scripts retain their geometry and order; an empty slot stays addressable.
static inline bool dg_film_driven(const dg_runtime_t *d, unsigned channel) {
  if (channel > 0 && channel < DG_SPRITES)
    for (unsigned i = 0; i < d->film_child_count; i++)
      if (d->film_children[i].parent == channel &&
          d->film_children[i].owner == d->sprites[channel].value.member) return true;
  return false;
}
static inline dg_film_layer_t *dg_film_frame_resolve(dg_runtime_t *d, unsigned parent,
    const dg_sprite_t *sprite, const dg_member_t *member, dg_film_frame_t *frame) {
  *frame = (dg_film_frame_t){0};
  unsigned context=dg_film_prepare(d,parent,sprite,member);
  bool driven=context!=0;
  unsigned pose = dg_film_storage_pose(member, sprite);
  unsigned count = member->film_track_count + (driven ? d->film_child_count : 0);
  if (member->film_frames && member->film_count) count += member->film_frames[pose].count;
  dg_film_layer_t *layers = calloc(count ? count : 1, sizeof(*layers));
  if (!layers) { lv_fail(d->values, "film loop child allocation"); return NULL; }
  dg_film_channels_t channels=dg_film_channels(d,member,pose,context);
  for (unsigned channel; (channel=dg_film_channels_next(&channels));) {
    dg_film_layer_t layer;
    bool present = driven ? dg_film_child_layer(d, context, channel, &layer) :
        dg_film_recorded_layer(d, member, pose, channel, &layer);
    if (present) layers[frame->count++] = layer;
  }
  frame->layers = layers;
  return layers;
}
static inline dg_film_layer_t *dg_film_driven_frame(dg_runtime_t *d, unsigned parent,
    const dg_member_t *member, dg_film_frame_t *frame) {
  return dg_film_frame_resolve(d, parent, &d->sprites[parent], member, frame);
}
#endif
#endif
