#include "compositor.h"
#include "linked_geometry.h"
#include "bitmap_tiles.h"
#include "ink.h"
#include "film_render.h"
#include "film_bounds.h"
#include "image_plan.h"
#include "flash_render.h"
#include "shape.h"
#include <stdio.h>
#ifdef DIRECTOR64_DEV_LENIENT
#define dev_print(...) fprintf(stderr, __VA_ARGS__)
#endif
#include <stdlib.h>
#include <string.h>
#include "compositor_cache.h"
#include "compositor_film.h"
#include "compositor_prefix.h"

static uint32_t *target(wc_t *c) { return c->target ? c->target : c->pixels; }

static inline uint32_t rgba(unsigned r, unsigned g, unsigned b, unsigned a) {
  return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24;
}
static inline unsigned five(unsigned v) { return (v << 3) | (v >> 2); }
static inline uint32_t from5551(uint16_t v) {
  return rgba(five((v >> 11) & 31), five((v >> 6) & 31), five((v >> 1) & 31),
              (v & 1) ? 255 : 0);
}
static inline uint32_t from8888(uint32_t v) {
  return rgba(v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255);
}
static inline uint32_t opaque_rgb(uint32_t rgb) {
  return rgba(rgb >> 16, (rgb >> 8) & 255, rgb & 255, 255);
}

bool wc_decode(const uint8_t *data, uint32_t length, unsigned width,
               unsigned height, unsigned ink, uint32_t *out,
               bool *follow_alpha) {
  if (length < 32 || !width || !height || width > UINT16_MAX ||
      height > UINT16_MAX)
    return false;
  bool rgba32 = !memcmp(data, "FDI2", 4), planes = !memcmp(data, "FDIA", 4);
  bool indexed = !memcmp(data, "FDIC", 4), plain = !memcmp(data, "FDI1", 4);
  uint32_t alpha_offset = 0, index_offset = 0;
  unsigned bits = 0, colors = 0;
  uint64_t pixels = bitmap_plane_pixels(width, height);
  bool valid;
  if (planes)
    valid = dg_fdia_valid(data, length, width, height, &alpha_offset);
  else if (indexed)
    valid = dg_fdic_valid(data, length, width, height, &bits, &colors,
                          &index_offset);
  else
    valid = (plain || rgba32) &&
            32u + pixels * (rgba32 ? 4u : 2u) == (uint64_t)length &&
            dg_fdi_be32(data + 4) == length &&
            dg_fdi_be16(data + 8) == width && dg_fdi_be16(data + 10) == height;
  if (!valid)
    return false;
  bool follow = dg_fdi_follow_alpha(data);
  bool rich = dg_fdi_rich_text(data);
#if !DG_MODERN
  // Coverage-carrying images are a D7-and-later form; plain D6 reads the
  // matte bit only.
  follow = false;
#endif
  if (rich) follow = ink == 36;
  *follow_alpha = follow;
  bool tiled = bitmap_needs_tiles(width, height);
  for (unsigned y = 0; y < height; y++)
    for (unsigned x = 0; x < width; x++) {
      uint64_t i = tiled ? bitmap_tile_index(width, x, y)
                         : (uint64_t)y * width + x;
      uint32_t pixel;
      if (indexed && dg_fdi_palette_brush(data)) {
        pixel=0xff000000u|dg_fdic_index(data,bits,index_offset,width,height,x,y);
      } else if (indexed) {
        uint16_t word = dg_fdic_texel(data, bits, index_offset, width, height,
                                      x, y);
        pixel = from5551(rich ? word : dg_bitmap_ink16(word, ink, follow));
      } else if (rgba32) {
        uint32_t raw = dg_fdi_be32(data + 32 + i * 4);
        pixel = from8888(rich ? raw : dg_bitmap_ink32(raw, ink, follow));
      } else if (planes) {
        uint16_t word = (uint16_t)dg_fdi_be16(data + 32 + i * 2);
        uint8_t alpha = rich ? data[alpha_offset + i] : dg_bitmap_alpha16(word, data[alpha_offset + i], ink);
        pixel = (from5551(word) & 0xffffffu) | (uint32_t)alpha << 24;
      } else {
        uint16_t word = (uint16_t)dg_fdi_be16(data + 32 + i * 2);
        pixel = from5551(rich ? word : dg_bitmap_ink16(word, ink, follow));
      }
      if (rich) {
        uint32_t packed = (pixel & 255) << 24 | (pixel & 0xff00) << 8 |
                          (pixel >> 16 & 255) << 8 | pixel >> 24;
        pixel = from8888(dg_rich_pixel32(packed, data, ink));
      }
      out[(size_t)y * width + x] = pixel;
    }
  return true;
}

void wc_init(wc_t *c, wc_host_t host, size_t image_budget) {
  memset(c, 0, sizeof(*c));
  c->host = host;
  c->image_budget = image_budget;
}
static void evict(wc_t *c, wc_image_t *image) {
  c->image_bytes -= image->bytes;
  free(image->pixels);
  memset(image, 0, sizeof(*image));
  c->evictions++;
}
void wc_release(wc_t *c) {
  free(c->palette_previous);
  c->palette_previous = NULL;
#if DG_D10
  for (unsigned i=0;i<8;i++) { free(c->film_scratch[i]); c->film_scratch[i]=NULL; }
  c->target=NULL; c->film_depth=0;
#endif
  while (c->planes) plane_evict(c, c->planes);
#if DG_D10
  free(c->stage_snapshot.pixels);
  memset(&c->stage_snapshot, 0, sizeof(c->stage_snapshot));
  c->snapshot_member = 0;
#endif
  for (unsigned i = 0; i < WC_IMAGE_SLOTS; i++)
    if (c->images[i].pixels)
      evict(c, &c->images[i]);
  for (unsigned i = 0; i < WC_TEXT_SLOTS; i++) {
    free(c->texts[i].coverage);
    free(c->texts[i].runs);
  }
  memset(c->texts, 0, sizeof(c->texts));
}
// Least recently used, never an image the composite in progress has drawn.
static bool evict_oldest(wc_t *c) {
  wc_image_t *oldest = NULL;
  wc_cached_plane_t *derived = NULL;
  for (wc_cached_plane_t *p = c->planes; p; p = p->next)
    // Derived planes are consumed immediately by draw_bitmap, which cannot
    // load another asset. They need no frame-long pin after that CPU draw.
    if (!oldest || p->image.last < oldest->last) {
      oldest = &p->image; derived = p;
    }
  for (unsigned i = 0; i < WC_IMAGE_SLOTS; i++) {
    wc_image_t *image = &c->images[i];
    if (image->pixels && image->last != c->serial &&
        (!oldest || image->last < oldest->last)) {
      oldest = image; derived = NULL;
    }
  }
  if (!oldest)
    return false;
  if (derived) plane_evict(c, derived);
  else evict(c, oldest);
  return true;
}

#if defined(DIRECTOR64_WEBP) && DIRECTOR64_WEBP
#include "webp_image.h"
#endif
// An image asset's stored bytes, malloc'd: an image the importer packed as
// WebP ("D64W", webp_image.c) decodes back to exactly them. NULL when the
// asset is missing or damaged.
static uint8_t *read_image(wc_t *c, const char *asset, uint32_t *length) {
  *length = 0;
  uint32_t n = asset && *asset ? c->host.read(c->host.ctx, asset, NULL, 0) : 0;
  uint8_t *data = n ? malloc(n) : NULL;
  if (!data || c->host.read(c->host.ctx, asset, data, n) != n) {
    free(data);
    return NULL;
  }
#if defined(DIRECTOR64_WEBP) && DIRECTOR64_WEBP
  if (dg_webp_packed(data, n)) {
    uint8_t *fdi = dg_webp_fdi(data, n, length);
    free(data);
    return fdi;
  }
#endif
  *length = n;
  return data;
}
static void image_coverage(wc_image_t *image) {
  image->coverage_scanned = true;
  image->binary_coverage = true;
  image->cover_l = image->width;
  image->cover_t = image->height;
  image->cover_r = image->cover_b = 0;
  for (unsigned y = 0; y < image->height; y++)
    for (unsigned x = 0; x < image->width; x++) {
      unsigned alpha = image->pixels[(size_t)y * (image->stride ? image->stride : image->width) + x] >> 24;
      if (alpha && alpha != 255) image->binary_coverage = false;
      if (alpha) {
        if (x < image->cover_l) image->cover_l = x;
        if (x >= image->cover_r) image->cover_r = x + 1;
        if (y < image->cover_t) image->cover_t = y;
        image->cover_b = y + 1;
      }
    }
}

const wc_image_t *wc_image(wc_t *c, lv_runtime_t *values, const char *asset,
                           unsigned width, unsigned height, unsigned ink) {
  if (!asset || !*asset || strlen(asset) >= sizeof(c->images[0].name)) {
    lv_fail(values, "missing native image file");
    return NULL;
  }
  wc_image_t *free_slot = NULL;
  for (unsigned i = 0; i < WC_IMAGE_SLOTS; i++) {
    wc_image_t *image = &c->images[i];
    if (!image->pixels) {
      if (!free_slot)
        free_slot = image;
      continue;
    }
    if (image->ink == ink && !strcmp(image->name, asset)) {
      image->last = c->serial;
      return image;
    }
  }
  // A pose cut to its coverage (film.rs trim) gives its own size.
  uint8_t *data = NULL;
  uint32_t length = 0;
  if (!width && !height) {
    data = read_image(c, asset, &length);
    if (!data || length < 32) {
      free(data);
      lv_fail(values, "missing native image file");
      return NULL;
    }
    width = dg_fdi_be16(data + 8);
    height = dg_fdi_be16(data + 10);
  }
  size_t bytes = (size_t)width * height * 4;
  while ((c->image_bytes + bytes > c->image_budget || !free_slot) &&
         evict_oldest(c))
    if (!free_slot)
      for (unsigned i = 0; i < WC_IMAGE_SLOTS && !free_slot; i++)
        if (!c->images[i].pixels)
          free_slot = &c->images[i];
  if (!free_slot) {
    free(data);
    lv_fail(values, "image cache handles exhausted");
    return NULL;
  }
  // The base-image budget is a soft target: assets used by this render stay
  // pinned until it completes. Preserve that working-set policy when every
  // remaining image is pinned; optional derived planes never over-admit.
  if (!data) {
    data = read_image(c, asset, &length);
    if (!data) {
      lv_fail(values, "missing native image file");
      return NULL;
    }
  }
  uint32_t *pixels = malloc(bytes ? bytes : 4);
  bool follow = false;
  bool ok = data && pixels && wc_decode(data, length, width, height, ink, pixels, &follow);
  int reg_x = ok ? (int16_t)dg_fdi_be16(data + 12) : 0, reg_y = ok ? (int16_t)dg_fdi_be16(data + 14) : 0;
  unsigned depth = ok ? dg_fdi_depth(data) : 0;
  bool rgb555 = ok && memcmp(data, "FDI2", 4);
  bool rich_text = ok && dg_fdi_rich_text(data);
  bool palette_brush = ok && dg_fdi_palette_brush(data);
  unsigned cover_l=0, cover_t=0, cover_r=0, cover_b=0;
  bool planned = ok && dg_image_coverage_hint(data, length, ink, follow, &cover_l, &cover_t, &cover_r, &cover_b);
  free(data);
  if (!ok) {
#ifdef DIRECTOR64_DEV_LENIENT
    dev_print( "DEV invalid native image %s %ux%u ink %u length %u\n", asset, width, height, ink, length);
#endif
    free(pixels);
    if (data && pixels) {
      char message[160];
      snprintf(message,sizeof(message),"invalid native image: %.40s expected=%ux%u bytes=%u",asset,width,height,length);
      lv_fail(values,message);
    } else lv_fail(values,"native image allocation");
    return NULL;
  }
  wc_image_t *image = free_slot;
  memcpy(image->name, asset, strlen(asset) + 1);
  image->ink = ink;
  image->width = width;
  image->height = height;
  image->depth = depth;
  image->rgb555 = rgb555;
  image->rich_text = rich_text;
  image->palette_brush = palette_brush;
  image->follow_alpha = follow;
  image->pixels = pixels;
  image->bytes = bytes;
  image->last = c->serial;
  image->reg_x = reg_x;
  image->reg_y = reg_y;
  if (planned) { image->cover_l=cover_l; image->cover_t=cover_t; image->cover_r=cover_r; image->cover_b=cover_b; }
  else image_coverage(image);
  c->image_bytes += bytes;
  c->loads++;
  return image;
}


bool wc_hit(wc_t *c, lv_runtime_t *values, const dg_member_t *m, unsigned ink,
            int x, int y) {
  // Director only uses the bitmap matte for matte ink. Background-transparent
  // sprites still receive input throughout their rectangular bounds.
  if (ink != 8)
    return true;
  if (x < 0 || y < 0 || x >= m->width || y >= m->height)
    return false;
  const wc_image_t *image = wc_image(c, values, m->asset, m->width, m->height, ink);
  return image && (image->pixels[(size_t)y * image->width + (unsigned)x] >> 24);
}

