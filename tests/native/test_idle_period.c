// Source-free idle scheduling: cadence, yielding, wrap, zero and isolation.
#include "director.h"
#include <assert.h>
#include <stdio.h>

static dg_runtime_t director, other;
static lv_runtime_t values, other_values;
static unsigned calls, entered;
static bool yield_idle;
static lv_flow_t idle(lv_runtime_t *r, lv_frame_t *f) {
  (void)r;
  if(yield_idle && !f->pc++)return LV_YIELD;
  calls++;
  return LV_RETURN;
}
static lv_flow_t enter(lv_runtime_t *r, lv_frame_t *f) {
  (void)r;(void)f;entered++;return LV_RETURN;
}
static const lv_handler_t handlers[]={
  {.name="idle",.member=1,.kind="MovieScript",.step=idle},
  {.name="enterframe",.member=1,.kind="MovieScript",.step=enter},
};
static const lv_movie_t code={.name="IDLE.DXR",.count=2,.handlers=handlers};
static const dg_frame_t frames[2]={{0}};
static const dg_movie_t movie={.code=&code,.id=1,.tempo=10,.frames=frames,.frame_count=2};
static void set_period(lv_runtime_t *r,int32_t period) {
  lv_set(r,NULL,"idlehandlerperiod",(lv_t){0},lv_num(period));
  assert(!r->failed);
  assert(lv_integer(r,lv_get(r,NULL,"idlehandlerperiod",(lv_t){0}))==period);
}
static void boot(int32_t period) {
  dg_init(&director,&values,(dg_platform_t){0},NULL,NULL,0,42);
  assert(lv_integer(&values,lv_get(&values,NULL,"idlehandlerperiod",(lv_t){0}))==
         (DG_D7_UP?1:0));
  assert(dg_enter(&director,&movie,1,NULL,0));
  calls=entered=0;yield_idle=false;
  set_period(&values,period);
}
static void ticks(unsigned count) {
  for(unsigned i=0;i<count;i++)assert(dg_tick(&director,0,0,false,DG_SERVICE_BUDGET));
  assert(!values.failed && !values.script_error_count);
}
int main(void) {
  unsigned periods[]={1,2,6,60};
  for(unsigned i=0;i<sizeof(periods)/sizeof(*periods);i++) {
    boot((int32_t)periods[i]);ticks(60);
    assert(calls==60/periods[i] && entered>=10 && entered<=11);
    unsigned before=calls;
    for(unsigned n=0;n<10;n++)assert(dg_service(&director,DG_SERVICE_BUDGET));
    assert(calls==before); // Host polling cannot shorten a positive period.
  }
  boot(2);ticks(60);assert(calls==30);
  set_period(&values,6);unsigned before=calls;ticks(60);assert(calls-before==10);
  boot(6);director.ticks=UINT32_MAX-3;ticks(12);assert(calls==2);
  boot(2);yield_idle=true;ticks(60);
  assert(calls>=29 && calls<=30 && entered>=9);
  boot(2);yield_idle=true;ticks(1);assert(!calls && values.depth);
  set_period(&values,6);ticks(60);assert(calls==10 && entered>=9);
  for(int period=0;period>=-1;period--) {
    boot(period);ticks(1);assert(calls>1 && calls<DG_SERVICE_BUDGET);
    before=calls;assert(dg_service(&director,DG_SERVICE_BUDGET));assert(calls>before);
    ticks(59);assert(entered>=9 && calls<61*DG_SERVICE_BUDGET);
  }
  boot(6);
  dg_init(&other,&other_values,(dg_platform_t){0},NULL,NULL,0,42);
  assert(lv_integer(&other_values,lv_get(&other_values,NULL,"idlehandlerperiod",(lv_t){0}))==
         (DG_D7_UP?1:0));
  set_period(&other_values,2);
  assert(lv_integer(&values,lv_get(&values,NULL,"idlehandlerperiod",(lv_t){0}))==6);
  dg_init(&other,&other_values,(dg_platform_t){0},NULL,NULL,0,42);
  assert(other.idle_handler_period==(DG_D7_UP?1:0));
  puts("idle: periods 1/2/6/60, bounded zero, yielding, frame progress, wrap and isolation pass");
}
