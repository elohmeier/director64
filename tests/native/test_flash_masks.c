// Original Adobe Flash32 geometric-mask contracts reproduced with synthetic
// assets. tools/reference/generate_flash_mask_probe.py: fill and clip opacity
// do not reduce mask coverage, while strokes contribute no geometry.
#include "director.h"
#include "flash_render.h"
#include "flash_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static dg_runtime_t d;
static lv_runtime_t values;
static const dg_film_layer_t root_layers[] = {
    {.channel = 1,
     .clip_id = 9,
     .name = "mask",
     .clip_depth = 2,
     .placement_generation = 1,
     .has_cxform = true,
     .cxform = {256, 256, 256, 0, 0, 0, 0, 0}},
    {.channel = 2,
     .asset = "blue",
     .name = "source",
     .placement_generation = 3,
     .value = {.width = 16, .height = 16}},
};
static const dg_film_layer_t mask_layers[] = {
    {.channel = 1,
     .clip_id = 10,
     .name = "inside",
     .placement_generation = 2,
     .affine = true,
     .matrix = {65536, 0, 0, 65536, 65536, 65536}},
};
static const dg_film_layer_t inner_layers[] = {
    {.channel = 1,
     .asset = "transparent-fill",
     .mask_asset = "fill-mask",
     .value = {.x = 1, .y = 1, .width = 4, .height = 4},
     .has_cxform = true,
     .cxform = {256, 256, 256, 0, 0, 0, 0, 0}},
    {.channel = 2,
     .asset = "stroke",
     .mask_asset = "empty-mask",
     .value = {.x = 9, .y = 1, .width = 4, .height = 4}},
};
static const dg_film_frame_t frames[] = {
    {2, root_layers}, {1, mask_layers}, {2, inner_layers}};
static const dg_flash_clip_t clips[] = {
    {.id = 0, .frame_count = 1},
    {.id = 9, .frame_count = 1, .frames = &frames[1]},
    {.id = 10, .frame_count = 1, .frames = &frames[2]},
};
static const dg_member_t member = {.id = DG_ID(1, 1, 1),
                                   .number = 1,
                                   .cast = 1,
                                   .type = 2,
                                   .source_xtra = 1,
                                   .width = 16,
                                   .height = 16,
                                   .film_count = 1,
                                   .film_frames = frames,
                                   .flash_version = 8,
                                   .flash_clip_count = 3,
                                   .flash_clips = clips};
static const dg_cast_t cast = {.name = "Internal", .file = 1, .cast = 1};
static const dg_movie_t movie = {.id = 1,
                                 .cast_count = 1,
                                 .casts = &cast,
                                 .member_count = 1,
                                 .members = &member};