#if DG_D10
// Director's mask ink: the cast member after the sprite's is its mask, and
// light mask pixels make the image transparent; a neighbour that is no mask
// leaves the image as it is (ink.h dg_mask_pair).
static bool mask_pair(const dg_member_t *member, const dg_member_t *mask) {
  return dg_mask_pair(member->name, mask->name, (int)mask->width - (int)member->width,
                      (int)mask->height - (int)member->height);
}
static void apply_mask(wc_t *c, dg_runtime_t *d, const dg_member_t *member, const dg_member_t *mask,
                       wc_image_t *image) {
  unsigned width, height;
  width = mask->width; height = mask->height;
  const dg_pixel_image_t *mutable = dg_member_image(d, mask->id);
  dg_member_t mask_info;
  const uint32_t *mask_pixels;
  if (mutable) {
    mask_info = *mask; mask = &mask_info;
    mask_info.width = width = mutable->width; mask_info.height = height = mutable->height;
    mask_info.reg_x = mutable->reg_x; mask_info.reg_y = mutable->reg_y;
    mask_pixels = mutable->pixels;
  } else {
    const wc_image_t *mask_image = wc_image(c, d->values, mask->asset, width, height, 8);
    if (!mask_image) return;
    mask_pixels = mask_image->pixels;
  }
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  // Director 8.5 masks are trimmed to their own bounds and placed by
  // registration point; the trimmed-away white is transparent.
  int offset_x = member->reg_x - mask->reg_x, offset_y = member->reg_y - mask->reg_y;
  bool trimmed = mask->width != member->width || mask->height != member->height ||
                 offset_x || offset_y;
#else
  (void)member;
  const bool trimmed = false;
  const int offset_x = 0, offset_y = 0;
#endif
  // Authored masks can differ by a pixel from their image; sampling clamps
  // over the overlap, like the original.
  unsigned full_width = image->source_width ? image->source_width : image->width;
  unsigned full_height = image->source_height ? image->source_height : image->height;
  for (unsigned y = 0; y < image->height; y++) {
    int64_t py = (int64_t)y + (image->source_height ? image->source_y : 0);
    unsigned my = py >= 0 && py < height ? (unsigned)py : height - 1;
    // Preserve both original integer sampling stages: full-plane pixel to
    // authored member coordinate, then authored mask coordinate to texel.
    // Combining the divisions changes phase for nonintegral prescaling.
    int64_t ay = py * member->height / full_height - offset_y;
    if (trimmed && (ay < 0 || ay >= mask->height)) {
      for (unsigned x = 0; x < image->width; x++)
        image->pixels[(size_t)y * image->width + x] &= 0x00ffffffu;
      continue;
    }
    if (trimmed)
      my = (unsigned)(ay * height / mask->height);
    for (unsigned x = 0; x < image->width; x++) {
      int64_t px = (int64_t)x + (image->source_width ? image->source_x : 0);
      unsigned mx = px >= 0 && px < width ? (unsigned)px : width - 1;
      if (trimmed) {
        int64_t ax = px * member->width / full_width - offset_x;
        if (ax < 0 || ax >= mask->width) {
          image->pixels[(size_t)y * image->width + x] &= 0x00ffffffu;
          continue;
        }
        mx = (unsigned)(ax * width / mask->width);
      }
      uint32_t mask_pixel = mask_pixels[(size_t)my * width + mx];
      uint32_t *pixel = &image->pixels[(size_t)y * image->width + x];
      unsigned alpha = dg_mask_alpha(mask_pixel, *pixel >> 24, image->follow_alpha);
      *pixel = (*pixel & 0x00ffffffu) | (uint32_t)alpha << 24;
    }
  }
  image_coverage(image);
}

typedef struct {
  char source[40], mask[40];
  unsigned width, height, source_width, source_height;
  int source_x, source_y;
  uint16_t member_width, member_height, mask_width, mask_height;
  int16_t member_x, member_y, mask_x, mask_y;
  bool follow_alpha;
} wc_mask_key_t;

static const wc_image_t *masked_image(wc_t *c, dg_runtime_t *d, const dg_member_t *member,
    const dg_member_t *mask, const wc_image_t *source, wc_image_t *temporary) {
  wc_mask_key_t key = {0};
  bool cacheable = source->name[0] && !source->palette_brush && mask->asset &&
      strlen(mask->asset) < sizeof(key.mask) && !dg_member_image(d, mask->id);
  if (cacheable) {
    memcpy(key.source, source->name, sizeof(key.source));
    memcpy(key.mask, mask->asset, strlen(mask->asset) + 1);
    key.width=source->width; key.height=source->height;
    key.source_width=source->source_width; key.source_height=source->source_height;
    key.source_x=source->source_x; key.source_y=source->source_y;
    key.member_width=member->width; key.member_height=member->height;
    key.mask_width=mask->width; key.mask_height=mask->height;
    key.member_x=member->reg_x; key.member_y=member->reg_y;
    key.mask_x=mask->reg_x; key.mask_y=mask->reg_y;
    key.follow_alpha=source->follow_alpha;
    const wc_image_t *cached = plane_find(c, 1, &key, sizeof(key));
    if (cached) return cached;
  }
  *temporary = *source;
  temporary->pixels = malloc((size_t)source->width * source->height * sizeof(*source->pixels));
  if (!temporary->pixels) { lv_fail(d->values, "mask image allocation"); return NULL; }
  memcpy(temporary->pixels, source->pixels,
         (size_t)source->width * source->height * sizeof(*source->pixels));
  apply_mask(c, d, member, mask, temporary);
  if (d->values->failed) return temporary;
  if (cacheable) {
    const wc_image_t *cached = plane_keep(c, 1, &key, sizeof(key), 0, temporary);
    if (cached) { temporary->pixels = NULL; return cached; }
  }
  return temporary;
}
static bool film_leaf_safe(void *context, dg_runtime_t *d,
    const dg_sprite_t *sprite, const dg_member_t *member,
    const dg_film_layer_t *layer) {
  wc_t *c = context;
  dg_member_t direct;
  if (layer->asset && *layer->asset) {
    direct = (dg_member_t){.type = 1, .asset = layer->asset,
        .width = layer->value.width, .height = layer->value.height};
    member = &direct;
  }
  if (!member) return sprite->value.type < 16;
  if (member->source_xtra || member->flash_field_count || member->flash_clip_count ||
      dg_member_image(d, member->id)) return false;
  if (member->type == 4 || member->type == 6 || member->type == 11 || member->type == 14)
    return true;
  unsigned ink = sprite->value.ink, width, height;
  const char *asset = member->asset;
  if (member->type == 8) {
    asset = dg_shape_texture(d, member, dg_member_pattern(d, member));
    if (!asset || !*asset) return true;
    width = height = ink = 0;
  } else if (member->type == 1 || member->type == 5 || member->type == 12) {
    width = member->width; height = member->height;
    if (ink == 36 && member->type != 12) ink = 0;
  } else if (member->type == 2) {
    asset = dg_film_asset(member, sprite);
    width = height = 0;
  } else return false;
  // Mask starts from the same matte-decoded bitmap as the actual draw path.
  if (ink == 9) ink = 8;
  const wc_image_t *source = wc_image(c, d->values, asset, width, height, ink);
  if (!source) return false;
  wc_image_t temporary = {0};
  if (sprite->value.ink == 9) {
    const dg_member_t *mask = dg_member(d, member->id + 1);
    if (mask && mask->type == 1 && mask_pair(member, mask)) {
      if (dg_member_image(d, mask->id)) return false;
      source = masked_image(c, d, member, mask, source, &temporary);
      if (!source) return false;
    }
  }
  // These records are owned mutable cache entries, even when returned through
  // a const view. Coverage hints avoid the initial scan; qualify binary alpha
  // once if a native-film composition needs this stronger property.
  if (!source->coverage_scanned) image_coverage((wc_image_t *)source);
  bool safe = source->binary_coverage;
  free(temporary.pixels);
  return safe;
}
static void film_destination_bounds(const wc_t *c, int *l, int *t, int *r, int *b) {
  if (c->film_transform_count)
    dg_film_transform_bounds(c->film_transform, l, t, r, b);
  if (*l < 0) *l = 0;
  if (*t < 0) *t = 0;
  if (*r > WC_WIDTH) *r = WC_WIDTH;
  if (*b > WC_HEIGHT) *b = WC_HEIGHT;
}
#endif

// ---- Drawing ----

static inline void blend(uint32_t *d, uint32_t s, unsigned alpha) {
  *d = dg_rgba_blend(*d,s,alpha);
}
static void fill(wc_t *c, int l, int t, int r, int b, uint32_t color) {
  if (l < 0) l = 0;
  if (t < 0) t = 0;
  if (r > WC_WIDTH) r = WC_WIDTH;
  if (b > WC_HEIGHT) b = WC_HEIGHT;
  for (int y = t; y < b; y++)
    for (int x = l; x < r; x++)
      target(c)[y * WC_WIDTH + x] = color;
}
#if DG_MODERN
// Director's inks that combine the source with the stage per pixel, as
// ScummVM's 32-bit path computes them (graphics.cpp): Darkest picks each RGB
// minimum; Transparent and Not Transparent are the 8-bit palette index ORs
// that, with white at index 0 and black at 255, become ANDs of the RGB
// (black source darkens, white leaves the stage; Not Transparent the
// reverse); Subtract Pin subtracts and stops at black. An enabled sprite
// blend takes precedence over the ink, and only the source's coverage is
// drawn.
static inline bool arithmetic_ink(unsigned ink) {
  return dg_ink_arithmetic(ink);
}
static inline void arithmetic(uint32_t *d, uint32_t s, unsigned ink, unsigned coverage,
                              unsigned opacity, uint32_t fore, uint32_t back) {
  *d = dg_rgba_ink(*d,s,ink,coverage,opacity,fore,back);
}
#endif

