// Runtime film composition, using synthetic text and colored bitmaps only.
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static dg_runtime_t director;
static lv_runtime_t values;
static wc_t compositor;
static unsigned rendered_font;
static const uint32_t palette[256] = {0xffffff, 0};
static const lv_movie_t code = {.name = "FILM.DXR"};
static const dg_cast_t cast = {.name="Internal", .file=1, .cast=1};
static const dg_frame_t score[] = {{0, 0}};
static const dg_text_style_t style = {.font_name = "FilmFont", .font_id = 2,
    .size = 10, .line_height = 2, .color = 0xff0000};
static const dg_flash_field_t fields[] = {{.name = "prompt", .text = "A",
    .width = 8, .height = 8, .style = &style}};
static const dg_flash_field_t depth_fields[] = {{.name = "label", .text = "A",
    .y = 8, .width = 8, .height = 8, .style = &style}};
static const dg_film_layer_t flash_layers[] = {
  {.channel = 1, .name = "sun", .asset = "red.fdi", .origin_x = 4 * 65536,
   .origin_y = 4 * 65536, .value = {.type = 16, .x = 2, .y = 0, .width = 8,
    .height = 8, .ink = 8, .blend = 100, .fore = 1}},
  {.channel = 2, .name = "label", .origin_y = 8 * 65536, .value = {.type = 16}},
  {.channel = 3, .name = "cover", .asset = "green.fdi", .origin_x = 14 * 65536,
   .origin_y = 0, .value = {.type = 16, .x = 14, .width = 8, .height = 8,
    .ink = 8, .blend = 100, .fore = 1}},
  {.channel = 4, .name = "textCover", .asset = "green.fdi", .origin_x = 0,
   .origin_y = 8 * 65536, .value = {.type = 16, .y = 8, .width = 8, .height = 8,
    .ink = 8, .blend = 100, .fore = 1}},
};
static const dg_film_frame_t flash_frames[] = {{4, flash_layers}};
static const dg_flash_field_t affine_fields[] = {{.name = "skewed", .text = "A",
    .width = 8, .height = 8, .style = &style,
    .local_bounds = {32768, 32768, 8 * 65536, 8 * 65536}}};
static const dg_film_layer_t affine_layers[] = {
  {.channel = 1, .name = "skewed", .origin_x = 65536, .origin_y = 65536,
   .affine = true, .matrix = {2 * 65536, 0, 32768, 65536, 65536, 65536}, .value = {.type = 16}},
  {.channel = 1, .name = "skewed", .origin_x = 20 * 65536, .origin_y = 65536,
   .affine = true, .matrix = {0, 65536, -65536, 0, 20 * 65536, 65536}, .value = {.type = 16}},
};
static const dg_film_frame_t affine_frames[] = {{1, affine_layers}, {1, affine_layers+1}};
static const char *const poses[] = {"red.fdi", "green.fdi"};
#define SPEC(member_, x_, y_, w_, h_) \
  .member = DG_ID(1, 1, member_), .type = 16, .x = x_, .y = y_, \
  .width = w_, .height = h_, .stretch = 1, .fore = 1, .back = 0, .blend = 100
