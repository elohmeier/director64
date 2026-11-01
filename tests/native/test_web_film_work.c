// Translated soft-mask films must retain the general renderer's exact pixels
// without scanning a full-stage wrapper for each small bitmap part.
#include "compositor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ID(n) DG_ID(1,1,n)
static wc_t optimized, reference;
static dg_runtime_t director;
static lv_runtime_t values;
static const lv_movie_t code={.name="TRANSLATED.DXR"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
static const dg_frame_t score={0};
static const uint32_t palette[256]={0xffffff,0};
static dg_film_layer_t leaf={.channel=1,.value={.member=ID(2),.type=16,
  .x=17,.y=107,.width=13,.height=9,.stretch=1,.fore=1,.blend=37,.ink=9,
  .thickness=DG_HAS_BLEND}};
static const dg_film_frame_t frame={1,&leaf};
static dg_film_layer_t nested={.channel=1,.value={.member=ID(1),.type=16,
  .x=5,.y=7,.width=WC_WIDTH+2,.height=WC_HEIGHT,.stretch=1,.fore=1,.blend=100}};
static const dg_film_frame_t nested_frame={1,&nested};
static const dg_member_t members[]={
 {.id=ID(1),.number=1,.cast=1,.type=2,.width=WC_WIDTH+2,.height=WC_HEIGHT,
  .reg_x=(WC_WIDTH+2)/2,.reg_y=WC_HEIGHT/2,.film_left=-2,
  .film_count=1,.film_frames=&frame},
 {.id=ID(2),.number=2,.cast=1,.type=1,.width=13,.height=9,.asset="part.fdi"},
 {.id=ID(3),.number=3,.cast=1,.type=1,.width=13,.height=9,.asset="mask.fdi"},
 {.id=ID(4),.number=4,.cast=1,.type=2,.width=WC_WIDTH,.height=WC_HEIGHT,
  .film_count=1,.film_frames=&nested_frame},
};
static const dg_movie_t movie={.id=1,.code=&code,.casts=&cast,.cast_count=1,
 .members=members,.member_count=4,.frames=&score,.frame_count=1,.palette=palette,
 .stage_color=0x756453};
static uint32_t asset(void *ctx,const char *name,uint8_t *out,uint32_t cap) {
 (void)ctx;unsigned n=32+13*9*4;if(!out)return n;assert(cap>=n);
 memset(out,0,n);memcpy(out,"FDI2",4);out[6]=n>>8;out[7]=n;out[9]=13;out[11]=9;
 out[16]=1;out[19]=32;
 for(unsigned i=0;i<13*9;i++) {
  uint8_t *p=out+32+i*4;
  if(!strcmp(name,"mask.fdi"))p[0]=p[1]=p[2]=(uint8_t)(31+i%190);
  else {p[0]=(uint8_t)(i*5);p[1]=(uint8_t)(i*3);p[2]=61;}
  p[3]=255;
 }
 return n;
}
static void compare(void) {
 dg_sprite_changed(&director,1);
 wc_render(&optimized,&director,0,0,false,0);
 wc_render(&reference,&director,0,0,false,0);
 if(values.failed)fprintf(stderr,"%s\n",values.error);
 assert(!values.failed);
 assert(!memcmp(optimized.pixels,reference.pixels,sizeof(optimized.pixels)));
}
int main(void) {
 dg_init(&director,&values,(dg_platform_t){0},NULL,NULL,0,42);
 assert(dg_enter(&director,&movie,1,NULL,0));
 wc_init(&optimized,(wc_host_t){.read=asset},8*1024*1024);
 wc_init(&reference,(wc_host_t){.read=asset},8*1024*1024);
 reference.film_clip_disabled=true;
 // Independent full-wrapper traversal remains the oracle for rounding and ink.
 wc_cache_enabled(&optimized,false);wc_cache_enabled(&reference,false);
 director.sprites[1]=(dg_sprite_t){.visible=true,.stretch=true,.value={
  .member=ID(1),.type=16,.x=WC_WIDTH/2,.y=WC_HEIGHT/2,
  .width=WC_WIDTH+2,.height=WC_HEIGHT,.stretch=1,.blend=100}};
 compare();
#if defined(DIRECTOR64_D85) && DIRECTOR64_D85
 assert(optimized.work.inverse_pixels<=(13u+2)*(9u+2));
 assert(reference.work.inverse_pixels==(uint64_t)WC_WIDTH*WC_HEIGHT);
 assert(reference.work.worst_inverse_pixels>reference.work.worst_inverse_samples*100);
 assert(optimized.work.worst_inverse_pixels<=(13u+2)*(9u+2));
 // An earlier legitimate crop has more absolute waste than the following
 // half-stage film scan. Keep the ratio offender independently of that crop.
 optimized.work=(wc_work_t){0};reference.work=(wc_work_t){0};
 uint64_t stage=(uint64_t)WC_WIDTH*WC_HEIGHT;
 optimized.work.worst_inverse_pixels=reference.work.worst_inverse_pixels=stage;
 optimized.work.worst_inverse_samples=reference.work.worst_inverse_samples=stage*5/12;
 director.sprites[1].value.width=WC_WIDTH/2;
 compare();
 assert(reference.work.worst_inverse_pixels==stage);
 assert(reference.work.wasteful_inverse.pixels>stage/4);
 assert(reference.work.wasteful_inverse.pixels>
        reference.work.wasteful_inverse.sampled_pixels*WC_INVERSE_SCAN_RATIO);
 director.sprites[1].value.width=WC_WIDTH+2;
#else
 // D10's native films have a separate container-compositing contract.
 assert(!reference.work.inverse_pixels && !optimized.work.inverse_pixels);
#endif
 unsigned inks[]={0,8,9,32,33,35,36,37,39,40,41};
 for(unsigned i=0;i<sizeof(inks)/sizeof(*inks);i++) {
  leaf.value.ink=inks[i];
  leaf.value.blend=(i*17)%101;
  for(unsigned p=0;p<9;p++) {
   director.sprites[1].value.x=WC_WIDTH/2+(int)p-4;
   director.sprites[1].value.y=WC_HEIGHT/2-(int)p+4;
   // Includes clipping at every stage edge and children outside a wrapper.
   leaf.value.x=p<4?(int)p*3-12:p==4?17:WC_WIDTH+(int)p-5;
   leaf.value.y=p<4?-(int)p: p==4?107:WC_HEIGHT-(int)p;
   leaf.value.width=13+(int)p;leaf.value.height=9+(int)p;
   compare();
  }
 }
 memset(&leaf.value,0,sizeof(leaf.value));
 leaf.value.member=ID(2);leaf.value.type=16;leaf.value.width=13;leaf.value.height=9;
 leaf.value.x=17;leaf.value.y=107;leaf.value.stretch=1;leaf.value.fore=1;
 leaf.value.blend=37;leaf.value.ink=9;leaf.value.thickness=DG_HAS_BLEND;
 director.sprites[1].value.member=ID(4);director.sprites[1].value.x=11;
 director.sprites[1].value.y=13;director.sprites[1].value.width=WC_WIDTH;
 director.sprites[1].value.height=WC_HEIGHT;
 compare();
 // Nonaffine, scaled and flipped containers retain the general path.
 director.sprites[1].value.rotation=1700;compare();
 director.sprites[1].value.rotation=0;director.sprites[1].flip_h=true;compare();
 director.sprites[1].flip_h=false;director.sprites[1].value.width=WC_WIDTH/2;compare();
 wc_release(&optimized);wc_release(&reference);dg_films_dispose(&director);dg_images_dispose(&director);
 puts("translated film: exact masks/inks/clipping/nesting, bounded pixel work and general fallback pass");
}
