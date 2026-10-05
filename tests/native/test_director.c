#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static lv_runtime_t values;
static dg_runtime_t director;
static const char *const names[] = {"observed"};
static lv_flow_t primary(lv_runtime_t *r, lv_frame_t *f) {
  if (!f->pc++) {
    r->globals[0] = lv_num(7);
    return LV_YIELD;
  }
  r->globals[0] = lv_num(10);
  return LV_RETURN;
}
static lv_flow_t clicked(lv_runtime_t *r, lv_frame_t *f) {
  (void)f;
  assert(lv_number(r, r->globals[0]) == 10);
  r->globals[0] = lv_num(11);
  return LV_RETURN;
}
static lv_flow_t callback(lv_runtime_t *r, lv_frame_t *f) {
  (void)f;
  r->globals[0] = lv_num(12);
  return LV_RETURN;
}
static lv_flow_t external(lv_runtime_t *r, lv_frame_t *f) {
  if (!f->pc++)
    return lv_invoke(r, f, "callback", 0, NULL);
  return LV_RETURN;
}
static const lv_handler_t external_handler = {.name = "mouseup",
                                              .member = 5,
                                              .cast = "External",
                                              .kind = "BehaviorScript",
                                              .step = external};
static const lv_movie_t external_code = {"EXTERNAL.CXT", 1, &external_handler,NULL, NULL, NULL, NULL, 0};
static const dg_cast_t external_cast = {"External", 2, 1};
static const dg_movie_t external_movie = {
    .code = &external_code, .id = 2, .cast_count = 1, .casts = &external_cast};
static lv_flow_t jumping(lv_runtime_t *r, lv_frame_t *f) {
  if (!f->pc) {
    f->pc = 1;
    return lv_invoke(r, f, "go", 1, (lv_t[]){lv_num(2)});
  }
  r->globals[0] = lv_get(r, f, "frame", (lv_t){0});
  return LV_RETURN;
}
static const lv_handler_t handlers[] = {
    {.name = "exitframe",
     .member = 10,
     .cast = "Internal",
     .kind = "BehaviorScript",
     .step = jumping},
    {.name = "exitframe",
     .member = 10,
     .cast = "Other",
     .kind = "BehaviorScript",
     .entry = 1,
     .step = jumping},
    {.name = "primary", .kind = "MovieScript", .step = primary},
    {.name = "mouseup",
     .member = 20,
     .cast = "Internal",
     .kind = "BehaviorScript",
     .step = clicked},
    {.name = "callback", .kind = "MovieScript", .step = callback}};
static const lv_movie_t code = {"SYNTHETIC.DXR", 5, handlers,NULL, NULL, NULL, NULL, 0};
static const dg_cast_t casts[] = {{"Internal", 1, 1}, {"Other", 1, 2}};
static const dg_member_t members[] = {{.id = 0x110001,
                                       .number = 1,
                                       .cast = 1,
                                       .type = 1,
                                       .width = 10,
                                       .height = 10,
                                       .reg_x = 5,
                                       .reg_y = 5},
                                      {.id = 0x110002,
                                       .number = 2,
                                       .cast = 1,
                                       .type = 8,
                                       .width = 100,
                                       .height = 40}};
static const dg_frame_t frames[] = {{0, 0}, {0, 0}};
static const dg_movie_t movie = {.code = &code,
                                 .id = 1,
                                 .tempo = 30,
                                 .cast_count = 2,
                                 .member_count = 2,
                                 .frame_count = 2,
                                 .casts = casts,
                                 .members = members,
                                 .frames = frames};
