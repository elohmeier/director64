// Browser backend compositor contracts: stored image decoding under each
// supported ink, the matte hit test, and composited sprites against the
// console's draw rules (white D6 stage, copy/matte/background-transparent
// inks, blend, stretching, shapes, draw order and the pointer).
#include "compositor.h"
#include "ink.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_runtime_t values;
static dg_runtime_t director;
static wc_t compositor;
static const char *const names[] = {"unused"};

static void be16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
// FDI1 with the given RGBA5551 words (row-major, or tile order when wide).
static uint8_t *fdi1(unsigned width, unsigned height, const uint16_t *words,
                     uint32_t *length) {
  uint64_t pixels = bitmap_plane_pixels(width, height);
  *length = (uint32_t)(32 + pixels * 2);
  uint8_t *data = calloc(1, *length);
  memcpy(data, "FDI1", 4);
  be32(data + 4, *length);
  be16(data + 8, width);
  be16(data + 10, height);
  for (uint64_t i = 0; words && i < (uint64_t)width * height; i++)
    be16(data + 32 + i * 2, words[i]);
  return data;
}
enum { RED = 0xf801, GREEN = 0x07c1, WHITE = 0xffff, WHITE_CLEAR = 0xfffe,
       BLUE_CLEAR = 0x003e };
static const uint16_t fixture[8] = {RED, GREEN, WHITE, BLUE_CLEAR,
                                    WHITE_CLEAR, RED, GREEN, WHITE};
static uint8_t *fixture_data;
static uint32_t fixture_length;
static unsigned reads;

static uint32_t read_asset(void *ctx, const char *name, uint8_t *out, uint32_t cap) {
  (void)ctx;
  if (strcmp(name, "fixture.fdi"))
    return 0;
  if (out) {
    assert(cap >= fixture_length);
    memcpy(out, fixture_data, fixture_length);
    reads++;
  }
  return fixture_length;
}
static bool text(void *ctx, const char *utf8, int width, int height,
                 const wc_text_style_t *style, uint8_t *coverage) {
  (void)style;
  (void)ctx;
  (void)height;
  // A one-pixel "glyph" per character on the first row.
  for (int i = 0; utf8[i] && i < width; i++)
    coverage[i] = 255;
  return true;
}

static uint32_t rgba(unsigned r, unsigned g, unsigned b) {
  return r | g << 8 | b << 16 | 0xff000000u;
}
static uint32_t pixel(int x, int y) { return compositor.pixels[y * WC_WIDTH + x]; }

static void decode_contracts(void) {
  uint32_t out[8];
  bool follow;
  // Copy ink makes every pixel opaque; matte keeps the stored coverage bit;
  // background transparent clears exactly the white pixels.
  assert(wc_decode(fixture_data, fixture_length, 4, 2, 0, out, &follow) && !follow);
  assert(out[0] == rgba(255, 0, 0) && out[1] == rgba(0, 255, 0));
  assert(out[3] >> 24 == 255 && out[4] >> 24 == 255);
  assert(wc_decode(fixture_data, fixture_length, 4, 2, 8, out, &follow));
  assert(out[3] >> 24 == 0 && out[4] >> 24 == 0 && out[2] >> 24 == 255);
  assert(wc_decode(fixture_data, fixture_length, 4, 2, 36, out, &follow));
  assert(out[2] >> 24 == 0 && out[7] >> 24 == 0 && out[0] >> 24 == 255);
  // Dimensions and lengths are checked before a pixel is read.
  assert(!wc_decode(fixture_data, fixture_length, 3, 2, 0, out, &follow));
  assert(!wc_decode(fixture_data, fixture_length - 1, 4, 2, 0, out, &follow));
  uint8_t bad[64];
  memcpy(bad, fixture_data, fixture_length);
  memcpy(bad, "FDIX", 4);
  assert(!wc_decode(bad, fixture_length, 4, 2, 0, out, &follow));
  // Images past the texture limit are stored tile-packed; decoding returns
  // them linear.
  uint32_t length;
  uint8_t *wide = fdi1(1056, 2, NULL, &length);
  be16(wide + 32 + bitmap_tile_index(1056, 1040, 1) * 2, RED);
  uint32_t *linear = malloc(1056 * 2 * 4);
  assert(wc_decode(wide, length, 1056, 2, 0, linear, &follow));
  assert(linear[1056 + 1040] == rgba(255, 0, 0) && linear[1040] == rgba(0, 0, 0));
  free(linear);
  free(wide);
  // FDIA: RGB555 plus an A8 plane; matte-ink white clears only under ink 36.
  uint32_t fdia_offset, fdia = dg_fdia_size(2, 1, &fdia_offset);
  uint8_t planes[64] = {0};
  memcpy(planes, "FDIA", 4);
  be32(planes + 4, fdia);
  be16(planes + 8, 2);
  be16(planes + 10, 1);
  planes[16] = 1;
  be32(planes + 20, fdia_offset);
  be16(planes + 32, WHITE_CLEAR);
  be16(planes + 34, RED & 0xfffe);
  planes[fdia_offset] = 200;
  planes[fdia_offset + 1] = 100;
  assert(wc_decode(planes, fdia, 2, 1, 0, out, &follow));
  assert(out[0] >> 24 == 200 && out[1] >> 24 == 100);
  assert(wc_decode(planes, fdia, 2, 1, 36, out, &follow));
  assert(out[0] >> 24 == 0 && out[1] >> 24 == 100);
}

