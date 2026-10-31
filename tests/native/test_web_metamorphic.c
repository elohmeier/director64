// Generated legal sprite edits must render identically with retained scene
// and plane caches enabled or bypassed. The reference still decodes assets.
#include "compositor.h"
#include "ink.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static wc_t cached, fresh;
static dg_runtime_t director;
static lv_runtime_t values;
static uint32_t random_state=0x64d1ffu;
static unsigned next(void) { random_state=random_state*1664525u+1013904223u;return random_state; }
static uint32_t palette[256]={0xffffff,0x285fbe};
#define ID(n) DG_ID(1,1,n)
static const lv_movie_t code={.name="GENERATED"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
static const dg_frame_t frame={0};
static const dg_member_t members[]={
 {.id=ID(1),.number=1,.cast=1,.type=1,.width=8,.height=8,.asset="color.fdi"},
 {.id=ID(2),.number=2,.cast=1,.type=1,.width=8,.height=8,.asset="mask.fdi"},
 {.id=ID(3),.number=3,.cast=1,.type=8,.shape=1,.filled=1,.pattern=1},
};
static const dg_movie_t movie={.id=1,.code=&code,.casts=&cast,.cast_count=1,
 .members=members,.member_count=3,.frames=&frame,.frame_count=1,.palette=palette,.stage_color=0x2a6c83};
static uint32_t asset(void *ctx,const char *name,uint8_t *out,uint32_t cap) {
 (void)ctx; if(!out)return 32+8*8*4;assert(cap>=32+8*8*4);
 memset(out,0,32+8*8*4);memcpy(out,"FDI2",4);out[6]=1;out[7]=32;out[9]=8;out[11]=8;out[16]=1;out[19]=32;
 for(unsigned i=0;i<64;i++){uint8_t *p=out+32+i*4;
  if(!strcmp(name,"mask.fdi"))p[0]=p[1]=p[2]=(i%3?0:255);
  else {p[0]=(uint8_t)(i*3);p[1]=(uint8_t)(i*5);p[2]=(uint8_t)(i*7);}
  p[3]=255;
 }return 32+8*8*4;
}
int main(void) {
 wc_init(&cached,(wc_host_t){.read=asset},8u*1024u*1024u);
 wc_init(&fresh,(wc_host_t){.read=asset},8u*1024u*1024u);wc_cache_enabled(&fresh,false);
 dg_init(&director,&values,(dg_platform_t){0},NULL,NULL,0,42);
 assert(dg_enter(&director,&movie,1,NULL,0));
 for(unsigned channel=1;channel<=3;channel++){
  director.sprites[channel]=(dg_sprite_t){.visible=true,.stretch=true,.value={
   .member=channel==2?ID(1):ID(3),.type=16,.x=0,.y=0,
   .width=WC_WIDTH,.height=WC_HEIGHT,.blend=100,.flags=48,
   .fore_rgb=0x1e7950,.back_rgb=0xffffff}};
  dg_sprite_changed(&director,channel);
 }
 const unsigned inks[]={0,8,9,32,33,35,36,37,39,40,41};
 for(unsigned iteration=0;iteration<256;iteration++){
  unsigned channel=next()%3+1;dg_sprite_t *s=&director.sprites[channel];
  switch(next()%8){
   case 0:s->value.x=(int)(next()%(WC_WIDTH+100))-50;s->value.y=(int)(next()%(WC_HEIGHT+100))-50;break;
   case 1:s->value.width=next()%320+1;s->value.height=next()%240+1;break;
   case 2:s->value.ink=inks[next()%(sizeof(inks)/sizeof(*inks))];break;
   case 3:s->value.blend=next()%101;s->value.thickness=DG_HAS_BLEND;break;
   case 4:s->flip_h=!s->flip_h;s->flip_v=!s->flip_v;break;
   case 5:s->value.rotation=(int)(next()%360);break;
   case 6:s->visible=!s->visible;break;
   case 7:s->value.fore_rgb=next()&0xffffffu;s->value.back_rgb=next()&0xffffffu;break;
  }
  dg_sprite_changed(&director,channel);
  wc_render(&cached,&director,0,0,false,0);wc_render(&fresh,&director,0,0,false,0);
  assert(!values.failed);
  if(memcmp(cached.pixels,fresh.pixels,sizeof(cached.pixels))){
   fprintf(stderr,"cache divergence seed=0x64d1ff iteration=%u channel=%u\n",iteration,channel);return 1;
  }
  unsigned scenes=cached.scene_renders;
  wc_render(&cached,&director,100,100,true,0);wc_render(&fresh,&director,100,100,true,0);
  assert(cached.scene_renders==scenes);
  assert(!memcmp(cached.pixels,fresh.pixels,sizeof(cached.pixels)));
 }
 wc_release(&fresh);wc_release(&cached);dg_films_dispose(&director);dg_images_dispose(&director);
 puts("generated compositor edits: cached/bypassed scenes and cursor output agree");
}
