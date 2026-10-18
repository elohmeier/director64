#include "director.h"
#include <assert.h>
#include <stdio.h>

static dg_runtime_t d;
static lv_runtime_t r;
static bool paused;
static unsigned pauses, resumes;
static unsigned releases;
static void sound_break_loop(void *ctx, unsigned channel) {
  (void)ctx;
  assert(channel == 1);
  releases++;
}
static void sound_pause(void *ctx, unsigned channel, bool on) {
  (void)ctx;
  assert(channel == 1);
  paused = on;
  if (on) pauses++; else resumes++;
}
static const dg_cast_t cast = {.name="Internal",.file=1,.cast=1};
static const dg_member_t members[] = {
  {.id=DG_ID(1,1,1),.number=1,.cast=1,.type=2,.film_count=3,.film_loop=1},
  {.id=DG_ID(1,1,2),.number=2,.cast=1,.type=2,.film_count=3,.film_loop=1},
};
static const lv_movie_t code = {.name="PLAYBACK.DXR"};
static const dg_delta_t delta = {.channel=6,.mask=DG_ALL,
  .value={.type=1,.member=DG_ID(1,1,1),.blend=100}};
static const dg_frame_t frames[] = {{0,1},{1,0},{1,0}};
static const dg_movie_t movie = {.code=&code,.id=1,.tempo=60,.cast_count=1,.casts=&cast,
  .member_count=2,.members=members,.frame_count=3,.frames=frames,.deltas=&delta};
static void call(const char *name, unsigned argc, const lv_t *args) {
  lv_t out = {0};
  bool handled = false;
  assert(r.services.call(&r,name,argc,args,&out,&handled));
  assert(!r.failed);
}
static void tick(void) { assert(dg_tick(&d,0,0,false,DG_SERVICE_BUDGET)); }
int main(void) {
  dg_init(&d,&r,(dg_platform_t){.sound_pause=sound_pause,.sound_break_loop=sound_break_loop},NULL,NULL,0,1);
  assert(dg_enter(&d,&movie,1,NULL,0));
  lv_t sprite = lv_make(LV_SPRITE,1), sound = lv_make(LV_SOUND,2);
  assert(d.sprites[1].value.member == members[0].id);
  call("gotoframe",2,(lv_t[]){sprite,lv_num(2)});
  call("stop",1,&sprite);
  for (unsigned i=0;i<8;i++) tick();
  assert(d.sprites[1].film_frame == 1);
  call("play",1,&sprite);
  tick();
  assert(d.sprites[1].film_frame == 2);
  tick();
  assert(d.sprites[1].film_frame == 0);
  call("gotoandstop",2,(lv_t[]){sprite,lv_num(3)});
  tick();
  assert(d.sprites[1].film_frame == 2);
  call("rewind",1,&sprite);
  tick();
  assert(d.sprites[1].film_frame == 0);
  call("gotoandplay",2,(lv_t[]){sprite,lv_num(2)});
  tick();
  assert(d.sprites[1].film_frame == 2);
  call("stop",1,&sprite);
  lv_set(&r,NULL,"member",sprite,lv_make(LV_MEMBER,members[1].id));
  assert(!r.failed && d.sprites[1].film_frame == 0 && !d.sprites[1].film_paused);
  call("pause",1,&sound);
  assert(paused && pauses == 1);
  call("breakLoop",1,&sound);
  assert(releases == 1 && paused);
  call("play",1,&sound);
  assert(!paused && resumes == 1);
  call("breakLoop",1,&sound);
  assert(releases == 2 && !paused);
  d.platform.sound_pause = NULL;
  lv_t out = {0};
  bool handled = false;
  assert(r.services.call(&r,"pause",1,&sound,&out,&handled));
  assert(r.failed); // unsupported adapters must not report a successful pause
  puts("Sprite stop/resume/seek/rewind and channel pause dispatch passed");
}