static const lv_movie_t code = {"SYNTHETIC.DXR", 0, NULL, NULL, NULL, NULL, NULL, 0};
static const dg_member_t members[] = {
    {.id = 0x110001, .number = 1, .cast = 1, .type = 1, .width = 4, .height = 2,
     .asset = "fixture.fdi"},
    {.id = 0x110002, .number = 2, .cast = 1, .type = 8,
     .shape = 1, .filled = 1},
    {.id = 0x110003, .number = 3, .cast = 1, .type = 3, .text = "Hi"},
    {.id = 0x110004, .number = 4, .cast = 1, .type = 1, .width = 4, .height = 2,
     .asset = "missing.fdi"}};
static const dg_frame_t frames[] = {{0, 0}};
static uint32_t palette[256];
static const dg_cast_t casts[] = {{"Internal", 1, 1}};
static dg_movie_t movie = {.code = &code, .id = 1, .tempo = 30, .cast_count = 1,
                           .member_count = 4, .frame_count = 1, .casts = casts,
                           .members = members, .frames = frames, .palette = palette};

static void place(unsigned channel, uint32_t member, int x, int y, int width,
                  int height, unsigned ink, unsigned blend) {
  director.sprites[channel] = (dg_sprite_t){
      .value = {.member = member, .x = (int16_t)x, .y = (int16_t)y,
                .width = (int16_t)width, .height = (int16_t)height, .type = 16,
                .ink = (uint8_t)ink, .blend = (uint8_t)blend, .fore = 6,
                .thickness = blend < 100 ? DG_HAS_BLEND : 0},
      .visible = true, .stretch = true};
  dg_sprite_changed(&director, channel);
}

static void render_contracts(void) {
  palette[6] = 0x0000ff; // the shape's foreground index
  wc_init(&compositor, (wc_host_t){.read = read_asset, .text = text}, 1024);
  dg_init(&director, &values, (dg_platform_t){0}, NULL, names, 1, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  // Channel order is draw order: the shape under three bitmaps.
  place(1, 0x110002, 0, 0, 40, 20, 0, 100);
  place(2, 0x110001, 0, 0, 4, 2, 0, 100);   // copy
  place(3, 0x110001, 8, 0, 4, 2, 8, 100);   // matte
  place(4, 0x110001, 16, 0, 8, 4, 36, 100); // background transparent, doubled
  place(5, 0x110001, 30, 0, 4, 2, 0, 50);   // blended copy
  place(6, 0x110003, 50, 50, 20, 10, 0, 100);
  assert(wc_render(&compositor, &director, 600, 400, false, 0));
  assert(!values.failed);
  // The D6 stage is white; the shape fills its rectangle with its palette colour.
  assert(pixel(100, 100) == 0xffffffffu && pixel(39, 19) == rgba(0, 0, 255));
  assert(pixel(0, 0) == rgba(255, 0, 0) && pixel(3, 0) == rgba(0, 0, 255));
  // Matte: the cleared pixels show the shape beneath.
  assert(pixel(8, 0) == rgba(255, 0, 0) && pixel(11, 0) == rgba(0, 0, 255));
  assert(pixel(8, 1) == rgba(0, 0, 255));
  // Background transparent, scaled 2x: white shows the shape, colours draw.
  assert(pixel(16, 0) == rgba(255, 0, 0) && pixel(17, 1) == rgba(255, 0, 0));
  assert(pixel(20, 0) == rgba(0, 0, 255) && pixel(20, 2) == rgba(0, 255, 0) && pixel(22, 2) == rgba(0, 0, 255));
  // Half blend of red over blue.
  uint32_t mixed = pixel(30, 0);
  assert((mixed & 255) >= 126 && (mixed & 255) <= 129 && ((mixed >> 16) & 255) >= 126);
  // Text draws in the sprite's foreground over its box.
  assert(pixel(50, 50) == rgba(0, 0, 255) && pixel(52, 50) == 0xffffffffu);
  // An unchanged stage is not recomposited; a moved pointer is.
  unsigned before = reads;
  assert(!wc_render(&compositor, &director, 600, 400, false, 0));
  assert(wc_render(&compositor, &director, 300, 300, true, 0));
  assert(reads == before); // images came from the cache
  assert(pixel(300, 300) == rgba(0, 0, 0)); // the arrow's tip
  // Matte hits answer from coverage; other inks from the rectangle.
  assert(wc_hit(&compositor, &values, &members[0], 8, 0, 0));
  assert(!wc_hit(&compositor, &values, &members[0], 8, 3, 0));
  assert(wc_hit(&compositor, &values, &members[0], 0, 3, 0));
  assert(!wc_hit(&compositor, &values, &members[0], 8, 4, 0));
  // Over budget, the least recently used image goes, never the frame's own.
  assert(compositor.image_bytes <= 1024);
  // A missing asset fails the runtime as the console does.
  place(7, 0x110004, 100, 100, 4, 2, 0, 100);
  assert(!wc_render(&compositor, &director, 300, 300, true, 0) || values.failed);
  assert(values.failed && strstr(values.error, "missing native image file"));
  wc_release(&compositor);
  assert(!compositor.image_bytes);
}

int main(void) {
  fixture_data = fdi1(4, 2, fixture, &fixture_length);
  decode_contracts();
  render_contracts();
  free(fixture_data);
  return 0;
}