static const dg_film_layer_t layers[] = {
  {.channel = 1, .value = {SPEC(2, 10, 20, 4, 4)}},
  {.channel = 2, .film_frame = 1, .value = {SPEC(3, 16, 22, 8, 8)}},
  {.channel = 3, .value = {SPEC(4, 28, 30, 2, 2), .flags = 16, .fore_rgb = 0x00ff00}},
};
static const dg_film_layer_t tall_layers[] = {
  {.channel = 1, .value = {SPEC(4, 10, 683, 2, 2), .flags = 16, .fore_rgb = 0x00ff00}},
};
static const dg_film_frame_t frames[] = {{3, layers}};
static const dg_film_frame_t tall_frames[] = {{1, tall_layers}};
// Three sparse poses: text at authored (10,20), x changes to14, then hidden.
static const uint8_t track_data[] = {
  0,0,255,0, 2,0,33,0, 10,0,20,0,4,0,4,0,0,100,16,0,1,0,
  1,0,2,0,14,0, 2,0,64,0,0,0,
};
static const dg_film_track_t tracks[] = {{1,3,sizeof(track_data),track_data}};
static const dg_member_t members[] = {
  {.id = DG_ID(1,1,1), .number = 1, .cast = 1, .type = 2, .width = 20, .height = 12,
   .film_count = 1, .film_frames = frames, .film_left = 10, .film_top = 20},
  {.id = DG_ID(1,1,2), .number = 2, .cast = 1, .type = 3, .width = 4, .height = 4,
   .text = "AB", .text_style = &style},
  {.id = DG_ID(1,1,3), .number = 3, .cast = 1, .type = 2, .width = 8, .height = 8,
   .film_count = 2, .film_assets = poses, .film_loop = 1,
   .flash_field_count = 1, .flash_fields = fields},
  {.id = DG_ID(1,1,4), .number = 4, .cast = 1, .type = 8, .shape = 1, .filled = 1},
  {.id = DG_ID(1,1,5), .number = 5, .cast = 1, .type = 2, .width = 4, .height = 665,
   .film_count = 1, .film_frames = tall_frames, .film_left = 10, .film_top = 20},
  {.id = DG_ID(1,1,6), .number = 6, .cast = 1, .type = 2, .source_xtra = 1,
   .width = 24, .height = 16, .film_count = 1, .film_frames = flash_frames,
   .flash_field_count = 1, .flash_fields = depth_fields},
  {.id = DG_ID(1,1,7), .number = 7, .cast = 1, .type = 2, .source_xtra = 1,
   .width = 30, .height = 20, .film_count = 2, .film_frames = affine_frames,
   .flash_field_count = 1, .flash_fields = affine_fields},
  {.id = DG_ID(1,1,8), .number = 8, .cast = 1, .type = 2, .source_xtra = 1,
   .width = 8, .height = 8, .film_count = 2, .film_assets = poses, .flash_direct_to_stage = 1},
  {.id = DG_ID(1,1,9), .number = 9, .cast = 1, .type = 2,
   .width = 12, .height = 8, .film_count = 3, .film_left = 10, .film_top = 20,
   .film_track_count = 1, .film_tracks = tracks},
};
static const dg_movie_t movie = {.id = 1, .code = &code, .cast_count = 1, .casts = &cast,
    .member_count = 9, .members = members, .frame_count = 1, .frames = score,
    .palette = palette, .stage_color = 0x0000ff};
