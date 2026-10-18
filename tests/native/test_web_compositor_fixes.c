// Pixel contracts for the audit fixes. Synthetic assets only.
#include "compositor.h"
#include "ink.h"
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
#include "projector_ink_observations.h"
#include "projector_shape_observations.h"
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_runtime_t values;
static dg_runtime_t director;
static wc_t compositor;
static unsigned failures, checks, text_size, text_font;
static uint8_t image[48], mask[48];
static const lv_movie_t code = {.name = "AUDIT.DXR"};
static const dg_cast_t casts[] = {{.name="Internal",.file=1,.cast=1}};
static const dg_frame_t frames[] = {{0, 0}};
static uint32_t palette[256];
static const dg_text_style_t initial = {
    .font_name = "First", .font_id = 1, .size = 12, .line_height = 2};
static const dg_text_style_t insertion = {
    .font_name = "Second", .font_id = 2, .size = 18, .line_height = 3};
static const char *const poses[] = {"image.fdi", "image.fdi", "image.fdi"};
static dg_member_t members[] = {
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
    {.id = DG_ID(1, 1, 6), .number = 6, .cast = 1, .type = 3,
     .width = 4, .height = 4, .name = "colored", .text = "A\rB",
     .text_style = &initial, .text_insert_style = &insertion},
    {.id = DG_ID(1, 1, 7), .number = 7, .cast = 1, .type = 1,
     .name = "unpaired", .width = 4, .height = 1, .asset = "image.fdi"},
    {.id = DG_ID(1, 1, 8), .number = 8, .cast = 1, .type = 3,
     .name = "not a mask", .text = "A", .text_style = &initial},
#if DG_D5 || DG_D10
    {.id = DG_ID(1, 1, 9), .number = 9, .cast = 1, .type = 10,
     .name = "video", .width = 2, .height = 2},
#endif
};
#if DG_D10
static const dg_text_run_t color_runs[] = {{0, 0xff0000}, {2, 0x00ff00}};
static const dg_link_t links[] = {{1,3,"synthetic-link"}};
static const dg_text_extra_t text_extras[] = {{DG_ID(1, 1, 6), 2, 1, color_runs, links}};
#endif
static const dg_movie_t movie = {
    .code = &code, .id = 1, .tempo = 30, .cast_count = 1,
    .member_count = sizeof(members) / sizeof(*members), .frame_count = 1,
    .casts = casts, .members = members, .frames = frames, .palette = palette,
#if DG_D10
    .text_extra_count = 1, .text_extras = text_extras,
#endif
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
  unsigned size = data[7];
  if (out) { assert(cap >= size); memcpy(out, data, size); }
  return size;
}
// Deterministic two-line text, with visibly different rows. No font dependency.
static bool text(void *ctx, const char *utf8, int w, int h,
                 const wc_text_style_t *style, uint8_t *coverage) {
  (void)ctx; (void)utf8;
  text_size = style ? style->size : 0;
  text_font = style ? style->font : 0;
  if (w > 0 && h > 0) coverage[0] = 255;
  if (w > 1 && h > 2) coverage[2 * w + 1] = 255;
  return true;
}
static int layout(void *ctx, const char *utf8, int w, int h,
                  const wc_text_style_t *style, int16_t *boxes, int count) {
  (void)ctx; (void)utf8; (void)w; (void)h; (void)style;
  assert(count >= 3);
  memcpy(boxes, (int16_t[]){0, 0, 1, 1, 0, 0, 0, 0, 1, 2, 1, 1}, 12 * sizeof(*boxes));
  return 3;
}
#if DG_D5 || DG_D10
static bool video_frame(void *ctx, const dg_runtime_t *owner, unsigned sprite,
                        uint32_t *out, uint32_t cap, unsigned *width, unsigned *height) {
  (void)ctx;
  assert(owner == &director && sprite == 1 && cap >= 4);
  memcpy(out, (uint32_t[]){0xff0000ff,0xff00ff00,0xffff0000,0xffffffff},4*sizeof(*out));
  *width = *height = 2;
  return true;
}
#endif
#if DG_D10
static bool read_pixels(void *ctx, uint32_t member, dg_pixel_image_t *out) {
  (void)ctx; return wc_read_image(&compositor, &director, member, out);
}
#endif
static bool hit(void *ctx, const dg_member_t *member, unsigned ink, int x, int y) {
  (void)ctx; return wc_hit(&compositor, &values, member, ink, x, y);
}
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
static unsigned link_queries;
static int link_x,link_y;
static int text_char_at(void *ctx,uint32_t member,unsigned sprite,int x,int y) {
  (void)ctx; assert(member == DG_ID(1,1,6) && sprite == 1);
  link_queries++; link_x=x; link_y=y;
  return wc_text_char_at(&compositor,&director,&members[5],"A\rB",4,4,x,y);
}
#endif
static void reset(void) {
#if DG_D10
  dg_images_dispose(&director);
#endif
  wc_release(&compositor);
  wc_init(&compositor, (wc_host_t){.read = read_asset, .text = text, .layout = layout,
#if DG_D5 || DG_D10
      .video_frame = video_frame,
#endif
  }, 4096);
  dg_platform_t platform = {.hit = hit};
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  platform.text_char_at = text_char_at;
  link_queries=0;
#endif
#if DG_D10
  platform.read_image = read_pixels;
#endif
  dg_init(&director, &values, platform, NULL, NULL, 0, 1);
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
int main(void) {
  static const struct { unsigned ink, channel, rgb; } cases[] = {
    {1, 137, 0x000000}, {2, 155, 0x00ff00}, {3, 219, 0xffff00},
    {4, 82, 0xffff00}, {5, 64, 0xff0000}, {6, 100, 0xff00ff},
    {7, 237, 0xff00ff}, {33, 255, 0xff00ff}, {34, 118, 0xff00ff},
    {35, 28, 0xff0000}, {37, 201, 0xff00ff}, {38, 28, 0xff0001},
    {39, 173, 0x000000},
  };
  for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
    reset();
    place(1, 1, cases[i].ink, 100);
    render();
    assert(!values.failed);
    check("arithmetic_channel", dg_ink_channel(173, 201, cases[i].ink) == cases[i].channel,
          dg_ink_channel(173, 201, cases[i].ink), cases[i].channel);
    check("arithmetic_pixel", pixel(20, 20) == cases[i].rgb, pixel(20, 20), cases[i].rgb);
  }
  // Observed from original DIRAPI/IML32 8.5.1r104 under Wine8, not a
  // second implementation's formula: no extra subtraction at the boundary.
  check("original_projector_subtract_pin", dg_ink_channel(60, 100, 35) == 40,
        dg_ink_channel(60, 100, 35), 40);
  reset(); place(1, 1, 42, 100); render(); assert(values.failed);
  reset();
  dg_sprite_t *tint = place(1, 1, 41, 100);
  tint->value.fore_rgb = 0x0a141e; tint->value.back_rgb = 0x6e7882;
  render(); assert(!values.failed);
  check("darken_maps_red_channels", pixel(20, 20) == 0x1e1477, pixel(20, 20), 0x1e1477);
  check("darken_preserves_matte", pixel(21, 20) == 0xff0000, pixel(21, 20), 0xff0000);
  check("darken_maps_white_channels", pixel(22, 20) == 0x9f8b77, pixel(22, 20), 0x9f8b77);
  check("darken_filters_channel", dg_ink_tint_channel(128, 20, 220) == 130,
        dg_ink_tint_channel(128, 20, 220), 130);
  tint->value.blend = 50; director.visual_revision++; render();
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  unsigned half = 0x80007e;
#else
  unsigned half = 0x7f007f;
#endif
  check("darken_blend_overrides_tint", pixel(20, 20) == half, pixel(20, 20), half);
  tint->value.ink = 40; tint->value.blend = 100; director.visual_revision++; render();
  check("lighten_maps_red_channels", pixel(20, 20) == 0x9c9cff, pixel(20, 20), 0x9c9cff);
  check("lighten_preserves_matte", pixel(21, 20) == 0xff0000, pixel(21, 20), 0xff0000);
  reset();
  dg_sprite_t *s = place(1, 3, 0, 50);
  s->value.width = s->value.height = 10;
  render();
  unsigned red = pixel(20, 20) & 255;
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  check("shape_half_opacity", red == 126, red, 126);
  // Original Birthday DIRAPI/IML32 8.5.1r104, ink-shape.lingo:
  // 33 stage observations, not values calculated with the tested helper.
  static const unsigned shape_percent[] = {0,1,10,25,49,50,51,75,90,99,100};
  static const unsigned shape_red[] = {0,2,24,63,124,126,129,190,228,251,255};
  static const unsigned shape_blue[] = {255,252,230,191,130,128,125,64,26,3,0};
  static const unsigned shape_inks[] = {0,8,32};
  for (unsigned i = 0; i < 3; i++)
    for (unsigned n = 0; n < 11; n++) {
      s->value.ink = shape_inks[i]; s->value.blend = shape_percent[n];
      director.visual_revision++; render();
      unsigned expected = shape_red[n] | shape_blue[n] << 16;
      check("projector_shape_blend", pixel(20,20) == expected, pixel(20,20), expected);
    }
#else
  check("shape_half_opacity", red == 127, red, 127);
#endif

  reset();
  place(1, 1, 0, 50);
  render();
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  unsigned copy_white = 0xff7e7e;
#else
  unsigned copy_white = 0xff0000;
#endif
  check("blended_copy_versioned_matte", pixel(21, 20) == copy_white,
        pixel(21, 20), copy_white);

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
  check("scrolled_hit_test", wc_text_char_at(&compositor, &director, &members[3],
        "A\rB", 10, 10, 1, 0) == 2, 0, 0);
  lv_set(&values, NULL, "text", field, lv_text(&values, "changed", false));
  render();
  assert(!values.failed && director.field_text_changed[0]);
  check("text_uses_insertion_style", text_size == insertion.size, text_size, insertion.size);
  check("text_uses_insertion_font", text_font == insertion.font_id, text_font, insertion.font_id);
  lv_set(&values, NULL, "font", field, lv_text(&values, "First", false));
  render();
  check("live_font", text_font == 1, text_font, 1);

  reset(); s = place(1,4,0,100);
  s->value.x = s->value.y = 0; s->value.width = s->value.height = 4;
  s->custom_quad = true;
  memcpy(s->quad,(float[]){40,40,40,48,32,48,32,40},sizeof(s->quad));
  lv_set(&values,NULL,"color",lv_make(LV_MEMBER,DG_ID(1,1,4)),lv_make(LV_COLOR,0xff0000));
  render(); assert(!values.failed);
  check("rotated_text_first_glyph", pixel(39,40) == 0xff,pixel(39,40),0xff);
  check("rotated_text_second_glyph", pixel(35,42) == 0xff,pixel(35,42),0xff);
  check("rotated_text_transparent_corner", pixel(32,40) == 0xff0000,pixel(32,40),0xff0000);
  s->flip_h = true; director.visual_revision++; render();
  check("flipped_rotated_text_first_glyph", pixel(39,47) == 0xff,pixel(39,47),0xff);
  check("flipped_rotated_text_vacated_glyph", pixel(39,40) == 0xff0000,pixel(39,40),0xff0000);
  lv_set(&values,NULL,"scrolltop",lv_make(LV_MEMBER,DG_ID(1,1,4)),lv_num(2));
  render(); assert(!values.failed);
  check("transformed_text_scroll",pixel(39,44) == 0xff,pixel(39,44),0xff);

#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  reset(); s = place(1,6,0,100);
  members[5].editable = true; // An interactive text receiver for the input test.
  s->value.x = s->value.y = 0; s->value.width = s->value.height = 4;
  s->custom_quad = true;
  memcpy(s->quad,(float[]){40,40,40,48,32,48,32,40},sizeof(s->quad));
  assert(dg_tick(&director,35,42,true,DG_SERVICE_BUDGET));
  assert(dg_tick(&director,35,42,false,DG_SERVICE_BUDGET));
  check("transformed_link_local_coordinates",link_queries == 1 && link_x == 1 && link_y == 2,
        (unsigned)(link_x*10+link_y),12);
  s->flip_h = true; dg_sprite_changed(&director,1);
  assert(dg_tick(&director,35,44,true,DG_SERVICE_BUDGET));
  assert(dg_tick(&director,35,44,false,DG_SERVICE_BUDGET));
  check("flipped_link_local_coordinates",link_queries == 2 && link_x == 1 && link_y == 2,
        (unsigned)(link_x*10+link_y),12);
  members[5].editable = false;
#endif

#if DG_D5 || DG_D10
  reset(); s = place(1,9,0,100);
  s->value.x = s->value.y = 0; s->value.width = s->value.height = 2;
  s->custom_quad = true;
  memcpy(s->quad,(float[]){40,40,44,44,40,48,36,44},sizeof(s->quad));
  render(); assert(!values.failed);
  check("video_quad_red",pixel(40,41) == 0xff,pixel(40,41),0xff);
  check("video_quad_green",pixel(42,44) == 0xff00,pixel(42,44),0xff00);
  check("video_quad_blue",pixel(37,44) == 0xff0000,pixel(37,44),0xff0000);
  check("video_quad_white",pixel(40,46) == 0xffffff,pixel(40,46),0xffffff);
  check("video_quad_outside",pixel(36,40) == 0xff0000,pixel(36,40),0xff0000);
  s->flip_h = true; director.visual_revision++; render();
  check("video_quad_flipped",pixel(40,41) == 0xff00,pixel(40,41),0xff00);
  s->value.blend = 50; director.visual_revision++; render();
  check("video_quad_opacity",pixel(40,41) == 0x807f00,pixel(40,41),0x807f00);
#endif

#if DG_D10
  reset();
  lv_t bitmap = lv_make(LV_MEMBER, DG_ID(1, 1, 1));
  lv_t source = lv_get(&values, NULL, "image", bitmap);
  lv_frame_t imaging_frame = {.movie = &code};
  lv_t cropped = lv_call(&values, &imaging_frame, "crop", 2,
      (lv_t[]){source, lv_rect(&values, 0, 0, 2, 1)});
  lv_set(&values, NULL, "image", bitmap, cropped);
  lv_set(&values, NULL, "regpoint", bitmap, lv_point(&values, 0, 0));
  place(1, 1, 8, 100); render();
  assert(!values.failed);
  check("mutable_matte_hit", dg_hit(&director, 20, 20) == 1, dg_hit(&director, 20, 20), 1);
  check("mutable_matte_miss", dg_hit(&director, 23, 20) == 0, dg_hit(&director, 23, 20), 0);
  lv_t bound = lv_get(&values, NULL, "image", bitmap);
  lv_set(&values, NULL, "usealpha", bound, lv_num(0));
  check("mutable_alpha_disabled_matte", dg_hit(&director, 23, 20) == 0, dg_hit(&director, 23, 20), 0);
  director.sprites[1].value.ink = 0; director.visual_revision++;
  check("mutable_copy_rectangle_hit", dg_hit(&director, 23, 20) == 1, dg_hit(&director, 23, 20), 1);

  reset();
  pixels(image, 0xf801, 0xfffe, 0xfffe, 0xfffe);
  place(1, 1, 9, 100);
  render();
  assert(!values.failed);
  // The explicit black mask makes every pixel opaque, including the image's
  // former matte. The cached bounds must encompass this newly visible area.
  check("mask_updates_coverage_bounds", pixel(21, 20) == 0xffffff,
        pixel(21, 20), 0xffffff);
  s = place(2, 7, 9, 100); s->value.y = 24;
  render(); assert(!values.failed);
  check("missing_mask_falls_back_to_matte", pixel(21, 24) == 0xff0000, pixel(21, 24), 0xff0000);
  check("mask_cache_is_per_member", pixel(21, 20) == 0xffffff, pixel(21, 20), 0xffffff);
  lv_t live_mask = lv_get(&values, NULL, "image", lv_make(LV_MEMBER, DG_ID(1, 1, 2)));
  lv_call(&values, &imaging_frame, "setpixel", 4,
      (lv_t[]){live_mask, lv_num(1), lv_num(0), lv_make(LV_COLOR, 0xffffff)});
  render(); assert(!values.failed);
  check("mask_mutation_repaints_source", pixel(21, 20) == 0xff0000, pixel(21, 20), 0xff0000);

  reset(); memcpy(mask, "FDI2", 4); mask[7] = 48;
  memcpy(mask + 32, (uint8_t[]){128,128,128,255,128,128,128,255,255,255,255,255,255,255,255,255},16);
  place(1, 1, 9, 100); render(); assert(!values.failed);
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  unsigned soft_mask = 0xff7e7e;
  check("original_gray_mask_precision", pixel(20, 20) == 0x80007e, pixel(20, 20), 0x80007e);
  check("original_unnamed_mask", dg_mask_pair(NULL, NULL, -4, -4), 1, 1);
#else
  unsigned soft_mask = 0xff0000;
#endif
  check("rgba32_mask_coverage", pixel(21, 20) == soft_mask, pixel(21, 20), soft_mask);
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  reset(); members[1].width = 2; members[1].reg_x = -1;
  memcpy(mask, "FDI2", 4); mask[7] = 40; word(mask + 8, 2);
  memcpy(mask + 32, (uint8_t[]){0,0,0,255,0,0,0,255},8);
  place(1, 1, 9, 100); render(); assert(!values.failed);
  check("trimmed_mask_registration", pixel(20, 20) == 0xff0000 && pixel(21, 20) == 0xffffff &&
      pixel(22, 20) == 0xffffff && pixel(23, 20) == 0xff0000, pixel(21, 20), 0xffffff);
  members[1].width = 4; members[1].reg_x = 0;
#endif

#endif
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  reset(); place(1, 6, 0, 100)->value.height = 4; render();
  check("authored_text_first_color", pixel(20, 20) == 0xff, pixel(20, 20), 0xff);
  check("authored_text_second_color", pixel(21, 22) == 0xff00, pixel(21, 22), 0xff00);
  dg_pixel_image_t text_image;
  assert(wc_read_image(&compositor, &director, DG_ID(1, 1, 6), &text_image));
  check("text_image_keeps_runs", text_image.pixels[9] == 0xff00ff00u, text_image.pixels[9], 0xff00ff00u);
  free(text_image.pixels);
  lv_set(&values, NULL, "color", lv_make(LV_MEMBER, DG_ID(1, 1, 6)), lv_make(LV_COLOR, 0x00ffff));
  render();
  check("live_color_overrides_runs", pixel(20, 20) == 0xffff00 && pixel(21, 22) == 0xffff00,
        pixel(21, 22), 0xffff00);
#endif
  reset();
  place(1, 1, 0, 100); render();
  check("opaque_copy_fills_matte", pixel(21, 20) == 0xffffff, pixel(21, 20), 0xffffff);
  director.sprites[1].value.blend = 50;
  director.visual_revision++;
  render();
  check("copy_cache_blend", pixel(21, 20) == copy_white, pixel(21, 20), copy_white);
  reset();
  image[19] = 1;
  place(1, 1, 0, 50); render();
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  check("one_bit_copy_blend", pixel(20, 20) == half, pixel(20, 20), half);
  check("one_bit_copy_rectangle", pixel(21, 20) == copy_white, pixel(21, 20), copy_white);
#else
  check("one_bit_copy_ignores_blend", pixel(20, 20) == 0xff, pixel(20, 20), 0xff);
  check("one_bit_copy_has_no_matte", pixel(21, 20) == 0xffffff, pixel(21, 20), 0xffffff);
#endif
  reset();
  s = place(1, 1, 36, 100); s->value.back = 1; s->value.flags &= ~32;
  render();
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  unsigned keyed_white = 0xff; // White maps to the chosen background color.
#else
  unsigned keyed_white = 0xffffff;
#endif
  check("key_keeps_nonmatching_white", pixel(22, 20) == keyed_white, pixel(22, 20), keyed_white);
  s->value.back = 0; director.visual_revision++; render();
  check("live_key_uses_same_asset", pixel(22, 20) == 0xff0000, pixel(22, 20), 0xff0000);
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  for (unsigned row = 0; row < sizeof(projector_stage_blend) / sizeof(*projector_stage_blend); row++) {
    const unsigned *observation = projector_stage_blend[row];
    reset();
    memcpy(image, "FDI2", 4); image[7] = 48; image[19] = 32;
    for (unsigned x = 0; x < 4; x++) {
      image[32 + x * 4] = 20; image[33 + x * 4] = 90;
      image[34 + x * 4] = 180; image[35 + x * 4] = 255;
    }
    s = place(1, 1, observation[0], observation[1]);
    s->value.fore_rgb = 0x14283c; s->value.back_rgb = 0xdcb48c;
    render(); assert(!values.failed);
    check("original_stage_blend", pixel(20, 20) == observation[2], pixel(20, 20), observation[2]);
  }
  for (unsigned row = 0; row < sizeof(projector_stage_alpha) / sizeof(*projector_stage_alpha); row++) {
    const unsigned *observation = projector_stage_alpha[row];
    reset();
    lv_frame_t frame = {.movie = &code};
    lv_t surface = lv_call(&values, &frame, "image", 3, (lv_t[]){lv_num(4), lv_num(1), lv_num(32)});
    lv_call(&values, &frame, "fill", 3, (lv_t[]){surface, lv_rect(&values, 0, 0, 4, 1), lv_make(LV_COLOR, observation[0])});
    lv_call(&values, &frame, "setalpha", 2, (lv_t[]){surface, lv_num(observation[1])});
    lv_set(&values, NULL, "usealpha", surface, lv_num(observation[2]));
    lv_t bitmap_member = lv_make(LV_MEMBER, DG_ID(1, 1, 1));
    lv_set(&values, NULL, "image", bitmap_member, surface);
    lv_set(&values, NULL, "regpoint", bitmap_member, lv_point(&values, 0, 0));
    s = place(1, 1, observation[3], observation[4]); s->value.fore_rgb = 0;
    render(); assert(!values.failed);
    check("original_mutable_alpha", pixel(20, 20) == observation[5], pixel(20, 20), observation[5]);
    if (observation[3] == 8) {
      bitmap_member = lv_make(LV_MEMBER, DG_ID(1, 1, 7));
      lv_set(&values, NULL, "image", bitmap_member, surface);
      lv_set(&values, NULL, "regpoint", bitmap_member, lv_point(&values, 0, 0));
      s = place(1, 7, 9, observation[4]); s->value.fore_rgb = 0;
      render(); assert(!values.failed);
      check("original_mutable_mask_fallback", pixel(20, 20) == observation[5], pixel(20, 20), observation[5]);
    }
  }
  // Independent projector shape results cover both pattern colors. In
  // particular Boolean shape inks must not use the RGB bitmap formulas.
  for (unsigned pattern = 0; pattern < 2; pattern++) {
    unsigned row = 0;
    for (unsigned dest = 0; dest < 3; dest++)
      for (unsigned fore = 0; fore < 2; fore++)
        for (unsigned back = 0; back < 2; back++)
          for (unsigned ink = 0; ink < 20; ink++)
            for (unsigned opacity = 50; opacity <= 100; opacity += 50) {
              reset();
              director.stage_color = projector_shape_dest[dest];
              members[2].pattern = pattern ? 15 : 1;
              s = place(1, 3, projector_shape_inks[ink], opacity);
              s->value.fore_rgb = projector_shape_fore[fore];
              s->value.back_rgb = projector_shape_back[back];
              render(); assert(!values.failed);
              unsigned expected = projector_shape_pixels[pattern][row++];
              check("original_shape_ink", pixel(20, 20) == expected, pixel(20, 20), expected);
            }
  }
  members[2].pattern = 0;
  // Every non-Copy ink in the original projector excludes exterior white.
  for (unsigned ink = 0; ink <= 41; ink++) {
    if (ink > 8 && ink < 32) continue;
    for (unsigned opacity = 50; opacity <= 100; opacity += 50) {
      reset(); pixels(image, 0xfffe, 0xfffe, 0xfffe, 0xfffe);
      s = place(1, 1, ink, opacity); s->value.fore_rgb = 0;
      render(); assert(!values.failed);
      unsigned expected = ink ? 0xff0000 : opacity == 50 ? 0xff7e7e : 0xffffff;
      check("original_stage_exterior_matte", pixel(20, 20) == expected, pixel(20, 20), expected);
    }
  }
#endif
  reset();
  s = place(1, 3, 0, 100);
  s->value.x = s->value.y = 0;
  s->value.width = s->value.height = 8;
  s->custom_quad = true;
  memcpy(s->quad, (float[]){20,16,24,20,20,24,16,20}, sizeof(s->quad));
  render(); assert(!values.failed);
  check("transformed_shape_inside", pixel(20,20) == 0xff, pixel(20,20), 0xff);
  check("transformed_shape_outside", pixel(16,16) == 0xff0000, pixel(16,16), 0xff0000);
  check("transformed_shape_edge", pixel(20,16) == 0xff, pixel(20,16), 0xff);
  reset();
  members[2].pattern = 39;
  s = place(1, 3, 0, 100);
  s->value.x = 3; s->value.y = 5; s->value.width = 16; s->value.height = 16;
  render(); assert(!values.failed);
  for (unsigned y = 5; y < 21; y++)
    for (unsigned x = 3; x < 19; x++) {
      unsigned expected = ((x & 4) != (y & 4)) ? 0xff : 0xffffff;
      check("stage_anchored_shape_pattern", pixel(x, y) == expected, pixel(x, y), expected);
    }
  members[2].pattern = 0;
  // Original shape-outline probe: a filled pattern15 keeps a solid stroke.
  for (unsigned line = 0; line <= 8; line++) {
    reset(); members[2].pattern = 15;
    s = place(1,3,0,100); s->value.width = s->value.height = 32;
    s->value.thickness = DG_HAS_BLEND | (line + 1);
    render(); assert(!values.failed);
    for (unsigned x = 0; x < 32; x++) {
      unsigned expected = x < line || x >= 32-line ? 0xff : 0xffffff;
      check("pattern_fill_solid_stroke", pixel(20+x,36) == expected, pixel(20+x,36), expected);
    }
  }
  members[2].pattern = 0;
  reset();
  members[2].pattern = 58; members[2].asset = "image.fdi";
  memcpy(image, "FDI2", 4); image[7] = 48; image[19] = 32;
  for (unsigned x = 0; x < 4; x++) {
    image[32+x*4] = 17+x*20; image[33+x*4] = 34+x*20;
    image[34+x*4] = 51+x*20; image[35+x*4] = 255;
  }
  s = place(1,3,0,100); s->value.x = 3; s->value.y = 5;
  s->value.width = 10; s->value.height = 3;
  s->value.fore_rgb = s->value.back_rgb = 0xff0000;
  render(); assert(!values.failed);
  for (unsigned x = 3; x < 13; x++) {
    unsigned offset = (x % 4) * 20;
    unsigned expected = (17+offset) | (34+offset)<<8 | (51+offset)<<16;
    check("shape_texture_stage_period_and_color", pixel(x,5) == expected, pixel(x,5), expected);
  }
  members[2].pattern = 0; members[2].asset = NULL;
  wc_release(&compositor);
#if DG_D10
  dg_images_dispose(&director);
#endif
  printf("%u mismatches in %u checks\n", failures, checks);
  return failures ? 1 : 0;
}
