#include "director.h"
#include "ink.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static lv_runtime_t values;
static dg_runtime_t director;
static char events[32];
static unsigned event_count, sound_channel;
static bool sound_loop;
static unsigned global_prepare, global_enter, global_exit;
static unsigned messages, fallback_messages;
static lv_flow_t message(lv_runtime_t *r, lv_frame_t *f) {
  assert(director.current_event_sprite == 800);
  assert(lv_type(f->locals[0]) == LV_INSTANCE);
  messages += (unsigned)lv_integer(r, f->locals[1]);
  r->result = lv_num(messages);
  return LV_RETURN;
}
static lv_flow_t fallback_message(lv_runtime_t *r, lv_frame_t *f) {
  (void)f;
  fallback_messages++;
  r->result = lv_num(55);
  return LV_RETURN;
}
static lv_flow_t global_frame_event(lv_runtime_t *r, lv_frame_t *f) {
  (void)r;
  if (!strcmp(f->handler->name, "prepareframe"))
    global_prepare++;
  if (!strcmp(f->handler->name, "enterframe"))
    global_enter++;
  if (!strcmp(f->handler->name, "exitframe"))
    global_exit++;
  return LV_RETURN;
}
static char saved[64];
static unsigned saved_length;
static bool saved_exists;
static bool read_file(void *context, const char *name, char *out,
                      unsigned capacity, unsigned *length) {
  (void)context;
  if (strcmp(name, "player1.p3") || !saved_exists || capacity <= saved_length)
    return false;
  memcpy(out, saved, saved_length);
  *length = saved_length;
  return true;
}
static bool write_file(void *context, const char *name, const char *data,
                       unsigned length) {
  (void)context;
  assert(!strcmp(name, "player1.p3") && length < sizeof(saved));
  memcpy(saved, data, length);
  saved_length = length;
  saved_exists = length != 0;
  return true;
}
static lv_t service(const char *name, unsigned count, const lv_t *args) {
  lv_t out = {0};
  bool yield = false;
  assert(values.services.call(&values, name, count, args, &out, &yield));
  assert(!values.failed && !yield);
  return out;
}
static const char *const names[] = {"me"};
static const char *const message_names[] = {"me", "amount"};
static const char *const properties[] = {"spritenum", "visits"};
static lv_flow_t movie_event(lv_runtime_t *r, lv_frame_t *f) {
  (void)r;
  events[event_count++] = f->handler->name[0];
  return LV_RETURN;
}
static lv_flow_t begin(lv_runtime_t *r, lv_frame_t *f) {
  assert(lv_type(f->locals[0]) == LV_INSTANCE);
  assert(lv_integer(r, lv_get(r, f, "spritenum", f->locals[0])) == 800);
  lv_set(r, f, "visits", f->locals[0], lv_num(42));
  events[event_count++] = 'b';
  return LV_RETURN;
}
static lv_flow_t prepare(lv_runtime_t *r, lv_frame_t *f) {
  assert(lv_integer(r, lv_get(r, f, "visits", f->locals[0])) == 42);
  if (event_count < sizeof(events))
    events[event_count++] = 'f';
  return LV_RETURN;
}
static void sound(void *context, unsigned channel, const dg_member_t *m) {
  (void)context;
  if (m) {
    assert(m->number == 3);
    sound_channel = channel + 1;
    sound_loop = m->looping;
  }
}
static const lv_handler_t handlers[] = {
    {.name = "message",
     .member = 2,
     .cast = "c12",
     .kind = "BehaviorScript",
     .arguments = 2,
     .locals = 2,
     .step = message,
     .local_names = message_names},
    {.name = "fallback",
     .member = 4,
     .cast = "c12",
     .kind = "MovieScript",
     .step = fallback_message},
    {.name = "prepareframe",
     .member = 4,
     .cast = "c12",
     .kind = "MovieScript",
     .step = global_frame_event},
    {.name = "enterframe",
     .member = 4,
     .cast = "c12",
     .kind = "MovieScript",
     .step = global_frame_event},
    {.name = "exitframe",
     .member = 4,
     .cast = "c12",
     .kind = "MovieScript",
     .step = global_frame_event},
    {.name = "preparemovie",
     .member = 2,
     .cast = "c12",
     .kind = "MovieScript",
     .step = movie_event},
    {.name = "startmovie",
     .member = 2,
     .cast = "c12",
     .kind = "MovieScript",
     .step = movie_event},
    {.name = "beginsprite",
     .member = 2,
     .cast = "c12",
     .kind = "BehaviorScript",
     .arguments = 1,
     .locals = 1,
     .step = begin,
     .local_names = names,
     .property_count = 2,
     .property_names = properties},
    {.name = "prepareframe",
     .member = 2,
     .cast = "c12",
     .kind = "BehaviorScript",
     .arguments = 1,
     .locals = 1,
     .step = prepare,
     .local_names = names,
     .property_count = 2,
     .property_names = properties},
};
static const lv_movie_t code = {"D8.DXR", 9, handlers,NULL, NULL, NULL, NULL, 0};
static const dg_cast_t casts[] = {
    {"c1", 1, 1}, {"c2", 1, 2},   {"c3", 1, 3},   {"c4", 1, 4},
    {"c5", 1, 5}, {"c6", 1, 6},   {"c7", 1, 7},   {"c8", 1, 8},
    {"c9", 1, 9}, {"c10", 1, 10}, {"c11", 1, 11}, {"c12", 1, 12}};
