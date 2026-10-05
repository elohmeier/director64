#include "director.h"
#include <assert.h>
#include <stdio.h>
static dg_runtime_t d;
static lv_runtime_t r;
static unsigned delivered, loops, idle_calls;
static lv_flow_t idle(lv_runtime_t *v,lv_frame_t *f) {
  (void)v;(void)f;idle_calls++;return LV_RETURN;
}
static lv_flow_t behavior(lv_runtime_t *v,lv_frame_t *f) {
  (void)v;
  // A yield must retain the broadcast cursor and the following receiver.
  if (!f->pc++) return LV_YIELD;
  assert(d.current_event_sprite==delivered%3+1);
  delivered++;
  return LV_RETURN;
}
static lv_flow_t frame(lv_runtime_t *v,lv_frame_t *f) {
  if (!f->pc++) {
    assert(delivered%3==2 && d.current_event_sprite==0);
    delivered++;loops++;
    return lv_invoke(v,f,"go",1,(lv_t[]){lv_num(d.frame)});
  }
  return LV_RETURN;
}
static const lv_handler_t handlers[]={
 {.name="exitframe",.member=1,.cast="Internal",.kind="BehaviorScript",.arguments=1,.locals=1,.step=behavior},
 {.name="exitframe",.member=3,.cast="Internal",.kind="BehaviorScript",.locals=1,.step=frame},
 {.name="mouseup",.member=4,.cast="Internal",.kind="BehaviorScript",.arguments=1,.locals=1,.step=behavior},
 {.name="idle",.member=5,.cast="Internal",.kind="MovieScript",.step=idle}};
static const lv_movie_t code={"LOOP.DXR",4,handlers,NULL, NULL, NULL, NULL, 0};
static const dg_cast_t casts[]={{"Internal",1,1}};
static const dg_member_t members[]={
 {.id=0x110001,.number=1,.cast=1,.type=11,.name="loop behavior"},
 {.id=0x110002,.number=2,.cast=1,.type=1,.width=10,.height=10,.name="Plane"}};
static const dg_behavior_t behaviors[]={{0x110001,""}};
static const dg_delta_t deltas[]={
 {.channel=0,.mask=DG_ALL,.value={.script=0x110003}},
 {.channel=6,.mask=DG_ALL,.value={.type=16,.member=0x110002,.behaviors=behaviors,.behavior_count=1}},
 {.channel=7,.mask=DG_ALL,.value={.type=16,.member=0x110002,.behaviors=behaviors,.behavior_count=1}}};
static const dg_frame_t frames[]={{0,3}};
static const dg_movie_t movie={.code=&code,.id=1,.tempo=30,.casts=casts,.cast_count=1,
 .members=members,.member_count=2,.deltas=deltas,.frames=frames,.frame_count=1};
int main(void) {
 dg_init(&d,&r,(dg_platform_t){0},NULL,NULL,0,1);
 assert(dg_enter(&d,&movie,1,NULL,0));
 for(unsigned i=0;i<120;i++) {
   assert(dg_tick(&d,50,50,false,2));
   lv_collect(&r);
 }
 assert(loops>=10 && idle_calls>=10 && idle_calls<=120 && !r.failed);
 // Animated cursor behavior above a mouse receiver must not steal its input.
 d.sprites[2].visible=false;
 lv_set(&r,NULL,"member",lv_make(LV_SPRITE, 3),lv_make(LV_MEMBER, 0x110002));
 lv_t cursor=lv_instance(&r,lv_make(LV_SCRIPT, 0x110001));
 lv_set(&r,NULL,"scriptinstancelist",lv_make(LV_SPRITE, 3),lv_list(&r,1,&cursor,false));
 lv_t button=lv_instance(&r,lv_make(LV_SCRIPT, 0x110004));
 lv_set(&r,NULL,"scriptinstancelist",lv_make(LV_SPRITE, 1),lv_list(&r,1,&button,false));
 assert(dg_hit(&d,5,5)==3 && dg_mouse_hit(&d,5,5)==1);
 lv_set(&r,NULL,"member",lv_make(LV_SPRITE, 4),lv_make(LV_MEMBER, 0x110002));
 assert(dg_hit(&d,5,5)==4 && dg_mouse_hit(&d,5,5)==1 && !r.failed);
 puts("D6 looping frame scripts preserve yielding sprite frame events");
}