static bool text(void *ctx, const char *str, int w, int h, const wc_text_style_t *font,
                 uint8_t *coverage) {
  (void)ctx; (void)str;
  rendered_font = font->font;
  if (w && h) coverage[0] = 255;
  if (w > 1 && h) coverage[1] = 128;
  return true;
}
static uint32_t read_asset(void *ctx, const char *name, uint8_t *out, uint32_t cap) {
  (void)ctx;
  assert(!strcmp(name, "red.fdi") || !strcmp(name, "green.fdi"));
  if (out) {
    assert(cap >= 160); memset(out, 0, 160); memcpy(out, "FDI1", 4);
    out[7] = 160; out[9] = out[11] = 8; out[16] = 1;
    unsigned color = !strcmp(name, "red.fdi") ? 0xf801 : 0x07c1;
    for (unsigned i = 32; i < 160; i += 2) { out[i] = color >> 8; out[i+1] = color; }
  }
  return 160;
}
static dg_sprite_t *place(unsigned channel, unsigned member, int x, int y, int w, int h) {
  dg_sprite_t *s = &director.sprites[channel];
  *s = (dg_sprite_t){.value = {SPEC(member, x, y, w, h)}, .visible = true, .stretch = true};
  dg_sprite_changed(&director, channel); return s;
}
static unsigned pixel(unsigned x, unsigned y) { return compositor.pixels[y*WC_WIDTH+x] & 0xffffff; }
static void render(void) {
  wc_render(&compositor, &director, 0, 0, false, 0);
  if (values.failed) fprintf(stderr, "%s\n", values.error);
  assert(!values.failed);
}
int main(void) {
  wc_init(&compositor, (wc_host_t){.read = read_asset, .text = text}, 4096);
  dg_init(&director, &values, (dg_platform_t){0}, NULL, NULL, 0, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  dg_sprite_t *loop = place(1, 1, 40, 40, 40, 24);
  place(2, 5, 100, -100, 4, 665);
  render();
  assert(rendered_font == 2);
  assert(pixel(40,40) == 0xff); // text at the nonzero source origin
  assert(pixel(42,40) == 0x7f0080); // antialias coverage over the stage, no black fringe
  assert(pixel(44,42) == 0xff0000); // transparent film background
  assert(pixel(52,44) == 0xff00); // nested film's selected second pose
  assert(pixel(56,48) == 0xff); // nested Flash text respects source offset
  assert(pixel(76,60) == 0xff00 && pixel(80,60) == 0xff0000); // right clipping
  assert(pixel(100,563) == 0xff00 && pixel(100,565) == 0xff0000); // second source tile

  lv_set(&values, NULL, "text", lv_make(LV_MEMBER, DG_ID(1,1,2)), lv_text(&values, "changed", false));
  lv_set(&values, NULL, "color", lv_make(LV_MEMBER, DG_ID(1,1,2)), lv_make(LV_COLOR, 0xffff00));
  render(); assert(pixel(40,40) == 0xffff); // live member edits inside recorded loops
  loop->flip_h = true; director.visual_revision++; render();
  assert(pixel(79,40) == 0xffff && pixel(40,40) == 0xff0000);
  loop->flip_h = false; loop->value.blend = 50; loop->value.thickness = DG_HAS_BLEND;
  director.visual_revision++; render();
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
  assert(pixel(40,40) == 0xffff); // Native containers do not blend their children.
  // Original Birthday engine: native film children remain visible even when
  // the parent getter reports blend0. Keep child coverage and effects intact.
  for (unsigned ink = 0; ink <= 9; ink++) {
    loop->value.blend = 0;
    loop->value.ink = ink;
    loop->value.flags |= 48;
    loop->value.fore_rgb = 0xff0000;
    loop->value.back_rgb = 0;
    director.visual_revision++;
    render();
    assert(lv_numeric(lv_get(&values, NULL, "blend", lv_make(LV_SPRITE, 1))) == 0);
    assert(pixel(40,40) == 0xffff);
    assert(pixel(42,40) == 0x7f8080);
    assert(pixel(52,44) == 0xff00);
  }
  loop->value.ink = 0;
  loop->value.flags = 0;
  loop->value.blend = 100;
  lv_t child = lv_make(LV_SPRITE, DG_CHILD_SPRITE | (1 << DG_CHILD_SHIFT) | 2);
  lv_set(&values, NULL, "member", child, lv_make(LV_MEMBER, DG_ID(1,1,2)));
  lv_set(&values, NULL, "loc", child, lv_point(&values, 12, 22));
  render();
  assert(pixel(40,40) == 0xffff && pixel(44,44) == 0xffff); // untouched authored text survives
  assert(pixel(45,45) == 0xffff); // child loc and pixels follow parent scaling
  lv_set(&values, NULL, "blend", child, lv_num(50));
  render(); assert(pixel(44,44) == 0x807f7f);
  loop->flip_h = true; director.visual_revision++; render();
  assert(pixel(75,44) != 0xff0000 && pixel(44,44) == 0xff0000);
#else
  assert(pixel(40,40) == 0x807f7f);
#endif
  dg_sprite_t *flash = place(3, 6, 200, 100, 48, 32);
  render();
  assert(pixel(204,100) == 0xff && pixel(200,100) == 0xff0000);
  assert(pixel(204,120) == 0xff00); // foreground depth hides live text
  lv_t parent = lv_make(LV_SPRITE, 3);
  lv_t sun = lv_get(&values, NULL, "sun", parent);
  assert(lv_type(sun) == LV_PROPLIST);
  assert(lv_numeric(lv_get(&values, NULL, "_x", sun)) == 4);
  lv_set(&values, NULL, "_x", sun, lv_num(12));
  render();
  assert(pixel(204,100) == 0xff0000); // old child position cleared
  assert(pixel(220,100) == 0xff); // cropped left is registration minus 2
  assert(pixel(228,100) == 0xff00); // later depth still covers the moved sun
  lv_set(&values, NULL, "_visible", sun, lv_num(0));
  render(); assert(pixel(220,100) == 0xff0000);
  lv_set(&values, NULL, "_visible", sun, lv_num(1));
  lv_set(&values, NULL, "_alpha", sun, lv_num(50));
  render();
  assert(pixel(220,100) == 0x80007f); // layer opacity then parent composition
  lv_set(&values, NULL, "_alpha", sun, lv_num(100));
  lv_set(&values, NULL, "_width", sun, lv_num(4));
  render();
  assert(pixel(220,100) == 0xff0000 && pixel(222,100) == 0xff);
  flash->flip_h = true; director.visual_revision++; render();
  assert(pixel(225,100) == 0xff && pixel(227,100) == 0xff0000);
  flash->flip_h = false; director.visual_revision++;
  lv_t cover = lv_get(&values, NULL, "textCover", parent);
  lv_set(&values, NULL, "_visible", cover, lv_num(0));
  render(); assert(pixel(204,120) == 0xff); // live edit field at its actual depth
  lv_t label = lv_get(&values, NULL, "label", parent);
  lv_set(&values, NULL, "color", label, lv_make(LV_COLOR, 0xffff00));
  render(); assert(pixel(204,120) == 0xffff);
  lv_set(&values, NULL, "_alpha", label, lv_num(0));
  render(); assert(pixel(204,120) == 0xff0000);
  dg_sprite_t *affine = place(4, 7, 300, 200, 30, 20);
  render();
  assert(pixel(307,203) == 0xff && pixel(307,204) == 0xff0000);
  assert(pixel(302,202) == 0xff0000); // transparent local field plane
  lv_t affine_parent = lv_make(LV_SPRITE, 4);
  lv_t skewed = lv_get(&values, NULL, "skewed", affine_parent);
  render(); assert(pixel(307,203) == 0xff); // reading bag preserves authored matrix
  affine->film_frame = 1; director.visual_revision++; render();
  assert(pixel(317,203) == 0xff && pixel(307,203) == 0xff0000); // next pose rotates text
  lv_set(&values, NULL, "_x", skewed, lv_num(10));
  render(); assert(pixel(307,203) == 0xff && pixel(317,203) == 0xff0000);
  lv_set(&values, NULL, "_xscale", skewed, lv_num(200));
  render(); assert(pixel(307,206) == 0xff && pixel(307,203) == 0xff0000);
  lv_set(&values, NULL, "_width", skewed, lv_num(6));
  render(); assert(pixel(307,206) == 0xff); // reflow leaves glyph scale independent
  lv_set(&values, NULL, "_rotation", skewed, lv_num(0));
  render(); assert(pixel(315,203) == 0xff && pixel(307,206) == 0xff0000);
  affine->flip_h = true; director.visual_revision++; render();
  assert(pixel(314,203) == 0xff && pixel(315,203) == 0xff0000);
  lv_set(&values, NULL, "_height", skewed, lv_num(2));
  render(); assert(pixel(314,203) == 0xff0000); // field clip before transform
  dg_sprite_t *dts=place(5,8,400,100,8,8);
  dts->value.blend=0; dts->value.thickness|=DG_HAS_BLEND; dts->value.ink=2;
  dg_sprite_t *later=place(6,4,400,100,10,10);
  later->value.flags=16; later->value.fore_rgb=0x00ff00;
  render();
  assert(pixel(400,100)==0xff); // Direct To Stage ignores sprite ink/blend and draws above later channels.
  assert(dg_hit(&director,400,100)==5); // Visible ordering and input ordering agree.
  assert(pixel(409,109)==0xff00); // Its picture does not hide unrelated stage pixels.
  dts->visible=false; director.visual_revision++; render();
  assert(pixel(400,100)==0xff00);
  dg_sprite_t *tracked = place(7,9,500,200,24,16);
  place(8,9,500,230,24,16);
  render(); assert(pixel(500,200)==0xffff && pixel(500,230)==0xffff);
  tracked->film_frame=1; director.visual_revision++; render();
  assert(pixel(500,200)==0xff0000 && pixel(508,200)==0xffff);
  assert(pixel(500,230)==0xffff); // another instance retains its own pose
  lv_t track_child=lv_make(LV_SPRITE,DG_CHILD_SPRITE|(7<<DG_CHILD_SHIFT)|1);
  lv_set(&values,NULL,"loc",track_child,lv_point(&values,16,24));
  render(); assert(pixel(512,208)==0xffff && pixel(508,200)==0xff0000);
  tracked->film_frame=2; director.visual_revision++; render();
  assert(pixel(512,208)==0xff0000 && pixel(500,230)==0xffff);
  wc_release(&compositor); dg_films_dispose(&director); dg_images_dispose(&director);
  puts("runtime film pixels: text, coverage, nested pose/Flash text, transforms, live edits, tall source tiles passed");
}
