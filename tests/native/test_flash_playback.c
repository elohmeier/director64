#include "director.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static dg_runtime_t d;
static lv_runtime_t r;
static const lv_movie_t code = {.name = "FLASH"};
static const dg_cast_t cast = {.name="Internal",.file=1,.cast=1};
static uint32_t positions[120];
static dg_member_t members[] = {
    {.id = DG_ID(1, 1, 1),
     .number = 1,
     .cast = 1,
     .type = 2,
     .source_xtra = 1,
     .film_count = 120,
     .film_loop = 1,
     .flash_rate = 12 * 65536,
     .flash_fixed_rate = 15 * 65536},
    {.id = DG_ID(1, 1, 2),
     .number = 2,
     .cast = 1,
     .type = 2,
     .source_xtra = 1,
     .film_count = 120,
     .film_loop = 1,
     .flash_rate = 12 * 65536,
     .flash_fixed_rate = 15 * 65536,
     .video_audio = "stream.wav64",
     .samples = 6000,
     .rate = 600,
     .flash_audio_positions = positions},
};
static const dg_delta_t deltas[] = {
    {.channel = 6,
     .mask = DG_ALL,
     .value = {.type = 1, .member = DG_ID(1, 1, 1)}},
    {.channel = 7,
     .mask = DG_ALL,
     .value = {.type = 1, .member = DG_ID(1, 1, 1)}},
};
static const dg_frame_t frames[] = {{0, 2}};
static dg_movie_t movie = {.id = 1,
                           .code = &code,
                           .casts = &cast,
                           .cast_count = 1,
                           .members = members,
                           .member_count = 2,
                           .tempo = 60,
                           .frame_count = 1,
                           .frames = frames,
                           .deltas = deltas};