static unsigned fills, empty;
static unsigned presses;
static void trace(void *context, const char *text) {
  (void)context;
  (void)text;
  presses++;
}
static bool hit_asset(void *context, const dg_member_t *m, unsigned ink, int x,
                      int y) {
  (void)context;
  assert(ink == 8 && x >= 0 && y >= 0 && x < m->width && y < m->height);
  assert(strcmp(m->asset, "transparent-fill") && strcmp(m->asset, "stroke"));
  return strcmp(m->asset, "empty-mask") != 0;
}
static bool asset(void *ctx, const dg_film_layer_t *layer,
                  dg_flash_raster_t *out) {
  (void)ctx;
  assert(dg_flash_raster_allocate(&d, out, layer->value.x, layer->value.y,
                                  layer->value.x + layer->value.width,
                                  layer->value.y + layer->value.height));
  uint32_t pixel;
  if (!strcmp(layer->asset, "fill-mask")) {
    pixel = 0xffffffff;
    fills++;
  } else if (!strcmp(layer->asset, "empty-mask")) {
    pixel = 0;
    empty++;
  } else {
    assert(!strcmp(layer->asset, "blue"));
    pixel = 0xffff0000;
  }
  for (unsigned n = 0; n < out->width * out->height; n++)
    out->pixels[n] = pixel;
  return true;
}
static unsigned text_rasters, text_hits;
static bool text(void *context, dg_flash_instance_t *instance,
                 const dg_flash_field_t *field, dg_flash_raster_t *out) {
  (void)context;
  (void)instance;
  (void)field;
  text_rasters++;
  assert(dg_flash_raster_allocate(&d, out, 0, 0, 8, 8));
  // Two covered glyph samples with a hole inside the text field's bounds.
  out->pixels[1 * 8 + 1] = out->pixels[3 * 8 + 4] = 0xffffffff;
  return true;
}
static bool text_hit(void *context, dg_runtime_t *owner,
                     struct dg_flash_instance *instance, float x, float y) {
  (void)context;
  (void)instance;
  assert(owner == &d);
  text_hits++;
  return ((int)x == 1 && (int)y == 1) || ((int)x == 4 && (int)y == 3);
}
static void call(dg_flash_instance_t *i, const char *name, unsigned count,
                 const lv_t *args) {
  dg_avm1_t *vm = dg_flash_vm(i);
  lv_t object = dg_flash_object(i), result;
  assert(dg_avm1_invoke(vm, dg_avm1_get(vm, object, name), object, count, args,
                        100000, &result));
  assert(!values.failed);
}
static void property(dg_flash_instance_t *i, const char *name, int number) {
  dg_avm1_set(dg_flash_vm(i), dg_flash_object(i), name, lv_num(number));
}
static dg_flash_instance_t *drawing(dg_flash_instance_t *parent,
                                    const char *name, unsigned depth, int x,
                                    int y, bool filled) {
  lv_t args[] = {lv_text(&values, name, false), lv_num(depth)};
  call(parent, "createEmptyMovieClip", 2, args);
  dg_flash_instance_t *child = dg_flash_child(parent, 16384 + depth);
  assert(child);
  if (filled) {
    args[0] = lv_num(0xff0000);
    args[1] = lv_num(0);
    call(child, "beginFill", 2, args);
  } else {
    lv_t line[] = {lv_num(2), lv_num(0xff0000), lv_num(100)};
    call(child, "lineStyle", 3, line);
  }
  args[0] = lv_num(x);
  args[1] = lv_num(y);
  call(child, "moveTo", 2, args);
  args[0] = lv_num(x + 4);
  call(child, "lineTo", 2, args);
  args[1] = lv_num(y + 4);
  call(child, "lineTo", 2, args);
  args[0] = lv_num(x);
  call(child, "lineTo", 2, args);
  args[1] = lv_num(y);
  call(child, "lineTo", 2, args);
  if (filled)
    call(child, "endFill", 0, NULL);
  return child;
}
int main(void) {
  dg_init(&d, &values,
          (dg_platform_t){
              .hit = hit_asset, .trace = trace, .flash_field_hit = text_hit},
          NULL, NULL, 0, 1);
  d.movie = &movie;
  d.loaded[0] = &movie;
  d.loaded_count = 1;
  d.sprites[1] = (dg_sprite_t){.value = {.type = 16, .member = member.id},
                               .visible = true};
  dg_sprite_changed(&d, 1);
  dg_flash_sync(&d);
  assert(dg_flash_service(&d, 100000));
  dg_flash_instance_t *root = dg_flash_root(&d, 1),
                      *mask = dg_flash_child(root, 1),
                      *inside = dg_flash_child(mask, 1);
  assert(mask && inside);
  property(mask, "_alpha", 0);
  property(inside, "_alpha", 0);
  property(mask, "_visible", 0);
  property(inside, "_visible", 0);
  dg_flash_instance_t *fill = drawing(mask, "drawn-fill", 10, 2, 10, true);
  property(fill, "_alpha", 0);
  property(fill, "_visible", 0);
  drawing(mask, "drawn-stroke", 11, 10, 10, false);
  lv_t field_args[] = {lv_text(&values, "device-text", false),
                       lv_num(12),
                       lv_num(8),
                       lv_num(8),
                       lv_num(8),
                       lv_num(8)};
  call(mask, "createTextField", 6, field_args);
  dg_flash_instance_t *field = dg_flash_child(mask, 16384 + 12);
  assert(field);
  dg_avm1_set(dg_flash_vm(field), dg_flash_object(field), "text",
              lv_text(&values, "X", false));
  // Its unresolved default font is irrelevant: device text is not mask
  // geometry and must never call the text raster provider.
  dg_flash_raster_host_t host = {.scale = 1, .asset = asset, .text = text};
  dg_flash_raster_t pixels;
  assert(dg_flash_render(&d, 1, &host, &pixels) && pixels.width == 16 &&
         pixels.height == 16);
  assert(fills == 1 && empty == 1 && text_rasters == 0);
  for (unsigned y = 0; y < 16; y++)
    for (unsigned x = 0; x < 16; x++) {
      bool included =
          (x >= 2 && x < 6) && ((y >= 2 && y < 6) || (y >= 10 && y < 14));
      assert(pixels.pixels[y * 16 + x] == (included ? 0xffff0000u : 0));
    }
  free(pixels.pixels);
  property(field, "embedFonts", 1);
  property(field, "_alpha", 0);
  property(field, "_visible", 0);
  assert(dg_flash_render(&d, 1, &host, &pixels));
  assert(text_rasters == 1);
  for (unsigned y = 0; y < 16; y++)
    for (unsigned x = 0; x < 16; x++) {
      bool included =
          ((x >= 2 && x < 6) && ((y >= 2 && y < 6) || (y >= 10 && y < 14))) ||
          (x == 9 && y == 9) || (x == 12 && y == 11);
      assert(pixels.pixels[y * 16 + x] == (included ? 0xffff0000u : 0));
    }
  free(pixels.pixels);
  dg_flash_instance_t *source = dg_flash_child(root, 2);
  dg_avm1_t *vm = dg_flash_vm(source);
  dg_avm1_set(vm, dg_flash_object(source), "onPress",
              dg_avm1_native(vm, "trace"));
  const int points[][3] = {{3, 3, 1},   {11, 3, 0}, {3, 11, 1},
                           {10, 10, 0}, {9, 9, 1},  {12, 11, 1}};
  for (unsigned n = 0; n < sizeof(points) / sizeof(points[0]); n++) {
    unsigned before = presses;
    dg_flash_pointer(&d, points[n][0], points[n][1], false);
    dg_flash_pointer(&d, points[n][0], points[n][1], true);
    assert(dg_flash_service(&d, 100000));
    dg_flash_pointer(&d, points[n][0], points[n][1], false);
    assert(dg_flash_service(&d, 100000));
    if (presses - before != (unsigned)points[n][2])
      fprintf(stderr, "point%u presses%u expected%d failed%d %s\n", n,
              presses - before, points[n][2], values.failed, values.error);
    assert(presses - before == (unsigned)points[n][2]);
  }
  assert(text_hits > 0);
  assert(!values.failed);
  d.platform.flash_field_hit = NULL;
  dg_flash_pointer(&d, 9, 9, false);
  assert(values.failed &&
         strstr(values.error, "mask hit provider unavailable"));
  dg_flash_dispose(&d);
  puts("Flash masks ignore fill/object opacity and color transforms, including "
       "nested clips and dynamic fills; strokes and device text are excluded, "
       "embedded text uses glyph coverage for rendering and pointer hits");
}
