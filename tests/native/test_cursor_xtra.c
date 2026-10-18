#include "director.h"
#include <assert.h>
#include <stdio.h>

static dg_runtime_t director;
static lv_runtime_t values;
static const lv_movie_t code = {.name = "CURSOR.DXR"};
static const dg_cast_t cast = {.name="Internal",.file=1,.cast=1};
static const dg_member_t member = {.id = 0x210001,
                                   .number = 1,
                                   .cast = 1,
                                   .type = 1,
                                   .source_xtra = 5,
                                   .width = 16,
                                   .height = 16,
                                   .reg_x = 7,
                                   .name = "color cursor",
                                   .asset = "synthetic.fdi"};
static const dg_frame_t frame = {0};
static const dg_movie_t movie = {.code = &code,
                                 .id = 1,
                                 .tempo = 30,
                                 .cast_count = 1,
                                 .casts = &cast,
                                 .member_count = 1,
                                 .members = &member,
                                 .frame_count = 1,
                                 .frames = &frame};
int main(void) {
  dg_init(&director, &values, (dg_platform_t){0}, NULL, NULL, 0, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  lv_t reference = lv_make(LV_MEMBER, (int32_t)member.id), result = 0;
  bool yield = false;
  assert(
      values.services.call(&values, "cursor", 1, &reference, &result, &yield));
  assert(!yield && !values.failed && director.cursor.image == member.id);
  assert(lv_get(&values, NULL, "cursor", 0) == reference);
  assert(!strcmp(lv_cstr(&values, lv_get(&values, NULL, "type", reference)),
                 "cursor"));
  uint16_t source[256] = {0}, pixels[256];
  source[0] = dg_cursor_ink(0, 255, 0);
  source[1] = 1;      // Black body.
  source[2] = 0xfffe; // White with converted Auto Mask alpha cleared.
  dg_cursor_bitmap_t cursor;
  assert(dg_cursor_color(&cursor, source, 16, 16, 7, 0));
  uint32_t hash = dg_cursor_bitmap_hash(&cursor);
  dg_cursor_pixels_ink(&cursor, pixels, dg_cursor_ink(255, 0, 0));
  assert(pixels[0] == source[0] && pixels[1] == 1 && !(pixels[2] & 1));
  assert(cursor.hot_x == 7 && cursor.hot_y == 0);
  source[0] = dg_cursor_ink(0, 0, 255);
  assert(dg_cursor_color(&cursor, source, 16, 16, 7, 0));
  assert(dg_cursor_bitmap_hash(&cursor) != hash);
  assert(!dg_cursor_color(&cursor, source, 17, 16, 7, 0));
  puts("Cursor Xtra color and reference: OK");
  return 0;
}
