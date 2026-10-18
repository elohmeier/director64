// Synthetic QuickTime/DirectMedia controls; no authored media or scripts.
#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static dg_runtime_t d;
static lv_runtime_t r;
static const lv_movie_t code={.name="VIDEO.DXR"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
#define ID(n) DG_ID(1,1,n)
static const dg_member_t members[]={
  {.id=ID(1),.number=1,.cast=1,.type=10,.source_xtra=4,.width=320,.height=240,.samples=1200,.video_flags=8},
  {.id=ID(2),.number=2,.cast=1,.type=10,.source_xtra=3,.width=320,.height=240,.samples=1200,.video_flags=8|256},
  {.id=ID(3),.number=3,.cast=1,.type=10,.source_xtra=4,.samples=1200,.video_flags=16},
};
static const dg_delta_t deltas[]={
  {.channel=6,.mask=DG_ALL,.value={.type=1,.member=ID(1),.blend=100}},
  {.channel=7,.mask=DG_ALL,.value={.type=1,.member=ID(2),.blend=100}},
  {.channel=8,.mask=DG_ALL,.value={.type=1,.member=ID(3),.blend=100}},
};
static const dg_frame_t frames[]={{0,3}};
static const dg_movie_t movie={.id=1,.code=&code,.casts=&cast,.cast_count=1,
  .members=members,.member_count=3,.frames=frames,.frame_count=1,.deltas=deltas,.tempo=60};
static lv_t sprite(unsigned n) {return lv_make(LV_SPRITE,n);}
static double get(const char *key,unsigned n) {return lv_number(&r,lv_get(&r,NULL,key,sprite(n)));}
static void set(const char *key,unsigned n,double value) {lv_set(&r,NULL,key,sprite(n),lv_num(value));assert(!r.failed);}
static void tick(unsigned n) {while(n--)assert(dg_tick(&d,0,0,false,DG_SERVICE_BUDGET));}
static void call(const char *name,unsigned argc,const lv_t *args) {
  lv_t out=0;bool yield=false;assert(r.services.call(&r,name,argc,args,&out,&yield));assert(!r.failed);
}
int main(void) {
  dg_init(&d,&r,(dg_platform_t){0},NULL,NULL,0,1);assert(dg_enter(&d,&movie,1,NULL,0));
  assert(dg_is_video(&members[0])&&dg_is_video(&members[1]));
  assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"type",lv_make(LV_MEMBER,ID(1)))),"quickTimeMedia"));
  assert(lv_number(&r,lv_get(&r,NULL,"duration",lv_make(LV_MEMBER,ID(1))))==120);
  assert(lv_number(&r,lv_get(&r,NULL,"duration",lv_make(LV_MEMBER,ID(2))))==2000);
  assert(get("movierate",1)==1 && get("movierate",2)==0);
  tick(30);assert(get("movietime",1)==30 && get("currenttime",1)==500 && get("movietime",2)==0);
  set("volume",1,128);assert(get("volume",1)==128 && d.sprites[1].video_volume==128);
  set("movierate",1,0);tick(3);assert(get("movietime",1)==30);
  unsigned serial=d.sprites[1].video_serial;set("movietime",1,70);assert(get("movietime",1)==70&&d.sprites[1].video_serial>serial);
  set("movierate",1,2);tick(25);assert(get("movietime",1)==120&&get("movierate",1)==0);
  set("movierate",1,-1);tick(120);assert(get("movietime",1)==0);tick(1);assert(get("movierate",1)==0);
  set("starttime",1,20);set("stoptime",1,40);set("movierate",1,1);tick(30);
  assert(get("movietime",1)==40&&get("movierate",1)==0);
  set("starttime",3,10);set("stoptime",3,20);set("movietime",3,19);set("movierate",3,2);tick(1);
  assert(get("movietime",3)==11&&get("movierate",3)==2);
  assert(lv_number(&r,lv_get(&r,NULL,"loop",lv_make(LV_MEMBER,ID(3))))==1);
  lv_set(&r,NULL,"loop",lv_make(LV_MEMBER,ID(3)),lv_num(0));tick(10);
  assert(get("movietime",3)==20&&get("movierate",3)==0);
  call("videoseek",2,(lv_t[]){sprite(2),lv_num(500)});assert(get("movietime",2)==30);
  call("videoplay",1,(lv_t[]){sprite(2)});tick(1);assert(get("movietime",2)==31);
  call("videopause",1,(lv_t[]){sprite(2)});tick(1);assert(get("movietime",2)==31);
  call("videostop",1,(lv_t[]){sprite(2)});assert(get("movietime",2)==0&&get("movierate",2)==0);
  assert(!r.failed);
  puts("QuickTime: identity, tick duration, playback, pause, seek, volume, bounds, looping; DirectMedia milliseconds passed");
}
