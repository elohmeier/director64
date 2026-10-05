#include "director.h"
#include <assert.h>
#include <stdio.h>

static lv_runtime_t values;
static dg_runtime_t director;
static unsigned calls, first_exit;
static lv_flow_t empty(lv_runtime_t *r, lv_frame_t *f) {
  (void)f;
  if (!calls++) first_exit = ((dg_runtime_t *)r->context)->ticks;
  return LV_RETURN;
}
static lv_flow_t looping(lv_runtime_t *r, lv_frame_t *f) {
  if (!f->pc++) {
    if (!calls++) first_exit = ((dg_runtime_t *)r->context)->ticks;
    return lv_invoke(r, f, "go", 1, (lv_t[]){lv_num(1)});
  }
  return LV_RETURN;
}
static lv_flow_t presenting(lv_runtime_t *r, lv_frame_t *f) {
  // The feather loop commits all nine objects, including inactive ones.
  if (f->pc++ < 9) return lv_invoke(r, f, "updatestage", 0, NULL);
  calls++;
  return LV_RETURN;
}
static lv_flow_t drawing(lv_runtime_t *r, lv_frame_t *f) {
  dg_runtime_t *d = r->context;
  if (f->pc < 3) {
    d->sprites[1] = (dg_sprite_t){
        .value={.type=16,.blend=100,.x=10*(int)f->pc,.width=5,.height=5},
        .visible=true,.trails=true};
    dg_sprite_changed(d, 1);
    f->pc++;
    return lv_invoke(r, f, "updatestage", 0, NULL);
  }
  return LV_RETURN;
}
static lv_flow_t busy_presenting(lv_runtime_t *r, lv_frame_t *f) {
  calls++;
  return lv_invoke(r, f, "updatestage", 0, NULL);
}
static lv_flow_t presenting_delay(lv_runtime_t *r, lv_frame_t *f) {
  if (f->pc++ < 9) return lv_invoke(r, f, "updatestage", 0, NULL);
  if (f->pc == 10) return lv_invoke(r, f, "delay", 1, (lv_t[]){lv_num(3)});
  calls++;
  return LV_RETURN;
}
static lv_flow_t delayed(lv_runtime_t *r, lv_frame_t *f) {
  if (!f->pc++) return lv_invoke(r, f, "delay", 1, (lv_t[]){lv_num(60)});
  return LV_RETURN;
}
static const dg_cast_t cast = {"Internal", 1, 1};
static const dg_delta_t script = {.channel=0, .value={.script=0x110001}};
static const dg_frame_t plain[] = {{0,0},{0,0}};
static const dg_frame_t scripted[] = {{0,1},{1,0}};
static lv_handler_t handler;
static lv_movie_t code;
static dg_movie_t movie;

static void boot(unsigned tempo, lv_step_fn step, const char *event) {
  handler = (lv_handler_t){.name=event,.member=1,.cast="Internal",
                          .kind="BehaviorScript",.step=step};
  code = (lv_movie_t){"CLOCK.DXR",step?1:0,&handler,NULL,NULL,NULL,NULL,0};
  movie = (dg_movie_t){.code=&code,.id=1,.tempo=tempo,.cast_count=1,.casts=&cast,
                       .frame_count=2,.frames=step?scripted:plain,.deltas=&script};
  dg_init(&director,&values,(dg_platform_t){0},NULL,NULL,0,1);
  calls = first_exit = 0;
  assert(dg_enter(&director,&movie,1,NULL,0));
}
static void tick(void) { assert(dg_tick(&director,0,0,false,DG_SERVICE_BUDGET)); }