static void draw_bitmap(wc_t *c, dg_runtime_t *d, const dg_sprite_t *s,
                        const wc_image_t *image, int l, int t, int r, int b,
                        unsigned ink, unsigned opacity, int ox, int oy) {
  bool transformed = false;
  float q[8];
#if DG_MODERN
  transformed = s->value.rotation || s->value.skew || s->flip_h || s->flip_v ||
                s->custom_quad;
  if (transformed) {
    dg_sprite_quad(d, s, q);
    for (unsigned k = 0; k < 8; k += 2) {
      q[k] += (float)ox;
      q[k + 1] += (float)oy;
    }
  }
#else
  (void)d;
  (void)s;
  (void)ink;
  (void)ox;
  (void)oy;
#endif
  int cl = l < 0 ? 0 : l, ct = t < 0 ? 0 : t;
  int cr = r > WC_WIDTH ? WC_WIDTH : r, cb = b > WC_HEIGHT ? WC_HEIGHT : b;
#if DG_D10
  film_destination_bounds(c, &cl, &ct, &cr, &cb);
#endif
  unsigned w = image->source_width ? image->source_width : image->width;
  unsigned h = image->source_height ? image->source_height : image->height;
  unsigned stride = image->stride ? image->stride : image->width;
  int source_x = image->source_width ? image->source_x : 0;
  int source_y = image->source_height ? image->source_y : 0;
  int64_t span_x = (int64_t)r - l, span_y = (int64_t)b - t;
#if DG_MODERN
  bool plain = !transformed && !arithmetic_ink(ink) && ink != 36;
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  uint32_t foreground = s->value.flags & 16 ? s->value.fore_rgb : dg_stage_palette(d)[s->value.fore & 255];
  uint32_t background = s->value.flags & 32 ? s->value.back_rgb : dg_stage_palette(d)[s->value.back & 255];
  bool controlled = opacity < 100 || foreground != 0 || background != 0xffffffu;
  if (controlled || ink == 9) plain = false;
#endif
#else
  bool plain = !transformed;
#endif
#if DG_D10
  if (c->film_transform_count) plain = false;
  if (image->linked_member && !(image->linked_member->linked_flags & 2)) plain = false;
#endif
  if (plain) {
    // The common case: each column's source mapped once, rows and columns
    // outside the image's covered rectangle skipped, opaque pixels stored.
    static unsigned columns[WC_WIDTH];
    int first = cr, last = cl;
    for (int x = cl; x < cr; x++) {
      unsigned sx = (unsigned)(((int64_t)(x - l) * 2 + 1) * w / (span_x * 2));
      if (sx >= w)
        sx = w - 1;
      columns[x] = sx - (unsigned)source_x;
      if ((int64_t)sx >= (int64_t)source_x + image->cover_l &&
          (int64_t)sx < (int64_t)source_x + image->cover_r) {
        if (x < first)
          first = x;
        last = x + 1;
      }
    }
    for (int y = ct; y < cb && first < last; y++) {
      unsigned sy = (unsigned)(((int64_t)(y - t) * 2 + 1) * h / (span_y * 2));
      if (sy >= h)
        sy = h - 1;
      if ((int64_t)sy < (int64_t)source_y + image->cover_t ||
          (int64_t)sy >= (int64_t)source_y + image->cover_b)
        continue;
      sy -= (unsigned)source_y;
      uint32_t *row = &target(c)[y * WC_WIDTH];
      const uint32_t *source = &image->pixels[(size_t)sy * stride];
      for (int x = first; x < last; x++) {
        uint32_t pixel = source[columns[x]];
        unsigned coverage = image->coverage ? image->coverage[(size_t)sy * stride + columns[x]]
                            : image->force_opaque ? 255 : pixel >> 24;
        if (!coverage)
          continue;
        if (coverage == 255 && opacity >= 100)
          row[x] = pixel | 0xff000000u;
        else
          blend(&row[x], pixel & 0xffffffu, coverage * opacity / 100);
      }
    }
    return;
  }
  // Keep the original float expression and sampling phase, but evaluate it
  // once per column/row for ordinary axis-aligned sprites (including masks
  // and partially blended films). Transparent cropped margins need no work.
  unsigned columns[WC_WIDTH];
  bool mapped = !transformed;
#if DG_D10
  if (image->linked_member || c->film_transform_count) mapped = false;
#endif
  if (mapped) {
    int first = cr, last = cl;
    for (int x = cl; x < cr; x++) {
      float u = (float)(x - l + 0.5f) / span_x;
      unsigned sx = (unsigned)(u * w);
      if (sx >= w) sx = w - 1;
      columns[x] = sx;
      if ((int64_t)sx >= (int64_t)source_x + image->cover_l &&
          (int64_t)sx < (int64_t)source_x + image->cover_r) {
        if (x < first) first = x;
        last = x + 1;
      }
    }
    cl = first; cr = last;
  }
#if DG_MODERN
  uint32_t fore = opaque_rgb(s->value.flags & 16 ? s->value.fore_rgb : dg_stage_palette(d)[s->value.fore & 255]);
  uint32_t back = opaque_rgb(s->value.flags & 32 ? s->value.back_rgb : dg_stage_palette(d)[s->value.back & 255]);
#endif
  for (int y = ct; y < cb; y++) {
    unsigned row_source = 0;
    if (mapped) {
      float v = (float)(y - t + 0.5f) / span_y;
      row_source = (unsigned)(v * h);
      if (row_source >= h) row_source = h - 1;
      if ((int64_t)row_source < (int64_t)source_y + image->cover_t ||
          (int64_t)row_source >= (int64_t)source_y + image->cover_b) continue;
    }
    uint32_t *row = &target(c)[y * WC_WIDTH];
    for (int x = cl; x < cr; x++) {
      unsigned sx = mapped ? columns[x] : 0, sy = row_source;
      float u = 0, v = 0;
      float sample_x = x + 0.5f, sample_y = y + 0.5f;
#if DG_D10
      if (c->film_transform_count &&
          !dg_film_transform_point(c->film_transform, c->film_transform_count,
                                   sample_x, sample_y, &sample_x, &sample_y)) continue;
      if (c->film_transform_count && !transformed) {
        if (sample_x < l || sample_y < t || sample_x >= r || sample_y >= b) continue;
        u = (sample_x - l) / span_x; v = (sample_y - t) / span_y;
        sx = (unsigned)(u * w); sy = (unsigned)(v * h);
      }
#endif
      if (transformed) {
        if (!dg_quad_uv(q, sample_x, sample_y, &u, &v))
          continue;
#if DG_MODERN
        if (s->flip_h) u = 1 - u;
        if (s->flip_v) v = 1 - v;
#endif
        sx = (unsigned)(u * w); sy = (unsigned)(v * h);
      }
#if DG_D10
      if (image->linked_member) {
        if (!transformed) { u = (sample_x - l) / span_x; v = (sample_y - t) / span_y; }
        if (!dg_linked_uv(image->linked_member, s, &u, &v)) continue;
        sx = (unsigned)(u * w); sy = (unsigned)(v * h);
      }
#endif
      if (sx >= w) sx = w - 1;
      if (sy >= h) sy = h - 1;
      if ((int64_t)sx < source_x || (int64_t)sy < source_y ||
          (int64_t)sx >= (int64_t)source_x + image->width ||
          (int64_t)sy >= (int64_t)source_y + image->height) continue;
      sx -= (unsigned)source_x; sy -= (unsigned)source_y;
      uint32_t pixel = image->pixels[(size_t)sy * stride + sx];
      unsigned coverage = image->coverage ? image->coverage[(size_t)sy * stride + sx]
                          : image->force_opaque ? 255 : pixel >> 24;
      if (!coverage) continue;
#if DG_MODERN
      if (ink == 36 && !image->rich_text) {
        uint32_t key = s->value.flags & 32 ? s->value.back_rgb
                          : dg_stage_palette(d)[s->value.back & 255];
        if (image->rgb555)
          key = five(key >> 19 & 31) << 16 | five(key >> 11 & 31) << 8 | five(key >> 3 & 31);
        if (image->depth == 1) {
          if (pixel & 0xffffffu) continue;
          uint32_t fore = s->value.flags & 16 ? s->value.fore_rgb
                            : dg_stage_palette(d)[s->value.fore & 255];
          pixel = opaque_rgb(fore);
        } else if ((pixel & 0xffffffu) == (opaque_rgb(key) & 0xffffffu))
          continue;
      }
      if (arithmetic_ink(ink)
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
          || controlled || ink == 9
#endif
      ) {
        arithmetic(&row[x], pixel, ink, coverage, opacity, fore, back);
        continue;
      }
#endif
      if (!coverage)
        continue;
      blend(&row[x], pixel & 0xffffffu, coverage * opacity / 100);
    }
  }
}

static bool text_is_utf8(const char *text) {
  const unsigned char *s = (const unsigned char *)text;
  while (*s) {
    if (*s < 0x80) {
      s++;
      continue;
    }
    unsigned extra = (*s & 0xe0) == 0xc0 ? 1 : (*s & 0xf0) == 0xe0 ? 2
                   : (*s & 0xf8) == 0xf0 ? 3 : 0;
    if (!extra)
      return false;
    for (unsigned i = 1; i <= extra; i++)
      if ((s[i] & 0xc0) != 0x80)
        return false;
    s += extra + 1;
  }
  return true;
}
static uint32_t text_key(const char *text, int width, int height,
                         const wc_text_style_t *style) {
  uint32_t hash = 2166136261u;
  for (const unsigned char *p = (const unsigned char *)text; *p; p++)
    hash = (hash ^ *p) * 16777619u;
  hash = (hash ^ (uint32_t)width) * 16777619u;
  hash = (hash ^ (uint32_t)height) * 16777619u;
  if (style) {
    hash = (hash ^ style->font) * 16777619u;
    hash = (hash ^ style->size) * 16777619u;
    hash = (hash ^ style->align) * 16777619u;
    hash = (hash ^ (uint32_t)style->ascent) * 16777619u;
    hash = (hash ^ (uint32_t)style->line_height) * 16777619u;
  }
  return hash;
}
// Recovered text is UTF-8; anything else is taken as Latin-1 rather than
// handed to the host as malformed UTF-8.
static const char *utf8_text(const char *text) {
  static char converted[8192];
  if (text_is_utf8(text))
    return text;
  size_t at = 0;
  for (const unsigned char *p = (const unsigned char *)text;
       *p && at + 3 < sizeof(converted); p++) {
    if (*p < 0x80)
      converted[at++] = (char)*p;
    else {
      converted[at++] = (char)(0xc0 | *p >> 6);
      converted[at++] = (char)(0x80 | (*p & 0x3f));
    }
  }
  converted[at] = 0;
  return converted;
}
// Where the host set each character of a box (x, y, w, h per character).
enum { WC_LAYOUT_CHARS = 4096 };
static int16_t layout_boxes[WC_LAYOUT_CHARS * 4];
static int text_layout(wc_t *c, const char *text, int width, int height,
                       const wc_text_style_t *style) {
  if (!c->host.layout)
    return 0;
  int count = c->host.layout(c->host.ctx, text, width, height, style, layout_boxes,
                             WC_LAYOUT_CHARS);
  return count < 0 ? 0 : count;
}
static bool draw_text(wc_t *c, const char *text, const wc_text_style_t *style,
                      uint32_t color, unsigned alpha, int l, int t, int r, int b,
                      const wc_text_run_t *runs, unsigned run_count, int scroll) {
  int width = r - l, visible_height = b - t;
  if (scroll < 0) scroll = 0;
  if (scroll > 4096) return false;
  int height = visible_height + scroll;
  if (!text || !*text || width <= 0 || height <= 0) return true;
  if (!c->host.text || width > 4096 || height > 4096) return false;
  text = utf8_text(text);
  uint32_t key = text_key(text, width, height, style);
  for (unsigned i = 0; i < run_count; i++) key = (key ^ runs[i].offset) * 16777619u;
  key = (key ^ run_count) * 16777619u;
  wc_text_t *slot = NULL, *oldest = &c->texts[0];
  for (unsigned i = 0; i < WC_TEXT_SLOTS; i++) {
    wc_text_t *entry = &c->texts[i];
    if (entry->coverage && entry->key == key && entry->width == width &&
        entry->height == height) {
      slot = entry;
      break;
    }
    if (!entry->coverage || entry->last < oldest->last)
      oldest = entry;
  }
  if (!slot) {
    slot = oldest;
    free(slot->coverage);
    free(slot->runs);
    slot->runs = NULL;
    slot->coverage = calloc((size_t)width * height, 1);
    if (!slot->coverage || !c->host.text(c->host.ctx, text, width, height,
                                         style, slot->coverage)) {
      free(slot->coverage);
      slot->coverage = NULL;
      return false;
    }
    slot->key = key;
    slot->width = width;
    slot->height = height;
    // Colour runs: each character's box takes its run's colour.
    int count = run_count ? text_layout(c, text, width, height, style) : 0;
    if (count && (slot->runs = calloc((size_t)width * height, 1))) {
      unsigned run = 0;
      for (int i = 0; i < count; i++) {
        while (run + 1 < run_count && runs[run + 1].offset <= (uint32_t)i)
          run++;
        if (runs[run].offset > (uint32_t)i)
          continue;
        const int16_t *box = &layout_boxes[i * 4];
        for (int y = box[1] < 0 ? 0 : box[1]; y < box[1] + box[3] && y < height; y++)
          for (int x = box[0] < 0 ? 0 : box[0]; x < box[0] + box[2] && x < width; x++)
            slot->runs[y * width + x] = (uint8_t)(run + 1);
      }
    }
  }
  slot->last = c->serial;
#if DG_D10
  if (c->film_transform_count) {
    int cl=l, ct=t, cr=r, cb=b;
    film_destination_bounds(c, &cl, &ct, &cr, &cb);
    for (int y=ct; y<cb; y++) for (int x=cl; x<cr; x++) {
      float px,py;
      if (!dg_film_transform_point(c->film_transform,c->film_transform_count,
                                  x+0.5f,y+0.5f,&px,&py) ||
          px<l || py<t || px>=r || py>=b) continue;
      unsigned sx=(unsigned)(px-l), sy=(unsigned)(py-t)+scroll;
      unsigned coverage=slot->coverage[(size_t)sy*width+sx];
      unsigned run=slot->runs ? slot->runs[(size_t)sy*width+sx] : 0;
      if (coverage) blend(&target(c)[(size_t)y*WC_WIDTH+x],
          run && run<=run_count ? runs[run-1].rgb : color, coverage*alpha/255);
    }
    return true;
  }
#endif
  for (int y = 0; y < visible_height; y++) {
    if (t + y < 0 || t + y >= WC_HEIGHT)
      continue;
    for (int x = 0; x < width; x++) {
      if (l + x < 0 || l + x >= WC_WIDTH)
        continue;
      unsigned coverage = slot->coverage[(y + scroll) * width + x];
      unsigned run = slot->runs ? slot->runs[(y + scroll) * width + x] : 0;
      if (coverage)
        blend(&target(c)[(t + y) * WC_WIDTH + l + x],
              run && run <= run_count ? runs[run - 1].rgb : color, coverage * alpha / 255);
    }
  }
  return true;
}

