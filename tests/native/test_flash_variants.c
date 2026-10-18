#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static dg_runtime_t d;
static lv_runtime_t r;
static const char *const poses[]={"0a","0b","0c","1b","1c","1a"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
static const dg_member_t members[]={
  {.id=DG_ID(1,1,1),.number=1,.cast=1,.type=2,.film_count=2,.film_loop=1,
   .source_xtra=1,.film_assets=poses,.flash_variant_count=3},
  {.id=DG_ID(1,1,2),.number=2,.cast=1,.type=1},
};
static const dg_delta_t deltas[]={
  {.channel=6,.mask=DG_ALL,.value={.type=1,.member=DG_ID(1,1,1),.blend=100}},
  {.channel=7,.mask=DG_ALL,.value={.type=1,.member=DG_ID(1,1,1),.blend=100}},
};
static const dg_frame_t frames[]={{0,2},{2,0},{2,0}};
static const lv_movie_t code={.name="VARIANTS.DXR"};
static const dg_movie_t movie={.code=&code,.id=1,.tempo=60,.cast_count=1,.casts=&cast,
  .member_count=2,.members=members,.frame_count=3,.frames=frames,.deltas=deltas};
static void call(const char *name,unsigned argc,const lv_t *args) {
  lv_t out={0};bool handled=false;
  assert(r.services.call(&r,name,argc,args,&out,&handled));assert(!r.failed);
}
static void tick(void) {assert(dg_tick(&d,0,0,false,DG_SERVICE_BUDGET));}
static void asset(unsigned channel,const char *name) {
  assert(!strcmp(dg_film_asset(members,&d.sprites[channel]),name));
}
int main(void) {
  dg_init(&d,&r,(dg_platform_t){0},NULL,NULL,0,1);
  assert(dg_enter(&d,&movie,1,NULL,0));
  assert(d.sprites[1].flash_variant==1 && d.sprites[2].flash_variant==2);
  asset(1,"0a");asset(2,"0b");
  uint32_t seed=r.random_state;
  dg_sprite_changed(&d,1);assert(r.random_state==seed);
  tick();asset(1,"1b");asset(2,"1c");
  tick();asset(1,"0c");asset(2,"0a"); // Root loops after2; child loops after3.
  lv_t sprite=lv_make(LV_SPRITE,1);
  call("gotoandplay",2,(lv_t[]){sprite,lv_num(2)});
  asset(1,"1c");assert(d.sprites[1].flash_variant==1 && r.random_state==seed);
  call("stop",1,&sprite);tick();asset(1,"1a"); // Nested child keeps playing.
  assert(d.sprites[1].film_paused && d.sprites[1].film_frame==1);
  tick();asset(1,"1b");
  d.sprites[1].value.member=DG_ID(1,1,2);dg_sprite_changed(&d,1);
  assert(d.sprites[1].flash_variant==0 && d.sprites[1].flash_clock==0);
  d.sprites[1].value.member=DG_ID(1,1,1);dg_sprite_changed(&d,1);
  assert(d.sprites[1].flash_variant && d.sprites[1].flash_clock==0 && r.random_state!=seed);
  puts("Flash instances retain random phase and independent child clocks across root loops, seek and pause");
}
