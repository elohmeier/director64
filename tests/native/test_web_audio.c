#include "web_runtime.h"
#include <assert.h>
#include <stdio.h>

static dg_runtime_t director;
static lv_runtime_t values;
static web_host_t host;
#include "../../platforms/web/audio.inc"

static unsigned starts, pauses, releases;
static void play(void *ctx, unsigned channel, const char *asset, bool looping,
                  uint32_t loop_start, uint32_t loop_end) {
  (void)ctx;
  assert(channel==0 && asset && looping && loop_start==120 && loop_end==360);
  starts++;
}
static void pause_voice(void *ctx, unsigned channel, bool paused) {
  (void)ctx; (void)paused;
  assert(channel==0);
  pauses++;
}
static void release(void *ctx, unsigned channel) {
  (void)ctx;
  assert(channel==0);
  releases++;
}
int main(void) {
  host=(web_host_t){.sound=play,.sound_pause=pause_voice,.sound_break_loop=release};
  const dg_member_t loop={.asset="loop",.looping=1,.samples=600,.rate=60,
    .loop_start=120,.loop_end=360};
  sound(NULL,0,&loop);
  assert(starts==1 && sound_count==1 && sound_busy(NULL,0));
  director.ticks=435; // 7.25 seconds, after one pass through [2,6).
  assert(sound_position(NULL,0)==3250);
  sound_pause(NULL,0,true);
  director.ticks=1000;
  assert(sound_position(NULL,0)==3250 && sound_busy(NULL,0));
  sound_break_loop(NULL,0);
  sound_break_loop(NULL,0);
  assert(releases==1 && sound_position(NULL,0)==3250);
  sound_pause(NULL,0,false);
  assert(pauses==2 && busy_until[0]==1405);
  director.ticks=1060;
  assert(sound_position(NULL,0)==4250 && sound_busy(NULL,0));
  director.ticks=1405;
  assert(!sound_busy(NULL,0) && sound_position(NULL,0)==10000);
  sound(NULL,0,&loop); // A new play restores authored looping.
  director.ticks+=435;
  assert(sound_position(NULL,0)==3250 && sound_busy(NULL,0));
  sound_break_loop(NULL,0);
  assert(releases==2 && busy_until[0]==director.ticks+405);
  assert(!values.failed);
  sound(NULL,0,&loop);
  host.sound_break_loop=NULL;
  sound_break_loop(NULL,0);
  assert(values.failed); // An audible host cannot silently omit the operation.
  puts("web audio: wrapped positions, paused loop release, tail lifetime and host failure passed");
}