// The style a text member sets in: its authored one, or NULL for the
// builtin monospace font.
static const wc_text_style_t *member_text_style(dg_runtime_t *d, const dg_member_t *m,
                                                wc_text_style_t *authored, int *scroll) {
  *scroll = 0;
#if DG_MODERN
  const dg_text_style_t *st = m->text_style;
  const char *font = NULL;
  unsigned size = 0, align = 0;
  for (unsigned j = 0; d && j < d->field_count; j++)
    if (d->field_members[j] == m->id) {
      *scroll = d->field_scroll[j];
      font = d->field_font_changed[j] ? d->field_fonts[j] : NULL;
      size = d->field_sizes[j];
      align = d->field_align[j];
      if (d->field_text_changed[j] && m->text_insert_style)
        st = m->text_insert_style;
      break;
    }
  if (st || font || size || align) {
    *authored = st ? (wc_text_style_t){st->font_id, st->size, st->align, st->ascent, st->line_height}
                   : (wc_text_style_t){0, 12, 0, 12, 15};
    if (size) {
      unsigned old = authored->size ? authored->size : 12;
      authored->ascent = (authored->ascent * size + old / 2) / old;
      authored->line_height = (authored->line_height * size + old / 2) / old;
      authored->size = size;
    }
    if (align) authored->align = align - 1;
    if (font && *font)
      for (unsigned file = 0; file < d->loaded_count; file++) {
        const dg_movie_t *movie = d->loaded[file];
        for (unsigned i = 0; movie && i < movie->member_count; i++) {
          const dg_member_t *candidate = &movie->members[i];
          const dg_text_style_t *styles[] = {candidate->text_style, candidate->text_insert_style};
          for (unsigned k = 0; k < 2; k++)
            if (styles[k] && styles[k]->font_name && !strcmp(styles[k]->font_name, font))
              authored->font = styles[k]->font_id;
        }
      }
    return authored;
  }
#else
  (void)d;
  (void)m;
  (void)authored;
#endif
  return NULL;
}
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
static const dg_text_extra_t *member_color_runs(dg_runtime_t *d, const dg_member_t *m,
                                                const char *text) {
  if (!m->text || strcmp(text, m->text)) return NULL;
  for (unsigned j = 0; j < d->field_count; j++)
    if (d->field_members[j] == m->id && (d->field_text_changed[j] ||
        lv_type(d->values->roots[DG_FIELD_COLOR_ROOT + j]) != LV_VOID)) return NULL;
  return dg_text_extra(d, m->id);
}
#endif
// The character at (x, y) of a text member set in a width x height box, as
// the host laid it out: 0-based in Unicode characters, -1 for none.
int wc_text_char_at(wc_t *c, dg_runtime_t *d, const dg_member_t *m, const char *text, int width, int height,
                    int x, int y) {
  if (!m || !text || !*text || width <= 0 || height <= 0)
    return -1;
  wc_text_style_t authored;
  int scroll;
  const wc_text_style_t *style = member_text_style(d, m, &authored, &scroll);
  text = utf8_text(text);
  if (scroll < 0) scroll = 0;
  if (scroll > 4096 || height > 4096 - scroll) return -1;
  y += scroll;
  int count = text_layout(c, text, width, height + scroll, style);
  for (int i = 0; i < count; i++) {
    const int16_t *box = &layout_boxes[i * 4];
    if (x >= box[0] && x < box[0] + box[2] && y >= box[1] && y < box[1] + box[3])
      return i;
  }
  return -1;
}
#if DG_D10
bool wc_read_image(wc_t *c, dg_runtime_t *d, uint32_t member, dg_pixel_image_t *out) {
  memset(out, 0, sizeof(*out));
  const dg_member_t *m = member ? dg_member(d, member) : NULL;
  unsigned width = m ? m->width : 800, height = m ? m->height : 600;
  if ((member && !m) || !width || !height || width > 4096 || height > 4096)
    return false;
  uint32_t *pixels = malloc((size_t)width * height * sizeof(*pixels));
  if (!pixels) return false;
  *out = (dg_pixel_image_t){.width = width, .height = height, .depth = 32,
      .pixels = pixels, .use_alpha = true};
  if (!member) {
    wc_render(c, d, 0, 0, false, c->externals);
    for (unsigned y = 0; y < height; y++)
      for (unsigned x = 0; x < width; x++)
        pixels[(size_t)y * width + x] = c->scene[
            (size_t)(y * WC_HEIGHT / height) * WC_WIDTH + x * WC_WIDTH / width];
    return !d->values->failed;
  }
  if (m->type == 1 || m->type == 5 || m->type == 12) {
    unsigned sw, sh;
    sw = m->width; sh = m->height;
    const wc_image_t *image = wc_image(c, d->values, m->asset, sw, sh, 8);
    if (image) {
      for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++)
          pixels[(size_t)y * width + x] = image->pixels[
              (size_t)(y * image->height / height) * image->width + x * image->width / width];
      out->depth = image->depth ? image->depth : 32;
      return true;
    }
  } else if (m->type == 3 || m->type == 7) {
    uint8_t *coverage = calloc((size_t)width * height, 1);
    const char *text = m->text;
    for (unsigned j = 0; j < d->field_count; j++)
      if (d->field_members[j] == member)
        text = lv_cstr(d->values, d->values->roots[DG_FIELD_TEXT_ROOT + j]);
    wc_text_style_t authored;
    int scroll;
    const wc_text_style_t *style = member_text_style(d, m, &authored, &scroll);
    bool ok = coverage && c->host.text && text &&
        c->host.text(c->host.ctx, utf8_text(text), (int)width, (int)height, style, coverage);
    if (ok) {
      uint32_t rgb = m->text_style ? m->text_style->color : 0;
      for (unsigned j = 0; j < d->field_count; j++)
        if (d->field_members[j] == member) {
          if (d->field_text_changed[j] && m->text_insert_style) rgb = m->text_insert_style->color;
          lv_t value = d->values->roots[DG_FIELD_COLOR_ROOT + j];
          if (lv_type(value) != LV_VOID) {
            unsigned n = (unsigned)lv_integer(d->values, value);
            rgb = lv_type(value) == LV_COLOR || n > 255 ? n : dg_stage_palette(d)[n];
          }
        }
      uint32_t color = opaque_rgb(rgb) & 0xffffffu;
      for (size_t i = 0; i < (size_t)width * height; i++)
        pixels[i] = coverage[i] ? color | (uint32_t)coverage[i] << 24 : 0x00ffffffu;
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
      const dg_text_extra_t *extra = member_color_runs(d, m, text);
      if (extra && extra->run_count) {
        int count = text_layout(c, utf8_text(text), width, height, style);
        unsigned run = 0;
        for (int i = 0; i < count; i++) {
          while (run + 1 < extra->run_count && extra->runs[run + 1].offset <= (unsigned)i) run++;
          if (extra->runs[run].offset > (unsigned)i) continue;
          uint32_t rgb = opaque_rgb(extra->runs[run].rgb) & 0xffffffu;
          const int16_t *box = &layout_boxes[i * 4];
          for (int y = box[1] < 0 ? 0 : box[1]; y < box[1] + box[3] && y < (int)height; y++)
            for (int x = box[0] < 0 ? 0 : box[0]; x < box[0] + box[2] && x < (int)width; x++) {
              size_t p = (size_t)y * width + x;
              if (coverage[p]) pixels[p] = rgb | (uint32_t)coverage[p] << 24;
            }
        }
      }
#endif
    }
    free(coverage);
    if (ok) return true;
  }
  free(pixels);
  memset(out, 0, sizeof(*out));
  return false;
}
#endif

static uint32_t indexed_color(const dg_runtime_t *d, unsigned index) {
  return opaque_rgb(dg_stage_palette(d)[index & 255]);
}
#if DG_MODERN
static uint32_t sprite_color(const dg_runtime_t *d, const dg_sprite_t *s,
                             bool back) {
  uint32_t rgb = back ? s->value.back_rgb : s->value.fore_rgb;
  if (!(s->value.flags & (back ? 32 : 16)))
    return indexed_color(d, back ? s->value.back : s->value.fore);
  return opaque_rgb(rgb);
}
#endif

#if DG_D10
// Prompt text written into a converted Flash member's named edit fields draws
// above the flattened card: wrapped in
// authored member space with the field's measured advances, so the lines
// agree with the textHeight the scripts read.
static void draw_flash_overlays(wc_t *c, dg_runtime_t *d, unsigned channel,
                                const dg_sprite_t *s, const dg_member_t *m, int ox, int oy, const char *only, bool local) {
  lv_runtime_t *values = d->values;
  static char wrapped[2048];
  for (unsigned f = 0; m && f < m->flash_field_count; f++) {
    const dg_flash_field_t *field = &m->flash_fields[f];
    if (only && dg_flash_field_find(m, only) != field) continue;
    // A field the scripts have addressed reads its live state from the
    // object bag; an untouched one shows its authored initial text.
    lv_t bag = {0};
    for (unsigned n = 0; n < d->flash_object_count; n++)
      if (d->flash_objects[n].sprite == channel &&
          d->flash_objects[n].member == m->id &&
          dg_flash_field_find(m, d->flash_objects[n].name) == field) {
        bag = values->roots[DG_FLASH_ROOT + n];
        break;
      }
    const char *text = field->text;
    int fx = field->x, fy = field->y, fw = field->width, fh = field->height;
    unsigned size = field->style->size;
    unsigned opacity = dg_opacity(s);
    uint32_t color = opaque_rgb(field->style->color);
    if (lv_type(bag) == LV_PROPLIST) {
      if (!lv_truth(values, lv_get(values, NULL, "_visible", bag)))
        continue;
      lv_t value = lv_get(values, NULL, "text", bag);
      if (lv_type(value) != LV_STRING)
        continue;
      text = lv_cstr(values, value);
      lv_t v;
      if (lv_type((v = lv_get(values, NULL, "_alpha", bag))) == LV_NUMBER) {
        double alpha = lv_numeric(v);
        if (!isfinite(alpha)) { lv_fail(values, "invalid Flash text alpha"); return; }
        opacity = (unsigned)(opacity * (alpha < 0 ? 0 : alpha > 100 ? 100 : alpha) / 100);
      }
      if (lv_type((v = lv_get(values, NULL, "_x", bag))) == LV_NUMBER) fx = (int)lv_numeric(v);
      if (lv_type((v = lv_get(values, NULL, "_y", bag))) == LV_NUMBER) fy = (int)lv_numeric(v);
      if (lv_type((v = lv_get(values, NULL, "_width", bag))) == LV_NUMBER) fw = (int)lv_numeric(v);
      if (lv_type((v = lv_get(values, NULL, "_height", bag))) == LV_NUMBER) fh = (int)lv_numeric(v);
      if (lv_type((v = lv_get(values, NULL, "size", bag))) == LV_NUMBER && lv_numeric(v) > 0)
        size = (unsigned)lv_numeric(v);
      if (lv_type((v = lv_get(values, NULL, "color", bag))) == LV_COLOR)
        color = opaque_rgb((uint32_t)lv_id(v));
    }
    if (local) { fx = fy = 0; fw = s->value.width; fh = s->value.height; opacity = 100; }
    if (!*text)
      continue;
    dg_flash_field_wrap(field, size, text, fw, wrapped, sizeof(wrapped));
    const dg_text_style_t *st = field->style;
    int ascent = st->ascent;
    if (size != st->size)
      ascent = ascent * (int)size / (st->size ? (int)st->size : 1);
    int line_height = (int)size + (field->leading < 0 ? 0 : field->leading);
    wc_text_style_t style = {st->font_id, size, st->align,
                             ascent, line_height};
    int al, at, ar, ab;
    dg_sprite_bounds(d, s, &al, &at, &ar, &ab);
    al += ox; ar += ox; at += oy; ab += oy;
    float sx = m->width ? (float)(ar - al) / m->width : 1.0f;
    float sy = m->height ? (float)(ab - at) / m->height : 1.0f;
    // The SWF renderer keeps a two-pixel gutter inside the field bounds.
    int l = al + (int)((fx + field->margin_left + field->indent) * sx) + 2;
    int t = at + (int)(fy * sy) + 2;
    int r = al + (int)((fx + fw - field->margin_right) * sx) - 2;
    int b = at + (int)((fy + fh) * sy);
    if(!draw_text(c, wrapped, &style, color & 0xffffffu, opacity * 255 / 100, l, t, r, b,
              NULL, 0, 0)) lv_fail(d->values,"Flash text rasterization failed: font, allocation or dimensions");
  }
}
static void draw_flash_field(wc_t *c, dg_runtime_t *d, unsigned parent,
    const dg_member_t *member, const dg_film_layer_t *layer, int ox, int oy) {
  const dg_flash_field_t *field = dg_flash_field_find(member, layer->name);
  if (!field) { lv_fail(d->values, "missing Flash field layer"); return; }
  dg_flash_plane_t layout;
  if (!dg_flash_field_plane(d, parent, member, field, layer, &layout)) return;
  unsigned width, height;
  width = layout.width; height = layout.height;
  uint32_t *plane = malloc((size_t)width * height * sizeof(*plane));
  uint32_t *saved = malloc(sizeof(c->pixels));
  if (!plane || !saved) { free(plane); free(saved); lv_fail(d->values, "Flash field framebuffer allocation"); return; }
  memcpy(saved, target(c), sizeof(c->pixels));
#if DG_D10
  unsigned transforms=c->film_transform_count;
  c->film_transform_count=0;
#endif
  dg_member_t local_member = *member;
  local_member.width = (uint16_t)layout.width; local_member.height = (uint16_t)layout.height;
  dg_sprite_t local_sprite = {.value = {.width = layout.width, .height = layout.height,
      .type = 16, .blend = 100}, .visible = true, .stretch = true, .custom_quad = true,
      .quad = {0, 0, (float)layout.width, 0, (float)layout.width, (float)layout.height, 0, (float)layout.height}};
  for (unsigned top = 0; top < height && !d->values->failed; top += WC_HEIGHT)
    for (unsigned left = 0; left < width && !d->values->failed; left += WC_WIDTH) {
      memset(target(c), 0, sizeof(c->pixels));
      draw_flash_overlays(c, d, parent, &local_sprite, &local_member,
          -((int)left), -((int)top), layer->name, true);
      unsigned rows = height - top < WC_HEIGHT ? height - top : WC_HEIGHT;
      unsigned columns = width - left < WC_WIDTH ? width - left : WC_WIDTH;
      for (unsigned y = 0; y < rows; y++)
        memcpy(plane + (size_t)(top + y) * width + left,
               target(c) + (size_t)y * WC_WIDTH, columns * sizeof(*plane));
    }
  memcpy(target(c), saved, sizeof(c->pixels)); free(saved);
#if DG_D10
  c->film_transform_count=transforms;
#endif
  if (!d->values->failed) {
    wc_image_t image = {.width = width, .height = height, .pixels = plane, .follow_alpha = true};
    image_coverage(&image);
    int l,t,r,b; dg_sprite_bounds(d, &layout.sprite, &l,&t,&r,&b);
    l = (l + ox); r = (r + ox);
    t = (t + oy); b = (b + oy);
    if (r > l && b > t)
      draw_bitmap(c,d,&layout.sprite,&image,l,t,r,b,8,dg_opacity(&layout.sprite),ox,oy);
  }
  free(plane);
}
typedef struct { wc_t *compositor; dg_runtime_t *director; } wc_flash_context_t;
static bool wc_flash_asset(void *context,const dg_film_layer_t *layer,dg_flash_raster_t *out) {
  wc_flash_context_t *ctx=context;unsigned width,height;
  width = layer->value.width; height = layer->value.height;
  const wc_image_t *image=wc_image(ctx->compositor,ctx->director->values,layer->asset,width,height,8);
  if(!image)return false;
  unsigned rw,rh;rw = layer->value.width; rh = layer->value.height;
  int left=layer->value.x,top=layer->value.y;
  if(!dg_flash_raster_allocate(ctx->director,out,left,top,left+(int)rw,top+(int)rh))return false;
  for(unsigned y=0;y<rh;y++)for(unsigned x=0;x<rw;x++)
    out->pixels[(size_t)y*rw+x]=image->pixels[(size_t)(y*image->height/rh)*image->width+x*image->width/rw];
  return true;
}
static bool wc_flash_text(void *context,dg_flash_instance_t *instance,const dg_flash_field_t *field,dg_flash_raster_t *out) {
  wc_flash_context_t *ctx=context;wc_t *c=ctx->compositor;dg_runtime_t *d=ctx->director;
  dg_flash_field_t effective;dg_text_style_t style;if(!dg_flash_field_snapshot(d,instance,field,&effective,&style))return false;
  dg_flash_view_t view;dg_flash_view(instance,&view);unsigned width,height;
  width = effective.width; height = effective.height;
  int left=((int)floorf(view.bounds[0])),top=((int)floorf(view.bounds[1]));
  if(!dg_flash_raster_allocate(d,out,left,top,left+(int)width,top+(int)height))return false;
  uint32_t *saved=malloc(sizeof(c->pixels));
  if(!saved){free(out->pixels);out->pixels=NULL;lv_fail(d->values,"Flash text stage preservation allocation");return false;}
  memcpy(saved,target(c),sizeof(c->pixels));
  unsigned transforms=c->film_transform_count;
  c->film_transform_count=0;
  dg_member_t member=*view.member;member.width=effective.width;member.height=effective.height;
  member.flash_fields=&effective;member.flash_field_count=1;
  dg_sprite_t sprite={.value={.type=16,.width=effective.width,.height=effective.height,.blend=100},
    .visible=true,.stretch=true,.custom_quad=true,
    .quad={0,0,effective.width,0,effective.width,effective.height,0,effective.height}};
  for(unsigned y=0;y<height&&!d->values->failed;y+=WC_HEIGHT)
    for(unsigned x=0;x<width&&!d->values->failed;x+=WC_WIDTH) {
      memset(target(c),0,sizeof(c->pixels));
      draw_flash_overlays(c,d,DG_SPRITES,&sprite,&member,-((int)x),-((int)y),NULL,true);
      unsigned rows=height-y<WC_HEIGHT?height-y:WC_HEIGHT,cols=width-x<WC_WIDTH?width-x:WC_WIDTH;
      for(unsigned row=0;row<rows;row++)memcpy(out->pixels+(size_t)(y+row)*width+x,target(c)+(size_t)row*WC_WIDTH,cols*4);
    }
  memcpy(target(c),saved,sizeof(c->pixels));free(saved);
  c->film_transform_count=transforms;
  return !d->values->failed;
}
bool wc_flash_field_hit(wc_t *c, dg_runtime_t *d, struct dg_flash_instance *instance,
                        float x, float y) {
  dg_flash_view_t view;
  if (!dg_flash_view(instance, &view) || !view.field) return false;
  wc_flash_context_t context = {c, d};
  dg_flash_raster_t raster = {0};
  bool ok = wc_flash_text(&context, instance, view.field, &raster), hit = false;
  int px = (int)floorf(x) - raster.left, py = (int)floorf(y) - raster.top;
  if (ok && px >= 0 && py >= 0 && px < (int)raster.width && py < (int)raster.height)
    hit = (raster.pixels[(size_t)py * raster.width + (unsigned)px] >> 24) != 0;
  free(raster.pixels);
  return hit;
}
#endif

