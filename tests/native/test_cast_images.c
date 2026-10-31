#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_runtime_t values;
static dg_runtime_t director;
static uint8_t saved[2u * 1024u * 1024u];
static unsigned saved_length;
static const lv_movie_t code = {.name = "MAIN.DXR"},
                        cast_code = {.name = "images.cst"};
static const dg_cast_t casts[] = {{.name = "Internal", .file = 1, .cast = 1},
                                  {.name = "images", .file = 2, .cast = 1}};
static const dg_frame_t frames[] = {{0, 0}};
static const dg_movie_t movie = {.id = 1,
                                 .code = &code,
                                 .tempo = 30,
                                 .casts = casts,
                                 .cast_count = 2,
                                 .frame_count = 1,
                                 .frames = frames};
static const dg_movie_t images = {.id = 2, .code = &cast_code};
static const dg_movie_t *shared[] = {&images};
static bool read_save(void *ctx, const char *name, char *out, unsigned cap,
                      unsigned *length) {
  (void)ctx;
  if (strcmp(name, "images.cst") || !saved_length)
    return false;
  assert(saved_length < cap);
  memcpy(out, saved, saved_length);
  *length = saved_length;
  return true;
}
static bool write_save(void *ctx, const char *name, const char *data,
                       unsigned length) {
  (void)ctx;
  assert(!strcmp(name, "images.cst") && length < sizeof(saved));
  memcpy(saved, data, length);
  saved_length = length;
  return true;
}
static bool boot(void) {
  dg_images_dispose(&director);
  dg_init(&director, &values,
          (dg_platform_t){.read_file = read_save, .write_file = write_save},
          NULL, NULL, 0, 1);
  return dg_enter(&director, &movie, 1, shared, 1);
}
static lv_t call(const char *name, unsigned count, const lv_t *args) {
  lv_frame_t caller = {.movie = &code};
  lv_t value = lv_call(&values, &caller, name, count, args);
  if (values.failed)
    fprintf(stderr, "%s: %s\n", name, values.error);
  assert(!values.failed);
  return value;
}
static lv_t text(const char *s) { return lv_text(&values, s, false); }
static uint32_t pattern(unsigned x, unsigned y) {
  return 0x80000000u | ((x * 31u + y * 17u) & 0xffffffu);
}
int main(void) {
  assert(boot());
  lv_t cast = lv_make(LV_CASTLIB, 2);
  lv_t names = call("new", 2, (lv_t[]){lv_symbol(&values, "field"), cast});
  lv_set(&values, NULL, "name", names, text("imageNames"));
  lv_set(&values, NULL, "text", names, text("[\"img1\"]"));
  lv_t member = call("new", 2, (lv_t[]){lv_symbol(&values, "bitmap"), cast});
  lv_set(&values, NULL, "name", member, text("img1"));
  lv_t image = call("image", 3, (lv_t[]){lv_num(560), lv_num(420), lv_num(32)});
  // Fill through the public imaging API, including alpha and registration.
  lv_set(&values, NULL, "useAlpha", image, lv_num(1));
  lv_set(&values, NULL, "image", member, image);
  dg_pixel_image_t *pixels =
      (dg_pixel_image_t *)dg_member_image(&director, (uint32_t)lv_id(member));
  assert(pixels && pixels->width == 560 && pixels->height == 420);
  for (unsigned y = 0; y < 420; y++)
    for (unsigned x = 0; x < 560; x++)
      pixels->pixels[y * 560 + x] = pattern(x, y);
  lv_set(&values, NULL, "regPoint", member, lv_point(&values, -3, 27));
  assert(lv_integer(&values,
                    call("save", 2,
                         (lv_t[]){cast, text("C:\\Bilder\\images.cst")})) == 1);
  assert(saved_length > 65000 && !memcmp(saved, "D6TF\2\0", 6));
  unsigned length = saved_length;
  assert(boot());
  uint32_t id = DG_ID(2, 1, 2);
  const dg_member_t *restored = dg_member(&director, id);
  assert(restored && restored->type == 1 && !strcmp(restored->name, "img1"));
  const dg_pixel_image_t *restored_pixels = dg_member_image(&director, id);
  assert(restored_pixels && restored_pixels->width == 560 &&
         restored_pixels->height == 420 && restored_pixels->use_alpha);
  assert(restored_pixels->reg_x == -3 && restored_pixels->reg_y == 27);
  for (unsigned y = 0; y < 420; y++)
    for (unsigned x = 0; x < 560; x++)
      assert(restored_pixels->pixels[y * 560 + x] == pattern(x, y));
  lv_t restored_names =
      lv_get(&values, NULL, "text", lv_make(LV_MEMBER, DG_ID(2, 1, 1)));
  assert(!strcmp(lv_cstr(&values, restored_names), "[\"img1\"]"));
  // Reject truncated/invalid image records before installing any members.
  saved_length = length - 1;
  assert(!boot() && values.failed && director.dynamic_count == 0);
  saved_length = length;
  saved[length] = 0;
  saved_length++;
  assert(!boot() && values.failed && director.dynamic_count == 0);
  saved_length = length;
  assert(boot());
  member = lv_make(LV_MEMBER, id);
  call("erase", 1, &member);
  assert(!dg_member_image(&director, id));
  call("save", 2, (lv_t[]){cast, text("images.cst")});
  assert(!memcmp(saved, "D6TF\1\0", 6) && saved_length < 65000);
  assert(boot() && !dg_member(&director, id));
  dg_images_dispose(&director);
  puts("Bitmap casts persist exact RGBA/name/registration beside fields, "
       "reject malformed records and erase cleanly");
}
