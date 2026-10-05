#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static dg_runtime_t director;
static lv_runtime_t values;
static char order[32];
static unsigned count, restored;
static const char *const local_names[] = {"me"};
static lv_flow_t global(lv_runtime_t *r, lv_frame_t *f) {
  (void)r;
  order[count++] = !strcmp(f->handler->name, "prepareframe") ? 'G' : 'I';
  return LV_RETURN;
}
static lv_flow_t activate_later(lv_runtime_t *r, lv_frame_t *f) {
  order[count++] = 'E';
  lv_set(r, f, "scriptinstancelist", lv_make(LV_SPRITE, 31),
         (lv_t){0});
  lv_set(r, f, "member", lv_make(LV_SPRITE, 32), lv_num(0));
  lv_set(r, f, "member", lv_make(LV_SPRITE, 799),
         lv_make(LV_MEMBER, 0x110003));
  lv_t instance = lv_instance(r, lv_make(LV_SCRIPT, 0x110002));
  lv_set(r, f, "scriptinstancelist", lv_make(LV_SPRITE, 800),
         lv_list(r, 1, &instance, false));
  return LV_RETURN;
}
static lv_flow_t later(lv_runtime_t *r, lv_frame_t *f) {
  (void)r;
  assert(director.current_event_sprite == 800);
  assert(lv_type(f->locals[0]) == LV_INSTANCE);
  order[count++] = 'L';
  return LV_RETURN;
}
static lv_flow_t cast(lv_runtime_t *r, lv_frame_t *f) {
  (void)r;
  (void)f;
  order[count++] = 'C';
  return LV_RETURN;
}
static lv_flow_t loaded_event(lv_runtime_t *r, lv_frame_t *f) {
  (void)r;
  (void)f;
  restored++;
  return LV_RETURN;
}
static lv_flow_t lifecycle(lv_runtime_t *r, lv_frame_t *f) {
  (void)r;
  order[count++] = !strcmp(f->handler->name, "beginsprite") ? 'B' : 'P';
  return LV_RETURN;
}
static unsigned visited[16], visits;
static lv_flow_t boundary(lv_runtime_t *r, lv_frame_t *f) {
  (void)r;
  (void)f;
  assert(visits < sizeof(visited) / sizeof(visited[0]));
  visited[visits++] = director.current_event_sprite;
  return LV_RETURN;
}
static bool eligible(unsigned i) {
  return (director.event_channels[i >> 5] & (UINT32_C(1) << (i & 31))) != 0;
}
static const lv_handler_t handlers[] = {
    {.name = "prepareframe", .member = 4, .cast = "Internal",
     .kind = "MovieScript", .step = global},
    {.name = "enterframe", .member = 4, .cast = "Internal",
     .kind = "MovieScript", .step = global},
    {.name = "prepareframe", .member = 1, .cast = "Internal",
     .kind = "BehaviorScript", .arguments = 1, .locals = 1,
     .step = activate_later, .local_names = local_names},
    {.name = "prepareframe", .member = 2, .cast = "Internal",
     .kind = "BehaviorScript", .arguments = 1, .locals = 1,
     .step = later, .local_names = local_names},
    {.name = "prepareframe", .member = 3, .cast = "Internal",
     .kind = "CastScript", .step = cast},
};
static const lv_movie_t code = {"EVENTS.DXR", 5, handlers,NULL, NULL, NULL, NULL, 0};
static const dg_cast_t casts[] = {{"Internal", 1, 1}, {"Other", 1, 2}};
static const dg_member_t members[] = {
    {.id = 0x110003, .number = 3, .cast = 1, .type = 1,
     .width = 1, .height = 1, .name = "later bitmap"},
};
static const dg_movie_t movie = {.code = &code, .id = 1, .tempo = 30,
                                 .cast_count = 2, .casts = casts,
                                 .member_count = 1, .members = members};
