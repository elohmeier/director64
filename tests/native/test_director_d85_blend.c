// Birthday keeps a 30% popup blocker over its room while looping inside one
// placeholder behavior span. Original 8.5.1r104 under Wine retains the room;
// losing the scripted blend on a rewind makes the blocker fully opaque.
// ScummVM 41ac2b31847622d0662d22c03fe6979e3b43cfbc also marks blend as an
// automatic property (lingo/lingo-the.cpp); no reference code is copied.
#include "director.h"
#include <assert.h>
#include <stdio.h>
static dg_runtime_t director;
static lv_runtime_t values;
static lv_flow_t begin(lv_runtime_t *r,lv_frame_t *f){(void)r;(void)f;return LV_RETURN;}
static const char *const locals[]={"me"};
static const lv_handler_t handler={.name="beginsprite",.member=2,.cast="Internal",
 .kind="BehaviorScript",.arguments=1,.locals=1,.local_names=locals,.step=begin};
static const lv_movie_t code={.name="POPUP.DXR",.count=1,.handlers=&handler};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
static const dg_member_t member={.id=DG_ID(1,1,1),.number=1,.cast=1,.type=1,.width=8,.height=8};
static const dg_behavior_t behavior={DG_ID(1,1,2),""};
static const dg_delta_t deltas[]={
 {.channel=6,.mask=DG_ALL|DG_SPAN,.value={.member=DG_ID(1,1,1),.type=16,
  .width=8,.height=8,.blend=100,.behaviors=&behavior,.behavior_count=1}},
 {.channel=6,.mask=DG_BLEND,.value={.blend=75}},
};
static const dg_frame_t frames[]={{0,1},{1,0},{1,1}};
static const dg_movie_t movie={.id=1,.code=&code,.casts=&cast,.cast_count=1,
 .members=&member,.member_count=1,.frames=frames,.frame_count=3,.deltas=deltas,.tempo=30};
int main(void){
 dg_init(&director,&values,(dg_platform_t){0},NULL,NULL,0,42);
 assert(dg_enter(&director,&movie,1,NULL,0));
 lv_set(&values,NULL,"blend",lv_make(LV_SPRITE,1),lv_num(30));
 assert(dg_opacity(&director.sprites[1])==30);
 assert(dg_seek(&director,2));
 assert(dg_seek(&director,1));
 assert(dg_opacity(&director.sprites[1])==30); // Same span rewind retains Lingo.
 assert(dg_seek(&director,3));
 assert(dg_opacity(&director.sprites[1])==75); // An authored blend delta releases it.
 assert(!values.failed);
 puts("Director 8.5 popup: scripted blend retained within span, released by authored change");
}