static bool hit(void *ctx, const dg_member_t *m, unsigned ink, int x, int y) {
  (void)ctx;
  (void)m;
  (void)ink;
  (void)y;
  return x >= 5;
}
int main(void) {
  dg_init(&director, &values, (dg_platform_t){.hit = hit}, NULL, names, 1, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  assert(dg_event(&director, "exitframe", 0x11000a));
  assert(lv_run(&values, 100) && values.yielded && director.next_frame == 2);
  assert(director.frame == 1 && values.depth == 1);
  assert(dg_tick(&director, 0, 0, false, 100));
  assert(lv_number(&values, values.globals[0]) == 2);
  // Equal member numbers in different casts must dispatch independently.
  assert(dg_event(&director, "exitframe", 0x12000a));
  assert(values.frames[values.depth - 1].pc == 1);
  assert(lv_run(&values, 100));
  director.sprites[1] = (dg_sprite_t){.value = {.member = 0x110001,
                                                .x = 15,
                                                .y = 15,
                                                .width = 10,
                                                .height = 10,
                                                .type = 16,
                                                .ink = 8,
                                                .blend = 100},
                                      .visible = true,
                                      .trails = true};
  dg_sprite_changed(&director, 1);
  dg_update_stage(&director);
  assert(dg_hit(&director, 11, 15) == 0 && dg_hit(&director, 16, 15) == 1);
  director.sprites[1].value.x = 25;
  dg_sprite_changed(&director, 1);
  dg_update_stage(&director);
  assert(director.trail_count == 1 && director.trails[0].sprite.value.x == 15);
  dg_update_stage(&director);
  assert(director.trail_count == 1);
  director.sprites[2] = (dg_sprite_t){.value = {.member = 0x110002,
                                                .width = 100,
                                                .height = 40,
                                                .type = 16,
                                                .blend = 100},
                                      .visible = true};
  dg_sprite_changed(&director, 2);
  dg_update_stage(&director);
  assert(!director.trail_count);
  director.resume_tick = director.ticks + 100;
  unsigned frame = director.frame;
  assert(dg_tick(&director, 10, 10, true, 100) && director.frame == frame);
  dg_update_stage(&director);
  director.resume_tick = director.ticks;
  // An external behavior dispatches exactly, then calls the active movie.
  values.depth = 0;
  assert(dg_enter(&director, &movie, 1, (const dg_movie_t *[]){&external_movie},
                  1));
  assert(dg_event(&director, "mouseup", 0x210005));
  assert(lv_run(&values, 100));
  assert(lv_number(&values, values.globals[0]) == 12);
  // Primary mouseUp handlers may yield before passing to the sprite handler.
  director.await_release = false;
  director.sprites[1] = (dg_sprite_t){.value = {.member = 0x110001,
                                                .script = 0x110014,
                                                .x = 15,
                                                .y = 15,
                                                .type = 16,
                                                .blend = 100},
                                      .visible = true,
                                      .puppet = true};
  strcpy(director.mouse_up_script, "primary");
  director.phase = 0;
  director.mouse_down = true;
  assert(dg_tick(&director, 16, 15, false, 100));
  assert(values.depth && director.mouse_up_dispatch);
  assert(lv_number(&values, values.globals[0]) == 7);
  assert(dg_tick(&director, 16, 15, false, 100));
  assert(lv_number(&values, values.globals[0]) == 11);
  // Movement preserves the grab offset, including the final release position.
  director.sprites[1].moveable = true;
  director.resume_tick = director.ticks + 100;
  assert(dg_tick(&director, 16, 15, true, 100));
  assert(dg_tick(&director, 56, 55, true, 100));
  assert(director.sprites[1].value.x == 55 &&
         director.sprites[1].value.y == 55);
  assert(dg_tick(&director, 66, 65, false, 100));
  assert(!director.drag_sprite && director.sprites[1].value.x == 65);
  // Held route presses are consumed until the physical button is released.
  director.mouse_down = true;
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  director.resume_tick = director.ticks + 100;
  assert(dg_tick(&director, 16, 15, true, 100));
  assert(director.await_release && !director.mouse_down &&
         !director.mouse_pressed);
  assert(dg_tick(&director, 16, 15, false, 100));
  assert(!director.await_release && !director.mouse_released);
  assert(dg_tick(&director, 16, 15, true, 100));
  assert(director.mouse_pressed);
  assert(!values.failed);
  puts("Director: frame continuation, exact cast dispatch, matte hits, sprite "
       "trails, timer waits OK");
}
