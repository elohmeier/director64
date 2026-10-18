#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static dg_runtime_t director;
static lv_runtime_t values;
#include "../../platforms/native/audio.inc"
int main(void) {
  dg_member_t loop = {.samples=600, .rate=60, .loop_start=120, .loop_end=360, .looping=1};
  sound(NULL, 0, &loop);
  director.ticks = 435;
  assert(busy(NULL,0) && position(NULL,0)==3250);
  sound_pause(NULL,0,true);
  director.ticks = 1000;
  sound_break_loop(NULL,0);
  sound_break_loop(NULL,0);
  assert(position(NULL,0)==3250 && busy(NULL,0));
  sound_pause(NULL,0,false);
  director.ticks = 1404;
  assert(busy(NULL,0) && position(NULL,0)==9983);
  director.ticks++;
  assert(!busy(NULL,0) && position(NULL,0)==10000);
  loop.loop_start=0; loop.loop_end=0;
  sound(NULL,0,&loop); // Invalid/empty region loops the whole sound.
  director.ticks += 601;
  assert(position(NULL,0)==16 && busy(NULL,0));
  sound_break_loop(NULL,0);
  director.ticks += 599;
  assert(!busy(NULL,0));
  sound(NULL,0,NULL);
  assert(!busy(NULL,0) && position(NULL,0)==0 && sound_count==3);
  loop.samples=UINT32_MAX; loop.rate=48000; loop.looping=0;
  sound(NULL,0,&loop);
  director.ticks += 60000;
  assert(position(NULL,0)==1000000 && busy(NULL,0));
  assert(!values.failed);
  puts("native probe audio: loop release, pause, whole-sound fallback and long samples passed");
}
