// Equivalent full/cropped planes, cache invalidation, and cursor-only redraws.
// The transparent bounding layer changes storage work, never the picture.
#include "compositor.h"
#include "ink.h"
#include "shape.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static wc_t actual, reference;
static dg_runtime_t director;
static lv_runtime_t values;
static uint32_t palette[256] = {0xffffff, 0};
static const lv_movie_t code = {.name = "CACHE.DXR"};
static const dg_cast_t cast = {.name = "Internal", .file = 1, .cast = 1};
static const dg_frame_t score[] = {{0, 0}};
static unsigned asset_reads;
#define ID(n) DG_ID(1, 1, n)
#define LAYER(n, x_, y_, w_, h_)                                               \
  {.member = ID(n),                                                            \
   .type = 16,                                                                 \
   .x = x_,                                                                    \
   .y = y_,                                                                    \
   .width = w_,                                                                \
   .height = h_,                                                               \
   .stretch = 1,                                                               \
   .fore = 1,                                                                  \
   .blend = 100}
static dg_film_layer_t children[] = {
    {.channel = 1, .value = LAYER(5, 7, 5, 12, 8)},
    {.channel = 2, .value = LAYER(7, 27, 17, 9, 8)},
    {.channel = 3, .value = LAYER(9, 0, 0, 40, 32)},
};
static dg_film_frame_t tight = {2, children}, full = {3, children};
static const char *const trimmed_pose[] = {"trim.fdi"}, *const full_pose[] = {
                                                            "pose.fdi"};
static dg_member_t members[] = {
    {.id = ID(1),
     .number = 1,
     .cast = 1,
     .type = 2,
     .width = 40,
     .height = 32,
     .film_count = 1,
     .film_frames = &tight},
    {.id = ID(2),
     .number = 2,
     .cast = 1,
     .type = 1,
     .width = 40,
     .height = 32,
     .asset = "parent-mask.fdi"},
    {.id = ID(3),
     .number = 3,
     .cast = 1,
     .type = 2,
     .width = 40,
     .height = 32,
     .film_count = 1,
     .film_frames = &full},
    {.id = ID(4),
     .number = 4,
     .cast = 1,
     .type = 1,
     .width = 40,
     .height = 32,
     .asset = "parent-mask.fdi"},
    {.id = ID(5),
     .number = 5,
     .cast = 1,
     .type = 1,
     .width = 12,
     .height = 8,
     .asset = "color.fdi"},
    {.id = ID(6),
     .number = 6,
     .cast = 1,
     .type = 1,
     .width = 12,
     .height = 8,
     .asset = "mask.fdi"},
    {.id = ID(7),
     .number = 7,
     .cast = 1,
     .type = 8,
     .shape = 1,
     .filled = 1,
     .pattern = 39},
    {.id = ID(8),
     .number = 8,
     .cast = 1,
     .type = 8,
     .shape = 1,
     .filled = 1,
     .pattern = 1},
    {.id = ID(9),
     .number = 9,
     .cast = 1,
     .type = 1,
     .width = 40,
     .height = 32,
     .asset = "empty.fdi"},
    {.id = ID(10),
     .number = 10,
     .cast = 1,
     .type = 2,
     .source_xtra = 1,
     .width = 8,
     .height = 8,
     .film_count = 1,
     .film_assets = trimmed_pose},
    {.id = ID(11),
     .number = 11,
     .cast = 1,
     .type = 2,
     .source_xtra = 1,
     .width = 8,
     .height = 8,
     .film_count = 1,
     .film_assets = full_pose},

};
static const dg_movie_t movie = {.id = 1,
                                 .code = &code,
                                 .casts = &cast,
                                 .cast_count = 1,
                                 .members = members,
                                 .member_count = 11,
                                 .frames = score,
                                 .frame_count = 1,
                                 .palette = palette,
                                 .stage_color = 0x174c9a};
