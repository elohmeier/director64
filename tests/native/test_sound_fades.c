#include "director.h"
#include <assert.h>
#include <stdio.h>

// Macromedia's MX Lingo Dictionary, fadeIn/fadeOut/fadeTo (p.229):
// object methods use milliseconds, fadeIn starts at zero and returns to the
// previous volume, and fadeTo can run in either direction. Actual LZ8 calls
// use 1500 ms and 1 ms; Deutsch relies on the one-second default.
static dg_runtime_t d;
static lv_runtime_t r;
static const lv_movie_t code = {.name="FADES.DXR"};
static const dg_frame_t frames[] = {{0,0}};
static const dg_movie_t movie = {.code=&code,.id=1,.tempo=30,
  .frame_count=1,.frames=frames};
static lv_t voice = lv_make(LV_SOUND,1);
static void call(const char *name, unsigned argc, const lv_t *args) {
  lv_t out={0}; bool yielded=false;
  assert(r.services.call(&r,name,argc,args,&out,&yielded));
  assert(!r.failed);
}
static void ticks(unsigned n) {
  while (n--) assert(dg_tick(&d,0,0,false,DG_SERVICE_BUDGET));
}
static void volume(unsigned n) { lv_set(&r,NULL,"volume",voice,lv_num(n)); }
int main(void) {
  dg_init(&d,&r,(dg_platform_t){0},NULL,NULL,0,1);
  assert(dg_enter(&d,&movie,1,NULL,0));
  volume(200);
  call("fadeIn",1,&voice);
  assert(d.channel_volume[0]==0);
  ticks(30); assert(d.channel_volume[0]==100);
  ticks(30); assert(d.channel_volume[0]==200 && !d.fade_duration[0]);
  call("fadeTo",3,(lv_t[]){voice,lv_num(80),lv_num(1500)});
  ticks(45); assert(d.channel_volume[0]==140);
  // Replacing a fade starts from its current level, with no volume jump.
  call("fadeTo",3,(lv_t[]){voice,lv_num(200),lv_num(1000)});
  assert(d.channel_volume[0]==140);
  ticks(30); assert(d.channel_volume[0]==170);
  ticks(30); assert(d.channel_volume[0]==200);
  call("fadeOut",1,&voice);
  ticks(30); assert(d.channel_volume[0]==100);
  volume(190); ticks(30); assert(d.channel_volume[0]==190);
  call("fadeTo",3,(lv_t[]){voice,lv_num(80),lv_num(1)});
  assert(d.channel_volume[0]==190);
  ticks(1); assert(d.channel_volume[0]==80);
  call("fadeTo",3,(lv_t[]){voice,lv_num(0),lv_num(0)});
  assert(d.channel_volume[0]==0);
  call("fadeIn",1,&voice); ticks(60); assert(d.channel_volume[0]==0);
  volume(200);
  call("sound_fadein",2,(lv_t[]){lv_num(1),lv_num(4)});
  assert(d.channel_volume[0]==0);
  ticks(1); assert(d.channel_volume[0]==50);
  ticks(3); assert(d.channel_volume[0]==200);
  call("sound_fadeout",2,(lv_t[]){lv_num(1),lv_num(4)});
  ticks(2); assert(d.channel_volume[0]==100);
  ticks(2); assert(d.channel_volume[0]==0);
  call("stop",1,&voice); assert(d.channel_volume[0]==200);
  puts("Timed channel fades, replacement, volume override and legacy tick units passed");
}
