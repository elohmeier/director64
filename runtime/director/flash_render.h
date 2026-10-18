#ifndef DIRECTOR64_FLASH_RENDER_H
#define DIRECTOR64_FLASH_RENDER_H
#include "flash_runtime.h"
#include "ink.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if DG_D10
// RGBA uses the same low-byte-red layout as Imaging Lingo and the web stage.
// left/top are pixel coordinates at the host's pixels-per-authored-unit scale.
typedef struct {
  int left, top;
  unsigned width, height;
  uint32_t *pixels;
} dg_flash_raster_t;
typedef struct {
  void *context;
  float scale;
  bool (*asset)(void *, const dg_film_layer_t *, dg_flash_raster_t *);
  bool (*text)(void *, dg_flash_instance_t *, const dg_flash_field_t *,
               dg_flash_raster_t *);
} dg_flash_raster_host_t;
typedef struct {
  dg_flash_raster_t source;
  float matrix[6];
  unsigned depth, mask_end;
  bool color;
  int16_t cxform[8];
  float alpha;
} dg_flash_raster_layer_t;
static inline bool dg_flash_field_snapshot(dg_runtime_t *d,
                                           dg_flash_instance_t *instance,
                                           const dg_flash_field_t *field,
                                           dg_flash_field_t *out,
                                           dg_text_style_t *style) {
  if (!field->style) {
    lv_fail(d->values, "Flash text field is missing its font descriptor");
    return false;
  }
  *out = *field;
  *style = *field->style;
  out->style = style;
  dg_avm1_t *vm = dg_flash_vm(instance);
  lv_t object = dg_flash_object(instance), value;
  dg_flash_view_t view;
  dg_flash_view(instance, &view);
  out->x = out->y = 0;
  out->width = (int16_t)ceilf(view.bounds[2]);
  out->height = (int16_t)ceilf(view.bounds[3]);
  unsigned wanted = style->size;
  value = dg_avm1_get(vm, object, "size");
  if (lv_type(value) == LV_NUMBER && lv_numeric(value) > 0 &&
      lv_numeric(value) <= 255)
    wanted = (unsigned)lv_numeric(value);
  value = dg_avm1_get(vm, object, "font");
  if (lv_type(value) == LV_STRING) {
    const char *name = lv_cstr(d->values, value);
    const dg_text_style_t *matched = NULL;
    if (style->font_name && !strcmp(name, style->font_name) &&
        style->font_id != 255 && style->size == wanted)
      matched = field->style;
    for (unsigned loaded = 0; !matched && loaded < d->loaded_count; loaded++) {
      const dg_movie_t *movie = d->loaded[loaded];
      if (!movie)
        continue;
      for (unsigned n = 0; !matched && n < movie->member_count; n++) {
        const dg_member_t *member = &movie->members[n];
        const dg_text_style_t *candidate = member->text_style;
        if (candidate && candidate->font_name && candidate->size == wanted &&
            candidate->font_id != 255 && !strcmp(name, candidate->font_name))
          matched = candidate;
        for (unsigned f = 0; !matched && f < member->flash_field_count; f++) {
          candidate = member->flash_fields[f].style;
          if (candidate && candidate->font_name && candidate->size == wanted &&
              candidate->font_id != 255 && !strcmp(name, candidate->font_name))
            matched = candidate;
        }
        for (unsigned f = 0; !matched && f < member->flash_font_count; f++) {
          candidate = member->flash_fonts[f];
          if (candidate && candidate->font_name && candidate->size == wanted &&
              candidate->font_id != 255 && !strcmp(name, candidate->font_name))
            matched = candidate;
        }
      }
    }
    if (matched) {
      uint32_t color = style->color;
      *style = *matched;
      style->color = color;
    } else
      style->font_id = 255;
  }
  value = dg_avm1_get(vm, object, "textColor");
  if (lv_type(value) == LV_NUMBER)
    style->color = (uint32_t)lv_numeric(value) & 0xffffffu;
  value = dg_avm1_get(vm, object, "wordWrap");
  if (lv_type(value) != LV_VOID)
    out->word_wrap = dg_avm1_truth(vm, value);
  value = dg_avm1_get(vm, object, "multiline");
  if (lv_type(value) != LV_VOID)
    out->multiline = dg_avm1_truth(vm, value);
  value = dg_avm1_get(vm, object, "text");
  if (lv_type(value) == LV_STRING)
    out->text = lv_cstr(dg_avm1_values(vm), value);
  if (out->text && *out->text && style->font_id == 255) {
    lv_fail(d->values,
            "live Flash text font is absent from recovered font inventory");
    return false;
  }
  return true;
}
static bool dg_flash_raster_allocate(dg_runtime_t *d, dg_flash_raster_t *out,
                                     int l, int t, int r, int b) {
  if (r <= l || b <= t) {
    *out = (dg_flash_raster_t){0};
    return true;
  }
  unsigned width = (unsigned)(r - l), height = (unsigned)(b - t);
  if (width > 32767 || height > 32767 ||
      (uint64_t)width * height > 16u * 1024u * 1024u) {
    lv_fail(d->values, "Flash display-list raster dimensions exceeded");
    return false;
  }
  uint32_t *pixels = calloc((size_t)width * height, sizeof(*pixels));
  if (!pixels) {
    lv_fail(d->values, "Flash display-list RGBA allocation");
    return false;
  }
  *out = (dg_flash_raster_t){l, t, width, height, pixels};
  return true;
}
static uint32_t dg_flash_raster_sample(const dg_flash_raster_layer_t *layer,
                                       float x, float y) {
  const float *m = layer->matrix;
  float determinant = m[0] * m[3] - m[1] * m[2];
  if (fabsf(determinant) < 0.0000001f)
    return 0;
  x -= m[4];
  y -= m[5];
  float u = (m[3] * x - m[2] * y) / determinant - layer->source.left;
  float v = (-m[1] * x + m[0] * y) / determinant - layer->source.top;
  if (u < 0 || v < 0 || u >= layer->source.width || v >= layer->source.height)
    return 0;
  uint32_t pixel =
      layer->source
          .pixels[(size_t)(unsigned)v * layer->source.width + (unsigned)u];
  unsigned bytes[4] = {pixel & 255, (pixel >> 8) & 255, (pixel >> 16) & 255,
                       pixel >> 24};
  if (layer->color)
    for (unsigned n = 0; n < 4; n++) {
      int value = (int)bytes[n] * layer->cxform[n] / 256 + layer->cxform[n + 4];
      bytes[n] = (unsigned)(value < 0 ? 0 : value > 255 ? 255 : value);
    }
  bytes[3] = (unsigned)(bytes[3] * layer->alpha / 100);
  return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
}
static bool dg_flash_path_fill(const dg_flash_point_t *points, unsigned count,
                               float x, float y) {
  bool inside = false;
  if (count < 3)
    return false;
  for (unsigned n = 0, previous = count - 1; n < count; previous = n++) {
    float ax = points[n].x, ay = points[n].y, bx = points[previous].x,
          by = points[previous].y;
    if ((ay > y) != (by > y) && x < (bx - ax) * (y - ay) / (by - ay) + ax)
      inside = !inside;
  }
  return inside;
}
static bool dg_flash_path_line(const dg_flash_point_t *points, unsigned count,
                               float x, float y, float radius) {
  for (unsigned n = 1; n < count; n++) {
    float ax = points[n - 1].x, ay = points[n - 1].y, dx = points[n].x - ax,
          dy = points[n].y - ay;
    float length = dx * dx + dy * dy,
          t = length ? ((x - ax) * dx + (y - ay) * dy) / length : 0;
    t = fmaxf(0, fminf(1, t));
    float xx = x - ax - t * dx, yy = y - ay - t * dy;
    if (xx * xx + yy * yy <= radius * radius)
      return true;
  }
  return false;
}
static bool dg_flash_raster_graphics(dg_runtime_t *d,
                                     dg_flash_instance_t *instance, float scale,
                                     bool geometric, dg_flash_raster_t *out) {
  const dg_flash_path_t *paths;
  const dg_flash_point_t *points;
  unsigned count = dg_flash_graphics(instance, &paths, &points);
  *out = (dg_flash_raster_t){0};
  float bounds[4] = {0};
  bool any = false;
  for (unsigned p = 0; p < count; p++) {
    const dg_flash_path_t *path = &paths[p];
    if (geometric && !path->filled)
      continue;
    float margin = path->stroked && !geometric
                       ? fmaxf(path->line_width, 1 / scale) / 2
                       : 0;
    for (unsigned n = 0; n < path->count; n++) {
      dg_flash_point_t pt = points[path->first + n];
      if (!any) {
        bounds[0] = pt.x - margin;
        bounds[1] = pt.y - margin;
        bounds[2] = pt.x + margin;
        bounds[3] = pt.y + margin;
        any = true;
      } else {
        bounds[0] = fminf(bounds[0], pt.x - margin);
        bounds[1] = fminf(bounds[1], pt.y - margin);
        bounds[2] = fmaxf(bounds[2], pt.x + margin);
        bounds[3] = fmaxf(bounds[3], pt.y + margin);
      }
    }
  }
  if (!any)
    return true;
  if (!dg_flash_raster_allocate(d, out, (int)floorf(bounds[0] * scale),
                                (int)floorf(bounds[1] * scale),
                                (int)ceilf(bounds[2] * scale),
                                (int)ceilf(bounds[3] * scale)))
    return false;
  // Sample coverage independently from color. Four-by-four subpixel samples
  // bound the CPU work; original-player edge precision is separately tested.
  for (unsigned p = 0; p < count; p++) {
    const dg_flash_path_t *path = &paths[p];
    if (geometric && !path->filled)
      continue;
    const dg_flash_point_t *pts = points + path->first;
    float radius = fmaxf(path->line_width, 1 / scale) / 2;
    for (unsigned y = 0; y < out->height; y++)
      for (unsigned x = 0; x < out->width; x++) {
        unsigned fill = 0, line = 0;
        for (unsigned sy = 0; sy < 4; sy++)
          for (unsigned sx = 0; sx < 4; sx++) {
            float px = ((float)out->left + x + (sx + 0.5f) / 4) / scale,
                  py = ((float)out->top + y + (sy + 0.5f) / 4) / scale;
            if (path->filled && dg_flash_path_fill(pts, path->count, px, py))
              fill++;
            if (!geometric && path->stroked &&
                dg_flash_path_line(pts, path->count, px, py, radius))
              line++;
          }
        size_t at = (size_t)y * out->width + x;
        if (fill)
          out->pixels[at] =
              dg_rgba_blend(out->pixels[at], path->fill,
                            (geometric ? 255 : path->fill >> 24) * fill / 16);
        if (line)
          out->pixels[at] = dg_rgba_blend(out->pixels[at], path->line,
                                          (path->line >> 24) * line / 16);
      }
  }
  return true;
}
static bool dg_flash_raster_instance(dg_runtime_t *d,
                                     dg_flash_instance_t *instance,
                                     const dg_flash_raster_host_t *host,
                                     dg_flash_raster_t *out, unsigned nesting,
                                     bool root, bool geometric) {
  *out = (dg_flash_raster_t){0};
  dg_flash_view_t view;
  if (!dg_flash_view(instance, &view) ||
      (!geometric && (!view.visible || view.alpha <= 0)))
    return true;
  if (nesting >= 32) {
    lv_fail(d->values, "Flash display-list nesting exceeded");
    return false;
  }
  if (view.field) {
    if (geometric &&
        !dg_avm1_truth(dg_flash_vm(instance),
                       dg_avm1_get(dg_flash_vm(instance),
                                   dg_flash_object(instance), "embedFonts")))
      return true;
    if (!host->text) {
      lv_fail(d->values, "Flash text raster provider unavailable");
      return false;
    }
    return host->text(host->context, instance, view.field, out);
  }
  const dg_flash_path_t *paths;
  const dg_flash_point_t *points;
  unsigned graphics = dg_flash_graphics(instance, &paths, &points) ? 1 : 0;
  unsigned count = dg_flash_layer_count(instance) + graphics;
  dg_flash_raster_layer_t *layers = calloc(count ? count : 1, sizeof(*layers));
  if (!layers) {
    lv_fail(d->values, "Flash display-list layer allocation");
    return false;
  }
  int left = 0, top = 0, right = 0, bottom = 0;
  bool any = false, ok = true;
  for (unsigned n = 0; n < count && ok; n++) {
    dg_flash_layer_view_t item = {0};
    if (n >= graphics && !dg_flash_layer_at(instance, n - graphics, &item))
      continue;
    dg_flash_raster_layer_t *layer = &layers[n];
    const dg_film_layer_t *p = item.layer;
    dg_flash_view_t child = {0};
    bool live = item.instance && dg_flash_view(item.instance, &child);
    layer->depth = item.depth;
    layer->mask_end = p ? p->clip_depth : 0;
    bool mask = geometric || layer->mask_end;
    layer->alpha = live && !mask ? child.alpha : 100;
    if (live && !child.visible && !mask)
      continue;
    layer->matrix[0] = layer->matrix[3] = 1;
    if (live)
      memcpy(layer->matrix, child.matrix, sizeof(layer->matrix));
    else if (p && p->affine)
      for (unsigned k = 0; k < 6; k++)
        layer->matrix[k] = p->matrix[k] / 65536.0f;
    layer->matrix[4] *= host->scale;
    layer->matrix[5] *= host->scale;
    if (!mask && p && p->has_cxform) {
      layer->color = true;
      memcpy(layer->cxform, p->cxform, sizeof(layer->cxform));
    }
    if (!mask && live && child.has_cxform) {
      layer->color = true;
      memcpy(layer->cxform, child.cxform, sizeof(layer->cxform));
    }
    if (graphics && !n)
      ok = dg_flash_raster_graphics(d, instance, host->scale, mask,
                                    &layer->source);
    else if (live && (!p || p->clip_id || p->button_id || p->field_id))
      ok = dg_flash_raster_instance(d, item.instance, host, &layer->source,
                                    nesting + 1, false, mask);
    else if (p && p->asset && *p->asset) {
      dg_film_layer_t raster = *p;
      if (mask) {
        if (!p->mask_asset || !*p->mask_asset) {
          lv_fail(d->values, "Flash geometric mask asset is missing");
          ok = false;
          continue;
        }
        raster.asset = p->mask_asset;
      }
      ok = host->asset(host->context, &raster, &layer->source);
    } else if (p && (p->clip_id || p->field_id || p->button_id)) {
      lv_fail(d->values, "Flash display-list instance missing at render");
      ok = false;
    }
    if (!ok || !layer->source.pixels)
      continue;
    const dg_flash_raster_t *s = &layer->source;
    float *m = layer->matrix;
    float px[4] = {(float)s->left, (float)s->left + s->width,
                   (float)s->left + s->width, (float)s->left};
    float py[4] = {(float)s->top, (float)s->top, (float)s->top + s->height,
                   (float)s->top + s->height};
    for (unsigned k = 0; k < 4; k++) {
      float x = m[0] * px[k] + m[2] * py[k] + m[4],
            y = m[1] * px[k] + m[3] * py[k] + m[5];
      if (!isfinite(x) || !isfinite(y) || fabsf(x) > 1000000 ||
          fabsf(y) > 1000000) {
        lv_fail(d->values, "invalid Flash raster transform");
        ok = false;
        break;
      }
      int l = (int)floorf(x), t = (int)floorf(y), r = (int)ceilf(x),
          b = (int)ceilf(y);
      if (!any) {
        left = l;
        right = r;
        top = t;
        bottom = b;
        any = true;
      } else {
        if (l < left)
          left = l;
        if (r > right)
          right = r;
        if (t < top)
          top = t;
        if (b > bottom)
          bottom = b;
      }
    }
  }
  if (root) {
    left = top = 0;
    right = (int)ceilf(view.member->width * host->scale);
    bottom = (int)ceilf(view.member->height * host->scale);
    any = true;
  }
  if (ok && any)
    ok = dg_flash_raster_allocate(d, out, left, top, right, bottom);
  if (ok && out->pixels)
    for (unsigned n = 0; n < count; n++) {
      const dg_flash_raster_layer_t *layer = &layers[n];
      if (!layer->source.pixels || layer->mask_end)
        continue;
      for (unsigned y = 0; y < out->height; y++)
        for (unsigned x = 0; x < out->width; x++) {
          float px = (float)out->left + x + 0.5f,
                py = (float)out->top + y + 0.5f;
          uint32_t source = dg_flash_raster_sample(layer, px, py);
          unsigned alpha = source >> 24;
          if (!alpha)
            continue;
          for (unsigned mask = 0; mask < n && alpha; mask++)
            if (layers[mask].mask_end >= layer->depth && layers[mask].mask_end)
              alpha = alpha *
                      (dg_flash_raster_sample(&layers[mask], px, py) >> 24) /
                      255;
          if (alpha) {
            size_t at = (size_t)y * out->width + x;
            out->pixels[at] = dg_rgba_blend(out->pixels[at], source, alpha);
          }
        }
    }
  for (unsigned n = 0; n < count; n++)
    free(layers[n].source.pixels);
  free(layers);
  if (!ok) {
    free(out->pixels);
    *out = (dg_flash_raster_t){0};
  }
  return ok;
}
static bool dg_flash_render(dg_runtime_t *d, unsigned sprite,
                            const dg_flash_raster_host_t *host,
                            dg_flash_raster_t *out) {
  *out = (dg_flash_raster_t){0};
  dg_flash_instance_t *root = dg_flash_root(d, sprite);
  if (!root) {
    lv_fail(d->values, "Flash root instance missing at render");
    return false;
  }
  return dg_flash_raster_instance(d, root, host, out, 0, true, false);
}
#endif
#endif