static const dg_member_t members[] = {
    {.id = 0x1c0001,
     .number = 1,
     .cast = 12,
     .type = 1,
     .width = 20,
     .height = 10,
     .reg_x = 10,
     .reg_y = 5,
     .name = "image"},
    {.id = 0x1c0002, .number = 2, .cast = 12, .type = 11, .name = "behavior"},
    {.id = 0x1c0003, .number = 3, .cast = 12, .type = 6, .name = "voice"},
};
static const dg_delta_t deltas[] = {
    {.channel = 1, .value = {.delay = 2}},
    {.channel = 805,
     .mask = DG_ALL,
     .value = {.member = 0x1c0001,
               .script = 0x1c0002,
               .x = 100,
               .y = 100,
               .width = 20,
               .height = 10,
               .type = 16,
               .blend = 100,
               .rotation = 9000,
               .flags = 16,
               .fore_rgb = 0x123456}},
    {.channel = 1, .value = {.tempo = 999}},
};
static const dg_frame_t frames[] = {{0, 2}, {2, 1}};
static const dg_label_t labels[] = {{"start", 1}, {"second", 2}};
static const dg_movie_t movie = {.code = &code,
                                 .id = 1,
                                 .tempo = 30,
                                 .cast_count = 12,
                                 .casts = casts,
                                 .member_count = 3,
                                 .members = members,
                                 .frame_count = 2,
                                 .frames = frames,
                                 .deltas = deltas,
                                 .label_count = 2,
                                 .labels = labels};
