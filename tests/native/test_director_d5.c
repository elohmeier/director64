#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static dg_runtime_t director;
static lv_runtime_t values;
static bool busy, pass_cast, reveal_after_drag;
static char events[32];
static unsigned count;
static bool sound_busy(void *ctx, unsigned channel) {
  (void)ctx;
  assert(channel == 0);
  return busy;
}
static lv_flow_t event(lv_runtime_t *r, lv_frame_t *f) {
  events[count++] = f->handler->member == 2 ? 'S' : 'C';
  if (f->handler->member == 2 || pass_cast) {
    lv_t out = {0};
    bool yield = false;
    assert(r->services.call(r, "pass", 0, NULL, &out, &yield));
  }
  return LV_RETURN;
}
static lv_flow_t drag(lv_runtime_t *r, lv_frame_t *f) {
  (void)r; (void)f;
  if (!reveal_after_drag)
    return LV_RETURN;
  if (director.mouse_down)
    return LV_YIELD;
  // The new foreground channel must not receive the release that ended this
  // drag. The original channel's mouseUp should still dispatch normally.
  lv_set(&values, NULL, "member", lv_make(LV_SPRITE, 2),
         lv_make(LV_MEMBER, 0x110003));
  director.sprites[2].value.x = director.sprites[2].value.y = 0;
  return LV_RETURN;
}
static const lv_handler_t handlers[] = {
    {.name = "mousedown", .member = 2, .cast = "Internal",
     .kind = "ScoreScript", .step = drag},
    {.name = "mouseup",
     .member = 2,
     .cast = "Internal",
     .kind = "ScoreScript",
     .step = event},
    {.name = "mouseup",
     .member = 3,
     .cast = "Internal",
     .kind = "CastScript",
     .step = event},
};
static const lv_movie_t code = {"D5.DXR", 3, handlers,NULL, NULL, NULL, NULL, 0};
static const dg_cast_t casts[] = {{"Internal", 1, 1}, {"Shared", 2, 1}};
static const uint32_t film_sounds[] = {0x110007, 0, 0x110007, 0, 0, 0};
static const dg_member_t members[] = {
    {.id = 0x110003,
     .number = 3,
     .cast = 1,
     .type = 1,
     .width = 100,
     .height = 100,
     .name = "door"},
    {.id = 0x110004,
     .number = 4,
     .cast = 1,
     .type = 10,
     .width = 100,
     .height = 100,
     .name = "video",
     .samples = 600,
     .rate = 600,
     .video_flags = 256},
    {.id = 0x110005,
     .number = 5,
     .cast = 1,
     .type = 10,
     .width = 100,
     .height = 100,
     .name = "second video",
     .samples = 1200,
     .rate = 600},
    {.id = 0x110006,
     .number = 6,
     .cast = 1,
     .type = 2,
     .name = "film",
     .film_count = 3,
     .film_loop = 1,
     .film_sounds = film_sounds},
    {.id = 0x110007, .number = 7, .cast = 1, .type = 6, .name = "film sound"},
    {.id = 0x110008, .number = 8, .cast = 1, .type = 6, .name = "Radio.AIF"},
    {.id = 0x110009, .number = 9, .cast = 1, .type = 6, .name = "Radio.WAV"},
    {.id = 0x11000a, .number = 10, .cast = 1, .type = 6, .name = "Exact.dir"},
    {.id = 0x11000b, .number = 11, .cast = 1, .type = 6, .name = "Exact.AIF"},
    {.id = 0x11000c, .number = 12, .cast = 1, .type = 6, .name = "Wave.WAV"},
    {.id = 0x11000d, .number = 13, .cast = 1, .type = 1, .name = "Radio.dir"},
    {.id = 0x11000e, .number = 14, .cast = 1, .type = 3,
     .name = "Speedanzeige", .text = "1"},
};
static dg_delta_t deltas[] = {
    {.channel = 1, .value = {.type = 135}},
    {.channel = 6,
     .mask = DG_ALL,
     .value = {.type = 16,
               .member = 0x110003,
               .script = 0x110002,
               .x = 0,
               .y = 0,
               .width = 100,
               .height = 100,
               .blend = 100}},
    {.channel = 1, .value = {.type = 128}},
};
static const dg_frame_t frames[] = {{0, 2}, {2, 1}};
static const dg_label_t labels[] = {{"first", 1}, {"second", 2}};
static const dg_movie_t movie = {.code = &code,
                                 .id = 1,
                                 .tempo = 30,
                                 .cast_count = 2,
                                 .casts = casts,
                                 .member_count = sizeof(members) / sizeof(*members),
                                 .members = members,
                                 .frame_count = 2,
                                 .frames = frames,
                                 .deltas = deltas,
                                 .label_count = 2,
                                 .labels = labels};
