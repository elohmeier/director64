// Source-free Imaging Lingo regression cases. No authored game scripts/assets.
#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_runtime_t values;
static dg_runtime_t director;
static const lv_movie_t code = {.name = "IMAGING.DXR"};
static const dg_cast_t casts[] = {{.name="Internal",.file=1,.cast=1}};
static const dg_frame_t frames[] = {{0, 0}};
static uint32_t palette[256];
static const dg_member_t members[] = {
    {.id = DG_ID(1, 1, 1), .number = 1, .cast = 1, .type = 1,
     .width = 4, .height = 2, .name = "source"},
    {.id = DG_ID(1, 1, 2), .number = 2, .cast = 1, .type = 1,
     .width = 1, .height = 1, .name = "destination"},
    {.id = DG_ID(1, 1, 3), .number = 3, .cast = 1, .type = 3,
     .width = 4, .height = 2, .name = "text", .text = "X"},
};
static const dg_movie_t movie = {
    .code = &code, .id = 1, .tempo = 30, .cast_count = 1,
    .member_count = 3, .frame_count = 1, .casts = casts,
    .members = members, .frames = frames, .palette = palette};
static bool read_image(void *ctx, uint32_t member, dg_pixel_image_t *out) {
  (void)ctx; (void)member;
  *out = (dg_pixel_image_t){.width = 4, .height = 2, .depth = 32,
      .pixels = malloc(8 * sizeof(uint32_t)), .use_alpha = true};
  assert(out->pixels);
  for (unsigned i = 0; i < 8; i++) out->pixels[i] = 0xff000000u | i;
  return true;
}
static lv_t call(const char *name, unsigned count, const lv_t *args) {
  lv_frame_t frame = {.movie = &code};
  lv_t value = lv_call(&values, &frame, name, count, args);
  if (values.failed) fprintf(stderr, "failed %s: %s\n", name, values.error);
  assert(!values.failed); return value;
}
static lv_t color(unsigned rgb) { return lv_make(LV_COLOR, rgb); }
static lv_t make(int w, int h, unsigned depth) {
  return call("image", 3, (lv_t[]){lv_num(w), lv_num(h), lv_num(depth)});
}
static lv_t rect(int l, int t, int r, int b) { return lv_rect(&values, l, t, r, b); }
static uint32_t pixel(lv_t image, int x, int y) {
  return (uint32_t)lv_id(call("getpixel", 3, (lv_t[]){image, lv_num(x), lv_num(y)}));
}
static void setpixel(lv_t image, int x, int y, unsigned rgb) {
  call("setpixel", 4, (lv_t[]){image, lv_num(x), lv_num(y), color(rgb)});
}
static void fill(lv_t image, lv_t box, unsigned rgb) {
  call("fill", 3, (lv_t[]){image, box, color(rgb)});
}
static lv_t option(const char *key, lv_t value) {
  return lv_list(&values, 2, (lv_t[]){lv_symbol(&values, key), value}, true);
}
int main(void) {
  dg_init(&director, &values, (dg_platform_t){.read_image = read_image}, NULL, NULL, 0, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  palette[0] = 0xffffff;
  lv_t source = make(4, 2, 32);
  assert(lv_id(lv_get(&values, NULL, "width", source)) == 4);
  assert(pixel(source, 0, 0) == 0xffffff);
  setpixel(source, 0, 0, 0xff0000);
  lv_t duplicate = call("duplicate", 1, &source);
  setpixel(duplicate, 0, 0, 0x0000ff);
  assert(pixel(source, 0, 0) == 0xff0000);
  assert(pixel(duplicate, 0, 0) == 0x0000ff);
  assert(lv_id(call("objectp", 1, &duplicate)) == 1);
  lv_t crop = call("crop", 2, (lv_t[]){source, rect(0, 0, 2, 1)});
  assert(lv_id(lv_get(&values, NULL, "width", crop)) == 2);
  assert(pixel(crop, 0, 0) == 0xff0000);
  setpixel(crop, 0, 0, 0);
  assert(pixel(source, 0, 0) == 0xff0000);
  // Clip a fill without altering pixels outside its rectangle.
  fill(source, rect(-2, -2, 2, 1), 0x00ff00);
  assert(pixel(source, 0, 0) == 0x00ff00 && pixel(source, 3, 0) == 0xffffff);
  // Scalar coordinate overload and geometric fills.
  call("fill", 6, (lv_t[]){source, lv_num(0), lv_num(1), lv_num(4), lv_num(2), color(0xff0000)});
  assert(pixel(source, 3, 1) == 0xff0000);
  lv_t canvas = make(8, 8, 32);
  lv_t oval = lv_list(&values, 4, (lv_t[]){lv_symbol(&values, "shapetype"), lv_symbol(&values, "oval"),
      lv_symbol(&values, "color"), color(0)}, true);
  call("fill", 3, (lv_t[]){canvas, rect(0, 0, 8, 8), oval});
  assert(pixel(canvas, 0, 0) == 0xffffff && pixel(canvas, 4, 4) == 0);
  call("draw", 4, (lv_t[]){canvas, lv_point(&values, 0, 0), lv_point(&values, 7, 7), color(0xff0000)});
  assert(pixel(canvas, 3, 3) == 0xff0000);
  // Copy scaling, clipped destination and overlap all sample stable source pixels.
  lv_t copy = make(4, 2, 32);
  call("copypixels", 4, (lv_t[]){copy, source, rect(0, 0, 4, 2), rect(0, 1, 4, 2)});
  assert(pixel(copy, 3, 0) == 0xff0000);
  setpixel(copy, 0, 0, 0x0000ff);
  call("copypixels", 4, (lv_t[]){copy, copy, rect(1, 0, 4, 1), rect(0, 0, 3, 1)});
  assert(pixel(copy, 1, 0) == 0x0000ff && pixel(copy, 2, 0) == 0xff0000);
  lv_t mask = make(4, 2, 8); fill(mask, rect(0, 0, 2, 2), 0);
  fill(copy, rect(0, 0, 4, 2), 0x0000ff);
  lv_set(&values, NULL, "usealpha", source, lv_num(0));
  call("copypixels", 5, (lv_t[]){copy, source, rect(0, 0, 4, 2), rect(0, 0, 4, 2), option("maskimage", mask)});
  assert(pixel(copy, 0, 0) == 0x00ff00 && pixel(copy, 3, 0) == 0x0000ff);
  // Ink41 scales by background and adds foreground, with distinct
  // RGB values to catch accidentally applying a grayscale tint.
  setpixel(source, 0, 0, 0x0080ff);
  lv_t tint = lv_list(&values, 6, (lv_t[]){lv_symbol(&values, "ink"), lv_num(41),
      lv_symbol(&values, "color"), color(0x201040), lv_symbol(&values, "bgcolor"), color(0xe0f080)}, true);
  call("copypixels", 5, (lv_t[]){copy, source, rect(0, 0, 1, 1), rect(0, 0, 1, 1), tint});
  assert(pixel(copy, 0, 0) == 0x2088bf);
  lv_set(&values, NULL, "ink", tint, lv_num(40));
  call("copypixels", 5, (lv_t[]){copy, source, rect(0, 0, 1, 1), rect(0, 0, 1, 1), tint});
  assert(pixel(copy, 0, 0) == 0x4098ff);
  // Independent projector RGB result: partial blend bypasses the ink and
  // bitwise-colorizes first. Original engine8.5.1r104, Birthday MX9 launcher.
  setpixel(source, 0, 0, 0xff0000);setpixel(copy, 0, 0, 0x0000ff);
  lv_t partial = lv_list(&values, 8, (lv_t[]){lv_symbol(&values,"ink"),lv_num(41),
      lv_symbol(&values,"color"),color(0x14283c),lv_symbol(&values,"bgcolor"),color(0xdcb48c),
      lv_symbol(&values,"blendlevel"),lv_num(128)},true);
  call("copypixels",5,(lv_t[]){copy,source,rect(0,0,1,1),rect(0,0,1,1),partial});
  assert(pixel(copy,0,0)==0x6e149c);
  setpixel(copy,0,0,0x0000ff);lv_set(&values,NULL,"ink",partial,lv_num(35));
  call("copypixels",5,(lv_t[]){copy,source,rect(0,0,1,1),rect(0,0,1,1),partial});
  assert(pixel(copy,0,0)==0x6e149c);
  // Alpha extraction round trips coverage, independently from the source.
  lv_set(&values, NULL, "usealpha", source, lv_num(1));
  call("setalpha", 2, (lv_t[]){source, lv_num(77)});
  lv_t alpha = call("extractalpha", 1, &source);
  assert(pixel(alpha, 0, 0) == 0xb2b2b2);
  call("setalpha", 2, (lv_t[]){copy, alpha});
  lv_t checkalpha = call("extractalpha", 1, &copy);
  assert(pixel(checkalpha, 0, 0) == 0xb2b2b2);
  assert(lv_id(call("setalpha", 2, (lv_t[]){mask, lv_num(128)})) == 0);
  // Picture assignment copies source and changes bounds; later mutations are independent.
  lv_t target = lv_make(LV_MEMBER, DG_ID(1, 1, 2));
  lv_set(&values, NULL, "picture", target, crop);
  assert(!values.failed && dg_member_image(&director, DG_ID(1, 1, 2))->width == 2);
  assert(lv_id(lv_get(&values, NULL, "width", target)) == 2);
  setpixel(crop, 0, 0, 0x123456);
  lv_t assigned = lv_get(&values, NULL, "image", target);
  assert(pixel(assigned, 0, 0) == 0);
  unsigned revision = director.visual_revision;
  setpixel(assigned, 0, 0, 0xabcdef);
  assert(director.visual_revision > revision);
  // Exterior white is transparent in a derived matte; enclosed white is
  // opaque. A later pixel mutation invalidates that cached topology.
  lv_t ring=make(5,5,32);fill(ring,rect(1,1,4,4),0);setpixel(ring,2,2,0xffffff);
  lv_set(&values,NULL,"image",target,ring);
  const dg_pixel_image_t *bound=dg_member_image(&director,DG_ID(1,1,2));
  const uint8_t *matte=dg_image_matte(&director,bound);
  assert(matte && !matte[0] && matte[12]==255);
  assert(dg_image_matte(&director,bound)==matte);
  assigned=lv_get(&values,NULL,"image",target);setpixel(assigned,2,1,0xffffff);
  matte=dg_image_matte(&director,bound);assert(matte && matte[12]==0);
  // Restore the member used by the subsequent collection assertion.
  setpixel(assigned,0,0,0xabcdef);
  // Both stage and text route through owned platform copies.
  lv_t stage = call("crop", 2, (lv_t[]){lv_make(LV_IMAGE, 0), rect(0, 0, 2, 1)});
  assert(pixel(stage, 1, 0) == 0x010000);
  lv_t text = lv_get(&values, NULL, "picture", lv_make(LV_MEMBER, DG_ID(1, 1, 3)));
  assert(pixel(text, 1, 0) == 0x010000);
  // Direct stage painting remains available until a normal scene change.
  lv_t stage_ref = lv_make(LV_IMAGE, 0);
  setpixel(stage_ref, 1, 0, 0x123456);
  assert(dg_stage_image(&director) && pixel(stage_ref, 1, 0) == 0x123456);
  director.visual_revision++;
  assert(!dg_stage_image(&director) && pixel(stage_ref, 1, 0) == 0x010000);
  // Reclamation sees a handle nested in an instance/list root across steps.
  values.roots[0] = lv_list(&values, 1, &stage, false);
  for (unsigned i = 0; i < 100; i++) { values.steps++; (void)make(8, 8, 32); }
  stage = lv_at(&values, values.roots[0], 1);
  assert(pixel(stage, 1, 0) == 0x010000);
  assert(pixel(lv_get(&values, NULL, "image", target), 0, 0) == 0xabcdef);
  dg_images_dispose(&director);
  puts("imaging: independent surfaces, mutations, alpha, copies, bounds and lifetime passed");
  return 0;
}