static void be16(uint8_t *p, unsigned n) {
  p[0] = n >> 8;
  p[1] = n;
}
static void be32(uint8_t *p, unsigned n) {
  p[0] = n >> 24;
  p[1] = n >> 16;
  p[2] = n >> 8;
  p[3] = n;
}
static uint32_t read_asset(void *ctx, const char *name, uint8_t *out,
                           uint32_t cap) {
  (void)ctx;
  bool parent = !strcmp(name, "parent-mask.fdi"),
       empty = !strcmp(name, "empty.fdi");
  bool mask = parent || !strcmp(name, "mask.fdi"),
       trim = !strcmp(name, "trim.fdi"), pose = !strcmp(name, "pose.fdi");
  unsigned w = parent || empty ? 40
               : trim          ? 2
               : pose          ? 8
                               : 12,
           h = parent || empty ? 32
               : trim          ? 2
                               : 8;
  unsigned bytes = 32 + w * h * 4;
  if (!out)
    return bytes;
  assert(cap >= bytes);
  asset_reads++;
  memset(out, 0, bytes);
  memcpy(out, "FDI2", 4);
  be32(out + 4, bytes);
  be16(out + 8, w);
  be16(out + 10, h);
  out[16] = 1;
  out[19] = 32;
  for (unsigned y = 0; y < h; y++)
    for (unsigned x = 0; x < w; x++) {
      uint8_t *p = out + 32 + (y * w + x) * 4;
      p[0] = mask ? (x * 17 + y * 9) % 256 : 230;
      p[1] = mask ? p[0] : 31;
      p[2] = mask ? p[0] : 83;
      p[3] = empty || (pose && (x >= 2 || y >= 2)) ? 0 : 255;
    }
  return bytes;
}
static bool read_pixels(void *ctx, uint32_t member, dg_pixel_image_t *out) {
  (void)ctx;
  return wc_read_image(&actual, &director, member, out);
}
static void invalidate(void) {
  director.visual_revision++;
  actual.drawn = reference.drawn = false;
}
static void render(wc_t *c) {
  wc_render(c, &director, 0, 0, false, 0);
  if (values.failed)
    fprintf(stderr, "%s\n", values.error);
  assert(!values.failed);
}
static void compare(unsigned a, unsigned b) {
  director.sprites[1].value.member = ID(b);
  dg_sprite_changed(&director, 1);
  invalidate();
  render(&reference);
  director.sprites[1].value.member = ID(a);
  dg_sprite_changed(&director, 1);
  invalidate();
  render(&actual);
  assert(!memcmp(actual.scene, reference.scene, sizeof(actual.scene)));
}
static void changed_compare(void) {
  // Check the actual cached instance before switching to the independent
  // full-plane reference. A fresh instance would hide a missing dependency.
  unsigned builds = actual.plane_builds;
  invalidate();
  render(&actual);
  assert(actual.plane_builds > builds);
  compare(1, 3);
}
static void place(unsigned member) {
  director.sprites[1] = (dg_sprite_t){
      .value = LAYER(1, 20, 20, 63, 49), .visible = true, .stretch = true};
  director.sprites[1].value.member = ID(member);
  dg_sprite_changed(&director, 1);
}
static void check_solid_shapes(void) {
  const unsigned inks[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
                           32, 33, 34, 35, 36, 37, 38, 39, 40, 41};
  for (unsigned pattern = 0; pattern <= 1; pattern++)
    for (unsigned kind = 1; kind <= 4; kind++)
      for (unsigned i = 0; i < sizeof(inks) / sizeof(*inks); i++)
        for (unsigned opacity = 1; opacity <= 100; opacity += 33) {
          members[7].shape = kind;
          members[7].pattern = pattern;
          members[7].filled = 1;
          place(8);
          dg_sprite_t *s = &director.sprites[1];
          s->value.x = 0;
          s->value.y = 0;
          s->value.width = 16;
          s->value.height = 12;
          s->value.ink = inks[i];
          s->value.blend = opacity;
          s->value.thickness = DG_HAS_BLEND | 3;
          s->value.flags = 48;
          s->value.fore_rgb = 0x1464c8;
          s->value.back_rgb = 0xdeeebc;
          invalidate();
          render(&actual);
          for (int y = 0; y < 12; y++)
            for (int x = 0; x < 16; x++) {
              uint32_t expected = 0xff9a4c17;
              if (dg_shape_contains(kind, 16, 12, x, y, 2, true, false) &&
                  dg_shape_ink_visible(true, inks[i], opacity, 0xffc86414,
                                       0xffbceede))
                expected = dg_rgba_shape(expected, 0, false, true, inks[i],
                                         opacity, 0xffc86414, 0xffbceede);
              assert(actual.scene[y * WC_WIDTH + x] == expected);
            }
        }
}
int main(void) {
  wc_init(&actual, (wc_host_t){.read = read_asset}, 4u * 1024u * 1024u);
  wc_init(&reference, (wc_host_t){.read = read_asset}, 4u * 1024u * 1024u);
  dg_init(&director, &values, (dg_platform_t){.read_image = read_pixels}, NULL,
          NULL, 0, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  children[0].value.ink = 9;
  children[2].value.ink = 8;
  place(1);
  for (unsigned ink = 0; ink < 3; ink++)
    for (unsigned flip = 0; flip < 4; flip++)
      for (unsigned rotation = 0; rotation < 2; rotation++) {
        dg_sprite_t *s = &director.sprites[1];
        s->value.ink = ink == 2 ? 9 : ink == 1 ? 8 : 0;
        s->flip_h = flip & 1;
        s->flip_v = flip & 2;
        s->value.rotation = rotation ? 17 : 0;
        s->value.blend = flip ? 37 : 100;
        s->value.thickness |= DG_HAS_BLEND;
        compare(1, 3);
      }
  place(1);
  compare(1, 3);
  unsigned hits = actual.plane_hits, builds = actual.plane_builds,
           reads = asset_reads;
  actual.drawn = false;
  render(&actual);
  assert(actual.plane_hits > hits && actual.plane_builds == builds &&
         asset_reads == reads);
  unsigned scenes = actual.scene_renders;
  for (int i = 0; i < 30; i++)
    wc_render(&actual, &director, 100 + i, 100, true, 0);
  assert(actual.scene_renders == scenes);
  members[4].reg_x = 3;
  changed_compare(); // Same-address source geometry mutation.
  members[5].reg_x = -2;
  changed_compare(); // Same-address mask registration mutation.
  palette[1] = 0x1851b7;
  changed_compare();
  lv_t shape = lv_make(LV_MEMBER, ID(7));
  lv_set(&values, NULL, "pattern", shape, lv_num(2));
  changed_compare();
  lv_t source = lv_get(&values, NULL, "image", lv_make(LV_MEMBER, ID(5)));
  lv_frame_t frame = {.movie = &code};
  lv_call(&values, &frame, "setpixel", 4,
          (lv_t[]){source, lv_num(4), lv_num(3), lv_make(LV_COLOR, 0x00ff00)});
  changed_compare();
  lv_t mask = lv_get(&values, NULL, "image", lv_make(LV_MEMBER, ID(6)));
  lv_call(&values, &frame, "setpixel", 4,
          (lv_t[]){mask, lv_num(4), lv_num(3), lv_make(LV_COLOR, 0xffffff)});
  changed_compare();
  // Trimmed representations preserve the full source mapping under transforms.
  place(10);
  director.sprites[1].value.width = 13;
  director.sprites[1].value.height = 11;
  for (unsigned flip = 0; flip < 4; flip++)
    for (unsigned rotate = 0; rotate < 2; rotate++) {
      director.sprites[1].flip_h = flip & 1;
      director.sprites[1].flip_v = flip & 2;
      director.sprites[1].value.rotation = rotate ? 13 : 0;
      compare(10, 11);
    }
  check_solid_shapes();
  wc_release(&reference);
  wc_release(&actual);
  dg_films_dispose(&director);
  dg_images_dispose(&director);
  // Base images retain their existing soft working-set target while pinned.
  // A derived cache must neither fail rendering nor admit beyond that target.
  lv_init(&values, (lv_services_t){0}, NULL, NULL, 0, 1);
  wc_init(&actual, (wc_host_t){.read = read_asset}, 12 * 8 * 4);
  assert(wc_image(&actual, &values, "color.fdi", 12, 8, 8));
  assert(wc_image(&actual, &values, "mask.fdi", 12, 8, 8) && !values.failed);
  size_t base_bytes = actual.image_bytes;
  assert(base_bytes > actual.image_budget);
  dg_init(&director, &values, (dg_platform_t){.read_image = read_pixels}, NULL,
          NULL, 0, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  members[4].reg_x = members[5].reg_x = 0;
  place(5);
  director.sprites[1].value.ink = 9;
  render(&actual);
  assert(!actual.planes && actual.image_bytes == base_bytes);
  wc_release(&actual);
  dg_films_dispose(&director);
  dg_images_dispose(&director);
  puts("web render cache: cropped/full pixels, transforms, masks, palette/live "
       "edits, cursor and budgets passed");
}
