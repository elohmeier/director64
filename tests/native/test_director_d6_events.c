#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static dg_runtime_t d;
static lv_runtime_t r;
static unsigned begins, ended, stopped, clicks, keys, idles;
static const lv_t member = lv_make(LV_MEMBER, 0x110004);
static lv_flow_t begin(lv_runtime_t *v, lv_frame_t *f) {
  begins++; lv_set(v,f,"enteredheld",f->locals[0],lv_num(0));return LV_RETURN;
}
static lv_flow_t end(lv_runtime_t *v, lv_frame_t *f) {
  assert(d.frame==1 && !dg_transition_ready(&d));
  if (!f->pc++) return LV_YIELD;
  unsigned identity=(unsigned)lv_integer(v,lv_get(v,f,"identity",f->locals[0]));
  ended |= 1u << identity;
  // A departing callback can mutate attachments, but it must not cancel the
  // other outgoing receiver or allow its instance to be garbage-collected.
  lv_set(v,f,"scriptinstancelist",lv_make(LV_SPRITE, 1),lv_list(v,0,NULL,false));
  return LV_RETURN;
}
static lv_flow_t stop(lv_runtime_t *v, lv_frame_t *f) {
  (void)v;assert(!dg_transition_ready(&d));
  if (!f->pc++) return LV_YIELD;
  stopped++;return LV_RETURN;
}
static lv_flow_t enter(lv_runtime_t *v, lv_frame_t *f) {
  lv_set(v,f,"enteredheld",f->locals[0],lv_num(d.mouse_down));return LV_RETURN;
}
static lv_flow_t leave(lv_runtime_t *v, lv_frame_t *f) {
  lv_set(v,f,"enteredheld",f->locals[0],lv_num(0));return LV_RETURN;
}
static lv_flow_t up(lv_runtime_t *v, lv_frame_t *f) {
  if (!lv_truth(v,lv_get(v,f,"enteredheld",f->locals[0]))) clicks++;
  return LV_RETURN;
}
static lv_flow_t key(lv_runtime_t *v, lv_frame_t *f) {
  keys++;
  lv_t text=lv_get(v,f,"text",member);
  if (lv_count(v,text)>3)
    lv_set(v,f,"text",member,lv_chunk_delete(v,"char",member,lv_num(4),lv_num(4)));
  lv_call(v,f,"pass",0,NULL);
  return LV_RETURN;
}
static lv_flow_t idle(lv_runtime_t *v, lv_frame_t *f) {(void)v;(void)f;idles++;return LV_RETURN;}
static const char *const properties[]={"dragging"};
#define H(name_, id_, fn_, kind_) {.name=name_,.member=id_,.cast="Internal",.kind=kind_,.arguments=1,.locals=1,.step=fn_,.property_count=1,.property_names=properties}
static const lv_handler_t handlers[]={
 H("beginsprite",1,begin,"BehaviorScript"),H("endsprite",1,end,"BehaviorScript"),
 H("endsprite",2,end,"BehaviorScript"),H("mouseenter",1,enter,"BehaviorScript"),
 H("mouseleave",1,leave,"BehaviorScript"),H("mouseup",1,up,"BehaviorScript"),
 H("keydown",3,key,"BehaviorScript"),H("idle",99,idle,"MovieScript"),
 H("stopmovie",99,stop,"MovieScript")};
static const lv_movie_t code={"DATA.CXT",sizeof(handlers)/sizeof(*handlers),handlers,NULL, NULL, NULL, NULL, 0};
static const dg_cast_t casts[]={{"Internal",1,1}};
static const uint8_t advances[95]={[87-32]=11,[73-32]=3};
static const int16_t kerning[]={87,73,-10};
static const dg_text_style_t style={.size=24,.line_height=24,.advances=advances,.kerning=kerning,.kerning_count=1};
static const dg_member_t members[]={
 {.id=0x110003,.cast=1,.number=3,.type=1,.width=30,.height=30,.name="Button",.text="",.asset=""},
 {.id=0x110004,.cast=1,.number=4,.type=3,.width=100,.height=24,.name="Name",.text="",.text_style=&style}};