static void test_sparse_film_advancement(void) {
  static const lv_movie_t film_code = {"FILMS.DXR", 0, NULL,NULL, NULL, NULL, NULL, 0};
  static const dg_member_t film_members[] = {
      {.id = 0x110001, .type = 2, .film_count = 3, .film_loop = true},
      {.id = 0x110002, .type = 2, .film_count = 2, .film_loop = false},
      {.id = 0x110003, .type = 1},
  };
  static const dg_movie_t film_movie = {
      .code = &film_code, .id = 1, .tempo = 30, .cast_count = 2,
      .casts = casts, .members = film_members, .member_count = 3};
  assert(dg_enter(&director, &film_movie, 1, NULL, 0));
  static const unsigned channels[] = {31, 32, 64, 799, 800};
  for (unsigned n = 0; n < sizeof(channels) / sizeof(channels[0]); n++) {
    unsigned i = channels[n];
    lv_set(&values, NULL, "member", lv_make(LV_SPRITE, (int32_t)i),
           lv_make(LV_MEMBER, i == 32 ? 0x110002 : 0x110001));
    director.sprites[i].value.type = i == 800 ? 0 : 1;
  }
  director.sprites[31].film_frame = 2;
  director.sprites[32].film_frame = 1;
  director.sprites[64].visible = false; // Hidden films still advance.
  director.sprites[63].film_frame = 9;
  lv_set(&values, NULL, "scriptinstancelist", lv_make(LV_SPRITE, 63),
         lv_list(&values, 0, NULL, false));
  director.sprites[255].value.member = 0x110003;
  director.sprites[255].value.type = 1;
  director.sprites[255].film_frame = 7;
  dg_sprite_changed(&director, 255);
  dg_update_stage(&director);
  director.next_frame = 1;
  assert(dg_service(&director, DG_SERVICE_BUDGET));
  assert(director.sprites[31].film_frame == 0);
  assert(director.sprites[32].film_frame == 1);
  assert(director.sprites[64].film_frame == 1);
  assert(director.sprites[799].film_frame == 1);
  assert(director.sprites[800].film_frame == 0);
  assert(director.sprites[63].film_frame == 9);
  assert(director.sprites[255].film_frame == 7);
  assert(director.stage_count == 3);
  assert(director.stage_channels[director.stage_count - 1] == 799);
  dg_update_stage(&director);
  // Runtime changes remain visible on the next same-frame go, including the
  // last channel and a recast which must restart its film before advancing.
  director.sprites[800].value.type = 1;
  dg_sprite_changed(&director, 800);
  lv_set(&values, NULL, "member", lv_make(LV_SPRITE, 799),
         lv_make(LV_MEMBER, 0x110002));
  assert(director.sprites[799].film_frame == 0);
  director.sprites[32].value.member = 0;
  dg_sprite_changed(&director, 32);
  assert(!eligible(32));
  director.next_frame = 1;
  assert(dg_service(&director, DG_SERVICE_BUDGET));
  assert(director.sprites[31].film_frame == 1);
  assert(director.sprites[32].film_frame == 1);
  assert(director.sprites[64].film_frame == 2);
  assert(director.sprites[799].film_frame == 1);
  assert(director.sprites[800].film_frame == 1);
  assert(!values.failed);
}
int main(void) {
  dg_init(&director, &values, (dg_platform_t){0}, NULL, NULL, 0, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  lv_t instance = lv_instance(&values, lv_make(LV_SCRIPT, 0x110001));
  lv_set(&values, NULL, "scriptinstancelist", lv_make(LV_SPRITE, 1),
         lv_list(&values, 1, &instance, false));
  instance = lv_instance(&values, lv_make(LV_SCRIPT, 0x110002));
  lv_set(&values, NULL, "scriptinstancelist", lv_make(LV_SPRITE, 31),
         lv_list(&values, 1, &instance, false));
  lv_set(&values, NULL, "member", lv_make(LV_SPRITE, 32),
         lv_make(LV_MEMBER, 0x110003));
  assert(dg_service(&director, DG_SERVICE_BUDGET));
  assert(!values.failed && !strcmp(order, "GECLI"));
  assert(!director.sprites[800].value.member);
  assert(director.phase == 2 && !director.event_cursor);
  // Cache misses are specific to both event and complete cast/member identity.
  assert(!dg_event(&director, "exitframe", 0x110003));
  assert(!dg_event(&director, "exitframe", 0x110003));
  assert(!dg_event(&director, "prepareframe", 0x120003));
  assert(dg_event(&director, "prepareframe", 0x110003));
  assert(lv_run(&values, 4) && !values.depth && order[count - 1] == 'C');
  assert(!dg_event(&director, "prepareframe", 0x110063));
  assert(!dg_event(&director, "prepareframe", 0x110063));
  static const lv_handler_t new_handlers[] = {
      {.name = "prepareframe", .member = 99, .cast = "Internal",
       .kind = "CastScript", .step = loaded_event},
      {.name = "customEvent", .member = 99, .cast = "Internal",
       .kind = "CastScript", .step = loaded_event},
  };
  static const lv_movie_t new_code = {"RELOADED.DXR", 2, new_handlers,NULL, NULL, NULL, NULL, 0};
  dg_movie_t reloaded = movie;
  reloaded.code = &new_code;
  assert(dg_enter(&director, &reloaded, 1, NULL, 0));
  assert(dg_event(&director, "prepareframe", 0x110063));
  assert(lv_run(&values, 4) && !values.depth && restored == 1);
  assert(!dg_event(&director, "ENTERFRAME", 0x110063));
  assert(dg_event(&director, "PrEpArEfRaMe", 0x110063));
  assert(lv_run(&values, 4) && !values.depth && restored == 2);
  assert(dg_event(&director, "CUSTOMevent", 0x110063));
  assert(lv_run(&values, 4) && !values.depth && restored == 3);
  // Cleared behavior roots can leave queued begins with no handler. Their
  // outer-loop slots still precede the next pending begin and a later prepare.
  static const lv_handler_t lifecycle_handlers[] = {
      {.name = "beginsprite", .member = 98, .cast = "Internal",
       .kind = "BehaviorScript", .arguments = 1, .locals = 1,
       .step = lifecycle, .local_names = local_names},
      {.name = "prepareframe", .member = 99, .cast = "Internal",
       .kind = "BehaviorScript", .arguments = 1, .locals = 1,
       .step = lifecycle, .local_names = local_names},
  };
  static const lv_movie_t lifecycle_code = {"LIFECYCLE.DXR", 2, lifecycle_handlers,NULL, NULL, NULL, NULL, 0};
  reloaded.code = &lifecycle_code;
  assert(dg_enter(&director, &reloaded, 1, NULL, 0));
  memset(order, 0, sizeof(order));
  count = 0;
  director.begin_pending[1] = director.begin_pending[2] = true;
  director.begin_pending[3] = director.begin_pending[100] = true;
  instance = lv_instance(&values, lv_make(LV_SCRIPT, 0x110062));
  lv_set(&values, NULL, "scriptinstancelist", lv_make(LV_SPRITE, 100),
         lv_list(&values, 1, &instance, false));
  instance = lv_instance(&values, lv_make(LV_SCRIPT, 0x110063));
  lv_set(&values, NULL, "scriptinstancelist", lv_make(LV_SPRITE, 799),
         lv_list(&values, 1, &instance, false));
  assert(dg_service(&director, DG_SERVICE_BUDGET));
  assert(!values.failed && !strcmp(order, "BP"));
  static const lv_handler_t boundary_handler = {
      .name = "prepareframe", .member = 99, .cast = "Internal",
      .kind = "BehaviorScript", .arguments = 1, .locals = 1,
      .step = boundary, .local_names = local_names};
  static const lv_movie_t boundary_code = {"BOUNDARIES.DXR", 1, &boundary_handler,NULL, NULL, NULL, NULL, 0};
  reloaded.code = &boundary_code;
  assert(dg_enter(&director, &reloaded, 1, NULL, 0));
  static const unsigned channels[] = {31, 32, 63, 64, 255, 256, 799, 800};
  instance = lv_instance(&values, lv_make(LV_SCRIPT, 0x110063));
  for (unsigned n = 0; n < sizeof(channels) / sizeof(channels[0]); n++) {
    unsigned i = channels[n];
    lv_set(&values, NULL, "scriptinstancelist", lv_make(LV_SPRITE, i),
           lv_list(&values, 1, &instance, false));
  }
  assert(dg_service(&director, DG_SERVICE_BUDGET));
  assert(visits == sizeof(channels) / sizeof(channels[0]));
  assert(!memcmp(visited, channels, sizeof(channels)));
  for (unsigned n = 0; n < sizeof(channels) / sizeof(channels[0]); n++) {
    unsigned i = channels[n];
    lv_t sprite = lv_make(LV_SPRITE, (int32_t)i);
    assert(eligible(i) && director.stage_dirty[i]);
    // Clear/restore while already dirty, then mutate a list without replacing
    // its root. Non-VOID empty lists must remain eligible throughout.
    lv_set(&values, NULL, "scriptinstancelist", sprite, (lv_t){0});
    assert(!eligible(i));
    lv_t list = lv_list(&values, 0, NULL, false);
    lv_set(&values, NULL, "scriptinstancelist", sprite, list);
    assert(eligible(i));
    assert(lv_set_at(&values, list, 1, instance));
    assert(eligible(i));
    lv_set(&values, NULL, "scriptinstancelist", sprite, (lv_t){0});
    director.sprites[i].value.member = 0x110003;
    dg_sprite_changed(&director, i);
    assert(eligible(i));
    director.sprites[i].value.member = 0;
    dg_sprite_changed(&director, i);
    assert(!eligible(i));
    // Releasing a puppet reapplies the score member after the dirty notification.
    director.sprites[i].puppet = true;
    director.score[i + 5].member = 0x110003;
    lv_t result = {0};
    bool yield = false;
    assert(values.services.call(&values, "puppetsprite", 2,
                                (lv_t[]){lv_num(i), lv_num(0)}, &result, &yield));
    assert(eligible(i) && !yield);
    director.sprites[i].puppet = true;
    director.score[i + 5].member = 0;
    assert(values.services.call(&values, "puppetsprite", 2,
                                (lv_t[]){lv_num(i), lv_num(0)}, &result, &yield));
    assert(!eligible(i) && !yield);
  }
  assert(!values.failed);
  static const dg_frame_t score_frames[] = {{0, 0}, {0, 1}};
  static const dg_delta_t score_delta = {
      .channel = 805, .mask = DG_MEMBER, .value = {.script = 0x110002}};
  reloaded = movie;
  reloaded.frame_count = 2;
  reloaded.frames = score_frames;
  reloaded.deltas = &score_delta;
  assert(dg_enter(&director, &reloaded, 1, NULL, 0));
  assert(!eligible(800));
  director.sprites[800].puppet = true;
  assert(dg_seek(&director, 2));
  assert(eligible(800) && !director.sprites[800].value.member);
  test_sparse_film_advancement();
  puts("D8 events: live mutation, sparse films, word boundaries and lifecycle PASS");
}
