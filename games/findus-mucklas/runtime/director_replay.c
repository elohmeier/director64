// Probe-only input: never change a source variable or bypass a game handler.
#include <strings.h>
#include "director_replay.h"
#include "text_input.h"
#include <libdragon.h>
#include <string.h>
#ifndef DIRECTOR64_REPLAY_SCENARIO
#define DIRECTOR64_REPLAY_SCENARIO 1
#endif
static unsigned phase, age, ready_tick;
static int pointer_x=320*INPUT_ONE, pointer_y=240*INPUT_ONE;
static const bool obstacle = DIRECTOR64_REPLAY_SCENARIO == 2;
static const bool departure = DIRECTOR64_REPLAY_SCENARIO == 3;
static const bool soak = DIRECTOR64_REPLAY_SCENARIO == 4;
static unsigned moving_tick, piston_updates;
static int32_t piston_rotation;
static bool motion_reported;
const char *director_replay_init(lv_runtime_t *values) {(void)values;return "START";}
static lv_t global(dg_runtime_t *d,const char *name) {
  int id=lv_global_id(d->values,name);
  return id<0?(lv_t){0}:d->values->globals[id];
}
// Without case: Lingo compares text so, and a state named by a symbol is
// spelled as the script that named it.
static bool is(lv_runtime_t *r,lv_t value,const char *name){return !strcasecmp(lv_cstr(r,value),name);}
static void press(input_sample_t *sample,dg_runtime_t *d,unsigned buttons) {
  sample->buttons=buttons;age=d->ticks;phase++;
}
static bool click(input_sample_t *sample,dg_runtime_t *d,unsigned id) {
  int l,t,r,b;dg_bounds(d,id,&l,&t,&r,&b);
  if (!d->sprites[id].visible||r<=l||b<=t) return false;
  sample->pointer_x=pointer_x=(l+r)/2*INPUT_ONE;
  sample->pointer_y=pointer_y=(t+b)/2*INPUT_ONE;
  debugf("DIRECTOR64 ACTIVITY_CLICK movie=%s sprite=%u phase=%u\n",d->movie->code->name,id,phase);
  press(sample,d,INPUT_A);return true;
}
static void answer_train(input_sample_t *sample, dg_runtime_t *d) {
  lv_runtime_t *r = d->values;
  lv_t board = global(d, "gtavlaobj"), passengers = global(d, "gpassageraregenerellobj");
  if (lv_type(board) != LV_INSTANCE || lv_type(passengers) != LV_INSTANCE ||
      !lv_truth(r, lv_get(r, NULL, "pvantapaforstafragan", board)) ||
      !lv_truth(r, global(d, "gljudok"))) return;
  unsigned number = lv_count(r, lv_get(r, NULL, "pantalpassagerarepastationen", passengers));
  if (!number || number > 30) { lv_fail(r, "replay train passenger count"); return; }
  int l, t, right, bottom;
  dg_bounds(d, 14, &l, &t, &right, &bottom);
  // TS.DXR's original number board: inset 13,15 and 37x39 cells.
  sample->pointer_x = pointer_x = (l + 13 + (int)((number-1)%10)*37 + 18)*INPUT_ONE;
  sample->pointer_y = pointer_y = (t + 15 + (int)((number-1)/10)*39 + 18)*INPUT_ONE;
  debugf("DIRECTOR64 ACTIVITY_TRAIN_ANSWER number=%u\n", number);
  press(sample, d, INPUT_A);
}
static void train_motion(dg_runtime_t *d) {
  lv_t train = global(d, "gtagetobj");
  if (lv_type(train) != LV_INSTANCE ||
      !lv_truth(d->values, lv_get(d->values, NULL, "ptagetflytta", train))) return;
  if (!moving_tick) moving_tick = d->ticks;
  for (unsigned channel = 37; channel <= 38; channel++) {
    float q[8];
    dg_sprite_quad(d, &d->sprites[channel], q);
    for (unsigned i = 0; i < 8; i++)
      if (!(q[i] >= 0 && q[i] <= (i%2 ? 480 : 640))) {
        lv_fail(d->values, "replay train piston outside stage"); return;
      }
  }
  if (motion_reported) return;
  int32_t rotation = d->sprites[39].value.rotation;
  if (rotation != piston_rotation) { piston_rotation = rotation; piston_updates++; }
  if (d->ticks-moving_tick >= 120 && piston_updates >= 12) {
    debugf("DIRECTOR64 ACTIVITY_TRAIN_MOTION ticks=%u updates=%u\n",
           (unsigned)(d->ticks-moving_tick), piston_updates);
    motion_reported = true;
  }
}
// Scenario 4: ride the train indefinitely, answering every station question
// through the original board, so a long capture measures whether station play
// degrades. The expected number mirrors the kollatavelknappen checks.
static unsigned soak_arrivals, soak_last_click, soak_last_report, soak_last_progress;
static bool soak_moving, soak_ready;
static void soak_train(input_sample_t *sample, dg_runtime_t *d) {
  lv_runtime_t *r = d->values;
  if (!soak_ready) {
    soak_ready = true;
    debugf("DIRECTOR64 ACTIVITY_READY movie=%s frame=%u tick=%lu\n",
           d->movie->code->name, d->frame, (unsigned long)d->ticks);
  }
  lv_t train = global(d, "gtagetobj");
  if (lv_type(train) == LV_INSTANCE &&
      lv_truth(r, lv_get(r, NULL, "ptagetflytta", train))) {
    soak_moving = true;
    return;
  }
  if (soak_moving) {
    soak_moving = false;
    soak_arrivals++;
    soak_last_progress = d->ticks;
    debugf("DIRECTOR64 ACTIVITY_TRAIN_ARRIVAL count=%u tick=%lu\n",
           soak_arrivals, (unsigned long)d->ticks);
  }
  if (d->ticks - soak_last_click < 60) return;
  lv_t board = global(d, "gtavlaobj");
  lv_t passengers = global(d, "gpassageraregenerellobj");
  lv_t questions = global(d, "gfragalist");
  if (lv_type(board) != LV_INSTANCE || lv_type(passengers) != LV_INSTANCE ||
      lv_type(questions) != LV_PROPLIST) return;
  if (d->ticks - soak_last_report > 1800) {
    soak_last_report = d->ticks;
    debugf("DIRECTOR64 ACTIVITY_TRAIN_STATE tick=%lu status=%d question=%d "
           "vantasvar=%d vantaljud=%d ljudok=%d station=%u av=%u tag=%u\n",
           (unsigned long)d->ticks,
           (int)lv_number(r, global(d, "gtsspelstatus")),
           (int)lv_number(r, lv_get(r, NULL, "vilkenfraga", questions)),
           (int)lv_number(r, lv_get(r, NULL, "pvantapaforstafragan", board)),
           (int)lv_number(r, lv_get(r, NULL, "pvantapaantalljud", board)),
           (int)lv_number(r, global(d, "gljudok")),
           lv_count(r, lv_get(r, NULL, "pantalpassagerarepastationen", passengers)),
           lv_count(r, lv_get(r, NULL, "pantaltemppassagerarepastationen", passengers)),
           lv_count(r, lv_get(r, NULL, "pantalpassagerarepataget", passengers)));
  }
  if (d->ticks - soak_last_progress > 5400) {
    // Stalled at a station: click the locomotive driver, whose original
    // handler replays the active question and rearms the board check.
    soak_last_progress = d->ticks;
    soak_last_click = d->ticks;
    sample->pointer_x = pointer_x = 85 * INPUT_ONE;
    sample->pointer_y = pointer_y = 350 * INPUT_ONE;
    debugf("DIRECTOR64 ACTIVITY_TRAIN_POKE tick=%lu\n", (unsigned long)d->ticks);
    sample->buttons = INPUT_A;
    return;
  }
  if (!lv_truth(r, lv_get(r, NULL, "pvantapaforstafragan", board)) ||
      lv_truth(r, lv_get(r, NULL, "pvantapaantalljud", board)) ||
      !lv_truth(r, global(d, "gljudok"))) return;
  int question = (int)lv_number(r, lv_get(r, NULL, "vilkenfraga", questions));
  unsigned number;
  if (question == 1)
    number = lv_count(r, lv_get(r, NULL, "pantalpassagerarepastationen", passengers));
  else if (question == 2)
    number = lv_count(r, lv_get(r, NULL, "pantaltemppassagerarepastationen", passengers));
  else if (question == 4)
    number = lv_count(r, lv_get(r, NULL, "pantalpassagerarepataget", passengers));
  else if (question == 5)
    number = lv_count(r, lv_index_get(r, lv_get(r, NULL, "porgpassagerarepatagetplats", passengers), lv_num(1))) -
             lv_count(r, lv_index_get(r, lv_get(r, NULL, "ppassagerarepatagetplats", passengers), lv_num(1)));
  else
    return;
  if (!number || number > 30) return;
  int l, t, right, bottom;
  dg_bounds(d, 14, &l, &t, &right, &bottom);
  sample->pointer_x = pointer_x = (l + 13 + (int)((number - 1) % 10) * 37 + 18) * INPUT_ONE;
  sample->pointer_y = pointer_y = (t + 15 + (int)((number - 1) / 10) * 39 + 18) * INPUT_ONE;
  debugf("DIRECTOR64 ACTIVITY_TRAIN_ANSWER number=%u question=%d\n", number, question);
  sample->buttons = INPUT_A;
  soak_last_click = d->ticks;
}
void director_replay_sample(input_sample_t *sample,dg_runtime_t *d) {
  *sample=(input_sample_t){.connected=true,.pointer_absolute=true,.pointer_x=pointer_x,.pointer_y=pointer_y};
  if (!d->movie || d->values->depth) return;
  if (departure && phase >= 8 && !strcmp(d->movie->code->name,"TS.DXR")) train_motion(d);
  bool lo=!strcmp(d->movie->code->name,"LO.DXR"), house=!strcmp(d->movie->code->name,"HUSET.DXR");
  if (phase==0&&lo&&is(d->values,global(d,"lo_state"),"valjer")) click(sample,d,17);
  else if(phase==1&&lo&&d->ticks-age>120)click(sample,d,39);
  else if(phase==2&&d->ticks-age>30) {
    if(!text_input_active()){lv_fail(d->values,"replay name keyboard did not open");return;}
    press(sample,d,INPUT_A); // First on-screen key is A.
  } else if(phase==3&&d->ticks-age>12)press(sample,d,INPUT_START);
  else if(phase==4&&lo&&d->ticks-age>30) {
    const dg_member_t *m=dg_member(d,d->sprites[39].value.member);
    if(!m||!is(d->values,lv_get(d->values,NULL,"text",lv_make(LV_MEMBER, (int32_t)m->id)),"A")) {
      lv_fail(d->values,"replay source name entry failed");return;
    }
    debugf("DIRECTOR64 ACTIVITY_NAME_TYPED value=A\n");click(sample,d,40);
  } else if(phase==5&&house&&d->ticks-age>2400)click(sample,d,obstacle?11:14);
  else if(phase==6&&house&&d->ticks-age>2400)click(sample,d,12);
  else if(phase==7&&!strcmp(d->movie->code->name,obstacle?"BR.DXR":"TS.DXR")) {
    if(obstacle) {
      lv_t course=global(d,"bana");
      if(lv_type(course)==LV_INSTANCE&&is(d->values,lv_get(d->values,NULL,"introstate",course),"vantapaklick"))click(sample,d,46);
    } else if(soak) {
      if(d->frame==7) soak_train(sample,d);
    } else if(d->frame==7&&d->ticks-age>2400) {
      if (departure) answer_train(sample, d);
      else click(sample,d,14);
    }
  } else if(phase==8&&d->ticks-age>600) {
    if(obstacle) {
      lv_t course=global(d,"bana");
      if(lv_type(course)!=LV_INSTANCE||!is(d->values,lv_get(d->values,NULL,"vy",course),"sidan"))return;
    }
    ready_tick=d->ticks;phase++;
    debugf("DIRECTOR64 ACTIVITY_READY movie=%s frame=%u tick=%lu\n",d->movie->code->name,d->frame,(unsigned long)d->ticks);
  } else if(phase==9&&d->ticks-ready_tick>1800) {
    // Exercise another input during capture, beyond an idle entrance.
    if(obstacle){sample->pointer_x=pointer_x=550*INPUT_ONE;press(sample,d,INPUT_A);}
    else click(sample,d,14);
    debugf("DIRECTOR64 ACTIVITY_INTERACTION movie=%s\n",d->movie->code->name);
  }
}
