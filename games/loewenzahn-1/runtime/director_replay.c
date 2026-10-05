// Probe-only pointer playback. No gameplay globals or script calls are injected.
#include "director_replay.h"
#include <libdragon.h>
#include <string.h>
typedef struct {
  unsigned ticks; int x,y; unsigned down; const char *movie;
  unsigned volumes[DG_SOUND_CHANNELS];
  unsigned tempo, sprite, sprite_member, sound_channel, sound_member;
  bool check_cursor;
  dg_cursor_t cursor;
  unsigned hot_x, hot_y;
} replay_step_t;
#include "replay.inc"
static unsigned index_, elapsed;
static bool complete;
const char *director_replay_init(lv_runtime_t *r) {
  r->random_state=42; // Same seed as the sanitized native journey.
  debugf("DIRECTOR64 REPLAY_START id=%s source=%s\n",replay_id,replay_source_sha256);
  return "START";
}
void director_replay_sample(input_sample_t *sample,dg_runtime_t *d) {
  *sample=(input_sample_t){.connected=true,.pointer_absolute=true,
                          .pointer_x=320*INPUT_ONE,.pointer_y=240*INPUT_ONE};
  if(index_ && !elapsed && strcmp(d->movie->code->name,replay_steps[index_-1].movie)) {
    debugf("DIRECTOR64 REPLAY_ERROR step=%u expected=%s actual=%s\n",index_-1,
           replay_steps[index_-1].movie,d->movie->code->name);
    lv_fail(d->values,"replay scene differs from native checkpoint");return;
  }
  if(index_ && !elapsed && !complete) {
    const replay_step_t *expected=&replay_steps[index_-1];
    if(expected->check_cursor) {
      dg_cursor_t actual=dg_cursor_current(d);
      dg_cursor_bitmap_t bitmap={0};
      if(actual.image) {
        const dg_member_t *m=dg_member(d,actual.image);
        if(!m && d->suspended_stage) m=dg_member(d->suspended_stage,actual.image);
        if(!m) {lv_fail(d->values,"cursor replay member missing");return;}
        dg_cursor_hotspot(&bitmap,m->reg_x,m->reg_y);
      } else (void)dg_cursor_builtin(&bitmap,actual.resource);
      if(!dg_cursor_equal(actual,expected->cursor) || bitmap.hot_x!=expected->hot_x ||
          bitmap.hot_y!=expected->hot_y) {
        debugf("DIRECTOR64 REPLAY_ERROR cursor step=%u image=%lu mask=%lu resource=%ld\n",
               index_-1,(unsigned long)actual.image,(unsigned long)actual.mask,(long)actual.resource);
        lv_fail(d->values,"cursor differs from native checkpoint");return;
      }
      debugf("DIRECTOR64 REPLAY_CURSOR step=%u image=%lu mask=%lu hotspot=%u,%u\n",
             index_-1,(unsigned long)actual.image,(unsigned long)actual.mask,bitmap.hot_x,bitmap.hot_y);
    }
    for(unsigned ch=0;ch<DG_SOUND_CHANNELS;ch++) {
      if(d->channel_volume[ch]!=expected->volumes[ch]) {
        debugf("DIRECTOR64 REPLAY_ERROR step=%u sound=%u expected=%u actual=%u\n",
               index_-1,ch+1,expected->volumes[ch],d->channel_volume[ch]);
        lv_fail(d->values,"replay volume differs from native checkpoint");return;
      }
    }
    if(!strcmp(d->movie->code->name,"DIALOG.DXR"))
      debugf("DIRECTOR64 REPLAY_VOLUME step=%u channels=%u,%u,%u,%u\n",index_-1,
             d->channel_volume[0],d->channel_volume[1],d->channel_volume[2],d->channel_volume[3]);
    if ((expected->tempo && d->clock_tempo != expected->tempo) ||
        (expected->sprite && d->sprites[expected->sprite].value.member != expected->sprite_member) ||
        (expected->sound_channel && d->sounds[expected->sound_channel - 1] != expected->sound_member)) {
      debugf("DIRECTOR64 REPLAY_ERROR step=%u tempo=%u sprite=%u sound=%u\n",
             index_-1,d->clock_tempo,expected->sprite,expected->sound_channel);
      lv_fail(d->values,"regression state differs from native checkpoint");return;
    }
    if (expected->tempo || expected->sprite || expected->sound_channel)
      debugf("DIRECTOR64 REGRESSION_CHECK id=%s step=%u tempo=%u sprite=%u member=%u sound=%u sound_member=%u\n",
             replay_id,index_-1,d->clock_tempo,expected->sprite,expected->sprite_member,
             expected->sound_channel,expected->sound_member);
  }
  if(index_==sizeof(replay_steps)/sizeof(*replay_steps)) {
    if(!complete) {
      debugf("DIRECTOR64 REPLAY_COMPLETE id=%s movie=%s frame=%u tick=%lu\n",
             replay_id,d->movie->code->name,d->frame,(unsigned long)d->ticks);complete=true;
    }
    return;
  }
  const replay_step_t *step=&replay_steps[index_];
  sample->pointer_x=step->x*INPUT_ONE;sample->pointer_y=step->y*INPUT_ONE;
  sample->buttons=step->down; // Recorded INPUT_A / INPUT_B mask; zero releases.
  if(++elapsed==step->ticks){elapsed=0;index_++;}
}