int main(void) {
  const unsigned rates[] = {1,2,3,4,5,7,8,9,11,13,25,50,60,120};
  for (unsigned mode=0;mode<3;mode++) {
    for (unsigned i=0;i<sizeof(rates)/sizeof(*rates);i++) {
      unsigned rate=rates[i];
      boot(rate,mode==0?NULL:mode==1?empty:looping,"exitframe");
      for (unsigned t=0;t<6000;t++) tick();
      if (mode<2) assert(director.frame_serial-1==rate*100);
      if (mode) {
        assert(calls==rate*100);
        assert(first_exit==(60+rate-1)/rate);
      }
      assert(director.ticks==6000);
    }
  }
  // Score rates persist through empty records and are reconstructed on jumps.
  static const dg_delta_t tempos[] = {
      {.channel=1,.value={.type=3}}, {.channel=1,.value={0}},
      {.channel=1,.value={.type=2}}, {.channel=1,.value={0}}};
  static const dg_frame_t timeline[] = {{0,0},{0,1},{1,1},{2,1},{3,1}};
  boot(10,NULL,"exitframe");
  movie.frames=timeline; movie.deltas=tempos; movie.frame_count=5;
  assert(dg_seek(&director,3) && director.tempo==3);
  assert(dg_seek(&director,5) && director.tempo==2);
  assert(dg_seek(&director,3) && director.tempo==3);
  assert(dg_seek(&director,1) && director.tempo==10);
  director.puppet_tempo=50;
  assert(dg_seek(&director,2) && director.tempo==3 && !director.puppet_tempo);
  director.puppet_tempo=50;
  assert(dg_seek(&director,3) && director.puppet_tempo==50);
  assert(dg_seek(&director,4) && !director.puppet_tempo && director.tempo==2);
  // D6's zero-filled inactive score record has raw blend 0 (100% opacity).
  // GARDEN returns to an early frame where later animation channels have no
  // record, then reuses them without a blend delta. Every activation must draw.
  static const dg_delta_t appearance[] = {{
      .channel=43,.mask=DG_MEMBER|DG_POSITION|DG_SIZE|DG_FLAGS,
      .value={.member=0x110053,.x=460,.y=272,.width=17,.height=43,
              .type=16,.blend=100}}, {
      .channel=44,.mask=DG_MEMBER|DG_FLAGS|DG_BLEND|DG_THICKNESS,
      .value={.member=0x110053,.type=16,.blend=0,.thickness=DG_HAS_BLEND}}};
  boot(3,NULL,"exitframe");
  movie.frames=(const dg_frame_t[]){{0,0},{0,2}};
  movie.deltas=appearance;
  for (unsigned activation=0;activation<4;activation++) {
    assert(dg_seek(&director,2));
    dg_update_stage(&director);
    assert(director.staged[38].value.type==16);
    assert(dg_opacity(&director.staged[38])==100);
    assert(director.staged[39].value.type==16);
    assert(dg_opacity(&director.staged[39])==0); // Authored transparency survives.
    assert(dg_seek(&director,1));
    assert(!director.sprites[38].value.type);
  }
  // Nine commits finish in one service tick without any framebuffer available.
  boot(3,presenting,"enterframe");
  tick(); assert(calls==1 && !values.depth && director.ticks==1);
  // Every trail pose survives, even when no physical render occurs between them.
  boot(3,drawing,"enterframe");
  tick();
  assert(!values.depth && director.trail_count==2);
  assert(director.trails[0].sprite.value.x==0);
  assert(director.trails[1].sprite.value.x==10);
  assert(director.staged[1].value.x==20);
  assert(!director.stage_count);
  dg_sprite_changed(&director, 1);
  dg_sprite_changed(&director, 1);
  assert(director.stage_count==1);
  dg_update_stage(&director);
  assert(director.trail_count==2 && !director.stage_count);
  // An unbounded updateStage loop cannot starve platform input/audio service.
  boot(3,busy_presenting,"enterframe");
  assert(dg_tick(&director,0,0,false,32) && calls==32 && values.depth);
  assert(dg_tick(&director,123,45,true,32) && calls==64 && director.ticks==2);
  assert(director.mouse_down && director.mouse_x==123 && director.mouse_y==45);
  // Removing display waits must not consume explicit delay ticks.
  boot(3,presenting_delay,"enterframe");
  tick(); assert(!calls && director.resume_tick==4);
  tick(); tick(); assert(!calls);
  tick(); assert(calls==1 && !values.depth);
  // A long script delay does not accumulate a burst of score frames.
  boot(120,delayed,"enterframe");
  for (unsigned i=0;i<60;i++) tick();
  assert(director.frame==1);
  tick(); assert(director.frame==2);
  // Long platform stalls are retained and drained in bounded batches.
  uint64_t phase=0; unsigned steps=0,total=0;
  for (unsigned i=0;i<1000;i++) {
    assert(dg_clock_advance(&phase,1000,&steps)); total+=steps;
  }
  assert(total==60 && !phase);
  assert(dg_clock_advance(&phase,1500000,&steps) && steps==4);
  total=steps;
  while (phase>=1000000) {
    assert(dg_clock_advance(&phase,0,&steps) && steps<=4); total+=steps;
  }
  assert(total==90 && !phase);
  assert(!dg_clock_advance(&phase,UINT64_MAX,&steps) && !phase);
  puts("Director timing: rates, frame events, jumps, stage commits, bounded waits and retained clock PASS");
}
