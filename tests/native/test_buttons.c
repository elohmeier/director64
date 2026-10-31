// Native controls toggle before mouseUp handlers and retain member state.
#include "buttons.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static dg_runtime_t d;
static lv_runtime_t r;
static unsigned calls;
static bool observed;
static lv_flow_t mouse_up(lv_runtime_t *v,lv_frame_t *frame) {
  (void)frame;calls++;observed=lv_truth(v,lv_get(v,NULL,"hilite",lv_make(LV_MEMBER,DG_ID(1,1,1))));
  return LV_RETURN;
}
static const lv_handler_t handlers[]={
 {.name="mouseup",.member=4,.kind="ScoreScript",.cast="Internal",.arguments=1,.locals=1,.step=mouse_up}};
static const lv_movie_t code={.name="BUTTONS.DXR",.handlers=handlers,.count=1};
static const dg_cast_t casts[]={{.name="Internal",.file=1,.cast=1}};
static const dg_member_t members[]={
 {.id=DG_ID(1,1,1),.number=1,.cast=1,.type=7,.shape=DG_BUTTON_CHECK,.width=16,.height=16,.text=""},
 {.id=DG_ID(1,1,2),.number=2,.cast=1,.type=7,.shape=DG_BUTTON_RADIO,.width=16,.height=16,.text=""},
 {.id=DG_ID(1,1,3),.number=3,.cast=1,.type=7,.shape=DG_BUTTON_PUSH,.width=40,.height=16,.text="Go"},
 {.id=DG_ID(1,1,4),.number=4,.cast=1,.type=11},
 {.id=DG_ID(1,1,5),.number=5,.cast=1,.type=7,.shape=DG_BUTTON_CHECK,.filled=1},
 {.id=DG_ID(1,1,6),.number=6,.cast=1,.type=7,.filled=7}};
static const dg_frame_t frames[]={{0,0}};
static const dg_movie_t movie={.id=1,.code=&code,.tempo=30,.casts=casts,.cast_count=1,
 .members=members,.member_count=6,.frames=frames,.frame_count=1};
static bool hit(void *ctx,const dg_member_t *member,unsigned channel,int x,int y) {
 (void)ctx;(void)member;(void)channel;(void)x;(void)y;return true;
}
static void tick(int x,int y,bool down) {
 bool ok=dg_tick(&d,x,y,down,10000);
 if(!ok||r.failed||r.script_error_count)fprintf(stderr,"button tick %d,%d/%d: %s / %s\n",x,y,down,r.error,r.last_script_error);
 assert(ok&&!r.failed&&!r.script_error_count);
}
static void click(int x,int y) {tick(x,y,false);tick(x,y,true);tick(x,y,false);}
int main(void) {
 dg_init(&d,&r,(dg_platform_t){.hit=hit},NULL,NULL,0,1);assert(dg_enter(&d,&movie,1,NULL,0));
 for(unsigned i=1;i<=3;i++){d.sprites[i]=(dg_sprite_t){.visible=true,.puppet=true,
  .value={.member=members[i-1].id,.script=i==1?members[3].id:0,.type=16,
   .x=(int16_t)((i-1)*50),.width=members[i-1].width,.height=16,.blend=100}};dg_sprite_changed(&d,i);}
 lv_t behavior=lv_instance(&r,lv_make(LV_SCRIPT,members[3].id));
 r.roots[DG_BEHAVIOR_ROOT+1]=lv_list(&r,1,&behavior,false);
 lv_t check=lv_make(LV_MEMBER,members[0].id);
 assert(!lv_truth(&r,lv_get(&r,NULL,"hilite",check)));
 assert(!dg_button_kind(&members[5])&&!dg_button_hilite(&d,members[5].id));
 assert(!dg_text_editable(&d,1));
 assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"type",check)),"button"));
 click(5,5);assert(calls==1&&observed&&dg_button_hilite(&d,members[0].id));
 click(5,5);assert(calls==2&&!observed&&!dg_button_hilite(&d,members[0].id));
 tick(5,5,true);tick(200,200,false);assert(!dg_button_hilite(&d,members[0].id));
 click(55,5);click(55,5);assert(dg_button_hilite(&d,members[1].id));
 tick(105,5,true);assert(dg_button_hilite(&d,members[2].id));
 tick(200,200,false);assert(!dg_button_hilite(&d,members[2].id));
 lv_set(&r,NULL,"hilite",check,lv_num(1));assert(dg_button_hilite(&d,members[0].id));
 assert(d.field_count==0);
 assert(dg_button_hilite(&d,members[4].id));
 dg_button_set_hilite(&d,members[4].id,false);assert(!dg_button_hilite(&d,members[4].id));
 lv_collect(&r);assert(dg_button_hilite(&d,members[0].id));
 lv_set(&r,NULL,"text",check,lv_text(&r,"caption",false));assert(dg_button_hilite(&d,members[0].id));
 lv_set(&r,NULL,"hilite",check,lv_num(0));assert(!dg_button_hilite(&d,members[0].id));
 assert(!r.failed);
#if DG_D10
 dg_images_dispose(&d);
#endif
 puts("Native buttons: state, GC, checkbox default before mouseUp, radio and push cancellation passed");
}
