// Recovered rich text retains readable text and uses its authored pixel surface.
#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static lv_runtime_t values;
static dg_runtime_t director;
static unsigned hits;
static const lv_movie_t code = {.name = "RICH.DXR"};
static const dg_cast_t casts[] = {{.name="Internal",.file=1,.cast=1}};
static const dg_frame_t frames[] = {{0, 0}};
static const dg_member_t members[] = {
  {.id = DG_ID(1,1,1), .number = 1, .cast = 1, .type = 12,
   .name = "label", .width = 2, .height = 1, .text = "Authored label"},
};
static const dg_movie_t movie = {.code = &code, .id = 1, .tempo = 30,
  .cast_count = 1, .member_count = 1, .frame_count = 1,
  .casts = casts, .members = members, .frames = frames};
static bool hit(void *ctx, const dg_member_t *m, unsigned ink, int x, int y) {
  (void)ctx; assert(m == &members[0]); assert(ink == 8); assert(y == 0);
  hits++; return x == 0;
}
int main(void) {
  dg_init(&director, &values, (dg_platform_t){.hit = hit}, NULL, NULL, 0, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  lv_t member = lv_make(LV_MEMBER, members[0].id);
  assert(!strcmp(lv_cstr(&values, lv_get(&values, NULL, "text", member)), "Authored label"));
  director.sprites[1] = (dg_sprite_t){.value = {.member = members[0].id,
    .type = 16, .x = 10, .y = 10, .width = 2, .height = 1, .ink = 8},
    .visible = true, .stretch = true};
  dg_sprite_changed(&director, 1);
  assert(dg_hit(&director, 10, 10) == 1);
  assert(dg_hit(&director, 11, 10) == 0);
  assert(hits == 2);
  lv_set(&values, NULL, "text", member, lv_text(&values, "replacement", false));
  assert(values.failed && strstr(values.error, "rich text editing"));
  assert(!strcmp(members[0].text, "Authored label"));
  puts("rich text read/hit/edit contracts passed");
}
