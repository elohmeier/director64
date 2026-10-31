// Native widget pixels are portable controls, not original Windows theme
// baselines. Source logical -2 width must retain the score's16x16 widget.
#include "compositor.h"
#include "buttons.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static lv_runtime_t values;
static dg_runtime_t director;
static wc_t cached,uncached;
static unsigned labels;
static const lv_movie_t code={.name="BUTTON.DXR"};
static const dg_cast_t cast={.name="Internal",.file=1,.cast=1};
static const dg_text_style_t font={.font_name="Test",.font_id=1,.size=12,.ascent=11,.line_height=14};
static const dg_member_t members[]={
 {.id=DG_ID(1,1,1),.number=1,.cast=1,.type=7,.shape=DG_BUTTON_CHECK,
  .name="Use in test",.text="",.width=65534,.height=16,.text_box_present=true,.text_box_type=2},
 {.id=DG_ID(1,1,2),.number=2,.cast=1,.type=7,.shape=DG_BUTTON_RADIO,
  .name="Radio",.text="",.width=16,.height=16},
 {.id=DG_ID(1,1,3),.number=3,.cast=1,.type=7,.shape=DG_BUTTON_PUSH,
  .name="Go",.text="OK",.width=40,.height=20,.text_style=&font}};
static const dg_frame_t frame={0,0};
static const uint32_t palette[256]={0};
static const dg_movie_t movie={.id=1,.code=&code,.casts=&cast,.cast_count=1,
 .members=members,.member_count=3,.frames=&frame,.frame_count=1,.palette=palette,.stage_color=0x669988};
static bool raster(void *ctx,const char *text,int width,int height,const wc_text_style_t *style,uint8_t *coverage) {
 (void)ctx;assert(!strcmp(text,"OK")&&style&&style->font==1);labels++;
 assert(width>8&&height>8);coverage[5*width+5]=255;return true;
}
static void render(void) {
 wc_render(&cached,&director,0,0,false,0);wc_render(&uncached,&director,0,0,false,0);
 assert(!values.failed&&!memcmp(cached.scene,uncached.scene,sizeof(cached.scene)));
}
static uint32_t pixel(int x,int y) {return cached.scene[y*WC_WIDTH+x]&0xffffff;}
static void selected(unsigned i,bool on) {
 lv_set(&values,NULL,"hilite",lv_make(LV_MEMBER,members[i].id),lv_num(on));render();
 assert(dg_button_hilite(&director,members[i].id)==on);
}
int main(void) {
 dg_init(&director,&values,(dg_platform_t){0},NULL,NULL,0,1);assert(dg_enter(&director,&movie,1,NULL,0));
 wc_init(&cached,(wc_host_t){.text=raster},1024);wc_init(&uncached,(wc_host_t){.text=raster},1024);uncached.cache_disabled=true;
 for(unsigned i=0;i<3;i++) {
  director.sprites[i+1]=(dg_sprite_t){.value={.member=members[i].id,.type=16,
   .x=(int16_t)(10+i*30),.y=10,.width=(int16_t)(i==2?40:16),.height=(int16_t)(i==2?20:16),.blend=100},.visible=true};
  dg_sprite_changed(&director,i+1);
 }
 int l,t,r,b;dg_bounds(&director,1,&l,&t,&r,&b);assert(l==10&&t==10&&r==26&&b==26);
 assert(lv_integer(&values,lv_get(&values,NULL,"width",lv_make(LV_MEMBER,members[0].id)))==-2);
 render();assert(!director.field_count&&labels>0);
 assert(pixel(16,18)==0xffffff&&pixel(46,17)==0xffffff&&pixel(95,20)==0xc0c0c0);
 selected(0,true);assert(pixel(16,18)==0);
 selected(0,false);assert(pixel(16,18)==0xffffff);
 selected(1,true);assert(pixel(46,17)==0);
 selected(2,true);assert(pixel(95,20)==0xa0a0a0);
 selected(1,false);assert(pixel(46,17)==0xffffff);
 wc_release(&cached);wc_release(&uncached);
 puts("native buttons: source16x16 checkbox, check/radio/push pixels, labels and cache invalidation passed");
}