static void stage_channels(void) {
  // Commits and retained trails must keep the full D8 channel identity.
  dg_sprite_t *s = &director.sprites[800];
  s->visible = true;
  s->trails = true;
  s->value = (dg_spec_t){.type = 1, .width = 10, .height = 10, .x = 40};
  dg_sprite_changed(&director, 800);
  dg_sprite_changed(&director, 800);
  assert(director.stage_count == 1 && director.stage_channels[0] == 800);
  dg_update_stage(&director);
  assert(!director.stage_count && !director.stage_dirty[800]);
  assert(director.staged[800].value.x == 40 && !director.staged[32].value.type);
  s->value.x = 50;
  dg_sprite_changed(&director, 800);
  dg_update_stage(&director);
  assert(director.trail_count == 1 && director.trails[0].channel == 800);
  assert(director.trails[0].sprite.value.x == 40);
  assert(director.staged[800].value.x == 50 && !director.stage_dirty[800]);
}
static void emptied_member_rect(void) {
  // NB.DXR restarts its baby game with sprite(k).rect = sprite(k).member.rect
  // after emptying those channels: an absent member's rect reads VOID, and
  // assigning VOID leaves the sprite as it was (ScummVM does both).
  lv_t spr = lv_make(LV_SPRITE, 798), empty =lv_make(LV_MEMBER, 0x10000);
  dg_sprite_t *s = &director.sprites[798];
  *s = (dg_sprite_t){.value = {.member = 0x1c0001, .x = 100, .y = 100,
                               .width = 40, .height = 30}};
  lv_t rect = lv_get(&values, NULL, "rect", empty);
  assert(lv_type(rect) == LV_VOID && !values.failed);
  lv_set(&values, NULL, "rect", spr, rect);
  assert(!values.failed && !s->stretch && s->value.width == 40 && s->value.x == 100);
}
static void sprite_dimensions(void) {
  // ScummVM 41ac2b3184: getTheSprite(width/height) reads Channel's sprite
  // dimensions, while rect/edges use its bounding box. TS.DXR member 5's
  // animerapistongen repeatedly uses height when updating the piston quad.
  lv_t spr = lv_make(LV_SPRITE, 799);
  dg_sprite_t *s = &director.sprites[799];
  *s = (dg_sprite_t){.value = {.member = 0x1c0001, .x = 100, .y = 100,
                               .width = 40, .height = 30, .rotation = 9000}};
  assert(lv_numeric(lv_get(&values, NULL, "width", spr)) == 20);
  assert(lv_numeric(lv_get(&values, NULL, "height", spr)) == 10);
  s->stretch = true;
  assert(lv_numeric(lv_get(&values, NULL, "width", spr)) == 40);
  assert(lv_numeric(lv_get(&values, NULL, "height", spr)) == 30);
  lv_t rect = lv_get(&values, NULL, "rect", spr);
  assert(lv_numeric(lv_at(&values, rect, 3)) - lv_numeric(lv_at(&values, rect, 1)) == 30);
  assert(lv_numeric(lv_at(&values, rect, 4)) - lv_numeric(lv_at(&values, rect, 2)) == 40);
  lv_set(&values, NULL, "skew", spr, lv_num(25));
  assert(lv_numeric(lv_get(&values, NULL, "width", spr)) == 40);
  assert(lv_numeric(lv_get(&values, NULL, "height", spr)) == 30);

  for (unsigned frame = 0; frame < 100; frame++) {
    double height = lv_numeric(lv_get(&values, NULL, "height", spr));
    double top = 80 + (int)(frame % 20);
    lv_t points[] = {lv_point(&values, 80, 70), lv_point(&values, 120, top),
                     lv_point(&values, 120, top + height), lv_point(&values, 80, 130)};
    lv_set(&values, NULL, "quad", spr, lv_list(&values, 4, points, false));
    assert(lv_numeric(lv_get(&values, NULL, "width", spr)) == 40);
    assert(lv_numeric(lv_get(&values, NULL, "height", spr)) == 30);
    int l, t, r, b;
    dg_bounds(&director, 799, &l, &t, &r, &b);
    assert(l == 80 && t == 70 && r == 120 && b == 130);
  }
  lv_set(&values, NULL, "locv", spr, lv_num(110));
  assert(lv_numeric(lv_get(&values, NULL, "top", spr)) == 80);
  assert(lv_numeric(lv_get(&values, NULL, "height", spr)) == 30);
  assert(!values.failed);
}
int main(void) {
  stage_channels();
  uint8_t header[32] = {0};
  assert(!dg_fdi_follow_alpha(header));
  header[16] = 1;
  assert(dg_fdi_follow_alpha(header));
  assert(dg_bitmap_ink16(0x1234, 0, dg_fdi_follow_alpha(header)) == 0x1234);
  assert(dg_bitmap_ink16(0x1234, 32, true) == 0x1234);
  assert(dg_bitmap_ink16(0x1234, 0, false) == 0x1235);
  assert(dg_bitmap_ink16(0xffff, 36, true) == 0xfffe);
  assert(dg_bitmap_ink16(0x1235, 8, true) == 0x1235);
  uint16_t source = (20 << 11) | (5 << 6) | (25 << 1) | 1;
  uint16_t destination = (10 << 11) | (15 << 6) | (8 << 1) | 1;
  assert(dg_dark_rgba16(source, destination, 100) ==
         ((10 << 11) | (5 << 6) | (8 << 1) | 1));
  assert(dg_dark_rgba16(source, destination, 50) ==
         ((15 << 11) | (10 << 6) | (16 << 1) | 1));
  assert(dg_dark_rgba16(source & ~1, destination, 100) == destination);
  assert(dg_dark_rgba16(source, destination, 0) == destination);
  float u, v;
  const float trapezoid[8] = {0, 0, 100, 0, 75, 100, 25, 100};
  assert(dg_quad_uv(trapezoid, 50, 50, &u, &v) && fabsf(u - 0.5f) < 0.001f &&
         fabsf(v - 0.5f) < 0.001f);
  assert(!dg_quad_uv(trapezoid, 2, 99, &u, &v));
  dg_init(&director, &values,
          (dg_platform_t){
              .sound = sound, .read_file = read_file, .write_file = write_file},
          NULL, NULL, 0, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  if (!dg_service(&director, 288))
    fprintf(stderr, "%s\n", values.error);
  assert(!values.failed);
  assert(!strncmp(events, "psbf", 4));
  assert(global_prepare == 1 && global_enter == 1 && global_exit == 0);
  assert(director.frame_deadline == 120000000);
  assert(director.stage_count == 800);
  int l, t, r, b;
  dg_bounds(&director, 800, &l, &t, &r, &b);
  assert(l == 95 && t == 90 && r == 105 && b == 110);
  assert(dg_hit(&director, 100, 109) == 800);
  assert(!dg_hit(&director, 109, 100));
  lv_t quad =
      lv_get(&values, NULL, "quad", lv_make(LV_SPRITE, 800));
  assert(lv_type(quad) == LV_LIST && lv_count(&values, quad) == 4);
  assert(fabs(lv_number(&values, lv_at(&values, lv_at(&values, quad, 1), 1)) -
              105) < 0.001);
  lv_t spr = lv_make(LV_SPRITE, 800);
  assert(lv_integer(&values, lv_get(&values, NULL, "forecolor", spr)) ==
         0x123456);
  lv_set(&values, NULL, "color", spr, lv_make(LV_COLOR, 0));
  lv_t black = lv_get(&values, NULL, "forecolor", spr);
  assert(lv_type(black) == LV_COLOR && lv_id(black) == 0);
  assert((director.sprites[800].value.flags & 16) &&
         (director.sprites[800].auto_mask & DG_FORE));
  lv_set(&values, NULL, "backcolor", spr, black);
  assert(lv_type(lv_get(&values, NULL, "backcolor", spr)) == LV_COLOR);
  assert((director.sprites[800].value.flags & 32) &&
         (director.sprites[800].auto_mask & DG_BACK));
  lv_set(&values, NULL, "forecolor", spr, lv_num(0));
  assert(lv_type(lv_get(&values, NULL, "color", spr)) == LV_NUMBER);
  assert(!(director.sprites[800].value.flags & 16));
  lv_set(&values, NULL, "color", spr, lv_make(LV_COLOR, 0x123456));
  // NY.setGBit parses member and cast identity from the mouse member string.
  director.mouse_x = 100;
  director.mouse_y = 100;
  lv_t mouse_member = lv_get(&values, NULL, "mousemember", (lv_t){0});
  assert(lv_type(mouse_member) == LV_MEMBER && lv_id(mouse_member) == 0x1c0001);
  lv_frame_t caller = {.movie = &code};
  lv_t member_string = lv_call(&values, &caller, "string", 1, &mouse_member);
  assert(!strcmp(lv_cstr(&values, member_string), "member 1 of castLib 12"));
  lv_t cast_number = lv_chunk(&values, "word", member_string, lv_num(5), lv_num(5));
  cast_number = lv_call(&values, &caller, "integer", 1, &cast_number);
  lv_t cast_ref = lv_call(&values, &caller, "castlib", 1,
                           &cast_number);
  assert(lv_type(cast_ref) == LV_CASTLIB && lv_id(cast_ref) == 12);
  assert(!strcmp(lv_cstr(&values, lv_get(&values, NULL, "name", cast_ref)), "c12"));
  lv_t member_number = lv_chunk(&values, "word", member_string, lv_num(2), lv_num(2));
  assert(lv_integer(&values, lv_call(&values, &caller, "integer", 1,
                                     &member_number)) + 20 == 21);
  director.mouse_x = director.mouse_y = 0;
  assert(lv_type(lv_get(&values, NULL, "mousemember", (lv_t){0})) == LV_VOID);
  lv_t stage = lv_get(&values, NULL, "stage", (lv_t){0});
  lv_t stage_rect = lv_get(&values, NULL, "rect", stage);
  assert(lv_integer(&values, lv_get(&values, NULL, "width", stage_rect)) == 640);
  assert(lv_integer(&values, lv_get(&values, NULL, "height", stage_rect)) == 480);
  assert(lv_integer(&values, lv_get(&values, NULL, "keycode", (lv_t){0})) == 0);
  assert(!lv_truth(&values, lv_get(&values, NULL, "shiftdown", (lv_t){0})));
  lv_t behavior_list = values.roots[DG_BEHAVIOR_ROOT + 800];
  lv_t instance = lv_at(&values, behavior_list, 1);
  values.roots[DG_BEHAVIOR_ROOT + 800] =
      lv_list(&values, 2, (lv_t[]){instance, instance}, false);
  unsigned previous_sprite = director.current_event_sprite;
  lv_t dispatched = service("sendsprite", 3,
                            (lv_t[]){lv_num(800), lv_text(&values, "Message", true),
                                     lv_num(3)});
  assert(messages == 6 && lv_integer(&values, dispatched) == 6);
  assert(director.current_event_sprite == previous_sprite);
  values.roots[DG_BEHAVIOR_ROOT + 800] = behavior_list;
  dispatched = service("sendsprite", 2,
                       (lv_t[]){lv_num(800), lv_text(&values, "fallback", true)});
  assert(fallback_messages == 1 && lv_integer(&values, dispatched) == 55);
  assert(!values.depth && !values.failed);
  assert(lv_integer(&values, lv_get(&values, NULL, "rotation", spr)) == 90);
  lv_t ref = lv_reference(&values, "script", lv_num(2), lv_num(12));
  assert(lv_type(ref) == LV_SCRIPT && lv_id(ref) == 0x1c0002);
  lv_t result = {0};
  bool yield = false;
  assert(values.services.call(
      &values, "puppetsound", 2,
      (lv_t[]){lv_num(8), lv_make(LV_MEMBER, 0x1c0003)}, &result,
      &yield));
  assert(!values.failed && sound_channel == 8 &&
         director.sounds[7] == 0x1c0003);
  for (unsigned i = 0; i < 119; i++)
    assert(dg_tick(&director, 0, 0, false, 288));
  assert(director.frame == 1);
  assert(global_prepare == 1 && global_enter == 1 && global_exit == 0);
  assert(dg_tick(&director, 0, 0, false, 288));
  assert(director.frame == 2 && director.tempo == 999 &&
         director.score_delay == 0);
  assert(global_prepare == 2 && global_enter == 2 && global_exit == 1);
  // Stable locZ ordering includes sprite 800 without byte-sized truncation.
  director.sprites[799] = director.sprites[800];
  director.sprites[799].value.loc_z = 900;
  director.sprites[800].value.loc_z = 10;
  dg_sprite_changed(&director, 799);
  dg_sprite_changed(&director, 800);
  assert(dg_hit(&director, 100, 100) == 799);
  dg_stop_sounds(&director);
  assert(!director.sounds[7]);
  lv_t sound_member = lv_make(LV_MEMBER, 0x1c0003);
  lv_set(&values, NULL, "loop", sound_member, lv_num(1));
  assert(lv_truth(&values, lv_get(&values, NULL, "loop", sound_member)));
  service("preload", 1, &sound_member);
  service("puppetsound", 2, (lv_t[]){lv_num(8), sound_member});
  assert(sound_loop && !members[2].looping);
  lv_set(&values, NULL, "loop", sound_member, lv_num(0));
  service("puppetsound", 2, (lv_t[]){lv_num(8), sound_member});
  assert(!sound_loop &&
         !lv_truth(&values, lv_get(&values, NULL, "loop", sound_member)));
  lv_t voice = service("sound", 1, (lv_t[]){lv_num(8)});
  lv_set(&values, NULL, "volume", voice, lv_num(200));
  service("sound_fadeout", 2, (lv_t[]){lv_num(8), lv_num(4)});
  assert(dg_tick(&director, 0, 0, false, 288));
  assert(lv_integer(&values, lv_get(&values, NULL, "volume", voice)) == 150);
  for (unsigned i = 0; i < 3; i++)
    assert(dg_tick(&director, 0, 0, false, 288));
  assert(lv_integer(&values, lv_get(&values, NULL, "volume", voice)) == 0);
  service("stop", 1, &voice);
  assert(lv_integer(&values, lv_get(&values, NULL, "volume", voice)) == 200);
  lv_t xtra = service("xtra", 1, (lv_t[]){lv_text(&values, "FileIO", false)});
  lv_t file = service("new", 1, &xtra);
  assert(lv_type(file) == LV_FILE);
  values.roots[1] = file;
  lv_t path = lv_text(&values, "C:\\p3data\\player1.p3", false);
  service("openfile", 3, (lv_t[]){file, path, lv_num(0)});
  assert(lv_integer(&values, service("status", 1, &file)) == -37);
  service("createfile", 2, (lv_t[]){file, path});
  service("openfile", 3, (lv_t[]){file, path, lv_num(0)});
  service("writestring", 2,
          (lv_t[]){file, lv_text(&values, "one\rtwo", false)});
  service("closefile", 1, &file);
  assert(saved_exists && saved_length == 7);
  service("openfile", 3, (lv_t[]){file, path, lv_num(0)});
  assert(lv_integer(&values, service("getlength", 1, &file)) == 7);
  assert(!strcmp(lv_cstr(&values, service("readline", 1, &file)), "one"));
  assert(!strcmp(lv_cstr(&values, service("readfile", 1, &file)), "two"));
  service("delete", 1, &file);
  assert(!saved_exists);
  for (unsigned i = 0; i < 16; i++) {
    lv_t transient = service("new", 1, &xtra);
    assert(lv_type(transient) == LV_FILE && lv_id(transient) != lv_id(file));
    service("closefile", 1, &transient);
  }
  assert(lv_integer(&values, service("status", 1, &file)) == 0);
  static const dg_delta_t intro_tempo[] = {
      {.channel = 1, .value = {.tempo = 12}},
      {.channel = 1, .value = {.delay = 1}},
      {.channel = 1, .value = {0}},
  };
  static const dg_frame_t intro_frames[] = {{0, 1}, {1, 1}, {2, 1}};
  dg_movie_t intro = movie;
  intro.frame_count = 3;
  intro.frames = intro_frames;
  intro.deltas = intro_tempo;
  dg_init(&director, &values, (dg_platform_t){0}, NULL, NULL, 0, 1);
  assert(dg_enter(&director, &intro, 1, NULL, 0));
  assert(director.tempo == 12 && director.frame_deadline == 5000000);
  assert(dg_seek(&director, 2) && director.score_delay == 1 &&
         director.frame_deadline == 60000000);
  assert(dg_seek(&director, 3) && director.score_delay == 0 &&
         director.tempo == 12 && director.frame_deadline == 5000000);
  sprite_dimensions();
  emptied_member_rect();

  // D8 keeps cross-movie frame labels: go("second", "D8") enters at label.
  dg_init(&director, &values,
          (dg_platform_t){
              .sound = sound, .read_file = read_file, .write_file = write_file},
          NULL, NULL, 0, 1);
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  assert(dg_service(&director, 288));
  lv_t go_label[] = {lv_text(&values, "second", false),
                     lv_text(&values, "D8", false)};
  lv_t go_out = {0};
  bool go_yield = false;
  assert(values.services.call(&values, "go", 2, go_label, &go_out, &go_yield));
  assert(!values.failed);
  assert(!strcmp(director.next_movie, "D8.DXR") ||
         !strcmp(director.next_movie, "D8"));
  assert(!strcmp(director.next_movie_label, "second"));
  director.next_movie[0] = 0;
  assert(dg_enter(&director, &movie, 1, NULL, 0));
  assert(dg_service(&director, 288));
  assert(!values.failed && director.frame == 2);
  puts("Director D8: 800 sprites, cast12, behavior self, lifecycle, "
       "delay/FPS999, RGB/rotation, locZ and audio8 PASS");
}
