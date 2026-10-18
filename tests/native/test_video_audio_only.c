// A source-free QuickTime sound track, with no picture track. The 600-unit
// internal duration intentionally ends between Director's 60 Hz ticks.
#include "web_runtime.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static dg_runtime_t director;
static lv_runtime_t values;
static web_host_t host;
#include "../../platforms/web/video.inc"

static unsigned updates, closes, serial;
static double position, rate;
static void video(void *ctx,unsigned id,const char *picture,const char *audio,
                  double time,double speed,unsigned generation,float gain) {
  (void)ctx;assert(id==1);
  if(!picture){closes++;return;}
  assert(!*picture && !strcmp(audio,"synthetic.wav64"));
  assert(gain>=0 && gain<=1);
  updates++;position=time;rate=speed;serial=generation;
}
static const lv_movie_t code={.name="AUDIO-ONLY.DXR"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
static const dg_member_t member={.id=DG_ID(1,1,1),.number=1,.cast=1,.type=10,
  .samples=12222,.rate=600,.video_flags=8,.video_audio="synthetic.wav64"};
static const dg_delta_t delta={.channel=6,.mask=DG_ALL,
  .value={.type=1,.member=DG_ID(1,1,1),.blend=100}};
static const dg_frame_t frame={0,1};
static const dg_movie_t movie={.id=1,.code=&code,.casts=&cast,.cast_count=1,
  .members=&member,.member_count=1,.tempo=60,.frames=&frame,.frame_count=1,.deltas=&delta};
static lv_t sprite(void){return lv_make(LV_SPRITE,1);}
static void set(const char *property,int value) {
  lv_set(&values,NULL,property,sprite(),lv_num(value));
  dg_update_stage(&director);update_videos();assert(!values.failed);
}
static void ticks(unsigned count) {
  while(count--)assert(dg_tick(&director,-100,-100,false,DG_SERVICE_BUDGET));
  update_videos();assert(!values.failed);
}
int main(void) {
  host.video=video;
  dg_init(&director,&values,(dg_platform_t){0},NULL,NULL,0,1);
  assert(dg_enter(&director,&movie,1,NULL,0));update_videos();
  assert(updates==1 && !closes && position==0 && rate==1);
  assert(lv_integer(&values,lv_get(&values,NULL,"duration",lv_make(LV_MEMBER,member.id)))==1223);
  ticks(60);assert(position==600 && rate==1);
  set("movierate",0);unsigned paused_serial=serial;
  ticks(120);assert(position==600 && rate==0 && serial==paused_serial);
  set("movietime",120);assert(position==1200 && rate==0 && serial!=paused_serial);
  set("movierate",1);ticks(1);assert(position==1210 && rate==1);
  set("movietime",1222);ticks(1);
  assert(position==12222 && rate==0); // exact media end, no synthetic frame needed
  assert(lv_integer(&values,lv_get(&values,NULL,"movietime",sprite()))==1223);
  unsigned done_serial=serial;ticks(10);
  assert(position==12222 && rate==0 && serial==done_serial);
  set("starttime",10);set("stoptime",15);set("movierate",1);ticks(5);
  assert(position==150 && rate==0);
  set("stoptime",0);set("movietime",0);set("movierate",1);ticks(1);
  assert(position==10 && rate==1);
  lv_set(&values,NULL,"member",sprite(),lv_num(0));
  dg_update_stage(&director);update_videos();assert(closes==1);
  close_videos();assert(closes==1 && !values.failed);
  puts("D5 audio-only media: duration, clock, pause, seek, completion, stopTime and host cleanup passed");
}