static const lv_movie_t shared_code = {"SHARED.CXT", 0, NULL,NULL, NULL, NULL, NULL, 0};
static const dg_cast_t shared_cast = {"Shared", 2, 1};
static const dg_member_t shared_member = {
    .id = 0x210005, .number = 5, .cast = 1, .type = 1, .name = "shared"};
static const dg_movie_t shared_movie = {.code = &shared_code,
                                        .id = 2,
                                        .cast_count = 1,
                                        .casts = &shared_cast,
                                        .member_count = 1,
                                        .members = &shared_member};
static void boot(void) {
  busy = true;
  count = 0;
  memset(events, 0, sizeof(events));
  dg_init(&director, &values, (dg_platform_t){.sound_busy = sound_busy}, NULL,
          NULL, 0, 1);
  const dg_movie_t *shared[] = {&shared_movie};
  assert(dg_enter(&director, &movie, 1, shared, 1));
}
static void tick(bool down) {
  assert(dg_tick(&director, 20, 20, down, DG_SERVICE_BUDGET));
}
static int call(const char *name, lv_t argument) {
  lv_t out = {0};
  bool yield = false;
  assert(values.services.call(&values, name, 1, &argument, &out, &yield));
  assert(!yield);
  return lv_integer(&values, out);
}
static void play_file(const char *name, unsigned expected) {
  lv_t arguments[] = {lv_num(3), lv_text(&values, name, false)}, out = {0};
  bool yield = false;
  assert(values.services.call(&values, "sound_playfile", 2, arguments, &out, &yield));
  assert(!yield);
  assert(!values.failed);
  assert(director.sounds[2] == expected);
  assert(director.sound_puppet[2]);
}
static int constrain(const char *name, int channel, int coordinate) {
  lv_t arguments[] = {lv_num(channel), lv_num(coordinate)}, out = {0};
  bool yield = false;
  assert(values.services.call(&values, name, 2, arguments, &out, &yield));
  assert(!values.failed && !yield);
  return lv_integer(&values, out);
}
int main(void) {
  boot();
  lv_t speed_field = lv_make(LV_FIELD, 0x11000e);
  lv_set(&values, NULL, "text", speed_field, lv_num(24));
  lv_t speed_text = lv_get(&values, NULL, "text", speed_field);
  assert(lv_type(speed_text) == LV_STRING && !strcmp(lv_cstr(&values, speed_text), "24"));
  assert(!strcmp(lv_cstr(&values, values.roots[DG_FIELD_TEXT_ROOT]), "24"));
  lv_set(&values, NULL, "text", speed_field, lv_text(&values, "Pause", true));
  assert(!strcmp(lv_cstr(&values, lv_get(&values, NULL, "text", speed_field)), "Pause"));
  lv_set(&values, NULL, "text", speed_field, (lv_t){0});
  assert(!strcmp(lv_cstr(&values, lv_get(&values, NULL, "text", speed_field)), ""));
  assert(!values.failed);

  boot();
  reveal_after_drag = true;
  tick(true);
  assert(values.depth);
  tick(false);
  assert(!values.depth && !strcmp(events, "SC"));
  assert(dg_hit(&director, 20, 20) == 2);
  reveal_after_drag = false;

  boot();
  director.sprites[3].value = (dg_spec_t){.type = 16, .member = 0x110003,
      .x = 140, .y = 130, .width = 100, .height = 100};
  assert(constrain("constrainh", 3, -10) == 140);
  assert(constrain("constrainh", 3, 190) == 190);
  assert(constrain("constrainh", 3, 999) == 240);
  assert(constrain("constrainv", 3, -10) == 130);
  assert(constrain("constrainv", 3, 200) == 200);
  assert(constrain("constrainv", 3, 999) == 230);
  director.sprites[3].value.x = 180;
  assert(constrain("constrainh", 3, 140) == 180);
  assert(constrain("constrainh", DG_SPRITES, 140) == 0);

  boot();
  play_file("C:\\AUDIO\\radio.dir", 0x110008); // Skip bitmap; .AIF before .WAV.
  play_file("AUDIO:RADIO", 0x110008);            // Extensionless Mac path.
  play_file("AUDIO/radio.wav", 0x110009);       // Exact extension takes priority.
  play_file("exact.DIR", 0x11000a);             // Preserve an unusual exact name.
  play_file("Wave.dir", 0x11000c);              // Fall back to .WAV if no .AIF.
  lv_t missing[] = {lv_num(3), lv_text(&values, "Absent.dir", false)}, out = {0};
  bool yield = false;
  unsigned sounds_before = director.sounds[2];
  assert(values.services.call(&values, "sound_playfile", 2, missing, &out, &yield));
  // The original playFile of a missing file plays nothing and moves on.
  assert(!values.failed && director.sounds[2] == sounds_before);

  boot();
  for (unsigned i = 0; i < 10; i++)
    tick(false);
  assert(director.frame == 1); // D5 135 means sound 1, never D6 video wait.
  tick(true);
  tick(false);
  assert(!strcmp(events, "SC")); // source pass in sprite handler reaches cast.
  assert(director.frame == 1);   // input does not release the sound wait.
  busy = false;
  tick(false);
  assert(director.frame == 2);
  for (unsigned i = 0; i < 10; i++)
    tick(false);
  assert(director.frame == 2); // 128 waits for the next press.
  tick(true);
  assert(director.frame == 1);

  boot();
  lv_t sprite = lv_make(LV_SPRITE, 2);
  assert(lv_integer(&values, lv_get(&values, NULL, "digitalvideotimescale",
                                    (lv_t){0})) == 60);
  // Classic "the volume of sound i" resolves a channel, not a cast member.
  for (unsigned i = 1; i <= 4; i++) {
    lv_t channel = lv_reference(&values, "sound", lv_num(i), (lv_t){0});
    assert(lv_type(channel) == LV_SOUND && lv_id(channel) == (int)i);
    lv_set(&values, NULL, "volume", channel, lv_num(i * 50));
    assert(director.channel_volume[i - 1] == i * 50);
    assert(lv_integer(&values, lv_get(&values, NULL, "volume", channel)) ==
           (int)i * 50);
  }
  lv_t shared = lv_reference(&values, "member", lv_num(131077), (lv_t){0});
  assert(lv_id(shared) == 0x210005);
  assert(lv_integer(&values, lv_get(&values, NULL, "number", shared)) ==
         131077);
  lv_set(&values, NULL, "member", sprite,
         lv_make(LV_MEMBER, 0x110004));
  director.sprites[2].value.type = 16;
  assert(lv_integer(&values, lv_get(&values, NULL, "duration",
                 lv_make(LV_MEMBER, 0x110004))) == 60);
  lv_set(&values, NULL, "starttime", sprite, lv_num(3));
  assert(lv_integer(&values, lv_get(&values, NULL, "starttime", sprite)) == 3);
  lv_set(&values, NULL, "movietime", sprite, lv_num(20));
  tick(false);
  assert(lv_integer(&values, lv_get(&values, NULL, "movietime", sprite)) ==
         20);
  lv_set(&values, NULL, "movierate", sprite, lv_num(2));
  lv_set(&values, NULL, "stoptime", sprite, lv_num(24));
  assert(lv_integer(&values, lv_get(&values, NULL, "stoptime", sprite)) == 24);
  tick(false);
  tick(false);
  assert(lv_integer(&values, lv_get(&values, NULL, "movietime", sprite)) ==
         24);
  assert(lv_number(&values, lv_get(&values, NULL, "movierate", sprite)) == 0);

  // A video wait releases when the authored stopTime is reached.
  for (unsigned i = 0; i < 10; i++)
    tick(false);
  director.score_wait = 137;
  director.sprites[2].video_time = 239;
  busy = false;
  tick(false);
  assert(director.frame == 1);
  director.sprites[2].video_time = 240;
  tick(false);
  assert(director.frame == 2);
  assert(call("label", lv_text(&values, "FIRST", false)) == 1);
  assert(call("label", lv_text(&values, "absent", false)) == 0);
  assert(call("marker", lv_num(0)) == 2);
  assert(call("marker", lv_num(-1)) == 1);
  assert(call("marker", lv_num(1)) == 2);

  // The renderer observes a replaced video before the next service tick.
  lv_set(&values, NULL, "member", sprite,
         lv_make(LV_MEMBER, 0x110005));
  assert(director.sprites[2].video_time == 0);
  assert(director.sprites[2].video_stop == 0);
  assert(director.sprites[2].video_rate == 1);
  director.sprites[1].video_time = 900;
  deltas[1].value.member = 0x110004;
  assert(dg_seek(&director, 1));
  assert(director.sprites[1].video_time == 0);
  assert(director.sprites[1].video_rate == 0);
  lv_set(&values, NULL, "member", sprite,
         lv_make(LV_MEMBER, 0x110006));
  director.sprites[2].puppet=true;
  director.sprites[2].value.type=16;
  assert(dg_seek(&director,director.frame==1?2:1));
  assert(director.sounds[0] == 0x110007);
  unsigned serial = director.sound_serial[0];
  director.sprites[2].film_frame = 1;
  assert(dg_seek(&director,director.frame==1?2:1));
  assert(director.sound_serial[0] ==
         serial); // Retained sound never restarts each pose.
  director.sprites[2].film_frame = 2;
  assert(dg_seek(&director,director.frame==1?2:1));
  assert(director.sounds[0] ==
         0); // Blank sound pose releases the previous sound.
  director.sprites[2].film_frame = 3;
  assert(dg_seek(&director,director.frame==1?2:1));
  assert(director.sounds[0] == 0x110007); // Loop reentry starts it again.
  director.sound_puppet[0] = true;
  director.sprites[2].film_frame = 2;
  assert(dg_seek(&director,director.frame==1?2:1));
  assert(director.sounds[0] ==
         0x110007); // Source puppet sound takes precedence.

  // A "go <marker>" primary event script navigates like the go command.
  boot();
  busy = false;
  snprintf(director.mouse_down_script, sizeof(director.mouse_down_script),
           "go next");
  assert(director.frame == 1);
  tick(true);
  tick(false);
  assert(!values.failed && values.script_error_count == 0);
  assert(director.frame == 2);

  // An unresolvable primary recovers like the original script alert.
  boot();
  busy = false;
  snprintf(director.mouse_down_script, sizeof(director.mouse_down_script),
           "warp somewhere");
  tick(true);
  tick(false);
  assert(!values.failed);
  assert(values.script_error_count == 1);
  assert(strstr(values.last_script_error, "unresolved primary mouse handler"));
  tick(false); // the score keeps running after recovery
  assert(!values.failed);
  puts("Director 5 input propagation, packed casts, waits and media clock "
       "passed");
}
