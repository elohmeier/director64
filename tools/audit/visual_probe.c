// Source-free diagnostic reproductions, separate from the passing contract suite.
// See docs/implementation-audit.md. Exit 1 means a visual/semantic mismatch.
#include "compositor.h"
#include "ink.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static lv_runtime_t values;
static dg_runtime_t director;
static wc_t compositor;
static unsigned failures, checks, text_size;
static uint8_t image[40], mask[40];
static const lv_movie_t code = {.name = "AUDIT.DXR"};
static const dg_cast_t casts[] = {{.name="Internal", .file=1, .cast=1}};
static const dg_frame_t frames[] = {{0, 0}};
static uint32_t palette[256];
static const dg_text_style_t initial = {
    .font_name = "First", .font_id = 1, .size = 12, .line_height = 2};
static const dg_text_style_t insertion = {
    .font_name = "Second", .font_id = 2, .size = 18, .line_height = 3};
static const char *const poses[] = {"image.fdi", "image.fdi", "image.fdi"};
static const dg_member_t members[] = {
    {.id = DG_ID(1, 1, 1), .number = 1, .cast = 1, .type = 1,
     .name = "sample", .width = 4, .height = 1, .asset = "image.fdi"},
    {.id = DG_ID(1, 1, 2), .number = 2, .cast = 1, .type = 1,
     .name = "sample_mask", .width = 4, .height = 1, .asset = "mask.fdi"},
    {.id = DG_ID(1, 1, 3), .number = 3, .cast = 1, .type = 8,
     .name = "rectangle", .shape = 1, .filled = 1},
    {.id = DG_ID(1, 1, 4), .number = 4, .cast = 1, .type = 3,
     .name = "field", .text = "A\rB", .text_style = &initial,
     .text_insert_style = &insertion},
    {.id = DG_ID(1, 1, 5), .number = 5, .cast = 1, .type = 2,
     .name = "animation", .width = 4, .height = 1,
     .film_count = 3, .film_assets = poses, .film_loop = 1},
};
static const dg_movie_t movie = {
    .code = &code, .id = 1, .tempo = 30, .cast_count = 1,
    .member_count = sizeof(members) / sizeof(*members), .frame_count = 1,
    .casts = casts, .members = members, .frames = frames, .palette = palette,
    .stage_color = 0x0000ff};