#if DG_D5 || DG_D10
// A digital video sprite shows the frame the host has decoded for its current
// time, stretched to the sprite's bounds.
static void draw_video(wc_t *c, dg_runtime_t *owner, unsigned sprite, const dg_sprite_t *s,
                       int l, int t, int r, int b, unsigned opacity, int ox, int oy) {
  static uint32_t frame[1024 * 768];
  unsigned w = 0, h = 0;
  if (!c->host.video_frame ||
      !c->host.video_frame(c->host.ctx, owner, sprite, frame, sizeof(frame) / sizeof(*frame), &w, &h) ||
      !w || !h || (size_t)w * h > sizeof(frame) / sizeof(*frame))
    return;
  float quad[8];
  dg_sprite_quad(owner,s,quad);
  for (unsigned at = 0; at < 8; at += 2) { quad[at] += ox; quad[at + 1] += oy; }
  int cl = l < 0 ? 0 : l, ct = t < 0 ? 0 : t;
  int cr = r > WC_WIDTH ? WC_WIDTH : r, cb = b > WC_HEIGHT ? WC_HEIGHT : b;
#if DG_D10
  film_destination_bounds(c, &cl, &ct, &cr, &cb);
#endif
  for (int y = ct; y < cb; y++) {
    for (int x = cl; x < cr; x++) {
      float u,v;
      float px=x+0.5f,py=y+0.5f;
#if DG_D10
      if (c->film_transform_count &&
          !dg_film_transform_point(c->film_transform,c->film_transform_count,
                                   px,py,&px,&py)) continue;
#endif
      if (!dg_quad_uv(quad,px,py,&u,&v)) continue;
      if (s->flip_h) u = 1 - u;
      if (s->flip_v) v = 1 - v;
      unsigned sx = (unsigned)(u*w), sy = (unsigned)(v*h);
      if (sx >= w) sx = w-1;
      if (sy >= h) sy = h-1;
      blend(&target(c)[y * WC_WIDTH + x], frame[(size_t)sy * w + sx] & 0xffffffu,
            255 * opacity / 100);
    }
  }
}
#endif

// One sprite (or trail), as the scene draws it in channel order.
static void draw_scene(wc_t *, dg_runtime_t *, int, int, bool);

static void draw_sprite(wc_t *, dg_runtime_t *, unsigned, const dg_sprite_t *, int, int);
#if DG_MODERN
static void draw_transformed_text(wc_t *c, dg_runtime_t *d, unsigned channel,
    const dg_sprite_t *sprite, int l, int t, int r, int b, int ox, int oy) {
  dg_sprite_t flat = *sprite;
  flat.value.rotation = flat.value.skew = 0;
  flat.flip_h = flat.flip_v = flat.custom_quad = false;
  flat.value.blend = 100; flat.value.thickness |= DG_HAS_BLEND;
  int left, top, right, bottom;
  dg_sprite_bounds(d, &flat, &left, &top, &right, &bottom);
  if (right <= left || bottom <= top) return;
  unsigned width = (unsigned)(right - left), height = (unsigned)(bottom - top);
#if DG_D10
#endif
  if (width > 16384 || height > 16384 || (size_t)width * height > 16u * 1024u * 1024u) {
    lv_fail(d->values, "transformed text dimensions exceed bound"); return;
  }
  uint32_t *plane = malloc((size_t)width * height * sizeof(*plane));
  uint32_t *saved = malloc(sizeof(c->pixels));
  if (!plane || !saved) {
    free(plane); free(saved); lv_fail(d->values, "transformed text allocation"); return;
  }
  memcpy(saved, target(c), sizeof(c->pixels));
  flat.value.x -= left; flat.value.y -= top;
#if DG_D10
  unsigned transforms=c->film_transform_count;
  c->film_transform_count=0;
#endif
  for (unsigned y = 0; y < height && !d->values->failed; y += WC_HEIGHT)
    for (unsigned x = 0; x < width && !d->values->failed; x += WC_WIDTH) {
      memset(target(c), 0, sizeof(c->pixels));
#if DG_D10
      draw_sprite(c,d,channel,&flat,-((int)x),-((int)y));
#else
      draw_sprite(c,d,channel,&flat,-(int)x,-(int)y);
#endif
      unsigned rows = height - y < WC_HEIGHT ? height - y : WC_HEIGHT;
      unsigned columns = width - x < WC_WIDTH ? width - x : WC_WIDTH;
      for (unsigned row = 0; row < rows; row++)
        memcpy(plane + (size_t)(y + row) * width + x, target(c) + (size_t)row * WC_WIDTH,
               columns * sizeof(*plane));
    }
  memcpy(target(c), saved, sizeof(c->pixels)); free(saved);
#if DG_D10
  c->film_transform_count=transforms;
#endif
  if (!d->values->failed) {
    unsigned opacity = dg_opacity(sprite);
    for (size_t at = 0; at < (size_t)width * height; at++)
      plane[at] = (plane[at] & 0xffffffu) | ((plane[at] >> 24) * opacity / 100) << 24;
    wc_image_t image = {.width = width, .height = height, .pixels = plane, .follow_alpha = true};
    image_coverage(&image);
    dg_sprite_t parent = *sprite;
    parent.value.fore_rgb = 0; parent.value.back_rgb = 0xffffffu; parent.value.flags |= 48;
    draw_bitmap(c,d,&parent,&image,l,t,r,b,8,100,ox,oy);
  }
  free(plane);
}
#endif

#if DG_D10
static void draw_film_direct(wc_t *c, dg_runtime_t *d, unsigned parent,
    const dg_sprite_t *sprite, const dg_member_t *member,
    const dg_film_frame_t *frame, int ox, int oy) {
  if (c->film_transform_count >= 8 || c->film_depth >= 8) {
    lv_fail(d->values,"film loop direct transform nesting exceeded"); return;
  }
  dg_film_transform_t transform;
  dg_film_transform_init(d,sprite,member,ox,oy,
      1.0f,&transform);
  unsigned previous=c->film_transform_count;
  // The common authored800x600 container contributes no mapping or clipping
  // beyond the stage itself, so its children retain their ordinary fast paths.
  if (previous || !dg_film_transform_full_identity(&transform,WC_WIDTH,WC_HEIGHT))
    c->film_transform[c->film_transform_count++]=transform;
  c->film_depth++;
  for (unsigned i=0; i<frame->count && !d->values->failed; i++) {
    const dg_film_layer_t *layer=&frame->layers[i];
    dg_sprite_t child;
    if (!dg_film_layer_sprite(d,parent,layer,&child)) continue;
    if (layer->asset && *layer->asset) {
      unsigned width,height;
      width = layer->value.width; height = layer->value.height;
      const wc_image_t *image=wc_image(c,d->values,layer->asset,width,height,child.value.ink);
      if (!image) continue;
      int l,t,r,b;dg_sprite_bounds(d,&child,&l,&t,&r,&b);
      draw_bitmap(c,d,&child,image,l,t,
          r,b,child.value.ink,dg_opacity(&child),0,0);
    } else if (!layer->value.member && layer->name && *layer->name) {
      dg_sprite_t field_parent={.value={.member=member->id,.type=16,
          .x=member->reg_x,.y=member->reg_y,.width=member->width,.height=member->height,
          .blend=100},.visible=true,.stretch=true};
      if (layer->affine) draw_flash_field(c,d,parent,member,layer,0,0);
      else draw_flash_overlays(c,d,parent,&field_parent,member,0,0,layer->name,false);
    } else draw_sprite(c,d,DG_SPRITES,&child,0,0);
  }
  c->film_depth--;
  c->film_transform_count=previous;
}
#endif