static lv_t sprite(unsigned n) { return lv_make(LV_SPRITE, (int32_t)n); }
static void ticks(unsigned n) {
  while (n--) {
    assert(dg_tick(&d, 0, 0, false, DG_SERVICE_BUDGET));
    assert(!r.failed);
  }
}
static void boot(unsigned tempo) {
  movie.tempo = tempo;
  dg_init(&d, &r, (dg_platform_t){0}, NULL, NULL, 0, 1);
  assert(dg_enter(&d, &movie, 1, NULL, 0));
  d.sprites[1].puppet = d.sprites[2].puppet = true;
}
static void set(const char *name, lv_t owner, lv_t value) {
  lv_set(&r, NULL, name, owner, value);
  assert(!r.failed);
}
static void call(const char *name, unsigned count, lv_t *args) {
  lv_t out = 0;
  bool yield = false;
  assert(r.services.call(&r, name, count, args, &out, &yield));
  assert(!r.failed);
}
int main(void) {
  for (unsigned i = 0; i < 120; i++)
    positions[i] = i * 50;
  boot(60);
  assert(lv_numeric(lv_get(&r,NULL,"framerate",lv_make(LV_MEMBER,DG_ID(1,1,1))))==12);
  ticks(60);
  assert(d.sprites[1].film_frame == 12 && d.sprites[2].film_frame == 12);
  boot(6);
  ticks(60);
  assert(d.sprites[1].film_frame ==
         6); // Normal is capped by Director frame opportunities.
  boot(60);
  set("playbackmode", sprite(1), lv_symbol(&r, "fixed"));
  set("fixedrate", sprite(1), lv_num(30));
  ticks(60);
  assert(d.sprites[1].film_frame == 30 && d.sprites[2].film_frame == 12);
  assert(lv_numeric(lv_get(&r, NULL, "fixedrate", sprite(1))) == 30);
  assert(!strcmp(lv_cstr(&r, lv_get(&r, NULL, "playbackmode", sprite(1))),
                 "fixed"));
  boot(60);
  set("playbackmode", lv_make(LV_MEMBER, DG_ID(1, 1, 1)),
      lv_symbol(&r, "lockStep"));
  ticks(60);
  assert(d.sprites[1].film_frame == 60 && d.sprites[2].film_frame == 60);
  boot(60);
  set("member", sprite(1), lv_make(LV_MEMBER, DG_ID(1, 1, 2)));
  ticks(30);
  dg_sprite_t *s = &d.sprites[1];
  assert(s->film_frame == 6 && fabs(s->video_time - 300) < 1e-6 &&
         s->video_rate == 1);
  call("stop", 1, (lv_t[]){sprite(1)});
  ticks(30);
  assert(s->film_frame == 6 && fabs(s->video_time - 300) < 1e-6 &&
         !s->video_rate);
  unsigned serial = s->video_serial;
  call("play", 1, (lv_t[]){sprite(1)});
  ticks(30);
  assert(s->film_frame == 12 && fabs(s->video_time - 600) < 1e-6 &&
         s->video_serial == serial);
  call("gotoandstop", 2, (lv_t[]){sprite(1), lv_num(61)});
  assert(s->film_frame == 60 && fabs(s->video_time - 3000) < 1e-6 &&
         !s->video_rate);
  call("play", 1, (lv_t[]){sprite(1)});
  ticks(300);
  assert(s->film_frame == 0 && s->video_time < 1e-6 &&
         s->video_serial > serial);
  // A stream skips pictures to preserve its sound clock at a slow host rate.
  boot(6);
  set("member", sprite(1), lv_make(LV_MEMBER, DG_ID(1, 1, 2)));
  ticks(60);
  assert(d.sprites[1].film_frame == 12 && d.sprites[2].film_frame == 6);
  // Missing stream blocks retain a preceding block; seeks into a gap use
  // its earlier sample anchor. Leading empty frames remain silent.
  positions[0] = UINT32_MAX;
  positions[2] = UINT32_MAX;
  boot(60);
  set("member", sprite(1), lv_make(LV_MEMBER, DG_ID(1, 1, 2)));
  ticks(1);
  assert(!d.sprites[1].video_rate);
  call("gotoandplay", 2, (lv_t[]){sprite(1), lv_num(3)});
  assert(fabs(d.sprites[1].video_time - 100) < 1e-6 &&
         d.sprites[1].video_rate == 1);
  ticks(1);
  assert(d.sprites[1].video_time > 100 && d.sprites[1].video_rate == 1);
  members[0].video_flags = 256;
  boot(60);
  ticks(30);
  assert(d.sprites[1].film_frame == 0 && d.sprites[1].film_paused);
  lv_t member = lv_make(LV_MEMBER, DG_ID(1, 1, 1));
  assert(lv_truth(&r, lv_get(&r, NULL, "pausedatstart", member)));
  set("pausedatstart", member, lv_num(0));
  assert(
      d.sprites[1]
          .film_paused); // Applies to a new appearance, not an active playhead.
  call("play", 1, (lv_t[]){sprite(1)});
  ticks(30);
  assert(d.sprites[1].film_frame == 6 && d.sprites[2].film_frame == 0);
  members[0].video_flags = 0;
  // Direct-to-stage uses its own Normal clock even while Director waits.
  members[0].flash_direct_to_stage = 1;
  boot(1);
  ticks(60);
  assert(d.sprites[1].film_frame == 12 && d.sprites[2].film_frame == 12);
  members[0].flash_direct_to_stage = 0;
  positions[0] = 0;
  positions[2] = 100;
  static const uint16_t stops[] = {2};
  members[1].flash_stops = stops;
  members[1].flash_stop_count = 1;
  boot(1);
  set("member", sprite(1), lv_make(LV_MEMBER, DG_ID(1, 1, 2)));
  ticks(60);
  assert(d.sprites[1].film_frame == 1 && d.sprites[1].film_paused &&
         d.sprites[1].video_time == 50);
  puts("Flash playback: Normal/Fixed/Lock-Step, independent overrides, stream "
       "clock, pause, seek and loop passed");
}
