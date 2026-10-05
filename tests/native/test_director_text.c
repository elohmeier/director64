#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static lv_runtime_t values;
static dg_runtime_t director;
static const lv_movie_t code = {.name = "TEXT.DXR"};
static const dg_cast_t casts[] = {{"Internal", 1, 1}};
static const dg_text_style_t initial = {.font_name = "Pettson *", .font_id = 1,
    .size = 12, .ascent = 11, .descent = 6, .line_height = 17};
static const dg_text_style_t insertion = {.font_name = "Pettson *", .font_id = 1,
    .size = 15, .ascent = 14, .descent = 7, .line_height = 21, .align = 1};
static const dg_member_t members[] = {{.id = 0x110001, .number = 1, .cast = 1,
    .type = 3, .name = "namn", .text = "Findus", .text_style = &initial,
    .text_insert_style = &insertion}};
static const dg_frame_t frames[] = {{0, 0}};
static const dg_movie_t movie = {.code = &code, .id = 1, .tempo = 30,
    .cast_count = 1, .casts = casts, .member_count = 1, .members = members,
    .frame_count = 1, .frames = frames};

int main(void) {
  dg_init(&director, &values, (dg_platform_t){0}, NULL, NULL, 0, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  assert(lv_numeric(lv_get(&values,NULL,"membernum",lv_make(LV_MEMBER, 0x110001))) == 1);
  unsigned revision = director.visual_revision;
  lv_t member = lv_make(LV_MEMBER, 0x110001);
  assert(!strcmp(lv_cstr(&values, lv_get(&values, NULL, "font", member)),
                 "Pettson *"));
  assert(director.field_count == 1 && !director.field_text_changed[0]);
  assert(!strcmp(lv_cstr(&values, lv_get(&values, NULL, "text", member)),
                 "Findus"));
  assert(!director.field_text_changed[0]);
  assert(director.visual_revision == revision); // Reads must not redraw.
  lv_set(&values, NULL, "font", member, lv_text(&values, "Pettson", false));
  assert(!strcmp(lv_cstr(&values, lv_get(&values, NULL, "font", member)),
                 "Pettson"));
  assert(!director.field_text_changed[0]);
  lv_set(&values, NULL, "text", member, lv_text(&values, "Muckla\rFindus", false));
  assert(director.field_text_changed[0]);
  assert(!strcmp(lv_cstr(&values, lv_get(&values, NULL, "text", member)),
                 "Muckla\rFindus"));
  assert(director.visual_revision > revision);
  assert(!values.failed);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  assert(director.field_count == 0 && !director.field_text_changed[0]);
  assert(!strcmp(lv_cstr(&values, lv_get(&values, NULL, "font", member)),
                 "Pettson *"));
  assert(!strcmp(lv_cstr(&values, lv_get(&values, NULL, "text", member)),
                 "Findus"));
  assert(!values.failed && !director.field_text_changed[0]);
  puts("Director authored text and mutation contracts passed");
}
