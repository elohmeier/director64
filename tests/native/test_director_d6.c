#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static dg_runtime_t d;
static lv_runtime_t r;
static unsigned char saved[65001];
static unsigned saved_length, visits;
static bool read_save(void *ctx,const char *name,char *out,unsigned cap,unsigned *length) {
  (void)ctx;
  if(strcmp(name,"DATA.CXT") || !saved_length)return false;
  assert(saved_length<cap);memcpy(out,saved,saved_length);*length=saved_length;return true;
}
static bool write_save(void *ctx,const char *name,const char *data,unsigned length) {
  (void)ctx;assert(!strcmp(name,"DATA.CXT") && length<sizeof(saved));
  memcpy(saved,data,length);saved_length=length;return true;
}
static lv_flow_t delayed(lv_runtime_t *v,lv_frame_t *f) {
  assert(d.current_event_sprite==1);
  if(!f->pc++)return LV_YIELD;
  assert(lv_numeric(f->locals[1])==73);visits++;
  v->result=lv_num(visits);return LV_RETURN;
}
static lv_flow_t construct(lv_runtime_t *v, lv_frame_t *f) {
  lv_set(v, f, "counter", f->locals[0], lv_num(3));
  v->result = f->locals[0]; return LV_RETURN;
}
static lv_flow_t inherited_counter(lv_runtime_t *v, lv_frame_t *f) {
  assert(lv_id(f->self) == lv_id(v->roots[1]) && lv_id(f->locals[0]) == lv_id(v->roots[0]));
  if (!f->pc++) return LV_YIELD;
  v->result = lv_self_get(v, f, "counter");
  return LV_RETURN;
}
static const lv_handler_t handlers[]={
  {.name="ping",.member=1,.cast="Internal",.kind="BehaviorScript",.arguments=2,.locals=2,.step=delayed},
  {.name="ping",.member=2,.cast="Internal",.kind="BehaviorScript",.arguments=2,.locals=2,.step=delayed},
  {.name="new",.member=1,.cast="Internal",.kind="BehaviorScript",.arguments=1,.locals=1,.step=construct},
  {.name="owncounter",.member=2,.cast="Internal",.kind="BehaviorScript",.arguments=1,.locals=1,.step=inherited_counter},
};
static const lv_movie_t code={"DATA.CXT",4,handlers,NULL, NULL, NULL, NULL, 0};
static const dg_cast_t casts[]={{"Internal",1,1}};
static const dg_member_t members[]={
  {.id=0x110003,.number=3,.cast=1,.type=3,.name="UsersDB",.text="[]",.editable=true},
  {.id=0x110004,.number=4,.cast=1,.type=1,.name="Dummy",.text="",.asset=""},
};
static const dg_behavior_t behaviors[]={{0x110001,"[#counter:7]"},{0x110002,""}};
static const dg_delta_t deltas[]={{.channel=6,.mask=DG_ALL,
  .value={.member=0x110003,.type=16,.behaviors=behaviors,.behavior_count=2}}};
static const dg_frame_t frames[]={{.first=0,.count=1}};
static const dg_movie_t movie={.code=&code,.id=1,.tempo=30,.cast_count=1,.casts=casts,
  .member_count=2,.members=members,.deltas=deltas,.frames=frames,.frame_count=1};
