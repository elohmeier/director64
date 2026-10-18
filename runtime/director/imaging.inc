// Imaging Lingo uses owned surfaces rather than member-ID aliases. Consulted
// ScummVM 41ac2b31847622d0662d22c03fe6979e3b43cfbc (clean): its keyword tables
// list these services but it does not implement them. API contracts follow the
// Macromedia Director MX 2004 Scripting Reference, Image methods/properties.
// All raster algorithms here are original implementations.
#include "ink.h"
typedef struct dg_image_entry {
  struct dg_image_entry *next;
  dg_pixel_image_t image;
  uint8_t *matte;
  uint32_t id, member, step, revision;
  dg_runtime_t *owner;
  bool changed, stage;
} dg_image_entry_t;
struct dg_imaging { dg_image_entry_t *head; uint32_t serial; };
static dg_runtime_t *imaging_host(const dg_runtime_t *d) {
  while (d->linked_parent) d = d->linked_parent;
  return (dg_runtime_t *)d;
}
static dg_imaging_t *imaging_store(const dg_runtime_t *d) { return imaging_host(d)->imaging; }
static size_t imaging_bytes(const dg_runtime_t *d) {
  size_t used = 0;
  dg_imaging_t *store = imaging_store(d);
  for (const dg_image_entry_t *p = store ? store->head : NULL; p; p = p->next)
    used += (size_t)p->image.width * p->image.height * (sizeof(uint32_t) + (p->matte ? 1 : 0));
  return used;
}

