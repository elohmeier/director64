#include "compositor.h"
#include "bitmap_tiles.h"
#include "ink.h"
#include "shape.h"
#include <stdlib.h>
#include <string.h>

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
#if !DG_MODERN
  // Coverage-carrying images are a D7-and-later form; plain D6 reads the
  // matte bit only, as the console's loader does for this family.
  follow = false;
#endif
  *follow_alpha = follow;
  bool tiled = bitmap_needs_tiles(width, height);
  for (unsigned y = 0; y < height; y++)
    for (unsigned x = 0; x < width; x++) {
      uint64_t i = tiled ? bitmap_tile_index(width, x, y)
                         : (uint64_t)y * width + x;
      uint32_t pixel;
      if (indexed) {
        uint16_t word = dg_fdic_texel(data, bits, index_offset, width, height,
                                      x, y);
        pixel = from5551(dg_bitmap_ink16(word, ink, follow));
      } else if (rgba32) {
        pixel = from8888(dg_bitmap_ink32(dg_fdi_be32(data + 32 + i * 4), ink,
                                         follow));
      } else if (planes) {
        uint16_t word = (uint16_t)dg_fdi_be16(data + 32 + i * 2);
        uint8_t alpha = dg_bitmap_alpha16(word, data[alpha_offset + i], ink);
        pixel = (from5551(word) & 0xffffffu) | (uint32_t)alpha << 24;
      } else {
        uint16_t word = (uint16_t)dg_fdi_be16(data + 32 + i * 2);
        pixel = from5551(dg_bitmap_ink16(word, ink, follow));
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
  for (unsigned i = 0; i < WC_IMAGE_SLOTS; i++)
    if (c->images[i].pixels)
      evict(c, &c->images[i]);
  for (unsigned i = 0; i < WC_TEXT_SLOTS; i++)
    free(c->texts[i].coverage);
  memset(c->texts, 0, sizeof(c->texts));
}
// Least recently used, never an image the composite in progress has drawn.
static bool evict_oldest(wc_t *c) {
  wc_image_t *oldest = NULL;
  for (unsigned i = 0; i < WC_IMAGE_SLOTS; i++) {
    wc_image_t *image = &c->images[i];
    if (image->pixels && image->last != c->serial &&
        (!oldest || image->last < oldest->last))
      oldest = image;
  }
  if (!oldest)
    return false;
  evict(c, oldest);
  return true;
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
  size_t bytes = (size_t)width * height * 4;
  while ((c->image_bytes + bytes > c->image_budget || !free_slot) &&
         evict_oldest(c))
    if (!free_slot)
      for (unsigned i = 0; i < WC_IMAGE_SLOTS && !free_slot; i++)
        if (!c->images[i].pixels)
          free_slot = &c->images[i];
  if (!free_slot) {
    lv_fail(values, "image cache handles exhausted");
    return NULL;
  }
  uint32_t length = c->host.read(c->host.ctx, asset, NULL, 0);
  if (!length) {
    lv_fail(values, "missing native image file");
    return NULL;
  }
  uint8_t *data = malloc(length);
  uint32_t *pixels = malloc(bytes ? bytes : 4);
  bool follow = false;
  bool ok = data && pixels &&
            c->host.read(c->host.ctx, asset, data, length) == length &&
            wc_decode(data, length, width, height, ink, pixels, &follow);
  free(data);
  if (!ok) {
    free(pixels);
    lv_fail(values, data && pixels ? "invalid native image"
                                   : "native image allocation");
    return NULL;
  }
  wc_image_t *image = free_slot;
  memcpy(image->name, asset, strlen(asset) + 1);
  image->ink = ink;
  image->width = width;
  image->height = height;
  image->follow_alpha = follow;
  image->pixels = pixels;
  image->bytes = bytes;
  image->last = c->serial;
  c->image_bytes += bytes;
  c->loads++;
  return image;
}

// A member's stored image dimensions: D10 converts its authored 800x600
// stage's images prescaled to 640x480 (ink.h), while the member keeps its
// authored dimensions for layout and scripts.
static void stored_dims(const dg_member_t *m, unsigned *width, unsigned *height) {
#if DG_D10
  dg_prescale_dims(m->width, m->height, width, height);
#else
  *width = m->width;
  *height = m->height;
#endif
}

bool wc_hit(wc_t *c, lv_runtime_t *values, const dg_member_t *m, unsigned ink,
            int x, int y) {
  // Director only uses the bitmap matte for matte ink. Background-transparent
  // sprites still receive input throughout their rectangular bounds.
  if (ink != 8)
    return true;
  if (x < 0 || y < 0 || x >= m->width || y >= m->height)
    return false;
  unsigned width, height;
  stored_dims(m, &width, &height);
#if DG_D10
  // Map the authored pixel onto the prescaled image, as the native probe does.
  x = (int)((unsigned)x * width / m->width);
  y = (int)((unsigned)y * height / m->height);
  if ((unsigned)x >= width) x = (int)width - 1;
  if ((unsigned)y >= height) y = (int)height - 1;
#endif
  const wc_image_t *image = wc_image(c, values, m->asset, width, height, ink);
  return image && (image->pixels[(size_t)y * image->width + (unsigned)x] >> 24);
}

#if DG_D10
// Director's mask ink: the cast member after the sprite's is its mask, and
// light mask pixels make the image transparent. The corpus pairs every
// masked image with a "<name>_mask" member; any other neighbour leaves the
// image as it is, as the console's loader does (director_main.c mask_pair).
static bool mask_pair(const dg_member_t *member, const dg_member_t *mask) {
  size_t length = strlen(member->name);
  return !strncmp(mask->name, member->name, length) &&
         !strcmp(mask->name + length, "_mask");
}
static void apply_mask(wc_t *c, const dg_member_t *mask, wc_image_t *image) {
  unsigned width, height;
  stored_dims(mask, &width, &height);
  uint32_t length = c->host.read(c->host.ctx, mask->asset, NULL, 0);
  uint8_t *data = length ? malloc(length) : NULL;
  bool ok = data && c->host.read(c->host.ctx, mask->asset, data, length) == length &&
            length >= 32 && !memcmp(data, "FDI1", 4) && width && height &&
            dg_fdi_be16(data + 8) == width && dg_fdi_be16(data + 10) == height &&
            32u + bitmap_plane_pixels(width, height) * 2u <= length;
  // Authored masks can differ by a pixel from their image; sampling clamps
  // over the overlap, like the original.
  for (unsigned y = 0; ok && y < image->height; y++) {
    unsigned my = y < height ? y : height - 1;
    for (unsigned x = 0; x < image->width; x++) {
      unsigned mx = x < width ? x : width - 1;
      uint64_t i = bitmap_needs_tiles(width, height) ? bitmap_tile_index(width, mx, my)
                                                     : (uint64_t)my * width + mx;
      uint16_t p = (uint16_t)dg_fdi_be16(data + 32 + i * 2);
      unsigned luminance = ((p >> 11) & 31) * 77 + ((p >> 6) & 31) * 151 + ((p >> 1) & 31) * 28;
      uint32_t *pixel = &image->pixels[(size_t)y * image->width + x];
      if (luminance >= 3968)
        *pixel &= 0x00ffffffu;
      else if (!image->follow_alpha)
        *pixel |= 0xff000000u;
    }
  }
  free(data);
}
#endif

// ---- Drawing ----

static inline void blend(uint32_t *d, uint32_t s, unsigned alpha) {
  if (alpha >= 255) {
    *d = s | 0xff000000u;
    return;
  }
  uint32_t o = *d, result = 0xff000000u;
  for (unsigned shift = 0; shift < 24; shift += 8) {
    unsigned sc = (s >> shift) & 255, dc = (o >> shift) & 255;
    result |= ((sc * alpha + dc * (255 - alpha) + 127) / 255) << shift;
  }
  *d = result;
}
static void fill(wc_t *c, int l, int t, int r, int b, uint32_t color) {
  if (l < 0) l = 0;
  if (t < 0) t = 0;
  if (r > WC_WIDTH) r = WC_WIDTH;
  if (b > WC_HEIGHT) b = WC_HEIGHT;
  for (int y = t; y < b; y++)
    for (int x = l; x < r; x++)
      c->pixels[y * WC_WIDTH + x] = color;
}
#if DG_MODERN
// Director's Darken ink: the RGB minimum, or an ordinary blend when the sprite
// has a blend of its own (ScummVM graphics.cpp), over the source coverage.
static inline void darken(uint32_t *d, uint32_t s, unsigned coverage,
                          unsigned opacity) {
  unsigned alpha = coverage * opacity;
  if (!alpha)
    return;
  uint32_t o = *d, result = 0xff000000u;
  for (unsigned shift = 0; shift < 24; shift += 8) {
    unsigned sc = (s >> shift) & 255, dc = (o >> shift) & 255;
    unsigned ink = opacity < 100 ? sc : (sc < dc ? sc : dc);
    result |= ((ink * alpha + dc * (25500u - alpha)) / 25500u) << shift;
  }
  *d = result;
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
#if DG_D10
    // The quad is authored; it rasterizes on the prescaled stage.
    for (unsigned k = 0; k < 8; k++)
      q[k] = q[k] * 4.0f / 5.0f;
#endif
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
  unsigned w = image->width, h = image->height;
  int64_t span_x = (int64_t)r - l, span_y = (int64_t)b - t;
  for (int y = ct; y < cb; y++) {
    uint32_t *row = &c->pixels[y * WC_WIDTH];
    unsigned sy = (unsigned)(((int64_t)(y - t) * 2 + 1) * h / (span_y * 2));
    if (sy >= h)
      sy = h - 1;
    for (int x = cl; x < cr; x++) {
      unsigned sx;
      if (transformed) {
        float u, v;
        if (!dg_quad_uv(q, x + 0.5f, y + 0.5f, &u, &v))
          continue;
#if DG_MODERN
        if (s->flip_h) u = 1 - u;
        if (s->flip_v) v = 1 - v;
#endif
        sx = (unsigned)(u * w);
        sy = (unsigned)(v * h);
        if (sx >= w) sx = w - 1;
        if (sy >= h) sy = h - 1;
      } else {
        sx = (unsigned)(((int64_t)(x - l) * 2 + 1) * w / (span_x * 2));
        if (sx >= w)
          sx = w - 1;
      }
      uint32_t pixel = image->pixels[(size_t)sy * w + sx];
      unsigned coverage = pixel >> 24;
#if DG_MODERN
      if (ink == 39) {
        darken(&row[x], pixel, coverage, opacity);
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
static void draw_text(wc_t *c, const char *text, const wc_text_style_t *style,
                      uint32_t color, unsigned alpha, int l, int t, int r, int b) {
  int width = r - l, height = b - t;
  if (!c->host.text || !text || !*text || width <= 0 || height <= 0 ||
      width > 4096 || height > 4096)
    return;
  // Recovered text is UTF-8; anything else is taken as Latin-1 rather than
  // handed to the host as malformed UTF-8.
  static char converted[8192];
  if (!text_is_utf8(text)) {
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
    text = converted;
  }
  uint32_t key = text_key(text, width, height, style);
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
    slot->coverage = calloc((size_t)width * height, 1);
    if (!slot->coverage || !c->host.text(c->host.ctx, text, width, height,
                                         style, slot->coverage)) {
      free(slot->coverage);
      slot->coverage = NULL;
      return;
    }
    slot->key = key;
    slot->width = width;
    slot->height = height;
  }
  slot->last = c->serial;
  for (int y = 0; y < height; y++) {
    if (t + y < 0 || t + y >= WC_HEIGHT)
      continue;
    for (int x = 0; x < width; x++) {
      if (l + x < 0 || l + x >= WC_WIDTH)
        continue;
      unsigned coverage = slot->coverage[y * width + x];
      if (coverage)
        blend(&c->pixels[(t + y) * WC_WIDTH + l + x], color,
              coverage * alpha / 255);
    }
  }
}

static uint32_t indexed_color(const dg_runtime_t *d, unsigned index) {
  return opaque_rgb(d->movie->palette[index & 255]);
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
// above the flattened card (director_main.c draw_flash_overlays): wrapped in
// authored member space with the field's measured advances, so the lines
// agree with the textHeight the scripts read.
static void draw_flash_overlays(wc_t *c, dg_runtime_t *d, unsigned channel,
                                const dg_sprite_t *s, const dg_member_t *m) {
  lv_runtime_t *values = d->values;
  static char wrapped[2048];
  for (unsigned f = 0; m && f < m->flash_field_count; f++) {
    const dg_flash_field_t *field = &m->flash_fields[f];
    // A field the scripts have addressed reads its live state from the
    // object bag; an untouched one shows its authored initial text.
    lv_t bag = {0};
    for (unsigned n = 0; n < d->flash_object_count; n++)
      if (d->flash_objects[n].sprite == channel &&
          dg_flash_field_find(m, d->flash_objects[n].name) == field) {
        bag = values->roots[DG_FLASH_ROOT + n];
        break;
      }
    const char *text = field->text;
    int fx = field->x, fy = field->y, fw = field->width, fh = field->height;
    unsigned size = field->style->size;
    uint32_t color = opaque_rgb(field->style->color);
    if (lv_type(bag) == LV_PROPLIST) {
      if (!lv_truth(values, lv_get(values, NULL, "_visible", bag)))
        continue;
      lv_t value = lv_get(values, NULL, "text", bag);
      if (lv_type(value) != LV_STRING)
        continue;
      text = lv_cstr(values, value);
      lv_t v;
      if (lv_type((v = lv_get(values, NULL, "_x", bag))) == LV_NUMBER) fx = (int)lv_numeric(v);
      if (lv_type((v = lv_get(values, NULL, "_y", bag))) == LV_NUMBER) fy = (int)lv_numeric(v);
      if (lv_type((v = lv_get(values, NULL, "_width", bag))) == LV_NUMBER) fw = (int)lv_numeric(v);
      if (lv_type((v = lv_get(values, NULL, "_height", bag))) == LV_NUMBER) fh = (int)lv_numeric(v);
      if (lv_type((v = lv_get(values, NULL, "size", bag))) == LV_NUMBER && lv_numeric(v) > 0)
        size = (unsigned)lv_numeric(v);
      if (lv_type((v = lv_get(values, NULL, "color", bag))) == LV_COLOR)
        color = opaque_rgb((uint32_t)lv_id(v));
    }
    if (!*text)
      continue;
    dg_flash_field_wrap(field, size, text, fw, wrapped, sizeof(wrapped));
    const dg_text_style_t *st = field->style;
    int ascent = st->ascent;
    if (size != st->size)
      ascent = ascent * (int)size / (st->size ? (int)st->size : 1);
    int line_height = (int)size + (field->leading < 0 ? 0 : field->leading);
    wc_text_style_t style = {st->font_id, (size * 4 + 2) / 5, st->align,
                             (ascent * 4 + 2) / 5, (line_height * 4 + 2) / 5};
    int al, at, ar, ab;
    dg_sprite_bounds(d, s, &al, &at, &ar, &ab);
    float sx = m->width ? (float)(ar - al) / m->width : 1.0f;
    float sy = m->height ? (float)(ab - at) / m->height : 1.0f;
    // The SWF renderer keeps a two-pixel gutter inside the field bounds.
    int l = al + (int)((fx + field->margin_left + field->indent) * sx) + 2;
    int t = at + (int)(fy * sy) + 2;
    int r = al + (int)((fx + fw - field->margin_right) * sx) - 2;
    int b = at + (int)((fy + fh) * sy);
    l = dg_prescale_coord(l); t = dg_prescale_coord(t);
    r = dg_prescale_coord(r); b = dg_prescale_coord(b);
    draw_text(c, wrapped, &style, color & 0xffffffu, dg_opacity(s) * 255 / 100, l, t, r, b);
  }
}
#endif

#if DG_D5
// A digital video sprite shows the frame the host has decoded for its current
// time (d5_video.c draws the console's), stretched to the sprite's bounds.
static void draw_video(wc_t *c, unsigned sprite, int l, int t, int r, int b,
                       unsigned opacity) {
  static uint32_t frame[1024 * 768];
  unsigned w = 0, h = 0;
  if (!c->host.video_frame ||
      !c->host.video_frame(c->host.ctx, sprite, frame, sizeof(frame) / sizeof(*frame), &w, &h) ||
      !w || !h || (size_t)w * h > sizeof(frame) / sizeof(*frame))
    return;
  int cl = l < 0 ? 0 : l, ct = t < 0 ? 0 : t;
  int cr = r > WC_WIDTH ? WC_WIDTH : r, cb = b > WC_HEIGHT ? WC_HEIGHT : b;
  for (int y = ct; y < cb; y++) {
    unsigned sy = (unsigned)((int64_t)(y - t) * h / (b - t));
    for (int x = cl; x < cr; x++) {
      unsigned sx = (unsigned)((int64_t)(x - l) * w / (r - l));
      blend(&c->pixels[y * WC_WIDTH + x], frame[(size_t)sy * w + sx] & 0xffffffu,
            255 * opacity / 100);
    }
  }
}
#endif

// Draws a scene; `ox, oy` shift it (D5's source dialog draws its movie at
// the dialog window's place over the suspended stage).
static void draw_scene(wc_t *c, dg_runtime_t *d, int ox, int oy) {
  lv_runtime_t *values = d->values;
#if DG_MODERN
  fill(c, 0, 0, WC_WIDTH, WC_HEIGHT, opaque_rgb(d->stage_color));
#else
  fill(c, 0, 0, WC_WIDTH, WC_HEIGHT, 0xffffffffu);
#endif
  const uint16_t *order;
  unsigned count = dg_draw_order(d, &order);
  for (unsigned draw = 0; draw < count && !values->failed; draw++) {
    unsigned i = order[draw];
    for (unsigned pass = 0; pass <= d->trail_count; pass++) {
      if (pass < d->trail_count && d->trails[pass].channel != i)
        continue;
      const dg_sprite_t *s = pass < d->trail_count ? &d->trails[pass].sprite
                                                   : &d->sprites[i];
      const dg_member_t *m = dg_member(d, s->value.member);
      // Palettes, sounds and script casts have no visual widget in Director.
      if (m && (m->type == 4 || m->type == 6 || m->type == 11 || m->type == 14))
        continue;
      if (m && (m->type == 1 || m->type == 2) && (!m->width || !m->height))
        continue;
      dg_member_t film;
      if (m && m->type == 2 && m->film_count) {
        film = *m;
        film.asset = m->film_assets[dg_film_pose(m, s)];
        film.type = 1;
        m = &film;
      }
      unsigned opacity = dg_opacity(s);
      if (!s->visible || !s->value.type || !opacity)
        continue;
      int l, t, r, b;
      dg_sprite_bounds(d, s, &l, &t, &r, &b);
      l += ox; r += ox;
      t += oy; b += oy;
#if DG_D10
      // Authored 800x600 geometry rasterizes onto the 640x480 stage.
      l = dg_prescale_coord(l); t = dg_prescale_coord(t);
      r = dg_prescale_coord(r); b = dg_prescale_coord(b);
#endif
      if (r <= 0 || b <= 0 || l >= WC_WIDTH || t >= WC_HEIGHT || r <= l || b <= t)
        continue;
#if DG_D5
      if (m && m->type == 10) {
        draw_video(c, i, l, t, r, b, opacity);
        continue;
      }
#endif
      if (m && m->type == 1) {
        unsigned ink = s->value.ink;
        if (ink != 0 && ink != 8 && ink != 36
#if DG_MODERN
            && ink != 32 && ink != 39
#endif
#if DG_D10
            && ink != 9
#endif
        ) {
          lv_fail(values, "unsupported native bitmap ink");
          break;
        }
        unsigned width, height;
        stored_dims(m, &width, &height);
        unsigned loads = c->loads;
        const wc_image_t *image = wc_image(c, values, m->asset, width, height, ink);
        if (!image)
          break;
#if DG_D10
        if (ink == 9 && c->loads != loads) {
          const dg_member_t *mask = dg_member(d, m->id + 1);
          if (mask && mask->type == 1 && mask->asset && mask->asset[0] && mask_pair(m, mask))
            apply_mask(c, mask, (wc_image_t *)image);
        }
#else
        (void)loads;
#endif
        draw_bitmap(c, d, s, image, l, t, r, b, ink, opacity, ox, oy);
#if DG_D10
        draw_flash_overlays(c, d, i, s, m);
#endif
      } else if (m && (m->type == 3 || m->type == 7)) {
        const char *text = m->text;
        uint32_t color = indexed_color(d, s->value.fore);
        const wc_text_style_t *style = NULL;
#if DG_MODERN
        wc_text_style_t authored;
        if (m->text_style) {
          const dg_text_style_t *st = m->text_style;
          authored = (wc_text_style_t){st->font_id, st->size, st->align,
                                       st->ascent, st->line_height};
#if DG_D10
          // Glyphs render at the stage prescale, as the console's 4/5 font
          // variants do; the vertical metrics scale with the sprite bounds.
          authored.size = (authored.size * 4 + 2) / 5;
          authored.ascent = (authored.ascent * 4 + 2) / 5;
          authored.line_height = (authored.line_height * 4 + 2) / 5;
#endif
          style = &authored;
        }
#endif
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
            lv_t field_color = values->roots[DG_FIELD_COLOR_ROOT + j];
            if (lv_type(field_color) != LV_VOID) {
              unsigned value = (unsigned)lv_integer(values, field_color);
              color = lv_type(field_color) == LV_COLOR || value > 255
                          ? opaque_rgb(value)
                          : indexed_color(d, value);
            }
          }
        draw_text(c, text, style, color & 0xffffffu, opacity * 255 / 100, l, t, r, b);
      } else if ((m && m->type == 8) || s->value.type < 16) {
        unsigned thickness = s->value.thickness & 15;
        unsigned line = thickness ? thickness - 1 : 0;
        if (m && !m->filled && !line)
          continue; // Invisible Director hit rectangle.
#if DG_MODERN
        uint32_t foreground = sprite_color(d, s, false);
        uint32_t background = sprite_color(d, s, true);
        // Background-transparent shapes with matching colours are hit areas.
        if (s->value.ink == 36 && foreground == background)
          continue;
#else
        uint32_t foreground = indexed_color(d, s->value.fore);
#endif
        unsigned kind = m ? m->shape : 1;
        bool filled = !m || m->filled;
        if (kind == 1 && filled)
          fill(c, l, t, r, b, foreground);
        else
          for (int y = t < 0 ? 0 : t; y < b && y < WC_HEIGHT; y++) {
            int spans[4];
            unsigned n = dg_shape_spans(kind, r - l, b - t, y - t, line, filled,
                                        m && m->line_direction == 6, spans);
            for (unsigned k = 0; k < n; k++)
              if (spans[k * 2] < spans[k * 2 + 1])
                fill(c, l + spans[k * 2], y, l + spans[k * 2 + 1], y + 1,
                     foreground);
          }
      } else if (m) {
#if DG_D10
        // Policy-pinned deferred Xtra members (the Flash intro timelines)
        // stay data-only and draw nothing, as on the console.
        if (m->type == 15)
          continue;
#endif
        lv_fail(values, "unsupported visible cast type");
      }
    }
  }
}

// ---- Pointer ----

static bool cursor_plane(wc_t *c, lv_runtime_t *values, const dg_member_t *m,
                         uint16_t out[256]) {
  // A cursor reads the stored RGBA5551 words themselves: its colours are
  // checked for the monochrome form, which the decoded RGBA plane cannot
  // tell from a near-white.
  memset(out, 0, 256 * sizeof(*out));
  uint32_t length = m && m->asset ? c->host.read(c->host.ctx, m->asset, NULL, 0) : 0;
  uint8_t *data = length ? malloc(length) : NULL;
  bool ok = data && c->host.read(c->host.ctx, m->asset, data, length) == length &&
            m->width && m->height && !bitmap_needs_tiles(m->width, m->height);
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
  if (!cursor_plane(c, d->values, m, pixels) ||
      (mask && !cursor_plane(c, d->values, mask, coverage)))
    return NULL;
  if (!dg_cursor_compose(&slot->bitmap, pixels, m->width, m->height,
                         mask ? coverage : NULL, mask ? mask->width : 0,
                         mask ? mask->height : 0, m->reg_x, m->reg_y)) {
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
        c->pixels[py * WC_WIDTH + px] = from5551(pixel);
    }
}

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
  uint32_t background = 0;
#if DG_MODERN
  background = d->stage_color;
#endif
#if DG_D5
  externals = externals * 31 + (d->ticks < d->alert_until);
  externals = externals * 31 + d->native_dialog;
#endif
  if (c->drawn && c->revision == d->visual_revision &&
      dg_cursor_equal(c->cursor, cursor) && c->pointer_count == count &&
      !memcmp(c->pointers, pointers, count * sizeof(*pointers)) &&
      c->background == background && c->externals == externals)
    return false;
  c->serial++;
  c->renders++;
#if DG_D5
  if (d->native_dialog && d->suspended_stage) {
    // The source dialog window (416x240) over the suspended stage.
    static uint32_t stage[WC_WIDTH * WC_HEIGHT];
    draw_scene(c, d->suspended_stage, 0, 0);
    memcpy(stage, c->pixels, sizeof(stage));
    draw_scene(c, d, 112, 100);
    for (int y = 0; y < WC_HEIGHT; y++)
      for (int x = 0; x < WC_WIDTH; x++)
        if (x < 112 || x >= 528 || y < 100 || y >= 340)
          c->pixels[y * WC_WIDTH + x] = stage[y * WC_WIDTH + x];
  } else
#endif
    draw_scene(c, d, 0, 0);
#if DG_D5
  if (d->ticks < d->alert_until && !d->values->failed) {
    // A recovered script alert, in the console's builtin font.
    fill(c, 48, 180, 592, 290, 0xffffffffu);
    draw_text(c, d->alert_text, NULL, 0, 255, 64, 201, 574, 281);
  }
#endif
  for (unsigned i = 0; i < count && !d->values->failed; i++)
    draw_cursor(c, d, cursor, pointers[i]);
  c->drawn = true;
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
  uint32_t length = c->host.read(c->host.ctx, m->asset, NULL, 0);
  if (!length) {
    lv_fail(v, "native cursor image missing");
    return false;
  }
  uint8_t *data = malloc(length);
  bool ok = data && c->host.read(c->host.ctx, m->asset, data, length) == length &&
            length >= 32;
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
        unsigned rgb = dg_fdi_be32(p) >> 8;
        ok = !rgb || rgb == 0xffffff;
        out[y * 16 + x] = rgb ? 0xffff : 1;
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
  bool ok = dg_cursor_compose(out, pixels, m->width, m->height, mask ? coverage : NULL,
                              mask ? mask->width : 0, mask ? mask->height : 0,
                              m->reg_x, m->reg_y);
  if (!ok)
    lv_fail(v, "unsupported color cursor bitmap");
  return ok;
}