static const dg_behavior_t button[]={{0x110001,"[#identity:1]"},{0x110002,"[#identity:2]"}};
static const dg_behavior_t edit[]={{0x110003,""}};
static const dg_delta_t deltas[]={
 {.channel=6,.mask=DG_ALL,.value={.type=16,.member=0x110003,.width=30,.height=30,.behaviors=button,.behavior_count=2}},
 {.channel=8,.mask=DG_ALL,.value={.type=16,.member=0x110004,.x=100,.width=100,.height=24,.behaviors=edit,.behavior_count=1}},
 {.channel=6,.mask=DG_ALL,.value={0}}};
static const dg_frame_t frames[]={{0,2},{2,1}};
static const dg_movie_t movie={.code=&code,.id=1,.tempo=1,.cast_count=1,.casts=casts,
 .member_count=2,.members=members,.frame_count=2,.frames=frames,.deltas=deltas};
static void boot(void) {
 dg_init(&d,&r,(dg_platform_t){0},NULL,NULL,0,1);
 begins=ended=stopped=clicks=keys=idles=0;
 assert(dg_enter(&d,&movie,1,NULL,0));assert(dg_tick(&d,50,50,false,1000));
}
static void tick(int x,int y,bool down) {assert(dg_tick(&d,x,y,down,1000));}
static void edit_contract(void) {
 boot();lv_frame_t caller={.movie=&code,.handler=&handlers[0]};
 assert(!dg_text_editable(&d,3));
 lv_set(&r,NULL,"editable",member,lv_num(1));assert(dg_text_editable(&d,3));
 lv_set(&r,NULL,"editabletext",lv_make(LV_SPRITE, 3),lv_num(0));
 assert(!dg_edit_text(&d,3,"ABCD"));
 lv_set(&r,NULL,"editabletext",lv_make(LV_SPRITE, 3),lv_num(1));
 assert(dg_edit_text(&d,3,"ABCD"));assert(!dg_edit_text(&d,3,"BUSY"));
 tick(50,50,false);assert(keys==4 && idles>=4 && !d.text_pending);
 assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"text",member)),"ABC"));
 lv_set(&r,NULL,"selstart",(lv_t){0},lv_num(1));
 assert(lv_numeric(lv_get(&r,NULL,"selstart",(lv_t){0}))==1);
 lv_set(&r,NULL,"text",member,lv_text(&r,"WI",false));
 lv_t pos=lv_call(&r,&caller,"charpostoloc",2,(lv_t[]){member,lv_num(2)});
 assert(lv_numeric(lv_at(&r,pos,1))==9 && lv_numeric(lv_at(&r,pos,2))==0);
 lv_set(&r,NULL,"text",member,lv_text(&r,"A\r\nB",false));
 assert(lv_numeric(lv_get(&r,NULL,"linecount",member))==2);
 pos=lv_call(&r,&caller,"charpostoloc",2,(lv_t[]){member,lv_num(4)});
 assert(lv_numeric(lv_at(&r,pos,2))==24);
 assert(lv_numeric(lv_call(&r,&caller,"length",1,(lv_t[]){(lv_t){0}}))==0);
 assert(lv_numeric(lv_call(&r,&caller,"length",1,(lv_t[]){lv_num(123)}))==0);
 assert(lv_type(lv_get(&r,NULL,"dragging",lv_make(LV_SPRITE, 1)))==LV_VOID);
 lv_set(&r,NULL,"dragging",lv_make(LV_SPRITE, 1),lv_num(1));
 assert(lv_numeric(lv_get(&r,NULL,"dragging",lv_make(LV_SPRITE, 1)))==1);
 lv_call(&r,&caller,"puppettransition",4,(lv_t[]){lv_num(1),lv_num(2),lv_num(5),lv_num(0)});
 assert(d.transition_type==1 && d.transition_serial==1);
 assert(d.transition_duration==30 && d.transition_chunk==5);
 // Zero time and chunk keep the original quarter-second floor and 1px chunk.
 lv_call(&r,&caller,"puppettransition",4,(lv_t[]){lv_num(10),lv_num(0),lv_num(0),lv_num(1)});
 assert(d.transition_type==10 && d.transition_serial==2);
 assert(d.transition_duration==15 && d.transition_chunk==1);
 assert(!r.failed);
 // The pinned chooser-opener trap raises a recoverable script alert, not a halt.
 lv_call(&r,&caller,"open_window_trap",0,NULL);
 assert(r.failed && r.script_error && strstr(r.error,"external file chooser window"));
 assert(lv_recover_script(&r));
 assert(!r.failed && r.script_error_count==1);
}
static void hover_contract(void) {
 boot();tick(50,50,true);tick(10,10,true);tick(10,10,false);
 assert(clicks==0);tick(50,50,false);tick(10,10,false);
 tick(10,10,true);tick(10,10,false);assert(clicks==1);
}
static void lifecycle_contract(void) {
 boot();assert(begins==1);assert(dg_seek(&d,2));
 assert(d.frame==1); // Old score stays readable while outgoing handlers yield.
 for(unsigned i=0;i<8 && d.frame!=2;i++){lv_collect(&r);tick(50,50,false);}
 assert(d.frame==2 && ended==6 && begins==1);
 snprintf(d.next_movie,sizeof(d.next_movie),"NEXT.DXR");assert(!dg_transition_ready(&d));
 tick(50,50,false);assert(stopped==0 && !dg_transition_ready(&d));
 for(unsigned i=0;i<8 && !dg_transition_ready(&d);i++)tick(50,50,false);
 assert(stopped==1 && dg_transition_ready(&d));
 assert(dg_service(&d,1000) && stopped==1);
 boot();lv_frame_t caller={.movie=&code,.handler=&handlers[0]};
 lv_call(&r,&caller,"quit",0,NULL);assert(!d.quit);
 for(unsigned i=0;i<12 && !d.quit;i++)dg_tick(&d,50,50,false,1000);
 assert(!r.failed && d.quit && ended==6 && stopped==1);
}
static void erase_contract(void) {
 boot();lv_frame_t caller={.movie=&code,.handler=&handlers[0]};
 lv_t cast=lv_make(LV_CASTLIB, 1);
 lv_t first=lv_call(&r,&caller,"new",2,(lv_t[]){lv_text(&r,"text",true),cast});
 lv_set(&r,NULL,"text",first,lv_text(&r,"DROP",false));
 lv_set(&r,NULL,"text",member,lv_text(&r,"KEEP",false));
 lv_set(&r,NULL,"editable",member,lv_num(1));
 lv_call(&r,&caller,"erase",1,&first); // Move the surviving field and its metadata.
 for(unsigned n=0;n<200;n++) {
  lv_t dynamic=lv_call(&r,&caller,"new",2,(lv_t[]){lv_text(&r,"text",true),cast});
  lv_set(&r,NULL,"name",dynamic,lv_text(&r,"UserTESTDB",false));
  lv_set(&r,NULL,"text",dynamic,lv_text(&r,"[]",false));
  lv_set(&r,NULL,"editable",dynamic,lv_num(1));
  lv_call(&r,&caller,"erase",1,&dynamic);lv_collect(&r);
  assert(!r.failed && d.field_count==1);
  assert(!dg_member(&d,lv_id(dynamic)));
  assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"text",member)),"KEEP"));
  assert(dg_text_editable(&d,3));
 }
}
int main(void) {edit_contract();hover_contract();lifecycle_contract();erase_contract();
 puts("D6 editable fields, source text events, hover, yielding cleanup and erase reuse passed");}