static void draw_sprite(wc_t *c, dg_runtime_t *d, unsigned i, const dg_sprite_t *s,
                        int ox, int oy) {
  lv_runtime_t *values = d->values;
  (void)i;
  const dg_member_t *m = dg_member(d, s->value.member);
#if DG_D10
  dg_sprite_t direct_stage;
  if (m && m->flash_direct_to_stage) {
    direct_stage=*s; s=&direct_stage;
    direct_stage.value.ink=8; direct_stage.value.blend=100;
    direct_stage.value.thickness|=DG_HAS_BLEND;
    direct_stage.value.fore_rgb=0; direct_stage.value.back_rgb=0xffffff;
    direct_stage.value.flags|=48;
  }
  bool driven = m && m->type == 2 && s == &d->sprites[i] && dg_film_driven(d, i);
  const dg_sprite_t *film_source = s;
  dg_sprite_t film_container;
  if (dg_film_container_effects(m)) {
    dg_film_container_sprite(&film_container, s);
    s = &film_container;
  }
#endif
  // Palettes, sounds and script casts have no visual widget in Director.
  if (m && (m->type == 4 || m->type == 6 || m->type == 11 || m->type == 14))
    return;
  if (m && (m->type == 1 || m->type == 2 || m->type == 5 || m->type == 12) && (!m->width || !m->height)
#if DG_D10
      && !dg_member_image(d, m->id)
#endif
  ) return;
  dg_member_t film;
  bool pose = false;
  if (m && m->type == 2 && m->film_count
#if DG_D10
      && !m->film_frames && !m->film_track_count && !driven
#endif
  ) {
    film = *m;
    film.asset = dg_film_asset(m, s);
    film.type = 1;
    m = &film;
    pose = true;
  }
  unsigned opacity = dg_opacity(s);
  if (!s->visible || !s->value.type || !opacity)
    return;
  int l, t, r, b;
  dg_sprite_bounds(d, s, &l, &t, &r, &b);
  l += ox; r += ox;
  t += oy; b += oy;
#if DG_D10
  // Authored 800x600 geometry rasterizes onto the 640x480 stage.
#endif
  if (r <= l || b <= t || (
#if DG_D10
      !c->film_transform_count &&
#endif
      (r <= 0 || b <= 0 || l >= WC_WIDTH || t >= WC_HEIGHT)))
    return;
#if DG_MODERN
  if (m && (m->type == 3 || m->type == 7) &&
      (s->value.rotation || s->value.skew || s->flip_h || s->flip_v || s->custom_quad)) {
    draw_transformed_text(c,d,i,s,l,t,r,b,ox,oy); return;
  }
#endif
#if DG_D10
  if(m&&m->type==2&&m->flash_clip_count) {
    wc_flash_context_t context={c,d};
    dg_flash_raster_host_t host={.context=&context,.scale=1.0f,
      .asset=wc_flash_asset,.text=wc_flash_text};
    dg_flash_raster_t plane;
    if(dg_flash_render(d,i,&host,&plane)&&plane.pixels) {
      wc_image_t image={.width=plane.width,.height=plane.height,.pixels=plane.pixels,.follow_alpha=true};
      image_coverage(&image);
      draw_bitmap(c,d,s,&image,l,t,r,b,s->value.ink,opacity,ox,oy);
    }
    free(plane.pixels);return;
  }
  if (m && m->type == 2 && (m->film_frames || m->film_track_count || driven)) {
    unsigned width, height;
    width = m->width; height = m->height;
    if (!width || !height || (size_t)width * height > 16u * 1024u * 1024u || c->film_depth >= 8) {
      lv_fail(values, "film loop render dimensions or nesting exceeded"); return;
    }
    dg_film_frame_t driven_frame;
    bool resolve = driven || m->film_track_count;
    dg_film_layer_t *driven_layers = resolve ? dg_film_frame_resolve(d, i, film_source, m, &driven_frame) : NULL;
    if (resolve && !driven_layers) return;
    const dg_film_frame_t *frame = resolve ? &driven_frame : &m->film_frames[dg_film_storage_pose(m, s)];
    dg_film_transform_t placement;
    dg_film_transform_init(d,s,m,ox,oy,1.0f,&placement);
    if (dg_film_container_effects(m) &&
        (c->film_transform_count || !dg_film_transform_raster_aligned(&placement) ||
         !dg_film_isolated_safe(d,i,m,frame,film_leaf_safe,c,0))) {
      if (!values->failed) draw_film_direct(c,d,i,s,m,frame,ox,oy);
      free(driven_layers); return;
    }
    wc_film_key_t key=film_key(d,i,film_source,m,frame);
    const wc_image_t *cached=key.valid ? plane_find(c,2,key.data,key.count) : NULL;
    if (cached) {
      draw_bitmap(c,d,s,cached,l,t,r,b,s->value.ink,opacity,ox,oy);
      free(driven_layers); free(key.data); return;
    }
    dg_film_bounds_t crop=dg_film_bounds(d,i,m,frame,width,height);
    if (!crop.width || !crop.height) { free(driven_layers); free(key.data); return; }
    uint32_t *plane=malloc((size_t)crop.width*crop.height*sizeof(*plane));
    if (!c->film_scratch[c->film_depth]) c->film_scratch[c->film_depth]=malloc(sizeof(c->pixels));
    if (!plane || !c->film_scratch[c->film_depth]) {
      free(plane); free(driven_layers); free(key.data);
      lv_fail(values,"film loop framebuffer allocation"); return;
    }
    uint32_t *previous=c->target;
    unsigned previous_transforms=c->film_transform_count;
    c->film_transform_count=0;
    c->target=c->film_scratch[c->film_depth++];
    for (unsigned top = 0; top < crop.height && !values->failed; top += WC_HEIGHT)
      for (unsigned left = 0; left < crop.width && !values->failed; left += WC_WIDTH) {
        memset(target(c), 0, sizeof(c->pixels));
        for (unsigned layer = 0; layer < frame->count && !values->failed; layer++) {
          const dg_film_layer_t *item = &frame->layers[layer];
          int offset_x = -m->film_left - ((int)(crop.left+left));
          int offset_y = -m->film_top - ((int)(crop.top+top));
          if (!item->value.member && item->name && *item->name &&
              (!item->asset || !*item->asset)) {
            dg_sprite_t field_parent = {.value = {.member = m->id, .type = 16,
                .x = m->reg_x, .y = m->reg_y, .width = m->width, .height = m->height,
                .blend = 100}, .visible = true, .stretch = true};
            if (item->affine)
              draw_flash_field(c, d, i, m, item, offset_x, offset_y);
            else draw_flash_overlays(c, d, i, &field_parent, m, offset_x, offset_y, item->name, false);
            continue;
          }
          dg_sprite_t child;
          if (!dg_film_layer_sprite(d, i, item, &child)) continue;
          if (item->asset && *item->asset) {
            unsigned iw, ih;
            iw = item->value.width; ih = item->value.height;
            const wc_image_t *image = wc_image(c, values, item->asset, iw, ih, child.value.ink);
            if (!image) continue;
            int cl, ct, cr, cb;
            dg_sprite_bounds(d, &child, &cl, &ct, &cr, &cb);
            cl = (cl + offset_x); cr = (cr + offset_x);
            ct = (ct + offset_y); cb = (cb + offset_y);
            if (cr > cl && cb > ct)
              draw_bitmap(c, d, &child, image, cl, ct, cr, cb, child.value.ink,
                          dg_opacity(&child), offset_x, offset_y);
          } else draw_sprite(c, d, DG_SPRITES, &child, offset_x, offset_y);
        }
        unsigned rows = crop.height - top < WC_HEIGHT ? crop.height - top : WC_HEIGHT;
        unsigned columns = crop.width - left < WC_WIDTH ? crop.width - left : WC_WIDTH;
        for (unsigned y = 0; y < rows; y++)
          memcpy(plane + (size_t)(top + y) * crop.width + left,
                 target(c) + (size_t)y * WC_WIDTH, columns * sizeof(*plane));
      }
    c->film_depth--; c->target=previous;
    c->film_transform_count=previous_transforms;
    if (!values->failed) {
      wc_image_t image = {.width = crop.width, .height = crop.height, .pixels = plane, .follow_alpha = true,
          .source_width=width, .source_height=height, .source_x=(int)crop.left, .source_y=(int)crop.top};
      image_coverage(&image);
      if (s->value.ink == 9) {
        const dg_member_t *mask = dg_member(d, m->id + 1);
        if (mask && mask->type == 1 && mask_pair(m, mask)) apply_mask(c, d, m, mask, &image);
      }
      if (key.valid) cached=plane_keep(c,2,key.data,key.count,key.identity_bytes,&image);
      draw_bitmap(c, d, s, cached ? cached : &image, l, t, r, b, s->value.ink, opacity, ox, oy);
      if (cached) plane=NULL;
    }
    free(plane); free(driven_layers); free(key.data);
    return;
  }
#endif
#if DG_D5 || DG_D10
  if (dg_is_video(m)) {
    draw_video(c, d, i, s, l, t, r, b, opacity, ox, oy);
    return;
  }
#endif
#if DG_D10
  if (m && m->type == 9) {
    unsigned ink = s->value.ink;
    if (ink != 0 && ink != 8 && ink != 32 && ink != 36 && !arithmetic_ink(ink)) {
      lv_fail(values, "unsupported linked movie ink");
      return;
    }
    unsigned width, height;
    width = m->width; height = m->height;
    if (!width || !height || width > WC_WIDTH || height > WC_HEIGHT) {
      lv_fail(values, "linked movie source rectangle exceeds compositor");
      return;
    }
    uint32_t *plane = malloc(sizeof(c->pixels));
    if (!plane) { lv_fail(values, "linked movie framebuffer allocation"); return; }
    memcpy(plane, target(c), sizeof(c->pixels));
    dg_runtime_t *child = dg_linked_begin(d, i);
    if (!child) {
      free(plane);
      lv_fail(values, "linked movie runtime unavailable");
      return;
    }
    unsigned transforms=c->film_transform_count;
    c->film_transform_count=0;
    draw_scene(c, child, -m->linked_left, -m->linked_top, false);
    c->film_transform_count=transforms;
    dg_linked_end(d);
    // The temporary plane now keeps the child while the parent's stage
    // returns to the live framebuffer. Shared decoded assets stay cached.
    for (size_t p = 0; p < (size_t)WC_WIDTH * WC_HEIGHT; p++) {
      uint32_t pixel = target(c)[p]; target(c)[p] = plane[p]; plane[p] = pixel;
    }
    wc_image_t image = {.width = width, .height = height, .stride = WC_WIDTH,
        .pixels = plane, .cover_r = width, .cover_b = height, .linked_member = m};
    draw_bitmap(c, d, s, &image, l, t, r, b, s->value.ink, opacity, ox, oy);
    free(plane);
    return;
  }
#endif
#if DG_D10
  const dg_pixel_image_t *mutable = m ? dg_member_image(d, m->id) : NULL;
  if (mutable) {
    unsigned ink = s->value.ink;
    if (ink != 0 && ink != 8 && ink != 9 && ink != 32 && ink != 36 && !arithmetic_ink(ink)) {
      lv_fail(values, "unsupported mutable image ink");
      return;
    }
    wc_image_t image = {.width = mutable->width, .height = mutable->height,
        .pixels = mutable->pixels, .follow_alpha = mutable->use_alpha,
        .force_opaque = !mutable->use_alpha};
    if (!mutable->use_alpha && !dg_ink_whole_rect(ink) && ink != 36) {
      image.coverage = dg_image_matte(d, mutable);
      if (!image.coverage) return;
    }
    // Independent Imaging Lingo planes are not cached decoded assets.
    // Their coverage bounds must reflect the latest pixel mutations.
    image_coverage(&image);
    if (image.force_opaque) {
      image.cover_l = image.cover_t = 0;
      image.cover_r = image.width; image.cover_b = image.height;
    }
    uint32_t *masked_pixels = NULL;
    if (ink == 9) {
      const dg_member_t *mask = dg_member(d, m->id + 1);
      if (mask && mask->type == 1 && mask_pair(m, mask)) {
        masked_pixels = malloc((size_t)image.width * image.height * sizeof(*masked_pixels));
        if (!masked_pixels) { lv_fail(values, "mutable mask allocation"); return; }
        memcpy(masked_pixels, image.pixels, (size_t)image.width * image.height * sizeof(*masked_pixels));
        image.pixels = masked_pixels; image.force_opaque = false; image.coverage = NULL;
        dg_member_t source = *m;
        source.width = image.width; source.height = image.height;
        source.reg_x = mutable->reg_x; source.reg_y = mutable->reg_y;
        apply_mask(c, d, &source, mask, &image);
      }
    }
    draw_bitmap(c, d, s, &image, l, t, r, b, s->value.ink, opacity, ox, oy);
    free(masked_pixels);
    return;
  }
#endif
  if (m && (m->type == 1 || m->type == 5 || m->type == 12)) {
    unsigned ink = s->value.ink;
    if (ink != 0 && ink != 8 && ink != 36
#if DG_MODERN
        && ink != 32 && !arithmetic_ink(ink)
#endif
#if DG_D10
        && ink != 9
#endif
    ) {
      lv_fail(values, "unsupported native bitmap ink");
      return;
    }
    unsigned width, height;
    width = m->width; height = m->height;
    // Older profiles use the matte for blended Copy. The original Birthday
    // engine preserves the whole rectangle, including one-bit sources.
    // A background key is applied at draw time, since sprites sharing an
    // asset can choose different keys and change them without reloading.
    unsigned decode_ink = ink == 0 && dg_blended_copy_matte(opacity) ? 8 : ink;
#if DG_MODERN
    if (ink == 36 && m->type != 12) decode_ink = 0;
#endif
#if DG_D10
    if (ink == 9) decode_ink = 8;
#endif
#if defined(DIRECTOR64_NATIVE_STAGE) && DIRECTOR64_NATIVE_STAGE
    // A film-loop pose cut to its coverage keeps its own size and draws at
    // its offset in the loop's rectangle, by the two registration points.
    const wc_image_t *image = wc_image(c, values, m->asset, pose ? 0 : width, pose ? 0 : height, decode_ink);
    if (!image)
      return;
    wc_image_t cropped_pose;
    if (pose && (image->width != width || image->height != height) && width && height) {
      cropped_pose = *image;
      cropped_pose.source_width = width; cropped_pose.source_height = height;
      cropped_pose.source_x = m->reg_x - image->reg_x;
      cropped_pose.source_y = m->reg_y - image->reg_y;
      image = &cropped_pose;
    }
#else
    (void)pose;
    const wc_image_t *image = wc_image(c, values, m->asset, width, height, decode_ink);
    if (!image)
      return;
#endif
    if (ink == 0 && image->depth == 1 && dg_blended_copy_matte(opacity)) {
      opacity = 100;
      image = wc_image(c, values, m->asset, image->width, image->height, 0);
      if (!image) return;
    }
#if DG_D10
    wc_image_t masked = {0};
    if (ink == 9) {
      const dg_member_t *mask = dg_member(d, m->id + 1);
      if (mask && mask->type == 1 && mask_pair(m, mask)) {
        image = masked_image(c, d, m, mask, image, &masked);
        if (!image) return;
      }
    }
#endif
    draw_bitmap(c, d, s, image, l, t, r, b, ink, opacity, ox, oy);
#if DG_D10
    free(masked.pixels);
    draw_flash_overlays(c, d, i, s, m, ox, oy, NULL, false);
#endif
  } else if (m && (m->type == 3 || m->type == 7)) {
    const char *text = m->text;
    uint32_t color = indexed_color(d, s->value.fore);
    wc_text_style_t authored;
    int scroll;
    const wc_text_style_t *style = member_text_style(d, m, &authored, &scroll);
#if DG_MODERN
    if (m->text_style) {
      uint32_t rgb = m->text_style->color;
      color = opaque_rgb(rgb);
    } else
      color = sprite_color(d, s, false);
#endif
    for (unsigned j = 0; j < d->field_count; j++)
      if (d->field_members[j] == m->id) {
        text = lv_cstr(values, values->roots[DG_FIELD_TEXT_ROOT + j]);
#if DG_MODERN
        if (d->field_text_changed[j] && m->text_insert_style)
          color = opaque_rgb(m->text_insert_style->color);
#endif
        lv_t field_color = values->roots[DG_FIELD_COLOR_ROOT + j];
        if (lv_type(field_color) != LV_VOID) {
          unsigned value = (unsigned)lv_integer(values, field_color);
          color = lv_type(field_color) == LV_COLOR || value > 255
                      ? opaque_rgb(value)
                      : indexed_color(d, value);
        }
      }
    // The authored text in its colour runs (Optik's green link words); a
    // field the scripts rewrote draws in its one colour.
    // The per-pixel run plane holds 254 runs; later ones keep the last.
    static wc_text_run_t runs[254];
    unsigned run_count = 0;
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
    const dg_text_extra_t *extra = member_color_runs(d, m, text);
    if (extra)
      for (; run_count < extra->run_count && run_count < 254; run_count++)
        runs[run_count] = (wc_text_run_t){extra->runs[run_count].offset,
                                          opaque_rgb(extra->runs[run_count].rgb) & 0xffffffu};
#endif
    if(!draw_text(c, text, style, color & 0xffffffu, opacity * 255 / 100, l, t, r, b, runs, run_count, scroll))
      lv_fail(d->values,"text rasterization failed: font, allocation or dimensions");
  } else if ((m && m->type == 8) || s->value.type < 16) {
    unsigned thickness = s->value.thickness & 15;
    unsigned line = thickness ? thickness - 1 : 0;
    if (m && !m->filled && !line)
      return; // Invisible Director hit rectangle.
#if DG_MODERN
    uint32_t foreground = sprite_color(d, s, false);
    uint32_t background = sprite_color(d, s, true);
#else
    uint32_t foreground = indexed_color(d, s->value.fore);
    uint32_t background = indexed_color(d, s->value.back);
#endif
    unsigned kind = m ? m->shape : 1;
    unsigned pattern = dg_member_pattern(d,m);
    bool filled = !m || m->filled;
    const wc_image_t *tile = NULL;
    const char *texture=dg_shape_texture(d,m,pattern);
    if (texture && *texture) tile = wc_image(c, values, texture, 0, 0, 0);
    if (pattern > 56 && !tile) {
      lv_fail(values, "missing shape texture pattern asset");
      return;
    }
    bool transformed = false;
    float shape_quad[8];
    int shape_width = r - l, shape_height = b - t;
#if DG_MODERN
    transformed = s->value.rotation || s->value.skew || s->flip_h || s->flip_v || s->custom_quad;
    if (transformed) {
      dg_sprite_t flat = *s;
      flat.value.rotation = flat.value.skew = 0;
      flat.flip_h = flat.flip_v = flat.custom_quad = false;
      int fl, ft, fr, fb;
      dg_sprite_bounds(d, &flat, &fl, &ft, &fr, &fb);
      shape_width = fr - fl; shape_height = fb - ft;
      dg_sprite_quad(d, s, shape_quad);
      for (unsigned point = 0; point < 8; point += 2) {
        shape_quad[point] += ox; shape_quad[point + 1] += oy;
      }
    }
#endif
    if (!transformed && !tile && pattern <= 1
#if DG_D10
        && !c->film_transform_count
#endif
    ) {
      if (!dg_shape_ink_visible(true, s->value.ink, opacity, foreground, background)) return;
      uint32_t channels[3][256];
      for (unsigned component = 0; component < 3; component++)
        for (unsigned value = 0; value < 256; value++)
          channels[component][value] = dg_shape_ink_channel(true, value, s->value.ink,
              opacity, foreground >> (component * 8) & 255,
              background >> (component * 8) & 255) << (component * 8);
      for (int y = t < 0 ? 0 : t; y < b && y < WC_HEIGHT; y++) {
        int spans[4];
        unsigned count = dg_shape_spans(kind, r-l, b-t, y-t, line, filled,
                                        m && m->line_direction == 6, spans);
        for (unsigned span = 0; span < count; span++) {
          int left = l + spans[span*2], right = l + spans[span*2+1];
          if (left < 0) left = 0;
          if (right > WC_WIDTH) right = WC_WIDTH;
          for (int x = left; x < right; x++) {
            uint32_t *pixel = &target(c)[(size_t)y * WC_WIDTH + x], old = *pixel;
            *pixel = (old >> 24) == 255 ? 0xff000000u | channels[0][old & 255] |
                channels[1][old >> 8 & 255] | channels[2][old >> 16 & 255] :
                dg_rgba_shape(old, 0, false, true, s->value.ink, opacity, foreground, background);
          }
        }
      }
      return;
    }
    int shape_l=l,shape_t=t,shape_r=r,shape_b=b;
#if DG_D10
    film_destination_bounds(c,&shape_l,&shape_t,&shape_r,&shape_b);
#endif
    for (int y = shape_t < 0 ? 0 : shape_t; y < shape_b && y < WC_HEIGHT; y++) {
      int spans[4] = {shape_l-l, shape_r-l};
      bool mapped_shape=transformed;
#if DG_D10
      mapped_shape=mapped_shape || c->film_transform_count;
#endif
      unsigned n = mapped_shape ? 1 : dg_shape_spans(kind, r - l, b - t, y - t, line, filled,
                                  m && m->line_direction == 6, spans);
      for (unsigned k = 0; k < n; k++) {
        int left = l + spans[k * 2], right = l + spans[k * 2 + 1];
        if (left < 0) left = 0;
        if (right > WC_WIDTH) right = WC_WIDTH;
        for (int x = left; x < right; x++) {
          int shape_x = x - l, shape_y = y - t;
          float sample_x=x+0.5f,sample_y=y+0.5f;
#if DG_D10
          if (c->film_transform_count &&
              !dg_film_transform_point(c->film_transform,c->film_transform_count,
                  sample_x,sample_y,&sample_x,&sample_y)) continue;
          if (c->film_transform_count && !transformed) {
            if (sample_x<l || sample_y<t || sample_x>=r || sample_y>=b) continue;
            shape_x=(int)(sample_x-l);shape_y=(int)(sample_y-t);
            if (!dg_shape_contains(kind,shape_width,shape_height,shape_x,shape_y,
                line,filled,m && m->line_direction==6)) continue;
          }
#endif
          if (transformed) {
            float u, v;
            if (!dg_quad_uv(shape_quad, sample_x, sample_y, &u, &v)) continue;
#if DG_MODERN
            if (s->flip_h) u = 1 - u;
            if (s->flip_v) v = 1 - v;
#endif
            int sx = (int)(u * shape_width), sy = (int)(v * shape_height);
            if (sx >= shape_width) sx = shape_width - 1;
            if (sy >= shape_height) sy = shape_height - 1;
            shape_x = sx; shape_y = sy;
            if (!dg_shape_contains(kind, shape_width, shape_height, sx, sy, line, filled,
                                   m && m->line_direction == 6)) continue;
          }

#if DG_D10
          int stage_x = ((int)floorf(sample_x)) - ox,
              stage_y = ((int)floorf(sample_y)) - oy;
#else
          int stage_x = x - ox, stage_y = y - oy;
#endif
          bool stroke = filled && line && dg_shape_contains(kind, shape_width, shape_height,
              shape_x, shape_y, line, false, m && m->line_direction == 6);
          bool bit = stroke || dg_shape_pattern(pattern, stage_x, stage_y);
          bool use_tile = tile && !stroke;
          uint32_t sample = 0;
          if (use_tile) {
            unsigned tx = (unsigned)((stage_x % (int)tile->width + (int)tile->width) % (int)tile->width);
            unsigned ty = (unsigned)((stage_y % (int)tile->height + (int)tile->height) % (int)tile->height);
            sample = tile->pixels[(size_t)ty * tile->width + tx];
            if(tile->palette_brush) {
              const uint32_t *palette=dg_stage_palette(d);
              if(!palette){lv_fail(values,"shape texture has no active palette");return;}
              sample=opaque_rgb(palette[sample&255]);
            }
            if (!(sample >> 24) || (s->value.ink == 36 &&
                (sample & 0xffffffu) == (background & 0xffffffu))) continue;
          } else if (!dg_shape_ink_visible(bit, s->value.ink, opacity, foreground, background)) continue;
          uint32_t *destination = &target(c)[y * WC_WIDTH + x];
          *destination=dg_rgba_shape(*destination,sample,use_tile,bit,s->value.ink,
                                     opacity,foreground,background);
        }
      }
    }
  } else if (m) {
    static char message[64];
    snprintf(message, sizeof(message), "unsupported visible cast type %u", m->type);
    lv_fail(values, message);
  }
}