static lv_frame_t caller;
static bool boot(void) {
  dg_init(&d,&r,(dg_platform_t){.read_file=read_save,.write_file=write_save},NULL,NULL,0,1);
  caller=(lv_frame_t){.movie=&code,.handler=&handlers[0]};return dg_enter(&d,&movie,1,NULL,0);
}
static lv_t call(const char *name,unsigned argc,const lv_t *args) {
  lv_t value=lv_call(&r,&caller,name,argc,args);
  if(r.failed)fprintf(stderr,"%s\n",r.error);
  assert(!r.failed);return value;
}
static void data_contracts(void) {
  assert(lv_truth(&r,lv_binary(&r,"=",lv_num(2),lv_text(&r,"2",false))));
  assert(lv_truth(&r,lv_binary(&r,"=",lv_text(&r,"2.0",false),lv_num(2))));
  assert(!lv_truth(&r,lv_binary(&r,"=",lv_text(&r,"02",false),lv_text(&r,"2",false))));
  assert(!lv_truth(&r,lv_binary(&r,"=",lv_num(2),lv_text(&r,"2x",false))));
  assert(!lv_truth(&r,lv_binary(&r,"=",lv_num(2),lv_text(&r,"2",true))));
  assert(lv_truth(&r,lv_binary(&r,"<>",lv_num(2),lv_text(&r,"3",false))));
  assert(!r.failed);
  for (unsigned i=1;i<=2;i++) {
    lv_t xtra=call("xtra",1,(lv_t[]){lv_num(i)});
    assert(lv_type(xtra)==LV_XTRA && (unsigned)lv_id(xtra)==i);
    assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"name",xtra)),i==1?"FileIO":"Glu32"));
  }
  const unsigned char raw[]={0,255,16,0,32};
  r.roots[0]=lv_bytes(&r,raw,sizeof(raw));
  r.roots[1]=lv_binary(&r,"&",r.roots[0],r.roots[0]);lv_collect(&r);
  assert(lv_chunk_count(&r,"char",r.roots[1])==10);
  lv_t byte=lv_chunk(&r,"char",r.roots[1],lv_num(7),lv_num(7));
  assert(lv_numeric(call("chartonum",1,&byte))==255);
  assert(!memcmp(lv_cstr(&r,r.roots[1]),raw,sizeof(raw)));
  r.roots[0]=lv_literal(&r,"[rect(1,2,3,4),point(-2,3)]");
  assert(r.objects[lv_id(lv_at(&r,r.roots[0],1))].geometry==4);
  assert(lv_type(lv_literal(&r,"point(alert(1),2)"))==LV_VOID);
  r.roots[0]=lv_literal(&r,"[3,1,2]");call("sort",1,&r.roots[0]);
  call("add",2,(lv_t[]){r.roots[0],lv_num(2)});
  assert(lv_numeric(lv_at(&r,r.roots[0],1))==1 && lv_numeric(lv_at(&r,r.roots[0],4))==3);
  r.roots[0]=lv_instance(&r,lv_make(LV_SCRIPT, 0x110001));
  r.roots[1]=lv_instance(&r,lv_make(LV_SCRIPT, 0x110002));
  lv_set(&r,NULL,"ancestor",r.roots[0],r.roots[1]);lv_set(&r,NULL,"score",r.roots[1],lv_num(9));
  assert(lv_numeric(lv_get(&r,NULL,"score",r.roots[0]))==9);
  lv_set(&r,NULL,"score",r.roots[0],lv_num(11));
  assert(lv_numeric(lv_get(&r,NULL,"score",r.roots[1]))==11);
  lv_set(&r,NULL,"counter",r.roots[0],lv_num(99));
  lv_set(&r,NULL,"counter",r.roots[1],lv_num(7));
  assert(lv_start_method(&r,r.roots[0],"owncounter",0,NULL));
  assert(lv_run(&r,100) && r.yielded);
  lv_collect(&r);
  assert(lv_run(&r,100) && !r.depth && lv_numeric(r.result)==7);
}
static void continuation_contract(void) {
  call("sendsprite",2,(lv_t[]){lv_num(0),lv_text(&r,"unbound",true)});
  call("sendsprite",2,(lv_t[]){lv_num(DG_SPRITES),lv_text(&r,"unbound",true)});
  assert(!r.failed && !r.script_error_count);
  lv_t list=lv_list(&r,2,(lv_t[]){r.roots[0],r.roots[1]},false);
  lv_set(&r,NULL,"scriptinstancelist",lv_make(LV_SPRITE, 1),list);
  lv_set(&r,NULL,"currentaction",r.roots[1],lv_text(&r,"none",true));
  lv_t sprite = lv_make(LV_SPRITE, 1);
  assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"currentaction",sprite)),"none"));
  lv_set(&r,NULL,"currentaction",sprite,lv_text(&r,"drag",true));
  assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"currentaction",r.roots[1])),"drag"));
  d.sprites[2].value=(dg_spec_t){.type=16,.x=30,.y=200,.width=109,.height=80};
  lv_set(&r,NULL,"member",lv_make(LV_SPRITE, 2),lv_make(LV_MEMBER, 0x110004));
  assert(!d.sprites[2].value.type && !dg_hit(&d,30,200));
  d.current_event_sprite=17;
  lv_t args[]={lv_num(1),lv_text(&r,"ping",true),lv_num(73)};
  lv_invoke(&r,&caller,"sendsprite",3,args);
  assert(lv_run(&r,100) && r.yielded && visits==0);
  lv_collect(&r);assert(lv_run(&r,100) && r.yielded && visits==1);
  assert(lv_run(&r,100) && !r.depth && visits==2 && d.current_event_sprite==17);
}
static void save_contract(void) {
  lv_t cast=lv_make(LV_CASTLIB, 1);
  lv_t member=call("new",2,(lv_t[]){lv_text(&r,"text",true),cast});
  lv_set(&r,NULL,"name",member,lv_text(&r,"UserTESTDB",false));
  lv_set(&r,NULL,"text",member,lv_text(&r,"[#car:[1,82,133,152]]",false));
  call("save",1,&cast);assert(saved_length>8);
  assert(boot());member=lv_reference(&r,"member",lv_text(&r,"UserTESTDB",false),cast);
  assert(lv_id(member)==0x110005);
  assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"text",member)),"[#car:[1,82,133,152]]"));
  d.sprites[1].value.member=0x110003;
  assert(dg_edit_text(&d,1,"TEST") && !dg_edit_text(&d,1,"too long player name here"));
  assert(!dg_edit_text(&d,2,"TEST"));
  saved[6]=255;assert(!boot() && r.failed);
}
int main(void) {
  assert(boot());
  lv_t initial = r.roots[DG_BEHAVIOR_ROOT + 1];
  assert(lv_count(&r, initial) == 2);
  assert(lv_numeric(lv_get(&r,NULL,"counter",lv_at(&r,initial,1))) == 7);
  data_contracts();continuation_contract();save_contract();
  puts("D6 binary topology, inherited properties, yielding behaviors and mutable cast saves passed");
}
