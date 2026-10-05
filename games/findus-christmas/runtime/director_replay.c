// Probe-only input follows the same pointer path as manual release play.
#include "director_replay.h"
#include <libdragon.h>
#include <string.h>
typedef struct { const char *movie; unsigned frame,ticks; int x,y; unsigned buttons; int objects; } replay_step_t;
#include "replay.inc"
static unsigned index_,elapsed,waiting;
static bool complete;
const char *director_replay_init(lv_runtime_t *r) {
  r->random_state=42;
  debugf("DIRECTOR64 REPLAY_START id=christmas-activities native=%s\n",replay_source_sha256);
  return "START";
}
void director_replay_sample(input_sample_t *sample,dg_runtime_t *d) {
  *sample=(input_sample_t){.connected=true,.pointer_absolute=true,
    .pointer_x=320*INPUT_ONE,.pointer_y=240*INPUT_ONE};
  if(index_==sizeof(replay_steps)/sizeof(*replay_steps)) {
    if(!complete) {
      debugf("DIRECTOR64 REPLAY_COMPLETE id=christmas-activities movie=%s frame=%u\n",d->movie->code->name,d->frame);
      complete=true;
    }
    return;
  }
  const replay_step_t *step=&replay_steps[index_];
  bool ready=(!*step->movie || (!strcmp(d->movie->code->name,step->movie) && d->frame==step->frame));
  if(step->objects>=0) {
    int id=lv_global_id(d->values,"gantalobj");
    ready &= id>=0 && lv_integer(d->values,d->values->globals[id])==step->objects;
  }
  if(!ready) {
    if(++waiting>18000)lv_fail(d->values,"Christmas replay wait timeout");
    return;
  }
  waiting=0;
  sample->pointer_x=step->x*INPUT_ONE;sample->pointer_y=step->y*INPUT_ONE;sample->buttons=step->buttons;
  if(++elapsed==step->ticks){elapsed=0;index_++;}
}