// Draws a scene; `ox, oy` shift it (D5's source dialog draws its movie at
// the dialog window's place over the suspended stage).
static void draw_scene(wc_t *c, dg_runtime_t *d, int ox, int oy, bool retained) {
#if DG_D10
  const dg_pixel_image_t *stage = dg_stage_image(d);
  if (stage) {
    int dx = ox, dy = oy;
    for (unsigned y = 0; y < WC_HEIGHT; y++)
      for (unsigned x = 0; x < WC_WIDTH; x++) {
        int sx = (int)x - dx, sy = (int)y - dy;
        target(c)[(size_t)y * WC_WIDTH + x] = sx >= 0 && sx < WC_WIDTH && sy >= 0 && sy < WC_HEIGHT
            ? stage->pixels[(size_t)((unsigned)sy * stage->height / WC_HEIGHT) * stage->width +
                            (unsigned)sx * stage->width / WC_WIDTH] | 0xff000000u
            : opaque_rgb(d->stage_color);
      }
    return;
  }
#endif
  lv_runtime_t *values = d->values;
  const uint16_t *order;
  unsigned count = dg_draw_order(d, &order);
  unsigned first = 0;
#if DG_D10
  unsigned prefix_end = 0;
  wc_film_key_t prefix = {0};
  if (retained && !ox && !oy && !c->target)
    prefix = prefix_key(c, d, order, count, &prefix_end);
  const wc_image_t *cached = prefix.valid ? plane_find(c, 3, prefix.data, prefix.count) : NULL;
  if (cached) {
    memcpy(target(c), cached->pixels, sizeof(c->pixels));
    c->prefix_hits++;
    first = prefix_end;
  } else
#else
  (void)retained;
#endif
#if DG_MODERN
  fill(c, 0, 0, WC_WIDTH, WC_HEIGHT, opaque_rgb(d->stage_color));
#else
  fill(c, 0, 0, WC_WIDTH, WC_HEIGHT, 0xffffffffu);
#endif
  for (unsigned direct=0;direct<2;direct++)
  for (unsigned draw = direct ? 0 : first; draw < count && !values->failed; draw++) {
    unsigned i = order[draw];
#if DG_D10
    const dg_member_t *member=dg_member(d,d->sprites[i].value.member);
    if ((member && member->flash_direct_to_stage)!=!!direct) continue;
#else
    if (direct) continue;
#endif
    for (unsigned pass = 0; pass <= d->trail_count; pass++) {
      if (pass < d->trail_count && d->trails[pass].channel != i)
        continue;
      const dg_sprite_t *s = pass < d->trail_count ? &d->trails[pass].sprite
                                                   : &d->sprites[i];
      draw_sprite(c, d, i, s, ox, oy);
    }
#if DG_D10
    if (!direct && !cached && prefix.valid && draw + 1 == prefix_end && !values->failed)
      prefix_capture(c, &prefix, target(c));
#endif
  }
#if DG_D10
  free(prefix.data);
#endif
}

// ---- Pointer ----

