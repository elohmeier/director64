#include "../../platforms/android/jni/audio_position.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
  audio_start_position_t p=audio_start_position(600,60,true,120,360,7.25,-1);
  assert(p.looping && p.position==195);
  p=audio_start_position(600,60,true,120,360,8.25,7.25);
  assert(!p.looping && p.position==255); // Decode completes one active second after release.
  p=audio_start_position(600,60,true,120,360,14,7.25);
  assert(!p.looping && p.position==600); // Pending release has already reached the tail end.
  p=audio_start_position(600,60,true,120,360,1.5,0.5);
  assert(!p.looping && p.position==90); // Release during the intro does not skip to the loop.
  p=audio_start_position(600,60,true,0,0,12,11);
  assert(!p.looping && p.position==120 && p.loop_start==0 && p.loop_end==600);
  p=audio_start_position(600,60,true,120,1000,10.5,-1);
  assert(p.looping && p.position==150 && p.loop_end==600);
  p=audio_start_position(600,60,false,120,360,8.25,7.25);
  assert(!p.looping && p.position==495); // Non-looping playback ignores release metadata.
  puts("Android audio: pending decode release preserves intro, wrapping and tail timing");
}