static void word(uint8_t *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static void pixels(uint8_t *data, unsigned a, unsigned b, unsigned c, unsigned d) {
  memset(data, 0, 40);
  memcpy(data, "FDI1", 4);
  data[7] = 40;
  word(data + 8, 4);
  word(data + 10, 1);
  word(data + 32, a); word(data + 34, b);
  word(data + 36, c); word(data + 38, d);
}
static uint32_t read_asset(void *ctx, const char *name, uint8_t *out, uint32_t cap) {
  (void)ctx;
  const uint8_t *data = !strcmp(name, "image.fdi") ? image
                         : !strcmp(name, "mask.fdi") ? mask : NULL;
  if (!data) return 0;
  if (out) { assert(cap >= 40); memcpy(out, data, 40); }
  return 40;
}
// Deterministic two-line text, with visibly different rows. No font dependency.
static bool text(void *ctx, const char *utf8, int w, int h,
                 const wc_text_style_t *style, uint8_t *coverage) {
  (void)ctx; (void)utf8;
  text_size = style ? style->size : 0;
  if (w > 0 && h > 0) coverage[0] = 255;
  if (w > 1 && h > 2) coverage[2 * w + 1] = 255;
  return true;
}
#if DG_D10
static bool flash_field_hit(void *ctx, dg_runtime_t *owner,
                            struct dg_flash_instance *field, float x, float y) {
  (void)ctx;
  return wc_flash_field_hit(&compositor, owner, field, x, y);
}
static bool read_pixels(void *ctx, uint32_t member, dg_pixel_image_t *out) {
  (void)ctx;
  return wc_read_image(&compositor, (dg_runtime_t *)values.context, member, out);
}
#endif
static void reset(void) {
#if DG_D10
  dg_linked_dispose(&director);
  dg_images_dispose(&director);
#endif
  wc_release(&compositor);
  wc_init(&compositor, (wc_host_t){.read = read_asset, .text = text}, 4096);
  dg_init(&director, &values, (dg_platform_t){
#if DG_D10
      .read_image = read_pixels, .flash_field_hit = flash_field_hit,
#endif
      .trace = NULL}, NULL, NULL, 0, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  palette[0] = 0xffffff;
  palette[1] = 0xff0000;
  palette[2] = 0x0000ff;
  pixels(image, 0xf801, 0xfffe, 0xffff, 0x07c1);
  pixels(mask, 1, 1, 1, 1);
  text_size = 0;
}
static dg_sprite_t *place(unsigned channel, unsigned member, unsigned ink, unsigned opacity) {
  dg_sprite_t *s = &director.sprites[channel];
  *s = (dg_sprite_t){
      .value = {.member = DG_ID(1, 1, member), .type = 16, .x = 20, .y = 20,
                .width = 4, .height = 1, .ink = ink, .blend = opacity,
                .thickness = DG_HAS_BLEND, .fore = 1, .back = 0,
                .fore_rgb = 0xff0000, .back_rgb = 0xffffff,
                .flags = 48, .loc_z = (int16_t)channel},
      .stretch = true, .visible = true};
  dg_sprite_changed(&director, channel);
  return s;
}
static void render(void) { wc_render(&compositor, &director, 0, 0, false, 0); }
static uint32_t pixel(unsigned x, unsigned y) {
  return compositor.pixels[y * WC_WIDTH + x] & 0xffffffu;
}
static void check(const char *name, bool ok, unsigned actual, unsigned expected) {
  checks++;
  if (!ok) failures++;
  printf("%s %s actual=%u expected=%u\n", ok ? "PASS" : "FAIL", name, actual, expected);
}
#if DG_D10
static lv_t call(const char *name, unsigned count, const lv_t *args) {
  lv_frame_t frame = {.movie = &code};
  return lv_call(&values, &frame, name, count, args);
}
#endif

int main(void) {
  const unsigned inks[] = {2, 4, 7, 33, 40, 41};
  for (unsigned i = 0; i < sizeof(inks) / sizeof(*inks); i++) {
    reset();
    place(1, 1, inks[i], 100);
    render();
    char label[32];
    snprintf(label, sizeof(label), "bitmap_ink_%u", inks[i]);
    check(label, !values.failed, values.failed, 0);
  }
  reset();
  dg_sprite_t *s = place(1, 3, 0, 50);
  s->value.width = s->value.height = 10;
  render();
  unsigned red = pixel(20, 20) & 255;
  check("shape_half_opacity", red >= 126 && red <= 129, red, 127);

  reset();
  place(1, 1, 0, 50);
  render();
  // Original 8.5 Copy ignores the matte when useAlpha is false. The white
  // source rectangle contributes 127/256 over red at blend50.
#ifdef DIRECTOR64_D85
  unsigned expected_copy = 0xff7e7e;
#else
  unsigned expected_copy = 0xff0000;
#endif
  check("blended_copy_profile_coverage", pixel(21, 20) == expected_copy,
        pixel(21, 20), expected_copy);

  reset();
  s = place(1, 1, 36, 100);
  s->value.back = 1;
  s->value.back_rgb = 0xff0000;
  render();
  check("background_ink_uses_backcolor", pixel(20, 20) == 0xff0000,
        pixel(20, 20), 0xff0000);

  reset();
  s = place(1, 4, 0, 100);
  s->value.width = s->value.height = 10;
  render();
  uint32_t first = pixel(20, 20);
  lv_t field = lv_make(LV_MEMBER, DG_ID(1, 1, 4));
  lv_set(&values, NULL, "scrolltop", field, lv_num(2));
  render();
  assert(!values.failed && director.field_scroll[0] == 2);
  check("scrolltop_moves_text", pixel(20, 20) != first, pixel(20, 20), 0xff0000);
  lv_set(&values, NULL, "text", field, lv_text(&values, "changed", false));
  render();
  assert(!values.failed && director.field_text_changed[0]);
  check("text_uses_insertion_style", text_size == insertion.size, text_size, insertion.size);

#if DG_D10
  reset();
  pixels(image, 0xf801, 0xfffe, 0xfffe, 0xfffe);
  place(1, 1, 9, 100);
  render();
  assert(!values.failed);
  // The explicit black mask makes every pixel opaque, including the image's
  // former matte. The cached bounds must encompass this newly visible area.
  check("mask_updates_coverage_bounds", pixel(21, 20) == 0xffffff,
        pixel(21, 20), 0xffffff);

  reset();
  lv_t source = lv_get(&values, NULL, "image", lv_make(LV_MEMBER, DG_ID(1, 1, 1)));
  lv_t cropped = call("crop", 2, (lv_t[]){source, lv_rect(&values, 0, 0, 2, 1)});
  lv_t width = lv_get(&values, NULL, "width", cropped);
  assert(!values.failed);
  check("crop_retains_image_geometry", lv_integer(&values, width) == 2,
        lv_integer(&values, width), 2);
  call("setpixel", 3, (lv_t[]){source, lv_point(&values, 0, 0), lv_make(LV_COLOR, 0xff0000)});
  lv_t color = call("getpixel", 2, (lv_t[]){source, lv_point(&values, 0, 0)});
  assert(!values.failed);
  check("setpixel_getpixel_roundtrip", (uint32_t)lv_id(color) == 0xff0000,
        (uint32_t)lv_id(color), 0xff0000);

  reset();
  s = place(1, 5, 0, 100);
  s->film_frame = 1;
  call("stop", 1, (lv_t[]){lv_make(LV_SPRITE, 1)});
  assert(!values.failed);
  check("stop_holds_current_pose", s->film_frame == 1, s->film_frame, 1);
#endif
  wc_release(&compositor);
  printf("%u mismatches in %u checks\n", failures, checks);
  return failures ? 1 : 0;
}
