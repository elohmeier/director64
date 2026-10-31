// Source-free stage changes reproduce the original D7/8.5 window probes.
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static dg_runtime_t d;
static lv_runtime_t v;
static wc_t c, uncached;
static unsigned entered;
static lv_flow_t enter(lv_runtime_t *r,lv_frame_t *f) {(void)r;(void)f;entered++;return LV_RETURN;}
static const lv_handler_t handlers[]={{.name="enterframe",.kind="MovieScript",.step=enter}};
static const lv_movie_t code={.name="SYNTHETIC.DXR",.count=1,.handlers=handlers};
static const dg_cast_t casts[]={{.name="Internal",.file=1,.cast=1}};
static const dg_member_t members[]={
 {.id=0x210001,.number=1,.cast=1,.type=8,.shape=1,.filled=1},
 {.id=0x210002,.number=2,.cast=1,.type=14,.transition_present=true,
  .transition_type=9,.transition_ms=500,.transition_chunk=2,.transition_area=0}};
static const dg_frame_t frames[]={{0,0},{0,0}};
static uint32_t palette[256];
static const dg_movie_t movie={.code=&code,.id=1,.tempo=1,.cast_count=1,.casts=casts,
 .member_count=2,.members=members,.frame_count=2,.frames=frames,.palette=palette};
static void boot(void) {
 wc_release(&c);wc_release(&uncached);
 wc_init(&c,(wc_host_t){0},1024);wc_init(&uncached,(wc_host_t){0},1024);
 wc_cache_enabled(&uncached,false);
 entered=0;
 dg_init(&d,&v,(dg_platform_t){0},NULL,NULL,0,1);
 assert(dg_enter(&d,&movie,1,NULL,0));
 assert(dg_service(&d,DG_SERVICE_BUDGET));
 palette[1]=0xff0000;palette[2]=0x0000ff;
 d.sprites[1]=(dg_sprite_t){.value={.member=0x210001,.type=16,.width=64,.height=48,.fore=1,.blend=100},.visible=true,.stretch=true};
 dg_sprite_changed(&d,1);
 assert(wc_render(&c,&d,0,0,false,0));assert(wc_render(&uncached,&d,0,0,false,0));
 assert(c.pixels[0]==0xff0000ffu);
}
static void queue(unsigned type,unsigned chunk,unsigned area) {
 lv_t args[]={lv_num((int)type),lv_num(2),lv_num((int)chunk),lv_num((int)area)},out={0};bool yield=false;
 assert(v.services.call(&v,"puppettransition",4,args,&out,&yield));
 assert(!yield && !v.failed && d.transition_pending==(type!=0));
 d.sprites[1].value.fore=2;dg_sprite_changed(&d,1);
 assert(v.services.call(&v,"updatestage",0,NULL,&out,&yield));
 assert(yield==(type!=0));
}
static void render(void) {
 wc_render(&c,&d,0,0,false,0);wc_render(&uncached,&d,0,0,false,0);
 assert(!v.failed);assert(!memcmp(c.pixels,uncached.pixels,sizeof(c.pixels)));
}
static void timing_and_geometry(void) {
 boot();queue(1,1,1);unsigned frame=d.frame,events=entered;
 render();assert(c.pixels[0]==0xff0000ffu);
 for(unsigned i=0;i<15;i++){assert(dg_tick(&d,0,0,false,DG_SERVICE_BUDGET));render();}
 assert(d.frame==frame && entered==events && d.transition_active);
 assert(c.pixels[10]==0xffff0000u && c.pixels[50]==0xff0000ffu);
 // updateStage continuation and enterFrame remain paused through the reveal.
 for(unsigned i=15;i<30;i++){assert(dg_tick(&d,0,0,false,DG_SERVICE_BUDGET));render();}
 assert(!d.transition_active && c.pixels[63]==0xffff0000u);
 assert(d.frame==frame);
 // Full-stage area uses the stage width, rather than the changed 64px shape.
 boot();queue(1,1,0);render();
 for(unsigned i=0;i<15;i++){assert(dg_tick(&d,0,0,false,DG_SERVICE_BUDGET));render();}
 assert(c.pixels[63]==0xffff0000u);
 // A skip press does not drag or send a delayed release to destination sprites.
 d.sprites[1].moveable=true;
 assert(dg_tick(&d,12,12,true,DG_SERVICE_BUDGET));render();
 assert(!d.transition_active && !d.drag_sprite && !d.mouse_pressed && !d.mouse_down);
 assert(dg_tick(&d,12,12,false,DG_SERVICE_BUDGET));
 assert(!d.mouse_released && !d.mouse_up_dispatch);
}
static void cast_and_repeated_reveals(void) {
 boot();lv_t member=lv_make(LV_MEMBER,0x210002),out={0};bool yield=false;
 assert(v.services.call(&v,"puppettransition",1,&member,&out,&yield));
 assert(d.transition_type==9 && d.transition_duration==30 && d.transition_chunk==2);
 assert(lv_integer(&v,lv_get(&v,NULL,"duration",member))==500 && !v.failed);
 // Every authored game effect changes intermediate pixels and reaches the
 // same destination, with caching enabled or disabled on every render tick.
 const unsigned effects[]={1,2,3,5,6,8,9,10,26,29,37,38,44,45,50,51,52};
 for(unsigned n=0;n<sizeof(effects)/sizeof(*effects);n++) {
  boot();queue(effects[n],effects[n]>=50?2:1,1);render();
  bool intermediate=false;
  for(unsigned i=0;i<30;i++) {
   assert(dg_tick(&d,0,0,false,DG_SERVICE_BUDGET));render();
   unsigned old=0,newer=0;
   for(unsigned y=0;y<48;y++)for(unsigned x=0;x<64;x++) {
    uint32_t p=c.pixels[y*WC_WIDTH+x];old+=p==0xff0000ffu;newer+=p==0xffff0000u;
   }
   intermediate|=old && newer;
  }
  assert(intermediate && c.pixels[63+47*WC_WIDTH]==0xffff0000u);
 }
 boot();queue(0,1,1);render();assert(!d.transition_active && c.pixels[0]==0xffff0000u);
}
static void score_reveal_pauses_enterframe(void) {
 boot();
 const dg_delta_t changes[]={{.channel=2,.value={.member=0x210002}}};
 const dg_frame_t score[]={{0,0},{0,1}};
 dg_movie_t authored=movie;authored.frames=score;authored.deltas=changes;
 d.movie=&authored;
 d.next_frame=2;
 unsigned before=entered;
 assert(dg_service(&d,DG_SERVICE_BUDGET));
 assert(d.frame==2 && d.transition_active && d.transition_type==9);
 assert(entered==before);
 for(unsigned i=0;i<29;i++)assert(dg_tick(&d,0,0,false,DG_SERVICE_BUDGET));
 assert(d.frame==2 && d.transition_active && entered==before);
 assert(dg_tick(&d,0,0,false,DG_SERVICE_BUDGET));
 assert(!d.transition_active && entered==before+1);
 d.movie=&movie;
}
int main(void) {timing_and_geometry();cast_and_repeated_reveals();score_reveal_pauses_enterframe();wc_release(&c);wc_release(&uncached);puts("transition timing/render/cache contracts passed");}