static bool native_cursor_plane(wc_t *, lv_runtime_t *, const dg_member_t *, uint16_t [256]);
static bool cursor_is_color(const dg_member_t *m) {
#if DG_D10
  return m && m->source_xtra == 5;
#else
  (void)m;
  return false;
#endif
}
static bool cursor_plane(wc_t *c, lv_runtime_t *values, const dg_member_t *m,
                         uint16_t out[256]) {
  // A cursor reads the stored RGBA5551 words themselves: its colours are
  // checked for the monochrome form, which the decoded RGBA plane cannot
  // tell from a near-white.
  memset(out, 0, 256 * sizeof(*out));
  uint32_t length = 0;
  uint8_t *data = m ? read_image(c, m->asset, &length) : NULL;
  bool ok = data && m->width && m->height && !bitmap_needs_tiles(m->width, m->height);
  bool plain = ok && length >= 32 && !memcmp(data, "FDI1", 4) &&
               32u + (uint64_t)m->width * m->height * 2 == length;
  bool wide = ok && length >= 32 && !memcmp(data, "FDI2", 4) &&
              32u + (uint64_t)m->width * m->height * 4 == length;
  ok = ok && (plain || wide);
  for (unsigned y = 0; ok && y < 16 && y < m->height; y++)
    for (unsigned x = 0; ok && x < 16 && x < m->width; x++) {
      size_t i = (size_t)y * m->width + x;
      if (wide) {
        uint32_t rgb = dg_fdi_be32(data + 32 + i * 4) >> 8;
        ok = !rgb || rgb == 0xffffff;
        out[y * 16 + x] = rgb ? 0xffff : 1;
      } else
        out[y * 16 + x] = (uint16_t)dg_fdi_be16(data + 32 + i * 2);
    }
  free(data);
  if (!ok)
    lv_fail(values, "invalid native cursor image");
  return ok;
}
static const dg_cursor_bitmap_t *cursor_bitmap(wc_t *c, dg_runtime_t *d,
                                               dg_cursor_t cursor) {
  static dg_cursor_bitmap_t builtin;
  if (!cursor.image) {
    if (!dg_cursor_builtin(&builtin, cursor.resource)) {
      lv_fail(d->values, "unsupported cursor resource");
      return NULL;
    }
    return &builtin;
  }
  const dg_member_t *m = dg_member(d, cursor.image);
  const dg_member_t *mask = cursor.mask ? dg_member(d, cursor.mask) : NULL;
  if (!m || !m->asset || (cursor.mask && (!mask || !mask->asset))) {
    lv_fail(d->values, "missing cursor bitmap member");
    return NULL;
  }
  const char *mask_asset = mask ? mask->asset : "";
  if (strlen(m->asset) >= sizeof(c->cursors[0].image) ||
      strlen(mask_asset) >= sizeof(c->cursors[0].mask)) {
    lv_fail(d->values, "missing cursor bitmap member");
    return NULL;
  }
  wc_cursor_t *slot = &c->cursors[0];
  for (unsigned i = 0; i < WC_CURSOR_SLOTS; i++) {
    wc_cursor_t *entry = &c->cursors[i];
    if (entry->last && dg_cursor_equal(entry->cursor, cursor) &&
        !strcmp(entry->image, m->asset) && !strcmp(entry->mask, mask_asset)) {
      entry->last = ++c->serial;
      return &entry->bitmap;
    }
    if (entry->last < slot->last)
      slot = entry;
  }
  uint16_t pixels[256], coverage[256];
  if (!(cursor_is_color(m) ? native_cursor_plane(c, d->values, m, pixels) : cursor_plane(c, d->values, m, pixels)) ||
      (mask && !cursor_plane(c, d->values, mask, coverage)))
    return NULL;
  if (!(cursor_is_color(m) ? dg_cursor_color(&slot->bitmap, pixels, m->width, m->height, m->reg_x, m->reg_y) :
      dg_cursor_compose(&slot->bitmap, pixels, m->width, m->height,
                         mask ? coverage : NULL, mask ? mask->width : 0,
                         mask ? mask->height : 0, m->reg_x, m->reg_y))) {
    lv_fail(d->values, "unsupported color cursor bitmap");
    return NULL;
  }
  slot->cursor = cursor;
  memcpy(slot->image, m->asset, strlen(m->asset) + 1);
  memcpy(slot->mask, mask_asset, strlen(mask_asset) + 1);
  slot->last = ++c->serial;
  return &slot->bitmap;
}
static void draw_cursor(wc_t *c, dg_runtime_t *d, dg_cursor_t cursor,
                        wc_pointer_t at) {
  if (!cursor.image && cursor.resource == 200)
    return; // Director's hidden cursor.
  const dg_cursor_bitmap_t *bitmap = cursor_bitmap(c, d, cursor);
  if (!bitmap)
    return;
  uint16_t pixels[256];
  dg_cursor_pixels_ink(bitmap, pixels, (uint16_t)at.ink);
  for (int row = 0; row < DG_CURSOR_SIZE; row++)
    for (int column = 0; column < DG_CURSOR_SIZE; column++) {
      int px = at.x - bitmap->hot_x + column, py = at.y - bitmap->hot_y + row;
      uint16_t pixel = pixels[row * DG_CURSOR_SIZE + column];
      if ((pixel & 1) && px >= 0 && py >= 0 && px < WC_WIDTH && py < WC_HEIGHT)
        target(c)[py * WC_WIDTH + px] = from5551(pixel);
    }
}

#if DG_D10
void wc_capture_stage(wc_t *c, uint32_t member) {
  wc_image_t *snapshot = &c->stage_snapshot;
  if (!snapshot->pixels) {
    snapshot->pixels = malloc(sizeof(c->scene));
    if (!snapshot->pixels)
      return;
    snapshot->bytes = sizeof(c->scene);
    snapshot->width = WC_WIDTH;
    snapshot->height = WC_HEIGHT;
    // Opaque throughout (below): its cover is the whole stage.
    snapshot->cover_l = snapshot->cover_t = 0;
    snapshot->cover_r = WC_WIDTH;
    snapshot->cover_b = WC_HEIGHT;
  }
  // Before anything was composited the stage is white.
  if (c->drawn)
    memcpy(snapshot->pixels, c->scene, sizeof(c->scene));
  else
    memset(snapshot->pixels, 255, sizeof(c->scene));
  for (size_t i = 0; i < (size_t)WC_WIDTH * WC_HEIGHT; i++)
    snapshot->pixels[i] |= 0xff000000u;
  c->snapshot_member = member;
}
#endif

bool wc_render(wc_t *c, dg_runtime_t *d, int pointer_x, int pointer_y,
               bool pointer_shown, unsigned externals) {
  const wc_pointer_t pointer = {pointer_x, pointer_y, 1};
  return wc_render_pointers(c, d, &pointer, pointer_shown ? 1 : 0, externals);
}
bool wc_render_pointers(wc_t *c, dg_runtime_t *d, const wc_pointer_t *pointers,
                        unsigned count, unsigned externals) {
  if (count > WC_POINTERS)
    count = WC_POINTERS;
  dg_cursor_t cursor = dg_cursor_current(d);
  dg_update_stage(d);
  unsigned fade_age = d->ticks - d->palette_fade_start;
  unsigned fade_half = d->palette_fade_half;
  bool fading = fade_half && fade_age < fade_half * 2;
  bool fade_changed = (fading || c->palette_previous) && c->palette_tick != d->ticks;
  if (c->palette_serial != d->palette_fade_serial || c->owner != d) {
    free(c->palette_previous);
    c->palette_previous = NULL;
    if (fading && c->drawn && c->owner == d) {
      c->palette_previous = malloc(sizeof(c->scene));
      if (!c->palette_previous) lv_fail(d->values, "palette fade snapshot allocation failed");
      else memcpy(c->palette_previous, c->scene, sizeof(c->scene));
    }
    c->palette_serial = d->palette_fade_serial;
    fade_changed = true;
  }
  uint32_t background = 0;
#if DG_MODERN
  background = d->stage_color;
#endif
#if DG_D5
  externals = externals * 31 + (d->ticks < d->alert_until);
  externals = externals * 31 + d->native_dialog;
#endif
  bool same_scene = c->drawn && c->owner == d && c->movie == d->movie &&
      c->revision == d->visual_revision && c->background == background &&
      c->externals == externals;
  if (!fade_changed && same_scene && dg_cursor_equal(c->cursor, cursor) && c->pointer_count == count &&
      !memcmp(c->pointers, pointers, count * sizeof(*pointers)) &&
      !d->values->failed)
    return false;
  c->serial++;
  c->renders++;
  if (same_scene) {
    memcpy(target(c), c->scene, sizeof(c->pixels));
  } else {
    c->scene_renders++;
#if DG_D5
  if (d->native_dialog && d->suspended_stage) {
    // The source dialog window (416x240) over the suspended stage.
    static uint32_t stage[WC_WIDTH * WC_HEIGHT];
    draw_scene(c, d->suspended_stage, 0, 0, false);
    memcpy(stage, target(c), sizeof(stage));
    draw_scene(c, d, 112, 100, false);
    for (int y = 0; y < WC_HEIGHT; y++)
      for (int x = 0; x < WC_WIDTH; x++)
        if (x < 112 || x >= 528 || y < 100 || y >= 340)
          target(c)[y * WC_WIDTH + x] = stage[y * WC_WIDTH + x];
  } else
#endif
    draw_scene(c, d, 0, 0, true);
#if DG_D5
  if (d->ticks < d->alert_until && !d->values->failed) {
    // A recovered script alert, in the builtin monospace font.
    fill(c, 48, 180, 592, 290, 0xffffffffu);
    if(!draw_text(c, d->alert_text, NULL, 0, 255, 64, 201, 574, 281, NULL, 0, 0))
      lv_fail(d->values,"alert text rasterization failed");
  }
#endif
    memcpy(c->scene, target(c), sizeof(c->scene));
  }
  if (fading) {
    unsigned alpha = fade_age < fade_half ? fade_age * 255 / fade_half
                                         : (fade_half * 2 - fade_age) * 255 / fade_half;
    if (fade_age < fade_half && c->palette_previous)
      memcpy(target(c), c->palette_previous, sizeof(c->scene));
    uint32_t color = d->palette_fade_kind == 0x40 ? 0xffffffffu : 0xff000000u;
    for (unsigned i = 0; i < WC_WIDTH * WC_HEIGHT; i++) blend(&target(c)[i], color & 0xffffffu, alpha);
  } else { free(c->palette_previous); c->palette_previous = NULL; }
  c->palette_tick = d->ticks;
  for (unsigned i = 0; i < count && !d->values->failed; i++)
    draw_cursor(c, d, cursor, pointers[i]);
  c->drawn = true;
  c->owner = d;
  c->movie = d->movie;
  c->revision = d->visual_revision;
  c->cursor = cursor;
  memcpy(c->pointers, pointers, count * sizeof(*pointers));
  c->pointer_count = count;
  c->background = background;
  c->externals = externals;
  return true;
}

// ---- The probe state's cursor (platforms/native/image.c) ----

static bool native_cursor_plane(wc_t *c, lv_runtime_t *v, const dg_member_t *m,
                                uint16_t out[256]) {
  if (!m || !m->asset || !*m->asset) {
    lv_fail(v, "native cursor bitmap missing");
    return false;
  }
  uint32_t length;
  uint8_t *data = read_image(c, m->asset, &length);
  if (!data) {
    lv_fail(v, "native cursor image missing");
    return false;
  }
  bool ok = length >= 32;
  bool rgba32 = ok && !memcmp(data, "FDI2", 4);
  bool planes = ok && !memcmp(data, "FDIA", 4);
  unsigned stride = rgba32 ? 4 : 2;
  uint64_t expected = 32u + bitmap_plane_pixels(m->width, m->height) * stride;
  uint32_t alpha_offset;
  ok = ok && m->width && m->height && !bitmap_needs_tiles(m->width, m->height);
  if (ok && planes)
    ok = dg_fdia_valid(data, length, m->width, m->height, &alpha_offset);
  else if (ok)
    ok = (rgba32 || !memcmp(data, "FDI1", 4)) && expected == length &&
         dg_fdi_be32(data + 4) == expected && dg_fdi_be16(data + 8) == m->width &&
         dg_fdi_be16(data + 10) == m->height;
  memset(out, 0, 256 * sizeof(*out));
  unsigned width = m->width < 16 ? m->width : 16;
  for (unsigned y = 0; ok && y < 16 && y < m->height; y++)
    for (unsigned x = 0; ok && x < width; x++) {
      const uint8_t *p = data + 32 + ((size_t)y * m->width + x) * stride;
      if (rgba32) {
        uint32_t pixel = dg_fdi_be32(p), rgb = pixel >> 8;
        if (cursor_is_color(m))
          out[y * 16 + x] = (uint16_t)(((pixel >> 27) << 11) | (((pixel >> 19) & 31) << 6) |
                                      (((pixel >> 11) & 31) << 1) | ((pixel & 255) >= 128));
        else {
          ok = !rgb || rgb == 0xffffff;
          out[y * 16 + x] = rgb ? 0xffff : 1;
        }
      } else
        out[y * 16 + x] = (uint16_t)dg_fdi_be16(p);
    }
  free(data);
  if (!ok)
    lv_fail(v, "invalid native cursor image");
  return ok;
}
bool wc_cursor_bitmap(wc_t *c, lv_runtime_t *v, const dg_member_t *m,
                      const dg_member_t *mask, dg_cursor_bitmap_t *out) {
  uint16_t pixels[256], coverage[256];
  if (!native_cursor_plane(c, v, m, pixels) ||
      (mask && !native_cursor_plane(c, v, mask, coverage)))
    return false;
  bool ok = cursor_is_color(m) ? dg_cursor_color(out, pixels, m->width, m->height, m->reg_x, m->reg_y) :
      dg_cursor_compose(out, pixels, m->width, m->height, mask ? coverage : NULL,
                              mask ? mask->width : 0, mask ? mask->height : 0,
                              m->reg_x, m->reg_y);
  if (!ok)
    lv_fail(v, "unsupported color cursor bitmap");
  return ok;
}
