// Rasterized rich text and PICT remain their authored cast types.
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static dg_runtime_t director;
static lv_runtime_t values;
static wc_t compositor;
static uint8_t asset[48] = {'F','D','I','2',0,0,0,48,0,4,0,1,0,0,0,0,3,1,0,32};
static const lv_movie_t code={.name="RASTER.DXR"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
static const dg_frame_t frame={0,0};
static const uint32_t palette[256]={0xffffff,0};
static const dg_member_t members[]={
  {.id=DG_ID(1,1,1),.number=1,.cast=1,.type=12,.width=4,.height=1,.asset="rich.fd2"},
  {.id=DG_ID(1,1,2),.number=2,.cast=1,.type=5,.width=4,.height=1,.asset="pict.fd2"},
};
static const dg_movie_t movie={.id=1,.code=&code,.casts=&cast,.cast_count=1,
  .members=members,.member_count=2,.frames=&frame,.frame_count=1,.tempo=30,.palette=palette,
#if DG_MODERN
  .stage_color=0x0000ff,
#endif
};
static uint32_t read_asset(void *ctx,const char *name,uint8_t *out,uint32_t cap) {
  (void)ctx;(void)name;
  if(out){assert(cap>=sizeof(asset));memcpy(out,asset,sizeof(asset));}
  return sizeof(asset);
}
static unsigned pixel(unsigned x){return compositor.pixels[20*WC_WIDTH+x]&0xffffff;}
static void draw(unsigned member,unsigned ink){
  director.sprites[1]=(dg_sprite_t){.value={.type=16,.member=DG_ID(1,1,member),
      .x=20,.y=20,.width=4,.height=1,.ink=ink,.blend=100,.thickness=DG_HAS_BLEND,
      .fore=1,.back=0},.visible=true,.stretch=true};
  dg_sprite_changed(&director,1);
  wc_render(&compositor,&director,0,0,false,0);
  if(values.failed)fprintf(stderr,"raster: %s\n",values.error);
  assert(!values.failed);
}
int main(void){
  asset[25]=255; // authored green background
  memcpy(asset+32,(uint8_t[]){255,0,0,0,255,0,0,128,255,0,0,255,255,255,255,255},16);
  wc_init(&compositor,(wc_host_t){.read=read_asset},4096);
  dg_init(&director,&values,(dg_platform_t){0},NULL,NULL,0,1);
  assert(dg_enter(&director,&movie,1,NULL,0));
  draw(1,0);
  assert(pixel(20)==0xff00 && pixel(21)==0x7f80 && pixel(22)==0xff && pixel(23)==0xffffff);
  draw(1,8); // rich text's authored background remains for matte ink too
  assert(pixel(20)==0xff00 && pixel(21)==0x7f80);
  draw(1,36); // only background-transparent ink uses glyph coverage
#if DG_MODERN
  assert(pixel(20)==0xff0000 && pixel(21)==0x7f0080);
#else
  assert(pixel(20)==0xffffff && pixel(21)==0x7f7fff);
#endif
  assert(pixel(22)==0xff && pixel(23)==0xffffff); // white foreground survives
  asset[16]=1; // ordinary PICT raster retains bitmap coverage semantics
  draw(2,8);
  assert(pixel(22)==0xff);
  wc_release(&compositor);
  puts("raster casts: rich-text authored background/coverage and PICT type preserved");
}
