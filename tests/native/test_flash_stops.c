#include "director.h"
#include <assert.h>
#include <stdio.h>

static dg_runtime_t d;
static lv_runtime_t r;
static const uint16_t stops[] = {1,3};
static const dg_cast_t cast = {.name="Internal",.file=1,.cast=1};
static const dg_member_t members[] = {
  {.id=DG_ID(1,1,1),.number=1,.cast=1,.type=2,.film_count=4,.film_loop=1,
   .source_xtra=1,.flash_stop_count=2,.flash_stops=stops},
};
static const dg_delta_t delta = {.channel=6,.mask=DG_ALL,
  .value={.type=1,.member=DG_ID(1,1,1),.blend=100}};
static const dg_frame_t frames[] = {{0,1},{1,0},{1,0}};
static const lv_movie_t code = {.name="STOPS.DXR"};
static const dg_movie_t movie = {.code=&code,.id=1,.tempo=60,.cast_count=1,.casts=&cast,
  .member_count=1,.members=members,.frame_count=3,.frames=frames,.deltas=&delta};
static void call(const char *name, unsigned argc, const lv_t *args) {
  lv_t out={0}; bool handled=false;
  assert(r.services.call(&r,name,argc,args,&out,&handled));
  assert(!r.failed);
}
static void tick(void) { assert(dg_tick(&d,0,0,false,DG_SERVICE_BUDGET)); }
int main(void) {
  dg_init(&d,&r,(dg_platform_t){0},NULL,NULL,0,1);
  assert(dg_enter(&d,&movie,1,NULL,0));
  lv_t sprite=lv_make(LV_SPRITE,1);
  assert(d.sprites[1].film_frame==0 && d.sprites[1].film_paused);
  tick(); assert(d.sprites[1].film_frame==0);
  call("play",1,&sprite);
  tick(); assert(d.sprites[1].film_frame==1 && !d.sprites[1].film_paused);
  tick(); assert(d.sprites[1].film_frame==2 && d.sprites[1].film_paused);
  tick(); assert(d.sprites[1].film_frame==2);
  call("play",1,&sprite);
  tick(); assert(d.sprites[1].film_frame==3);
  tick(); assert(d.sprites[1].film_frame==0 && d.sprites[1].film_paused);
  call("gotoandplay",2,(lv_t[]){sprite,lv_num(3)});
  assert(d.sprites[1].film_frame==2 && d.sprites[1].film_paused);
  call("gotoandplay",2,(lv_t[]){sprite,lv_num(2)});
  assert(d.sprites[1].film_frame==1 && !d.sprites[1].film_paused);
  tick(); assert(d.sprites[1].film_frame==2 && d.sprites[1].film_paused);
  puts("Flash Stop actions apply on entry, advance and seek; Play resumes once");
}
