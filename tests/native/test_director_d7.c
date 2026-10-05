#include "director.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
// ScummVM 41ac2b31847622d0662d22c03fe6979e3b43cfbc, corroborated by
// KALENDER frame mouseUp, DAG04 local helpers, and DAG23's absent sound 124.
static dg_runtime_t d;
static lv_runtime_t r;
static unsigned clicked, global_calls, sounds, empty_sounds;
static char saved[128];
static unsigned hit_calls;
static bool hit(void *ctx,const dg_member_t *m,unsigned ink,int x,int y) {
 (void)ctx;(void)m;(void)ink;(void)x;(void)y;hit_calls++;return true;
}
static const char *date(void *ctx) {(void)ctx;return "Sunday, December 24, 2000";}
static bool read_file(void *ctx,const char *name,char *out,unsigned cap,unsigned *length) {
 (void)ctx;if(strcmp(name,"ByggFil.txt"))return false;
 if(!*saved)return false;
 assert(strlen(saved)<cap);strcpy(out,saved);*length=strlen(saved);return true;
}
static bool write_file(void *ctx,const char *name,const char *data,unsigned length) {
 (void)ctx;assert(!strcmp(name,"ByggFil.txt") && length<sizeof(saved));
 memcpy(saved,data,length);saved[length]=0;return true;
}
static void sound(void *ctx,unsigned channel,const dg_member_t *m) {(void)ctx;(void)channel;if(m)sounds++;}
static void trace(void *ctx,const char *s) {(void)ctx;if(strstr(s,"SCORE_SOUND_EMPTY"))empty_sounds++;}
static lv_flow_t local(lv_runtime_t *v,lv_frame_t *f) {
 assert(lv_type(f->self)==LV_VOID && lv_numeric(f->locals[0])==42);
 if(!f->pc++)return LV_YIELD;
 v->result=lv_num(99);clicked++;return LV_RETURN;
}
static lv_flow_t up(lv_runtime_t *v,lv_frame_t *f) {
 if(!f->pc++)return lv_invoke(v,f,"helper",1,(lv_t[]){lv_num(42)});
 assert(lv_numeric(v->result)==99);return LV_RETURN;
}
static lv_flow_t global(lv_runtime_t *v,lv_frame_t *f) {(void)v;(void)f;global_calls++;return LV_RETURN;}
static const lv_handler_t handlers[]={
 {.name="mouseup",.member=1,.cast="Internal",.kind="BehaviorScript",.step=up},
 {.name="helper",.member=1,.cast="Internal",.kind="BehaviorScript",.arguments=1,.locals=1,.step=local},
 {.name="helper",.member=2,.cast="Internal",.kind="MovieScript",.step=global},
 {.name="mouseup",.member=2,.cast="Internal",.kind="MovieScript",.step=global},
};
static const lv_movie_t code={"D7.DXR",4,handlers,NULL, NULL, NULL, NULL, 0};
static const dg_cast_t casts[]={{"Internal",1,1},{"Extra",1,2}};
static const dg_member_t members[]={
 {.id=0x12000a,.number=10,.cast=2,.type=1,.width=16,.height=16,.name="first"},
 {.id=0x12000b,.number=11,.cast=2,.type=1,.width=16,.height=16,.name="second"},
};
static const dg_delta_t deltas[]={
 {.channel=0,.mask=DG_ALL,.value={.script=0x110001}},
 {.channel=3,.mask=DG_ALL,.value={.member=0x11007c}},
};
static const dg_frame_t frames[]={{0,2}};
static const dg_movie_t movie={.code=&code,.id=1,.tempo=1,.cast_count=2,.casts=casts,.members=members,.member_count=2,
 .frame_count=1,.frames=frames,.deltas=deltas};
static lv_t call(const char *name,unsigned n,const lv_t *a) {
 lv_frame_t f={.movie=&code,.handler=&handlers[0]};return lv_call(&r,&f,name,n,a);
}
int main(void) {
 assert(DG_MAX_TEMPO==999 && DG_SPRITES>=501);
 dg_init(&d,&r,(dg_platform_t){.long_date=date,.read_file=read_file,.write_file=write_file,
  .sound=sound,.trace=trace,.hit=hit},NULL,NULL,0,42);
 assert(dg_enter(&d,&movie,1,NULL,0));
 assert(!strcmp(lv_cstr(&r,lv_get(&r,NULL,"longdate",(lv_t){0})),date(NULL)));
 assert(dg_tick(&d,320,240,true,1000));
 for(unsigned i=0;i<4;i++)assert(dg_tick(&d,320,240,false,1000));
 assert(clicked==1 && !global_calls && !sounds && empty_sounds==1);
 for(unsigned i=0;i<2;i++) {
  lv_t file=call("fileio",3,(lv_t[]){lv_text(&r,"mnew",true),lv_text(&r,"append",false),lv_text(&r,"ByggFil.txt",false)});
  assert(lv_type(file)==LV_FILE);
  call("file_method",3,(lv_t[]){file,lv_text(&r,"mwritestring",true),lv_text(&r,i?"two\r":"one\r",false)});
  call("file_method",2,(lv_t[]){file,lv_text(&r,"mdispose",true)});
 }
 assert(!r.failed && !strcmp(saved,"one\rtwo\r"));
 call("puppettransition",1,(lv_t[]){lv_num(9)});
 assert(!r.failed && d.transition_type==9 && d.transition_duration==15);
 lv_t sprite=lv_make(LV_SPRITE, 1);
 lv_set(&r,NULL,"membernum",sprite,lv_num(10));
 assert(!r.failed && d.sprites[1].value.member==0x11000a);
 lv_set(&r,NULL,"member",sprite,lv_make(LV_MEMBER, 0x12000a));
 assert(lv_numeric(lv_get(&r,NULL,"membernum",sprite))==10);
 assert(lv_numeric(lv_get(&r,NULL,"castnum",sprite))==131082);
 lv_set(&r,NULL,"membernum",sprite,lv_num(11));
 assert(d.sprites[1].value.member==0x12000b);
 lv_set(&r,NULL,"castnum",sprite,lv_num(131082));
 assert(d.sprites[1].value.member==0x12000a);
 lv_set(&r,NULL,"puppet",sprite,lv_num(1));
 assert(lv_numeric(lv_get(&r,NULL,"puppet",sprite))==1);
 call("cursor",1,(lv_t[]){lv_list(&r,2,(lv_t[]){lv_num(2000),lv_num(2001)},false)});
 assert(!r.failed && d.cursor.image==0);
 lv_set(&r,NULL,"moveablesprite",sprite,lv_num(1));
 lv_set(&r,NULL,"visible",sprite,lv_num(1));
 hit_calls=0;
 for(unsigned i=0;i<100;i++)assert(dg_mouse_hit(&d,5,5)==1);
 assert(hit_calls==1);
 lv_set(&r,NULL,"visible",sprite,lv_num(0));
 assert(dg_mouse_hit(&d,5,5)==0);
 lv_set(&r,NULL,"visible",sprite,lv_num(1));
 assert(dg_mouse_hit(&d,5,5)==1 && hit_calls==2);
 call("unload",2,(lv_t[]){lv_num(1),lv_num(9)});
 lv_frame_t caller={.movie=&code,.handler=&handlers[0]};
 assert(lv_invoke(&r,&caller,"go",2,(lv_t[]){lv_num(1),lv_text(&r,"kalender.dir",false)})==LV_YIELD);
 assert(!r.failed && !strcmp(d.next_movie,"kalender.DXR"));
 puts("D7 frame events, yielding local helpers, date, empty sound, FileIO append and transition passed");
}
