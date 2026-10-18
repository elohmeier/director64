#include "director.h"
#include <assert.h>
#include <stdio.h>

static dg_runtime_t d;
static lv_runtime_t r;
static unsigned sprite_calls, frame_calls, movie_calls;
static bool sprite_pass, frame_pass;
static lv_flow_t sprite_key(lv_runtime_t *v, lv_frame_t *f) {
  if (!f->pc++)
    return LV_YIELD;
  sprite_calls++;
  if (sprite_pass)
    lv_call(v, f, "pass", 0, NULL);
  return LV_RETURN;
}
static lv_flow_t frame_key(lv_runtime_t *v, lv_frame_t *f) {
  if (!f->pc++)
    return LV_YIELD;
  frame_calls++;
  if (frame_pass)
    lv_call(v, f, "pass", 0, NULL);
  return LV_RETURN;
}
static lv_flow_t movie_key(lv_runtime_t *v, lv_frame_t *f) {
  (void)v;
  (void)f;
  movie_calls++;
  return LV_RETURN;
}
#define HANDLER(event, member_, kind_, step_)                                  \
  {.name = event,                                                              \
   .member = member_,                                                          \
   .kind = kind_,                                                              \
   .cast = "Internal",                                                         \
   .arguments = 1,                                                             \
   .locals = 1,                                                                \
   .step = step_}
static const lv_handler_t handlers[] = {
    HANDLER("keydown", 1, "ScoreScript", sprite_key),
    HANDLER("keyup", 1, "ScoreScript", sprite_key),
    HANDLER("keydown", 2, "ScoreScript", frame_key),
    HANDLER("keyup", 2, "ScoreScript", frame_key),
    HANDLER("keydown", 3, "MovieScript", movie_key),
    HANDLER("keyup", 3, "MovieScript", movie_key),
};
static const lv_movie_t code = {
    .name = "KEYS.DXR", .count = 6, .handlers = handlers};
static const dg_cast_t cast = {.name="Internal",.file=1,.cast=1};
static const dg_member_t members[] = {
    {.id = DG_ID(1, 1, 1), .cast = 1, .number = 1, .type = 11},
    {.id = DG_ID(1, 1, 2), .cast = 1, .number = 2, .type = 11},
    {.id = DG_ID(1, 1, 3), .cast = 1, .number = 3, .type = 11},
    {.id = DG_ID(1, 1, 4),
     .cast = 1,
     .number = 4,
     .type = 3,
     .text = "",
     .width = 20,
     .height = 20,
     .editable = true},
};
static const dg_behavior_t behavior = {DG_ID(1, 1, 1), ""};
static const dg_delta_t deltas[] = {
    {.channel = 0, .mask = DG_ALL, .value = {.script = DG_ID(1, 1, 2)}},
    {.channel = 6,
     .mask = DG_ALL,
     .value = {.type = 1,
               .member = DG_ID(1, 1, 4),
               .width = 20,
               .height = 20,
               .behaviors = &behavior,
               .behavior_count = 1}},
};
static const dg_frame_t frames[] = {{0, 2}};
static const dg_movie_t movie = {.id = 1,
                                 .code = &code,
                                 .casts = &cast,
                                 .cast_count = 1,
                                 .members = members,
                                 .member_count = 4,
                                 .tempo = 1,
                                 .frames = frames,
                                 .frame_count = 1,
                                 .deltas = deltas};
static void tick(void) {
  assert(dg_tick(&d, 100, 100, false, DG_SERVICE_BUDGET));
  assert(!r.failed);
}
static void boot(bool focused) {
  dg_init(&d, &r, (dg_platform_t){0}, NULL, NULL, 0, 1);
  assert(dg_enter(&d, &movie, 1, NULL, 0));
  d.keyboard_focus = focused ? 1 : 0;
  sprite_calls = frame_calls = movie_calls = 0;
  tick();
}
static void key(bool down) {
  dg_key(&d, 0, 'a', down);
  for (unsigned i = 0; i < 8; i++) {
    tick();
    lv_collect(&r);
  }
}
int main(void) {
  sprite_pass = frame_pass = true;
  boot(true);
  key(true);
  key(false);
  assert(sprite_calls == 2 && frame_calls == 2 && movie_calls == 2);
  sprite_pass = false;
  boot(true);
  key(true);
  key(false);
  assert(sprite_calls == 2 && !frame_calls && !movie_calls);
  sprite_pass = true;
  frame_pass = false;
  boot(true);
  key(true);
  key(false);
  assert(sprite_calls == 2 && frame_calls == 2 && !movie_calls);
  frame_pass = true;
  boot(false);
  key(true);
  key(false);
  assert(!sprite_calls && frame_calls == 2 && movie_calls == 2);
  // A focus left behind by the departing movie cannot address the next one.
  d.keyboard_focus = 1;
  assert(dg_enter(&d, &movie, 1, NULL, 0) && !d.keyboard_focus);
  puts("keyboard: focused text, yielding pass/consume hierarchy, release and "
       "movie reset passed");
}
