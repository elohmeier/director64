// Probe-only controller inputs exercise the same keyboard and pointer as release.
#include <strings.h>
#include "director_replay.h"
#include "game.h"
#include <libdragon.h>
#include <string.h>
typedef struct { const char *movie; unsigned frame,ticks; int x,y; unsigned buttons; } replay_step_t;
#include "replay.inc"
static unsigned index_,elapsed,waiting;
static bool complete, notice_seen;
const char *director_replay_init(lv_runtime_t *r) {
  r->random_state=42;
  debugf("DIRECTOR64 REPLAY_START id=car-save-load native=%s\n",replay_source_sha256);
  return "START";
}
void director_replay_sample(input_sample_t *sample,dg_runtime_t *d) {
  if (*d->notice) notice_seen=true;
  if (d->values->script_error_count) {
    lv_fail(d->values,"unexpected script alert during controller replay");
    return;
  }
  *sample=(input_sample_t){.connected=true,.pointer_absolute=true,
    .pointer_x=320*INPUT_ONE,.pointer_y=240*INPUT_ONE};
  if(index_==sizeof(replay_steps)/sizeof(*replay_steps)) {
    if(!complete) {
      lv_runtime_t *r=d->values;
      int global=lv_global_id(r,"gmulleglobals");
      lv_t globals=global>=0?r->globals[global]:(lv_t){0};
      lv_t user=lv_type(globals)==LV_INSTANCE?lv_get(r,NULL,"user",globals):(lv_t){0};
      lv_t car=lv_type(user)==LV_INSTANCE?lv_get(r,NULL,"car",user):(lv_t){0};
      lv_t name=lv_type(car)==LV_INSTANCE?lv_get(r,NULL,"name",car):(lv_t){0};
      if(strcmp(d->movie->code->name,"03.DXR") || d->frame!=2 || game_save_generation()<2 ||
         lv_type(name)!=LV_STRING || strcasecmp(lv_cstr(r,name),"ROADSTER") ||
         !notice_seen || *d->notice) {
        lv_fail(r,"car save/load replay checkpoint");return;
      }
      debugf("DIRECTOR64 REPLAY_COMPLETE id=car-save-load movie=03.DXR name=ROADSTER notice=dismissed save=%u\n",game_save_generation());
      complete=true;
    }
    return;
  }
  const replay_step_t *step=&replay_steps[index_];
  if(*step->movie && (strcmp(d->movie->code->name,step->movie) || d->frame!=step->frame || d->values->depth)) {
    if(++waiting>18000)lv_fail(d->values,"controller replay wait timeout");
    return;
  }
  waiting=0;
  sample->pointer_x=step->x*INPUT_ONE;sample->pointer_y=step->y*INPUT_ONE;sample->buttons=step->buttons;
  if(++elapsed==step->ticks){elapsed=0;index_++;}
}
