// Source-free SWA lifecycle and last mouse-press contracts, also in D6-D8.
#include "director.h"
#include <assert.h>
#include <stdio.h>

static dg_runtime_t d;
static lv_runtime_t r;
static bool busy;
static unsigned plays;
#define ID(n) DG_ID(1,1,n)
static const lv_movie_t code = {.name="SWA.DXR"};
static const dg_member_t members[] = {
  {.id=ID(1),.number=1,.cast=1,.type=6,.source_xtra=6,.name="Voice",
   .asset="voice.wav64",.text="@:SND:voice.swa",.rate=1000,.samples=2500},
  {.id=ID(2),.number=2,.cast=1,.type=6,.source_xtra=6,.name="Absent",.text="@:absent.swa"},
};
static const dg_frame_t frames[] = {{0,0}};
static const dg_movie_t movie = {.id=1,.code=&code,.members=members,.member_count=2,
  .frames=frames,.frame_count=1,.tempo=30};
static void sound(void *ctx,unsigned channel,const dg_member_t *m) {
  (void)ctx; assert(channel==4); busy=m!=NULL; plays++;
}
static bool sound_busy(void *ctx,unsigned channel) {(void)ctx;return channel==4&&busy;}
static lv_t member(unsigned n) {return lv_make(LV_MEMBER,ID(n));}
static double get(unsigned n,const char *name) {return lv_number(&r,lv_get(&r,NULL,name,member(n)));}
static void call(unsigned n,const char *name) {
  lv_t out=0,arg=member(n); bool yield=false;
  assert(r.services.call(&r,name,1,&arg,&out,&yield)); assert(!r.failed);
}
int main(void) {
  dg_init(&d,&r,(dg_platform_t){.sound=sound,.sound_busy=sound_busy},NULL,NULL,0,1);
  assert(dg_enter(&d,&movie,1,NULL,0));
  assert(get(1,"state")==0&&get(1,"duration")==2500);
  call(1,"preloadbuffer"); assert(get(1,"state")==2&&get(1,"percentstreamed")==100);
  lv_set(&r,NULL,"volume",member(1),lv_num(73));
  call(1,"play_frame");
  assert(get(1,"state")==3&&d.sounds[4]==ID(1)&&d.channel_volume[4]==73&&plays==1);
  busy=false; assert(get(1,"state")==5);
  call(1,"play"); call(1,"stop"); assert(get(1,"state")==0&&!busy&&d.sounds[4]==0);
  unsigned before=plays;
  call(2,"preloadbuffer"); assert(get(2,"state")==9&&get(2,"percentstreamed")==0);
  call(2,"play"); assert(get(2,"state")==9&&plays==before);
  assert(dg_tick(&d,31,47,true,DG_SERVICE_BUDGET));
  assert(dg_tick(&d,99,101,false,DG_SERVICE_BUDGET));
  lv_t point=lv_get(&r,NULL,"clickloc",0);
  assert(lv_number(&r,lv_at(&r,point,1))==31&&lv_number(&r,lv_at(&r,point,2))==47);
  assert(!r.failed);
  puts("SWA lifecycle, completion, independent channel, volume, missing-file error and clickLoc passed");
}