void dg_images_dispose(dg_runtime_t *d) {
  dg_imaging_t *store = imaging_store(d);
  if (!store) return;
  if (d->linked_parent) {
    // A shared global may retain an image after its creating child is removed.
    // Detach stage/member bindings; ordinary handle collection owns its pixels.
    for (dg_image_entry_t *p = store->head; p; p = p->next)
      if (p->owner == d) { p->owner = NULL; p->member = 0; p->stage = false; }
    return;
  }
  dg_image_entry_t *p = store->head;
  while (p) { dg_image_entry_t *next = p->next; free(p->image.pixels); free(p->matte); free(p); p = next; }
  free(d->imaging);
  d->imaging = NULL;
}
static dg_image_entry_t *imaging_member(const dg_runtime_t *d, uint32_t member) {
  for (dg_image_entry_t *p = imaging_store(d) ? imaging_store(d)->head : NULL; p; p = p->next)
    if (p->owner == d && p->member == member && member) return p;
  return NULL;
}
const dg_pixel_image_t *dg_member_image(const dg_runtime_t *d, uint32_t member) {
  dg_image_entry_t *p = imaging_member(d, member);
  return p && p->changed ? &p->image : NULL;
}
// The projector derives a matte from white pixels connected to the outer
// edge when a mutable bitmap disables alpha. Interior white remains opaque.
// Cache once until pixels/useAlpha change; both rendering and hits share it.
const uint8_t *dg_image_matte(dg_runtime_t *d, const dg_pixel_image_t *image) {
  dg_image_entry_t *p = imaging_store(d) ? imaging_store(d)->head : NULL;
  while (p && &p->image != image) p = p->next;
  if (!p || !image->width || !image->height) return NULL;
  if (p->matte) return p->matte;
  size_t count = (size_t)image->width * image->height;
  if (count * 5 + imaging_bytes(d) > 64u * 1024u * 1024u) {
    lv_fail(d->values, "image matte memory budget exhausted"); return NULL;
  }
  uint8_t *matte = malloc(count);
  uint32_t *queue = malloc(count * sizeof(uint32_t));
  if (!matte || !queue) {
    free(matte); free(queue); lv_fail(d->values, "image matte allocation failed"); return NULL;
  }
  memset(matte, 255, count);
  size_t head = 0, tail = 0;
  for (unsigned y = 0; y < image->height; y++) for (unsigned x = 0; x < image->width; x++) {
    if (x && y && x + 1 < image->width && y + 1 < image->height) continue;
    size_t at = (size_t)y * image->width + x;
    if ((image->pixels[at] & 0xffffffu) == 0xffffffu) { matte[at] = 0; queue[tail++] = (uint32_t)at; }
  }
  while (head < tail) {
    size_t at = queue[head++];
    unsigned x = at % image->width, y = at / image->width;
    size_t neighbor[4] = {x ? at-1 : at, x+1 < image->width ? at+1 : at,
      y ? at-image->width : at, y+1 < image->height ? at+image->width : at};
    for (unsigned i = 0; i < 4; i++) {
      size_t n = neighbor[i];
      if (matte[n] && (image->pixels[n] & 0xffffffu) == 0xffffffu) { matte[n] = 0; queue[tail++] = (uint32_t)n; }
    }
  }
  free(queue); p->matte = matte; return matte;
}
const dg_pixel_image_t *dg_stage_image(const dg_runtime_t *d) {
  for (dg_image_entry_t *p = imaging_store(d) ? imaging_store(d)->head : NULL; p; p = p->next)
    if (p->owner == d && p->stage && p->changed && p->revision == d->visual_revision) return &p->image;
  return NULL;
}
static void imaging_collect(dg_runtime_t *d) {
  dg_imaging_t *store = imaging_store(d);
  if (!store) return;
  dg_image_entry_t **link = &store->head;
  while (*link) {
    dg_image_entry_t *p = *link;
    // Native expressions keep untabled operands until the step boundary.
    // Member storage survives even when no script currently holds its image.
    if (!p->member && !(p->stage && p->owner && p->revision == p->owner->visual_revision) &&
        p->step != d->values->steps &&
        !lv_has_reference(d->values, LV_IMAGE, (int32_t)p->id)) {
      *link = p->next;
      free(p->image.pixels); free(p->matte); free(p);
    } else link = &p->next;
  }
}
static dg_image_entry_t *imaging_new(dg_runtime_t *d, int w, int h, unsigned depth) {
  if (w < 0 || h < 0 || w > 65535 || h > 65535 ||
      (uint64_t)(unsigned)w * (unsigned)h > 16u * 1024u * 1024u) {
    lv_fail(d->values, "invalid image dimensions"); return NULL;
  }
  if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16 && depth != 32) {
    lv_fail(d->values, "invalid image depth"); return NULL;
  }
  imaging_collect(d);
  size_t used = imaging_bytes(d);
  // Bound host allocations too: overcommit must not turn malformed dimensions
  // or a runaway imaging loop into an operating-system OOM kill.
  if ((uint64_t)(unsigned)w * (unsigned)h * 4 + used > 64u * 1024u * 1024u) {
    lv_fail(d->values, "image memory budget exhausted"); return NULL;
  }
  dg_runtime_t *host = imaging_host(d);
  if (!host->imaging) host->imaging = calloc(1, sizeof(*host->imaging));
  dg_imaging_t *store = host->imaging;
  dg_image_entry_t *p = store ? calloc(1, sizeof(*p)) : NULL;
  if (!p) { lv_fail(d->values, "image allocation failed"); return NULL; }
  size_t count = (size_t)w * (unsigned)h;
  p->image.pixels = count ? malloc(count * sizeof(uint32_t)) : NULL;
  if (count && !p->image.pixels) {
    free(p); lv_fail(d->values, "image pixel allocation failed"); return NULL;
  }
  for (size_t i = 0; i < count; i++) p->image.pixels[i] = UINT32_MAX;
  p->image.reg_x = w / 2; p->image.reg_y = h / 2;
  p->image.width = (unsigned)w; p->image.height = (unsigned)h; p->image.depth = depth;
  p->id = 0x80000000u | ++store->serial;
  p->owner = d;
  p->step = d->values->steps;
  p->next = store->head; store->head = p;
  return p;
}
static lv_t imaging_value(const dg_image_entry_t *p) {
  return p ? lv_make(LV_IMAGE, (int32_t)p->id) : (lv_t)0;
}
static dg_image_entry_t *imaging_read(dg_runtime_t *d, uint32_t member) {
  dg_image_entry_t *p = imaging_member(d, member);
  if (!member) {
    for (dg_image_entry_t *stage = imaging_store(d) ? imaging_store(d)->head : NULL; stage; stage = stage->next)
      if (stage->owner == d && stage->stage && stage->revision == d->visual_revision) { stage->step = d->values->steps; return stage; }
  }
  const dg_member_t *m = member ? dg_member(d, member) : NULL;
  if (p && (p->changed || !m || m->type != 3 || p->revision == d->visual_revision)) {
    p->step = d->values->steps; return p;
  }
  dg_pixel_image_t image = {0};
  if (!d->platform.read_image || !d->platform.read_image(d->context, member, &image)) {
    free(image.pixels);
    lv_fail(d->values, "image pixels unavailable from platform"); return NULL;
  }
  if ((image.width && image.height && !image.pixels) || image.width > 65535 || image.height > 65535) {
    free(image.pixels); lv_fail(d->values, "invalid platform image"); return NULL;
  }
  imaging_collect(d);
  size_t previous = p ? (size_t)p->image.width * p->image.height * 4 : 0;
  if ((uint64_t)image.width * image.height * 4 + imaging_bytes(d) - previous > 64u * 1024u * 1024u) {
    free(image.pixels); lv_fail(d->values, "image memory budget exhausted"); return NULL;
  }
  if (!p) p = imaging_new(d, 0, 0, image.depth ? image.depth : 32);
  if (!p) { free(image.pixels); return NULL; }
  free(p->image.pixels); free(p->matte); p->matte = NULL; p->image = image;
  p->image.reg_x = m ? m->reg_x : (int)image.width / 2;
  p->image.reg_y = m ? m->reg_y : (int)image.height / 2;
  if (!p->image.depth) p->image.depth = 32;
  p->member = member; p->stage = member == 0; p->revision = d->visual_revision; p->step = d->values->steps;
  return p;
}
static dg_image_entry_t *imaging_resolve(dg_runtime_t *d, lv_t value) {
  if (lv_type(value) == LV_MEMBER) return imaging_read(d, (uint32_t)lv_id(value));
  if (lv_type(value) != LV_IMAGE) { lv_fail(d->values, "expected image object"); return NULL; }
  uint32_t id = (uint32_t)lv_id(value);
  for (dg_image_entry_t *p = imaging_store(d) ? imaging_store(d)->head : NULL; p; p = p->next)
    if (p->id == id) { p->step = d->values->steps; return p; }
  // Compatibility with image references created by hand-written native clients.
  if (!(id & 0x80000000u)) return imaging_read(d, id);
  lv_fail(d->values, "expired image object"); return NULL;
}
static dg_image_entry_t *imaging_copy(dg_runtime_t *d, dg_image_entry_t *source,
                                     int l, int t, int r, int b) {
  if (!source) return NULL;
  dg_image_entry_t *copy = imaging_new(d, r > l ? r - l : 0, b > t ? b - t : 0,
                                      source->image.depth);
  if (!copy) return NULL;
  copy->image.use_alpha = source->image.use_alpha;
  copy->image.grayscale = source->image.grayscale;
  for (unsigned y = 0; y < copy->image.height; y++)
    for (unsigned x = 0; x < copy->image.width; x++) {
      int sx = l + (int)x, sy = t + (int)y;
      if (sx >= 0 && sy >= 0 && (unsigned)sx < source->image.width && (unsigned)sy < source->image.height)
        copy->image.pixels[(size_t)y * copy->image.width + x] =
            source->image.pixels[(size_t)sy * source->image.width + (unsigned)sx];
    }
  return copy;
}
static void imaging_changed(dg_runtime_t *d, dg_image_entry_t *p) {
  free(p->matte); p->matte = NULL;
  p->changed = true;
  if (p->owner) d = p->owner;
  d->mouse_hit_valid = false;
  if (p->stage) { d->visual_revision++; p->revision = d->visual_revision; }
  if (p->member) {
    d->visual_revision++;
    for (unsigned i = 1; i < DG_SPRITES; i++)
      if (d->sprites[i].value.member == p->member) dg_sprite_changed(d, i);
  }
  if (p->member || p->stage)
    for (dg_runtime_t *host = d->linked_parent; host; host = host->linked_parent) {
      host->visual_revision++;
      host->mouse_hit_valid = false;
    }
}
static uint32_t imaging_rgb(dg_runtime_t *d, lv_t color) {
  uint32_t c;
  if (lv_type(color) == LV_COLOR) c = (uint32_t)lv_id(color);
  else {
    int n = lv_integer(d->values, color);
    c = d->movie && dg_stage_palette(d) && n >= 0 && n <= 255
            ? dg_stage_palette(d)[n] : (uint32_t)n;
  }
  return 0xff000000u | (c >> 16 & 255u) | (c & 0xff00u) | (c & 255u) << 16;
}
static unsigned imaging_gray(uint32_t pixel) {
  return ((pixel & 255u) * 77u + (pixel >> 8 & 255u) * 150u + (pixel >> 16 & 255u) * 29u) >> 8;
}
static uint32_t imaging_quantize(const dg_pixel_image_t *im, uint32_t pixel) {
  if (im->grayscale || im->depth == 1) {
    unsigned gray = imaging_gray(pixel);
    if (im->depth < 8) {
      unsigned levels = (1u << im->depth) - 1;
      gray = ((gray * levels + 127) / 255) * 255 / levels;
    }
    return (pixel & 0xff000000u) | gray * 0x010101u;
  }
  if (im->depth == 16) {
    uint32_t result = pixel & 0xff000000u;
    for (unsigned shift = 0; shift < 24; shift += 8) {
      unsigned c = pixel >> shift & 255u;
      c >>= 3; result |= ((c << 3) | (c >> 2)) << shift;
    }
    return result;
  }
  return pixel;
}
static lv_t imaging_option(dg_runtime_t *d, lv_t options, const char *name) {
  return lv_type(options) == LV_PROPLIST ? lv_get(d->values, NULL, name, options) : (lv_t)0;
}
static bool imaging_rect(dg_runtime_t *d, lv_t v, int rect[4]) {
  if (lv_type(v) != LV_LIST || lv_count(d->values, v) != 4 ||
      lv_type(lv_at(d->values, v, 1)) == LV_LIST) {
    lv_fail(d->values, "expected image rectangle"); return false;
  }
  for (unsigned i = 0; i < 4; i++) rect[i] = lv_integer(d->values, lv_at(d->values, v, i + 1));
  for (unsigned i = 0; i < 4; i++)
    if (rect[i] < -65535 || rect[i] > 65535) { lv_fail(d->values, "image coordinate range"); return false; }
  return true;
}
static void imaging_put(dg_image_entry_t *p, int x, int y, uint32_t pixel) {
  if (x >= 0 && y >= 0 && (unsigned)x < p->image.width && (unsigned)y < p->image.height)
    p->image.pixels[(size_t)y * p->image.width + (unsigned)x] = imaging_quantize(&p->image, pixel);
}
static bool imaging_get(dg_runtime_t *d, const char *name, lv_t owner, lv_t *out) {
  unsigned ln = lv_name_id(d->values, name);
  if (lv_type(owner) == LV_MEMBER && (ln == LN_IMAGE || ln == LN_PICTURE)) {
    *out = imaging_value(imaging_read(d, (uint32_t)lv_id(owner))); return true;
  }
  dg_image_entry_t *p = NULL;
  if (lv_type(owner) == LV_IMAGE) p = imaging_resolve(d, owner);
  else if (lv_type(owner) == LV_MEMBER) {
    p = imaging_member(d, (uint32_t)lv_id(owner));
    if (!p || !p->changed) return false;
  } else return false;
  if (!p) return true;
  if (ln == LN_RECT) *out = lv_rect(d->values, 0, 0, (int)p->image.width, (int)p->image.height);
  else if (ln == LN_WIDTH) *out = lv_num((int32_t)p->image.width);
  else if (ln == LN_HEIGHT) *out = lv_num((int32_t)p->image.height);
  else if (ln == LN_REGPOINT) *out = lv_point(d->values, p->image.reg_x, p->image.reg_y);
  else if (ln == LN_USEALPHA) *out = lv_num(p->image.use_alpha);
  else if (eq(name, "depth") || eq(name, "bitdepth")) *out = lv_num((int32_t)p->image.depth);
  else if (eq(name, "alphadepth")) *out = lv_num(p->image.use_alpha ? 8 : 0);
  else if (eq(name, "palette")) *out = lv_symbol(d->values, p->image.grayscale ? "grayscale" : "systemwin");
  else return false;
  return true;
}
static bool imaging_set(dg_runtime_t *d, const char *name, lv_t owner, lv_t value) {
  unsigned ln = lv_name_id(d->values, name);
  if (ln == LN_REGPOINT && lv_type(owner) == LV_MEMBER) {
    dg_image_entry_t *p = imaging_read(d, (uint32_t)lv_id(owner));
    if (p && lv_type(value) == LV_LIST && lv_count(d->values, value) == 2) {
      p->image.reg_x = lv_integer(d->values, lv_at(d->values, value, 1));
      p->image.reg_y = lv_integer(d->values, lv_at(d->values, value, 2));
      imaging_changed(d, p);
    } else if (p) lv_fail(d->values, "invalid member registration point");
    return true;
  }
  if (ln == LN_USEALPHA && (lv_type(owner) == LV_MEMBER || lv_type(owner) == LV_IMAGE)) {
    dg_image_entry_t *p = imaging_resolve(d, owner);
    if (p) { p->image.use_alpha = lv_integer(d->values, value) != 0; imaging_changed(d, p); }
    return true;
  }
  if (lv_type(owner) != LV_MEMBER || (ln != LN_IMAGE && ln != LN_PICTURE)) return false;
  dg_image_entry_t *source = imaging_resolve(d, value);
  if (!source) return true;
  uint32_t member = (uint32_t)lv_id(owner);
  if (source->owner == d && source->member == member) { imaging_changed(d, source); return true; }
  dg_image_entry_t *p = imaging_copy(d, source, 0, 0, (int)source->image.width, (int)source->image.height);
  if (!p) return true;
  dg_image_entry_t *old = imaging_member(d, member);
  if (old) old->member = 0;
  p->member = member; imaging_changed(d, p);
  return true;
}
static bool imaging_shape(dg_runtime_t *d, dg_image_entry_t *p, bool filled,
                           unsigned argc, const lv_t *a) {
  lv_runtime_t *r = d->values;
  int box[4]; lv_t color = 0, options = 0;
  if (argc >= 3 && lv_type(a[1]) == LV_LIST && lv_count(r, a[1]) == 4) {
    if (!imaging_rect(d, a[1], box)) return true;
    color = a[2]; if (argc > 3) options = a[3];
  } else if (argc >= 4 && lv_type(a[1]) == LV_LIST && lv_type(a[2]) == LV_LIST &&
             lv_count(r, a[1]) == 2 && lv_count(r, a[2]) == 2) {
    for (unsigned i = 0; i < 2; i++) {
      box[i] = lv_integer(r, lv_at(r, a[1], i + 1));
      box[i + 2] = lv_integer(r, lv_at(r, a[2], i + 1));
    }
    color = a[3]; if (argc > 4) options = a[4];
  } else if (argc == 6 || argc == 7) {
    for (unsigned i = 0; i < 4; i++) box[i] = lv_integer(r, a[i + 1]);
    color = a[5]; if (argc == 7) options = a[6];
  } else { lv_fail(r, "invalid image drawing arguments"); return true; }
  for (unsigned i = 0; i < 4; i++)
    if (box[i] < -65535 || box[i] > 65535) { lv_fail(r, "image coordinate range"); return true; }
  if (lv_type(color) == LV_PROPLIST) { options = color; color = imaging_option(d, options, "color"); }
  if (!color) color = lv_make(LV_COLOR, 0);
  lv_t shape_value = imaging_option(d, options, "shapetype");
  const char *shape = shape_value ? lv_cstr(r, shape_value) : filled ? "rect" : "line";
  bool line = eq(shape, "line"), oval = eq(shape, "oval"), rounded = eq(shape, "roundrect");
  if (!line && !oval && !rounded && !eq(shape, "rect")) {
    lv_fail(r, "unsupported image shape"); return true;
  }
  lv_t size_value = imaging_option(d, options, "linesize");
  int size = size_value ? lv_integer(r, size_value) : 1;
  if (size < 0 || size > 65535) { lv_fail(r, "invalid image line size"); return true; }
  uint32_t pixel = imaging_rgb(d, color);
  int l = box[0] < box[2] ? box[0] : box[2], t = box[1] < box[3] ? box[1] : box[3];
  int rr = box[0] > box[2] ? box[0] : box[2], bb = box[1] > box[3] ? box[1] : box[3];
  double cx = ((double)l + rr) / 2, cy = ((double)t + bb) / 2;
  double rx = ((double)rr - l) / 2, ry = ((double)bb - t) / 2;
  int minx = line ? l - size : l, miny = line ? t - size : t;
  int maxx = line ? rr + size + 1 : rr, maxy = line ? bb + size + 1 : bb;
  if (minx < 0) minx = 0;
  if (miny < 0) miny = 0;
  if (maxx > (int)p->image.width) maxx = (int)p->image.width;
  if (maxy > (int)p->image.height) maxy = (int)p->image.height;
  double dx = (double)box[2] - box[0], dy = (double)box[3] - box[1];
  double length2 = dx * dx + dy * dy;
  for (int y = miny; y < maxy; y++) for (int x = minx; x < maxx; x++) {
    bool include = true;
    if (line) {
      if (!size) continue;
      double u = length2 ? ((x - box[0]) * dx + (y - box[1]) * dy) / length2 : 0;
      if (u < 0) u = 0;
      if (u > 1) u = 1;
      double ex = x - box[0] - u * dx, ey = y - box[1] - u * dy;
      include = ex * ex + ey * ey <= (double)size * size / 4;
    } else if (oval) {
      double ex = x + .5 - cx, ey = y + .5 - cy;
      include = rx > 0 && ry > 0 && ex * ex / (rx * rx) + ey * ey / (ry * ry) <= 1;
      if (include && !filled && rx > size && ry > size)
        include = ex * ex / ((rx - size) * (rx - size)) +
                  ey * ey / ((ry - size) * (ry - size)) >= 1;
    } else if (rounded) {
      double radius = fmin(8, fmin(rx, ry));
      double ex = fmax(0, fabs(x + .5 - cx) - rx + radius);
      double ey = fmax(0, fabs(y + .5 - cy) - ry + radius);
      include = ex * ex + ey * ey <= radius * radius;
      if (include && !filled && x >= l + size && x < rr - size &&
          y >= t + size && y < bb - size) {
        double inner = fmax(0, radius - size);
        include = ex * ex + ey * ey > inner * inner;
      }
    } else if (!filled) include = x < l + size || x >= rr - size || y < t + size || y >= bb - size;
    if (include) imaging_put(p, x, y, pixel);
  }
  imaging_changed(d, p); return true;
}
static unsigned imaging_ink(dg_runtime_t *d, lv_t value) {
  if (!value || lv_type(value) == LV_NUMBER) return value ? (unsigned)lv_integer(d->values, value) : 0;
  static const char *const names[] = {"copy", "transparent", "reverse", "ghost", "notcopy", "nottransparent", "notreverse", "notghost", "matte", "mask"};
  const char *name = lv_cstr(d->values, value);
  for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (eq(name, names[i])) return i;
  static const char *const modern[] = {"blend", "addpin", "add", "subtractpin", "backgroundtransparent", "light", "subtract", "dark", "lighten", "darken", "blendtransparent"};
  for (unsigned i = 0; i < sizeof(modern) / sizeof(modern[0]); i++) if (eq(name, modern[i])) return i + 32;
  lv_fail(d->values, "unsupported image ink"); return 255;
}
static uint32_t imaging_over(uint32_t source, uint32_t destination, unsigned alpha,
                              unsigned dest_alpha, unsigned ink) {
  unsigned out_alpha = alpha + (dest_alpha * (255 - alpha) + 127) / 255;
  uint32_t out = out_alpha << 24;
  if (!out_alpha) return source & 0xffffffu;
  for (unsigned shift = 0; shift < 24; shift += 8) {
    unsigned s = dg_ink_channel(source >> shift & 255u, destination >> shift & 255u, ink);
    unsigned dest = destination >> shift & 255u;
    unsigned value = (s * alpha * 255 + dest * dest_alpha * (255 - alpha) + out_alpha * 127) / (out_alpha * 255);
    out |= value << shift;
  }
  return out;
}
static bool imaging_pixels(dg_runtime_t *d, dg_image_entry_t *dest, unsigned argc, const lv_t *a) {
  lv_runtime_t *r = d->values;
  if (argc != 4 && argc != 5) { lv_fail(r, "invalid copyPixels arguments"); return true; }
  dg_image_entry_t *source = imaging_resolve(d, a[1]);
  if (!source) return true;
  int src[4], box[4]; float quad[8]; bool is_quad = false;
  if (!imaging_rect(d, a[3], src)) return true;
  if (lv_type(a[2]) == LV_LIST && lv_count(r, a[2]) == 4 && lv_type(lv_at(r, a[2], 1)) == LV_LIST) {
    is_quad = true;
    for (unsigned i = 0; i < 4; i++) for (unsigned j = 0; j < 2; j++)
      quad[i * 2 + j] = (float)lv_number(r, lv_at(r, lv_at(r, a[2], i + 1), j + 1));
    for (unsigned i = 0; i < 8; i++)
      if (!isfinite(quad[i]) || quad[i] < -65535 || quad[i] > 65535) {
        lv_fail(r, "invalid image quad coordinate"); return true;
      }
    box[0] = box[2] = (int)quad[0]; box[1] = box[3] = (int)quad[1];
    for (unsigned i = 1; i < 4; i++) {
      if (quad[i * 2] < box[0]) box[0] = (int)floorf(quad[i * 2]);
      if (quad[i * 2] > box[2]) box[2] = (int)ceilf(quad[i * 2]);
      if (quad[i * 2 + 1] < box[1]) box[1] = (int)floorf(quad[i * 2 + 1]);
      if (quad[i * 2 + 1] > box[3]) box[3] = (int)ceilf(quad[i * 2 + 1]);
    }
  } else if (!imaging_rect(d, a[2], box)) return true;
  lv_t options = argc == 5 ? a[4] : 0;
  unsigned ink = imaging_ink(d, imaging_option(d, options, "ink"));
  if (ink > 42 || (ink > 9 && ink < 32)) { lv_fail(r, "unsupported image ink"); return true; }
  lv_t blend = imaging_option(d, options, "blendlevel");
  int alpha = blend ? lv_integer(r, blend) : 255;
  if (!blend && (blend = imaging_option(d, options, "blend"))) alpha = lv_integer(r, blend) * 255 / 100;
  if (alpha < 0) alpha = 0;
  if (alpha > 255) alpha = 255;
  lv_t back = imaging_option(d, options, "bgcolor"), fore = imaging_option(d, options, "color");
  uint32_t background = back ? imaging_rgb(d, back) & 0xffffffu : 0xffffffu;
  uint32_t foreground = fore ? imaging_rgb(d, fore) & 0xffffffu : 0;
  dg_image_entry_t *mask = NULL;
  lv_t mask_value = imaging_option(d, options, "maskimage");
  if (mask_value && !source->image.use_alpha) mask = imaging_resolve(d, mask_value);
  if (mask_value && !source->image.use_alpha && !mask) return true;
  int mx = 0, my = 0;
  lv_t offset = imaging_option(d, options, "maskoffset");
  if (offset) { mx = lv_integer(r, lv_at(r, offset, 1)); my = lv_integer(r, lv_at(r, offset, 2)); }
  // Take a stable copy for overlap; a copy from this image never reads pixels
  // that this very operation has already overwritten.
  uint32_t *scratch = NULL; const uint32_t *pixels = source->image.pixels;
  if (source == dest && source->image.width && source->image.height) {
    size_t bytes = (size_t)source->image.width * source->image.height * 4;
    scratch = malloc(bytes);
    if (!scratch) { lv_fail(r, "copyPixels overlap allocation failed"); return true; }
    memcpy(scratch, pixels, bytes); pixels = scratch;
  }
  int l = box[0] < box[2] ? box[0] : box[2], rr = box[0] > box[2] ? box[0] : box[2];
  int t = box[1] < box[3] ? box[1] : box[3], bb = box[1] > box[3] ? box[1] : box[3];
  if (l < 0) l = 0;
  if (t < 0) t = 0;
  if (rr > (int)dest->image.width) rr = (int)dest->image.width;
  if (bb > (int)dest->image.height) bb = (int)dest->image.height;
  for (int y = t; y < bb; y++) for (int x = l; x < rr; x++) {
    float u, v;
    if (is_quad) { if (!dg_quad_uv(quad, x + .5f, y + .5f, &u, &v)) continue; }
    else { u = (x + .5f - box[0]) / (box[2] - box[0]); v = (y + .5f - box[1]) / (box[3] - box[1]); }
    int sx = (int)floorf(src[0] + u * (src[2] - src[0]));
    int sy = (int)floorf(src[1] + v * (src[3] - src[1]));
    if (sx < 0 || sy < 0 || (unsigned)sx >= source->image.width || (unsigned)sy >= source->image.height) continue;
    uint32_t pixel = pixels[(size_t)sy * source->image.width + (unsigned)sx];
    unsigned coverage = source->image.use_alpha ? pixel >> 24 : 255;
    if (mask) {
      int xx = sx - mx, yy = sy - my;
      coverage = xx >= 0 && yy >= 0 && (unsigned)xx < mask->image.width && (unsigned)yy < mask->image.height
          ? 255 - imaging_gray(mask->image.pixels[(size_t)yy * mask->image.width + (unsigned)xx]) : 0;
    }
    if (alpha == 255 && (ink == 36 || ink == 42) && (pixel & 0xffffffu) == background) coverage = 0;
    if (source->image.depth == 1 && (fore || back)) pixel = (pixel & 0xffffffu) ? background : foreground;
    if (alpha == 255 && (ink == 40 || ink == 41)) {
      uint32_t tinted = pixel & 0xff000000u;
      for (unsigned shift = 0; shift < 24; shift += 8)
        tinted |= (ink == 40
            ? dg_ink_lighten_channel(pixel >> shift & 255u, foreground >> shift & 255u, background >> shift & 255u)
            : dg_ink_tint_channel(pixel >> shift & 255u, foreground >> shift & 255u, background >> shift & 255u)) << shift;
      pixel = tinted;
    }
    if (!coverage || !alpha) continue;
    size_t at = (size_t)y * dest->image.width + (unsigned)x;
    uint32_t old = dest->image.pixels[at];
    unsigned operation = ink;
    if (alpha < 255) {
      // Birthday original projector (8.5.1r104 engine, MX9 launcher) under
      // Wine8: partial blend bypasses ink, bitwise-colorizes, then uses an
      // eight-bit fixed weight. Confirmed by 260 independent RGB observations.
      unsigned weight = (unsigned)alpha + ((unsigned)alpha >> 7);
      uint32_t colored = (pixel & background) | ((~pixel) & foreground);
      uint32_t mixed = pixel & 0xff000000u;
      for (unsigned shift = 0; shift < 24; shift += 8)
        mixed |= ((((colored >> shift & 255u) * weight +
                    (old >> shift & 255u) * (256 - weight)) >> 8) << shift);
      pixel = mixed;
      operation = 0;
    }
    dest->image.pixels[at] = imaging_quantize(&dest->image,
        imaging_over(pixel, old, coverage, dest->image.use_alpha ? old >> 24 : 255, operation));
  }
  free(scratch); imaging_changed(d, dest); return true;
}
static bool imaging_call(dg_runtime_t *d, const char *name, unsigned argc,
                          const lv_t *a, lv_t *out) {
  lv_runtime_t *r = d->values;
  unsigned ln = lv_name_id(r, name);
  if (ln == LN_IMAGE && argc >= 2 && argc <= 5 && lv_type(a[0]) != LV_IMAGE && lv_type(a[0]) != LV_MEMBER) {
    unsigned depth = argc >= 3 ? (unsigned)lv_integer(r, a[2]) : 32;
    dg_image_entry_t *p = imaging_new(d, lv_integer(r, a[0]), lv_integer(r, a[1]), depth);
    if (p) {
      if (argc >= 4 && lv_type(a[3]) == LV_NUMBER && depth == 32) {
        int alpha = lv_integer(r, a[3]);
        if (alpha != 0 && alpha != 8) { lv_fail(r, "invalid image alpha depth"); return true; }
        p->image.use_alpha = alpha == 8;
      }
      lv_t palette = argc >= 5 ? a[4] : argc >= 4 ? a[3] : 0;
      p->image.grayscale = lv_type(palette) == LV_SYMBOL && eq(lv_cstr(r, palette), "grayscale");
    }
    *out = imaging_value(p); return true;
  }
  if (!argc || lv_type(a[0]) != LV_IMAGE) return false;
  bool mask_op = eq(name, "createmask"), matte_op = eq(name, "creatematte");
  if (ln != LN_DUPLICATE && ln != LN_CROP && ln != LN_GETPIXEL && ln != LN_SETPIXEL &&
      ln != LN_FILL && ln != LN_DRAW && ln != LN_COPYPIXELS && ln != LN_SETALPHA &&
      ln != LN_EXTRACTALPHA && !mask_op && !matte_op) return false;
  dg_image_entry_t *p = imaging_resolve(d, a[0]);
  if (!p) return true;
  *out = lv_num(1);
  if ((ln == LN_DUPLICATE && argc == 1) || (ln == LN_CROP && (argc == 2 || argc == 3))) {
    int box[4] = {0, 0, (int)p->image.width, (int)p->image.height};
    if (ln == LN_CROP) {
      if (argc == 2) { if (!imaging_rect(d, a[1], box)) return true; }
      else if (lv_type(a[1]) == LV_LIST && lv_count(r, a[1]) == 2 &&
               lv_type(a[2]) == LV_LIST && lv_count(r, a[2]) == 2) {
        for (unsigned i = 0; i < 2; i++) {
          box[i] = lv_integer(r, lv_at(r, a[1], i + 1));
          box[i + 2] = lv_integer(r, lv_at(r, a[2], i + 1));
        }
      } else { lv_fail(r, "invalid image crop points"); return true; }
    }
    *out = imaging_value(imaging_copy(d, p, box[0], box[1], box[2], box[3])); return true;
  }
  if (ln == LN_GETPIXEL || ln == LN_SETPIXEL) {
    bool set = ln == LN_SETPIXEL;
    int x, y; unsigned used;
    if (argc >= 2 && lv_type(a[1]) == LV_LIST && lv_count(r, a[1]) == 2) {
      x = lv_integer(r, lv_at(r, a[1], 1)); y = lv_integer(r, lv_at(r, a[1], 2)); used = 2;
    } else if (argc >= 3) { x = lv_integer(r, a[1]); y = lv_integer(r, a[2]); used = 3; }
    else { lv_fail(r, "invalid image pixel coordinates"); return true; }
    if ((set && argc != used + 1) || (!set && argc != used && argc != used + 1)) {
      lv_fail(r, "invalid image pixel arguments"); return true;
    }
    if (set) { imaging_put(p, x, y, imaging_rgb(d, a[used])); imaging_changed(d, p); }
    else {
      if (x < 0 || y < 0 || (unsigned)x >= p->image.width || (unsigned)y >= p->image.height) { *out = 0; return true; }
      uint32_t pixel = p->image.pixels[(size_t)y * p->image.width + (unsigned)x];
      uint32_t rgb = (pixel & 255u) << 16 | (pixel & 0xff00u) | (pixel >> 16 & 255u);
      if (argc > used && eq(lv_cstr(r, a[used]), "integer")) {
        if (p->image.depth <= 8 && p->image.grayscale) rgb = 255 - imaging_gray(pixel);
        else if (p->image.depth == 1) rgb = imaging_gray(pixel) < 128;
        else if (p->image.depth == 32) rgb |= (255 - (pixel >> 24)) << 24;
        *out = lv_num((int32_t)rgb);
      } else *out = lv_make(LV_COLOR, (int32_t)rgb);
    }
    return true;
  }
  if ((ln == LN_EXTRACTALPHA && argc == 1) || mask_op || matte_op) {
    if ((ln == LN_EXTRACTALPHA && p->image.depth != 32) || argc > 2) { *out = 0; return true; }
    dg_image_entry_t *alpha = imaging_new(d, (int)p->image.width, (int)p->image.height, 8);
    if (!alpha) return true;
    alpha->image.grayscale = true;
    unsigned threshold = argc == 2 ? (unsigned)lv_integer(r, a[1]) : 0;
    for (size_t i = 0, n = (size_t)p->image.width * p->image.height; i < n; i++) {
      unsigned gray;
      if (mask_op) gray = imaging_gray(p->image.pixels[i]);
      else if (matte_op && !p->image.use_alpha) gray = (p->image.pixels[i] & 0xffffffu) == 0xffffffu ? 255 : 0;
      else { unsigned coverage = p->image.pixels[i] >> 24; gray = matte_op && coverage <= threshold ? 255 : 255 - coverage; }
      alpha->image.pixels[i] = 0xff000000u | gray * 0x010101u;
    }
    *out = imaging_value(alpha); return true;
  }
  if (ln == LN_SETALPHA && argc == 2) {
    if (p->image.depth != 32) { *out = lv_num(0); return true; }
    dg_image_entry_t *alpha = NULL;
    int scalar = 0;
    if (lv_type(a[1]) == LV_IMAGE) {
      alpha = imaging_resolve(d, a[1]);
      if (!alpha) return true;
      if (alpha->image.depth != 8 || alpha->image.width != p->image.width || alpha->image.height != p->image.height) {
        *out = lv_num(0); return true;
      }
    } else {
      scalar = lv_integer(r, a[1]);
      if (scalar < 0 || scalar > 255) { *out = lv_num(0); return true; }
    }
    for (size_t i = 0, n = (size_t)p->image.width * p->image.height; i < n; i++) {
      unsigned coverage = alpha ? 255 - imaging_gray(alpha->image.pixels[i]) : (unsigned)scalar;
      p->image.pixels[i] = (p->image.pixels[i] & 0xffffffu) | coverage << 24;
    }
    imaging_changed(d, p); return true;
  }
  if (ln == LN_FILL || ln == LN_DRAW) return imaging_shape(d, p, ln == LN_FILL, argc, a);
  if (ln == LN_COPYPIXELS) return imaging_pixels(d, p, argc, a);
  lv_fail(r, "invalid image operation arguments"); return true;
}
